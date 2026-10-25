package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;

/** Unused preparation candidate for initial input reconciliation. Original
 * routines remain intact. A cloned route/prediction calculation omits control
 * outputs; normal native monitoring resumes on its ordinary path afterwards.
 *
 * Direct-call separation is NOT full isolation qualification. Transitive and
 * virtual call effects, owned outputs, native banks, anchors and command-thread
 * exclusion must pass before any caller or capability can use these methods. */
public final class FmcRouteRefreshPrepare {
    record Clone(Prepare.MethodId original,String generated){}
    static final List<Clone> CLONES=List.of(
        new Clone(new Prepare.MethodId("Routes$RteBody","readGlob","(Ljava/lang/String;)V"),"$psxExporter$fmcReadEnteredRoute"),
        new Clone(new Prepare.MethodId("Routes$RteBody","calcActWptData","(I)V"),"$psxExporter$fmcCalcEnteredAct"),
        new Clone(new Prepare.MethodId("FmcVnav$Pred","readGlob","()V"),"$psxExporter$fmcReadLivePrediction"));
    record Omit(Prepare.MethodId method,Opcode opcode,int singleSlots){}
    static final List<Omit> OMIT=List.of(
        new Omit(new Prepare.MethodId("FmcGlobal$MemBits","trans","(ZZI)V"),Opcode.INVOKEVIRTUAL,4),
        new Omit(new Prepare.MethodId("NavRadios","setIlsAutotune","(Z)V"),Opcode.INVOKESTATIC,1),
        new Omit(new Prepare.MethodId("NavRadios","checkProcedureTuning","()V"),Opcode.INVOKESTATIC,0),
        new Omit(new Prepare.MethodId("Routes$RteBody","sendGlob","(I)V"),Opcode.INVOKEVIRTUAL,2),
        new Omit(new Prepare.MethodId("FmcVnav","monitor","()V"),Opcode.INVOKESTATIC,0));
    private FmcRouteRefreshPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC route refresh candidate "+why);}
    private static Prepare.MethodId identity(InvokeInstruction i){return new Prepare.MethodId(i.owner().asInternalName(),i.name().stringValue(),i.type().stringValue());}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var originals=new HashMap<Prepare.MethodId,MethodModel>();var redirects=new HashMap<Prepare.MethodId,String>();
        var omitCounts=new HashMap<Prepare.MethodId,Integer>();OMIT.forEach(o->omitCounts.put(o.method(),0));
        for(var clone:CLONES){
            var model=data.models.get(clone.original().owner());require(model!=null,"owner");
            var method=model.methods().stream().filter(m->m.methodName().equalsString(clone.original().name())&&m.methodType().equalsString(clone.original().descriptor())).findFirst().orElseThrow();
            require((method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0&&method.code().isPresent(),"ordinary instance method "+clone.original());
            originals.put(clone.original(),method);redirects.put(clone.original(),clone.generated());
            for(var i:Prepare.instructions(method))if(i instanceof InvokeInstruction call){
                var id=identity(call);for(var omitted:OMIT)if(id.equals(omitted.method())){
                    require(call.opcode()==omitted.opcode(),"control-output invocation kind");omitCounts.compute(id,(k,v)->v+1);
                }
            }
        }
        require(omitCounts.values().stream().allMatch(v->v>0),"complete direct control-output frontier");
        var changed=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String name:List.of("Routes$RteBody","FmcVnav$Pred")){
            byte[] input=lower.getOrDefault(name,data.inputs.get(name));require(input!=null,"final owner");var model=data.cf.parse(input);before.put(name,input);
            for(var clone:CLONES)if(clone.original().owner().equals(name))require(model.methods().stream().noneMatch(m->m.methodName().equalsString(clone.generated())),"bridge collision");
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){for(var clone:CLONES)if(clone.original().owner().equals(name)){
                    var original=originals.get(clone.original());
                    // Native readGlob holds its body monitor. Preserve that
                    // contract in this worker-only candidate; this is never a
                    // physics callback and does not establish writer exclusion.
                    b.withMethod(clone.generated(),MethodTypeDesc.ofDescriptor(clone.original().descriptor()),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC|(original.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED),m->m.transformCode(original.code().orElseThrow(),(code,e)->{
                        if(e instanceof InvokeInstruction call){
                            var id=identity(call);var omit=OMIT.stream().filter(o->o.method().equals(id)).findFirst();
                            if(omit.isPresent()){for(int i=0;i<omit.get().singleSlots();i++)code.pop();return;}
                            String redirect=redirects.get(id);if(redirect!=null){
                                require(call.opcode()==Opcode.INVOKEVIRTUAL,"calculation invocation kind");
                                code.invokevirtual(ClassDesc.of(id.owner()),redirect,MethodTypeDesc.ofDescriptor(id.descriptor()));return;
                            }
                        }
                        code.with(e);
                    }));
                }}
            });
            require(data.cf.verify(output).isEmpty(),"generated code verification "+name);changed.put(name,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report");Path jar=Path.of(arguments[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        var out=new StringBuilder("{\"schema\":1,\"state\":\"direct_frontier_candidate_only\",\"captureQualified\":false,\"recallQualified\":false,\"classes\":[");boolean comma=false;
        for(String name:List.of("Routes$RteBody","FmcVnav$Pred")){
            if(comma)out.append(',');comma=true;out.append("{\"name\":").append(FmcQualification.quote(name)).append(",\"inputSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(name))))
                .append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(changed.get(name)))).append('}');
        }
        out.append("],\"limit\":\"Direct-call separation only. Transitive bank transfers, CDU callbacks, network effects and virtual dispatch remain unqualified. No native caller/adapter or FMC capability uses these candidate methods.\"}\n");
        Files.writeString(Path.of(arguments[1]),out,StandardOpenOption.CREATE_NEW);
        System.out.println("Direct route calculation frontier candidate verified as data; full isolation unavailable");
    }
}
