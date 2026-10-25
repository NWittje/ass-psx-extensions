package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Worker/ordinary-thread geometry candidate, never an extra simulation step.
 * Every reachable vendor method is inspected as data. Instance-write ownership
 * is only an effect boundary: the eventual native adapter must also prove that
 * each receiver belongs to its staged/current route and bind coherent live
 * navigation inputs. This component cannot advertise capture or recall. */
public final class FmcGeometryPrepare {
    record Clone(Prepare.MethodId original,String generated){}
    record Effects(Set<Prepare.MethodId> methods,Set<FmcQualification.Member> writes,
                   Set<FmcQualification.Member> staticInputs,int arrayStores){}
    static final List<Clone> CLONES=List.of(
        new Clone(id("Routes$RteBody","calcActWptData","(I)V"),"$psxExporter$fmcCalculateRouteGeometry"),
        new Clone(id("Routes$RteBody","makeCrsTo2","(I)V"),"$psxExporter$fmcMakeRouteCourse"));
    static final Prepare.MethodId SEND=id("Routes$RteBody","sendGlob","(I)V"),
        BANK=id("Qi","transfer","(ICI)I"),MESSAGE=id("FmcGlobal","addHiMsg","(I)V");
    static final Set<String> WRITE_OWNERS=Set.of("Routes$RteBody","RouteWpt","Calc$PlaceBearingDistance",
        "Calc$PlaceBearingPlaceBearing","FmcWindOat","FmcWindOat$WindOatAlt");
    static final Set<Prepare.MethodId> EXTERNAL=Set.of(
        id("java/lang/Math","abs","(D)D"),id("java/lang/Math","acos","(D)D"),
        id("java/lang/Math","asin","(D)D"),id("java/lang/Math","atan2","(DD)D"),
        id("java/lang/Math","cos","(D)D"),id("java/lang/Math","round","(F)I"),
        id("java/lang/Math","sin","(D)D"),id("java/lang/Math","sqrt","(D)D"),
        id("java/lang/Math","tan","(D)D"),id("java/lang/String","isEmpty","()Z"),
        id("java/lang/Object","<init>","()V"));
    private FmcGeometryPrepare(){}
    private static Prepare.MethodId id(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    private static Prepare.MethodId id(InvokeInstruction i){return id(i.owner().asInternalName(),i.name().stringValue(),i.type().stringValue());}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC geometry "+why);}
    private static MethodModel declared(Map<String,ClassModel> models,Prepare.MethodId id){
        var owner=models.get(id.owner());require(owner!=null,"missing method owner "+id);
        var all=owner.methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).toList();
        require(all.size()==1,"missing/ambiguous descriptor "+id);return all.getFirst();
    }
    // Normalize constant-pool indices in a single-method class. This retains
    // branch/exception targets and operands, unlike an opcode-only fingerprint.
    private static byte[] body(Prepare data,String owner,MethodModel method){
        return data.cf.build(ClassDesc.of(owner),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),
            m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));
    }
    private static boolean subtype(Map<String,ClassModel> models,String owner,String base){
        var seen=new HashSet<String>();String current=owner;
        while(seen.add(current)){
            if(current.equals(base))return true;var model=models.get(current);
            if(model==null||model.superclass().isEmpty())return false;current=model.superclass().get().asInternalName();
        }
        throw new IllegalArgumentException("FMC geometry cyclic superclass");
    }
    static Effects qualify(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var pending=new ArrayDeque<Prepare.MethodId>();
        CLONES.forEach(c->pending.add(c.original()));var seen=new HashSet<Prepare.MethodId>();var writes=new HashSet<FmcQualification.Member>();
        var staticInputs=new HashSet<FmcQualification.Member>();int arrays=0;
        var omitted=new HashMap<Prepare.MethodId,Integer>();for(var x:List.of(SEND,BANK,MESSAGE))omitted.put(x,0);
        while(!pending.isEmpty()){
            var id=pending.removeFirst();if(!seen.add(id))continue;require(seen.size()<=128,"method closure bound");
            var method=declared(models,id);require(method.code().isPresent()&&(method.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"opaque native method "+id);
            if(CLONES.stream().anyMatch(c->c.original().equals(id)))require((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0,"instance calculation root");
            var original=declared(data.models,id);require(original.code().isPresent()&&Arrays.equals(body(data,id.owner(),original),body(data,id.owner(),method)),"changed final method body "+id);
            var instructions=Prepare.instructions(method);
            for(int index=0;index<instructions.size();index++){
                var instruction=instructions.get(index);var op=instruction.opcode();
                if(instruction instanceof FieldInstruction f&&(op==Opcode.GETSTATIC||op==Opcode.GETFIELD)&&!f.owner().asInternalName().startsWith("java/")){
                    var input=FmcQualification.resolveField(models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());
                    require(input!=null,"unresolved input field "+f.owner()+"."+f.name());
                    var field=models.get(input.owner()).fields().stream().filter(x->x.fieldName().equalsString(input.name())&&x.fieldType().equalsString(input.descriptor())).findFirst().orElseThrow();
                    require(((field.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==(op==Opcode.GETSTATIC),"input field staticness "+input);
                    // A write-owner whitelist does not prove isolated staging.
                    // These exact inputs require coherent current-state binding;
                    // route object identity also changes native branch choices.
                    if(op==Opcode.GETSTATIC)staticInputs.add(input);
                }
                if(instruction instanceof FieldInstruction f&&(op==Opcode.PUTSTATIC||op==Opcode.PUTFIELD)){
                    require(op==Opcode.PUTFIELD&&WRITE_OWNERS.contains(f.owner().asInternalName()),"external/static write "+f.owner()+"."+f.name());
                    var fields=models.get(f.owner().asInternalName());require(fields!=null,"missing write owner");
                    require(fields.fields().stream().anyMatch(x->x.fieldName().equalsString(f.name().stringValue())&&x.fieldType().equalsString(f.type().stringValue())
                        &&(x.flags().flagsMask()&ClassFile.ACC_STATIC)==0),"instance write descriptor");
                    writes.add(new FmcQualification.Member(f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue()));
                }
                if(op==Opcode.IASTORE||op==Opcode.LASTORE||op==Opcode.FASTORE||op==Opcode.DASTORE||op==Opcode.AASTORE||op==Opcode.BASTORE||op==Opcode.CASTORE||op==Opcode.SASTORE)arrays++;
                require(!(instruction instanceof InvokeDynamicInstruction),"dynamic call");
                if(!(instruction instanceof InvokeInstruction call))continue;var target=id(call);
                if(omitted.containsKey(target)){
                    // Only the two cloned bodies remove outputs. A newly
                    // reachable indirect bank/message call rejects the layer.
                    require(CLONES.stream().anyMatch(c->c.original().equals(id)),"indirect output "+target);
                    require(call.opcode()==(target.equals(MESSAGE)?Opcode.INVOKESTATIC:Opcode.INVOKEVIRTUAL),"output invocation kind");
                    if(target.equals(BANK))require(index+1<instructions.size()&&instructions.get(index+1).opcode()==Opcode.POP,"bank result must be discarded");
                    omitted.compute(target,(k,v)->v+1);continue;
                }
                if(target.owner().startsWith("java/")){
                    require(EXTERNAL.contains(target),"unqualified external call "+target);
                    require(call.opcode()==(target.owner().equals("java/lang/Math")?Opcode.INVOKESTATIC:target.name().equals("<init>")?Opcode.INVOKESPECIAL:Opcode.INVOKEVIRTUAL),"external invocation kind");continue;
                }
                var called=declared(models,target);boolean stat=(called.flags().flagsMask()&ClassFile.ACC_STATIC)!=0;
                require(stat==(call.opcode()==Opcode.INVOKESTATIC),"method staticness "+target);
                require(call.opcode()!=Opcode.INVOKEINTERFACE,"interface dispatch");
                if(call.opcode()==Opcode.INVOKEVIRTUAL){
                    // Binding exact receivers is still required. Also reject
                    // every installed override that changes the static closure.
                    for(var entry:models.entrySet())if(!entry.getKey().equals(target.owner())&&subtype(models,entry.getKey(),target.owner()))
                        require(entry.getValue().methods().stream().noneMatch(m->m.methodName().equalsString(target.name())&&m.methodType().equalsString(target.descriptor())),"unqualified virtual override "+target);
                }
                pending.addLast(target);
            }
        }
        require(omitted.values().stream().allMatch(n->n>0),"complete output frontier");
        return new Effects(Set.copyOf(seen),Set.copyOf(writes),Set.copyOf(staticInputs),arrays);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        qualify(data,lower);String owner="Routes$RteBody";byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));var model=data.cf.parse(input);
        for(var clone:CLONES)require(model.methods().stream().noneMatch(m->m.methodName().equalsString(clone.generated())),"bridge collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){for(var clone:CLONES){var original=declared(data.models,clone.original());
                b.withMethod(clone.generated(),MethodTypeDesc.ofDescriptor(clone.original().descriptor()),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC|(original.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED),
                    m->m.transformCode(original.code().orElseThrow(),(c,e)->{
                        if(e instanceof InvokeInstruction call){var target=id(call);
                            if(target.equals(SEND)){c.pop().pop();return;}
                            if(target.equals(MESSAGE)){c.pop();return;}
                            if(target.equals(BANK)){c.pop().pop().pop().pop().iconst_0();return;}
                            var redirect=CLONES.stream().filter(x->x.original().equals(target)).findFirst();
                            if(redirect.isPresent()){c.invokevirtual(ClassDesc.of(owner),redirect.get().generated(),MethodTypeDesc.ofDescriptor(target.descriptor()));return;}
                        }
                        c.with(e);
                    }));
            }}
        });
        require(data.cf.verify(output).isEmpty(),"generated bytecode verification");var before=new HashMap<>(lower);before.put(owner,input);var changed=new HashMap<>(lower);changed.put(owner,output);
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report");Path jar=Path.of(arguments[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var effects=qualify(data,Map.of());var changed=transform(data,Map.of());
        String inputs=effects.staticInputs().stream().sorted().map(x->FmcQualification.quote(x.toString())).collect(java.util.stream.Collectors.joining(","));
        String report="{\"schema\":1,\"state\":\"route_geometry_effect_component\",\"captureQualified\":false,\"recallQualified\":false,\"nativeMethods\":"+effects.methods().size()+",\"instanceOutputs\":"+effects.writes().size()+",\"staticInputs\":["+inputs+"],\"arrayStores\":"+effects.arrayStores()+",\"preparedSha256\":"+FmcQualification.quote(Identity.hash(changed.get("Routes$RteBody")))+",\"limit\":\"Receiver ownership, route parsing, live-input coherence, native dispatcher and complete adapter remain unqualified. Installed classes are data only.\"}\n";
        Files.writeString(Path.of(arguments[1]),report,StandardOpenOption.CREATE_NEW);System.out.println("Route geometry effect component methods="+effects.methods().size()+"; native adapter unavailable");
    }
}
