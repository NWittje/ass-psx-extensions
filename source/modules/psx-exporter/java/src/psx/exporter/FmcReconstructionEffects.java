package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;

/** Current navigation reconstruction, copied from exact installed arithmetic.
 * These narrow clones omit route-bank publication and waypoint wind erasure;
 * all other instructions, branch targets and native exceptions are retained.
 * Effect qualification rejects static writes, array membership changes,
 * opaque calls, virtual overrides and newly reachable external effects. The
 * ordinary EXEC adapter must bind exact owned bodies/waypoints and coherently
 * exclude the listed current inputs before invoking them. */
final class FmcReconstructionEffects {
    record Clone(Prepare.MethodId original,String generated){}
    static final List<Clone> CLONES=List.of(
        new Clone(id("Routes$RteBody","setCrsFrom6R","(I)V"),"$psxExporter$fmcReconstructEnteredCourse"),
        new Clone(id("Routes$RteBody","setRwyAnchor","(I)V"),"$psxExporter$fmcReconstructGroundAnchor"),
        new Clone(id("Routes$RteBody","execCrsFromWpt","()V"),"$psxExporter$fmcReconstructCourseFrom"),
        new Clone(id("Routes$RteBody","setDynCrsDirectTo","()V"),"$psxExporter$fmcReconstructDynamicDirect"),
        new Clone(id("Routes$RteBody","doOneSecondTask","()V"),"$psxExporter$fmcReconstructDynamicCourse"),
        new Clone(id("RouteWpt","blank","()V"),"$psxExporter$fmcBlankPreserveForecast"));
    private static final Prepare.MethodId SEND=id("Routes$RteBody","sendGlob","(I)V"),WIND_BLANK=id("FmcWindOat","blank","()V"),RADIUS_OUTPUT=id("Qi","transfer","(ICI)I");
    private static final Set<String> WRITE_OWNERS=Set.of("Routes$RteBody","RouteWpt","Calc$PlaceBearingDistance","Calc$PlaceBearingPlaceBearing");
    private static final Set<Prepare.MethodId> JAVA=Set.of(
        id("java/lang/Math","abs","(D)D"),id("java/lang/Math","sin","(D)D"),id("java/lang/Math","cos","(D)D"),
        id("java/lang/Math","atan2","(DD)D"),id("java/lang/Math","asin","(D)D"),id("java/lang/Math","acos","(D)D"),id("java/lang/Math","sqrt","(D)D"),id("java/lang/Math","tan","(D)D"),
        id("java/lang/String","equals","(Ljava/lang/Object;)Z"),id("java/lang/Object","<init>","()V"));
    record Qualified(Map<String,byte[]> changed,Set<Prepare.MethodId> methods,
                     Set<FmcQualification.Member> writes,Set<FmcQualification.Member> currentInputs){}
    private FmcReconstructionEffects(){}
    private static Prepare.MethodId id(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    private static Prepare.MethodId id(InvokeInstruction call){return id(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction effects "+why);}
    private static MethodModel method(Map<String,ClassModel> models,Prepare.MethodId id){
        var owner=models.get(id.owner());require(owner!=null,"method owner "+id);
        var matches=owner.methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).toList();
        require(matches.size()==1&&matches.getFirst().code().isPresent(),"exact method body "+id);return matches.getFirst();
    }
    private static byte[] normalized(Prepare data,Prepare.MethodId id,MethodModel m){
        return data.cf.build(ClassDesc.of(id.owner()),b->b.withMethod(id.name(),m.methodTypeSymbol(),m.flags().flagsMask(),x->x.transformCode(m.code().orElseThrow(),(c,e)->c.with(e))));
    }
    private static boolean subtype(Map<String,ClassModel> models,String owner,String base){
        var seen=new HashSet<String>();while(seen.add(owner)){if(owner.equals(base))return true;var m=models.get(owner);if(m==null||m.superclass().isEmpty())return false;owner=m.superclass().get().asInternalName();}
        throw new IllegalArgumentException("FMC reconstruction cyclic class hierarchy");
    }
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var pending=new ArrayDeque<Prepare.MethodId>();CLONES.forEach(x->pending.add(x.original()));
        var seen=new HashSet<Prepare.MethodId>();var writes=new HashSet<FmcQualification.Member>();var inputs=new HashSet<FmcQualification.Member>();int sends=0,windBlanks=0,radiusReads=0;
        while(!pending.isEmpty()){
            var next=pending.removeFirst();if(!seen.add(next))continue;require(seen.size()<=40,"finite arithmetic closure");
            var nativeMethod=method(models,next);require((nativeMethod.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"opaque calculation "+next);
            require(Arrays.equals(normalized(data,next,method(data.models,next)),normalized(data,next,nativeMethod)),"changed original method "+next);
            var instructions=Prepare.instructions(nativeMethod);for(int index=0;index<instructions.size();index++){var ins=instructions.get(index);
                var op=ins.opcode();require(op!=Opcode.PUTSTATIC&&op!=Opcode.AASTORE&&op!=Opcode.IASTORE&&op!=Opcode.FASTORE&&op!=Opcode.DASTORE
                    &&op!=Opcode.LASTORE&&op!=Opcode.CASTORE&&op!=Opcode.SASTORE&&op!=Opcode.BASTORE,"static/array write "+next);
                require(!(ins instanceof InvokeDynamicInstruction),"opaque dynamic call");
                if(ins instanceof FieldInstruction f){
                    var field=FmcQualification.resolveField(models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());
                    require(field!=null,"field binding "+f.owner()+"."+f.name());
                    if(op==Opcode.PUTFIELD){require(WRITE_OWNERS.contains(field.owner()),"external instance write "+field+" in "+next);writes.add(field);}
                    if(op==Opcode.GETSTATIC||op==Opcode.GETFIELD&&!WRITE_OWNERS.contains(field.owner()))inputs.add(field);
                }
                if(!(ins instanceof InvokeInstruction call))continue;var target=id(call);
                if(target.equals(SEND)){require(CLONES.stream().anyMatch(x->x.original().equals(next))&&call.opcode()==Opcode.INVOKEVIRTUAL,"direct bank-publication frontier");sends++;continue;}
                if(target.equals(WIND_BLANK)){require(next.equals(id("RouteWpt","blank","()V"))&&call.opcode()==Opcode.INVOKEVIRTUAL,"direct current-forecast erase frontier");windBlanks++;continue;}
                if(target.equals(RADIUS_OUTPUT)){
                    require(next.equals(id("Routes$RteBody","setDynCrsDirectTo","()V"))&&call.opcode()==Opcode.INVOKEVIRTUAL&&index>=9,"direct calculated-radius output");
                    require(field(instructions.get(index-9),"Glob","qi","[LQi;")&&field(instructions.get(index-8),"Glob","qiActLnavRadius","I")
                        &&instructions.get(index-7).opcode()==Opcode.AALOAD&&constant(instructions.get(index-2),(int)'w')&&constant(instructions.get(index-1),1)
                        &&index+1<instructions.size()&&instructions.get(index+1).opcode()==Opcode.POP,"exact radius receiver/write mode/discarded result");
                    radiusReads++;continue;
                }
                if(target.owner().startsWith("java/")){require(JAVA.contains(target),"external arithmetic call "+target);continue;}
                var called=method(models,target);require(((called.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==(call.opcode()==Opcode.INVOKESTATIC),"call staticness "+target);
                require(call.opcode()!=Opcode.INVOKEINTERFACE,"opaque interface call");
                if(call.opcode()==Opcode.INVOKEVIRTUAL)for(var candidate:models.entrySet())if(!candidate.getKey().equals(target.owner())&&subtype(models,candidate.getKey(),target.owner()))
                    require(candidate.getValue().methods().stream().noneMatch(m->m.methodName().equalsString(target.name())&&m.methodType().equalsString(target.descriptor())),"virtual override "+target);
                pending.addLast(target);
            }
        }
        require(sends==5&&windBlanks==1&&radiusReads==1,"exact output/forecast/current-radius frontier");
        var changed=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String owner:List.of("Routes$RteBody","RouteWpt")){
            byte[] original=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,original);var m=data.cf.parse(original);
            for(var clone:CLONES)if(clone.original().owner().equals(owner))require(m.methods().stream().noneMatch(x->x.methodName().equalsString(clone.generated())),"clone collision");
            byte[] output=data.cf.transformClass(m,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){for(var clone:CLONES)if(clone.original().owner().equals(owner)){
                    var source=method(data.models,clone.original());int[] offset={0};b.withMethod(clone.generated(),MethodTypeDesc.ofDescriptor(clone.original().descriptor()),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC,
                        x->x.transformCode(source.code().orElseThrow(),(c,e)->{
                            if(e instanceof Instruction instruction){int position=offset[0];offset[0]+=instruction.sizeInBytes();
                                if(clone.original().name().equals("setCrsFrom6R")){
                                    // The saved native course is tenths of a
                                    // degree, including typed-bearing E input.
                                    // Retain its precision while using the
                                    // native degree projection instructions.
                                    if(position==81||position==151){require(instruction instanceof LoadInstruction load&&load.slot()==1,"exact entered course operand");c.iload(1).i2d().loadConstant(.1d).dmul();return;}
                                    if(position==82||position==152){require(instruction.opcode()==Opcode.I2D,"exact entered course conversion");return;}
                                    if(position==204){require(constant(instruction,10),"exact entered course scale");return;}
                                    if(position==206){require(instruction.opcode()==Opcode.IMUL,"exact entered course assignment scale");return;}
                                }
                            }
                            if(e instanceof InvokeInstruction call){var target=id(call);
                                if(target.equals(SEND)){c.pop().pop();return;}
                                if(target.equals(WIND_BLANK)){c.pop();return;}
                                // The staged body retains its own calculated
                                // radius; native guidance publication belongs
                                // to the final ordinary EXEC recipe.
                                if(target.equals(RADIUS_OUTPUT)){c.pop().pop().pop().pop().iconst_0();return;}
                                var replacement=CLONES.stream().filter(y->y.original().equals(target)).findFirst();
                                if(replacement.isPresent()){c.invokevirtual(ClassDesc.of(target.owner()),replacement.get().generated(),MethodTypeDesc.ofDescriptor(target.descriptor()));return;}
                            }
                            c.with(e);
                        }));
                }}
            });require(data.cf.verify(output).isEmpty(),"generated verifier "+owner);changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        return new Qualified(Map.copyOf(changed),Set.copyOf(seen),Set.copyOf(writes),Set.copyOf(inputs));
    }
    private static boolean constant(Instruction instruction,int expected){return instruction instanceof ConstantInstruction c&&Objects.equals(c.constantValue(),expected);}
    private static boolean field(Instruction instruction,String owner,String name,String type){return instruction instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals(owner)&&f.name().equalsString(name)&&f.type().equalsString(type);}
}
