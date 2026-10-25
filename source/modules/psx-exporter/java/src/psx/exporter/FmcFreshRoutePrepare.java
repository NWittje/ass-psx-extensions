package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Fresh private route construction preserves ordinary native object defaults.
 * The generated entry takes only six trusted live indices: no artifact may
 * supply them, and no caller can pass an existing route/body reference. Native
 * constructors never run during preparation. This component supplies neither
 * an input profile nor publication/exclusion/capture/recall qualification. */
public final class FmcFreshRoutePrepare {
    static final String NAME="$psxExporter$fmcFreshRoute";
    static final String HEADER="FmcRte$Rte",BODY="Routes$RteBody";
    static final String HEADER_CTOR="(IIIIILRoutes$RteBody;)V";
    static final Set<String> OWNERS=Set.of("Calc$LonsAtLat","Calc$PlaceBearingDistance","Calc$PlaceBearingPlaceBearing",
        "FmcDepArr$AppList","FmcDepArr$AppPack","FmcDepArr$ArrList","FmcDepArr$ArrPack",
        "FmcDepArr$SidList","FmcDepArr$SidPack","FmcDepArr$StringList",HEADER,"FmcRte$Rwy",
        "FmcWindOat","FmcWindOat$WindOatAlt","RouteWpt","Routes$RhoLatBrg",BODY,"Routes$Via","ScanRunway$RwyInfo");
    static final Set<Prepare.MethodId> JDK=Set.of(id("java/lang/Object","()V"),id("java/lang/StringBuilder","()V"),
        id("java/lang/StringBuilder","(I)V"),id("java/lang/StringBuilder","(Ljava/lang/String;)V"),id("java/awt/geom/GeneralPath","()V"));
    record Qualified(Map<String,byte[]> changed,Set<Prepare.MethodId> constructors){}
    private FmcFreshRoutePrepare(){}
    private static Prepare.MethodId id(String owner,String descriptor){return new Prepare.MethodId(owner,"<init>",descriptor);}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC fresh route "+why);}
    private static MethodModel method(Map<String,ClassModel> models,Prepare.MethodId id){
        var owner=models.get(id.owner());require(owner!=null,"missing constructor owner");
        var matches=owner.methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).toList();
        require(matches.size()==1,"constructor descriptor");return matches.getFirst();
    }
    private static byte[] body(Prepare data,String owner,MethodModel method){
        return data.cf.build(ClassDesc.of(owner),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),
            m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));
    }
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var pending=new ArrayDeque<Prepare.MethodId>();
        pending.add(id(HEADER,HEADER_CTOR));pending.add(id(BODY,"(I)V"));var seen=new HashSet<Prepare.MethodId>();
        while(!pending.isEmpty()){
            var next=pending.removeFirst();if(!seen.add(next))continue;require(seen.size()<=32&&OWNERS.contains(next.owner()),"constructor closure");
            var current=method(models,next);require(current.code().isPresent()&&(current.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==0,"ordinary constructor");
            require(current.code().orElseThrow().exceptionHandlers().isEmpty(),"constructor handler");
            var original=method(data.models,next);require(Arrays.equals(body(data,next.owner(),original),body(data,next.owner(),current)),"changed native constructor body");
            // All entry references are fresh: the only top-level reference
            // parameter is the body allocated by the generated bridge. Nested
            // references originate in that fresh graph or immutable literals.
            // No static read, opaque call or external reference source is allowed.
            for(var instruction:Prepare.instructions(current)){
                var op=instruction.opcode();
                require(op!=Opcode.GETSTATIC&&op!=Opcode.PUTSTATIC&&op!=Opcode.MONITORENTER&&op!=Opcode.MONITOREXIT,"static state/monitor");
                require(!(instruction instanceof InvokeDynamicInstruction),"dynamic invocation");
                if(instruction instanceof ConstantInstruction c){var v=c.constantValue();require(v==null||v instanceof Number||v instanceof String,"non-data constant");}
                if(instruction instanceof NewObjectInstruction n){String owner=n.className().asInternalName();require(OWNERS.contains(owner)||owner.equals("java/lang/StringBuilder")||owner.equals("java/awt/geom/GeneralPath"),"unqualified allocation");}
                if(instruction instanceof NewReferenceArrayInstruction n){String owner=n.componentType().asInternalName();require(OWNERS.contains(owner)||owner.equals("java/lang/String")||owner.equals("java/lang/StringBuilder"),"unqualified array component");}
                if(instruction instanceof TypeCheckInstruction n){String owner=n.type().asInternalName();require(OWNERS.contains(owner)||owner.equals("java/lang/String"),"unqualified reference conversion");}
                if(instruction instanceof FieldInstruction f){
                    require(OWNERS.contains(f.owner().asInternalName())&&(op==Opcode.GETFIELD||op==Opcode.PUTFIELD),"external field access");
                    var owner=models.get(f.owner().asInternalName());require(owner!=null,"field owner");
                    require(owner.fields().stream().anyMatch(x->x.fieldName().equalsString(f.name().stringValue())&&x.fieldType().equalsString(f.type().stringValue())&&(x.flags().flagsMask()&ClassFile.ACC_STATIC)==0),"field descriptor");
                }
                if(!(instruction instanceof InvokeInstruction call))continue;
                var target=new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());
                require(call.opcode()==Opcode.INVOKESPECIAL&&target.name().equals("<init>"),"non-constructor call");
                if(JDK.contains(target))continue;
                require(OWNERS.contains(target.owner()),"external constructor");pending.add(target);
            }
        }
        // This exact installed family constructs the complete fresh graph;
        // omission or expansion rejects the optional component.
        require(seen.stream().map(Prepare.MethodId::owner).collect(java.util.stream.Collectors.toSet()).equals(OWNERS)&&seen.size()==OWNERS.size(),"complete constructor family");
        var source=models.get(HEADER);require(source.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"factory collision");
        ClassDesc header=ClassDesc.of(HEADER),routeBody=ClassDesc.of(BODY);
        byte[] prepared=data.cf.transformClass(source,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){b.withMethodBody(NAME,MethodTypeDesc.of(CD_Object,CD_int,CD_int,CD_int,CD_int,CD_int,CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                c.new_(header).dup();for(int i=0;i<5;i++)c.iload(i);
                c.new_(routeBody).dup().iload(5).invokespecial(routeBody,"<init>",MethodTypeDesc.of(CD_void,CD_int));
                c.invokespecial(header,"<init>",MethodTypeDesc.ofDescriptor(HEADER_CTOR)).areturn();
            });}
        });
        require(data.cf.verify(prepared).isEmpty(),"factory verifier");
        var before=new HashMap<>(lower);before.put(HEADER,lower.getOrDefault(HEADER,data.inputs.get(HEADER)));
        var changed=new HashMap<>(lower);changed.put(HEADER,prepared);TerrainAccess.verifyAdded(before,changed,data.inputs);
        return new Qualified(Map.copyOf(changed),Set.copyOf(seen));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");Path jar=Path.of(args[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var qualified=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"fresh_native_route_constructor_component_only\",\"captureQualified\":false,\"recallQualified\":false,\"constructors\":"+qualified.constructors().size()
            +",\"headerInputSha256\":\""+Identity.hash(data.inputs.get(HEADER))+"\",\"preparedSha256\":\""+Identity.hash(qualified.changed().get(HEADER))+"\",\"limits\":\"Initialization, trusted live index binding, entry profile, anchor/progress rebuilding, aliases, writer/job exclusion and publication remain unqualified. No installed class defined/initialized/invoked.\"}\n";
        Files.writeString(Path.of(args[1]),report,StandardOpenOption.CREATE_NEW);
        System.out.println("Fresh route constructor class-data qualification only; native FMC capture/recall unavailable");
    }
}
