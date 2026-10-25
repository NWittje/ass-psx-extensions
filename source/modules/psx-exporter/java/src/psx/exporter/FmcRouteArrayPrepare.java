package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Call-site witnesses chosen only by the receiver-flow proof. Native argument
 * evaluation precedes the wrapper exactly as before. Helpers cannot replace a
 * native return value or exception, including when failure reporting fails. */
public final class FmcRouteArrayPrepare {
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcRouteArrayCoherence");
    private static final String PREFIX="$psxExporter$fmcRouteArrayMutation";
    private record Wrapper(String name,MethodTypeDesc type,FmcRouteArrayFlowAudit.Site site){}
    private FmcRouteArrayPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var proof=FmcRouteArrayFlowAudit.audit(data,lower);var groups=new TreeMap<String,List<FmcRouteArrayFlowAudit.Site>>();
        for(var site:proof.mutations())groups.computeIfAbsent(site.method().owner(),k->new ArrayList<>()).add(site);
        var before=new HashMap<>(lower);var changed=new HashMap<>(lower);
        for(var group:groups.entrySet()){
            String owner=group.getKey();var input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);
            if(model.methods().stream().anyMatch(m->m.methodName().stringValue().startsWith(PREFIX)))throw new IllegalArgumentException("FMC route array wrapper collision");
            group.getValue().sort(Comparator.comparing((FmcRouteArrayFlowAudit.Site s)->s.method().toString()).thenComparingInt(FmcRouteArrayFlowAudit.Site::ordinal));
            var wrappers=new HashMap<Prepare.MethodId,Map<Integer,Wrapper>>();var ordered=new ArrayList<Wrapper>();
            for(var site:group.getValue()){
                var nativeType=MethodTypeDesc.ofDescriptor(site.descriptor());
                var type=site.stat()?nativeType:nativeType.insertParameterTypes(0,ClassDesc.of(site.owner().replace('/','.')));
                var wrapper=new Wrapper(PREFIX+ordered.size(),type,site);ordered.add(wrapper);
                wrappers.computeIfAbsent(site.method(),k->new HashMap<>()).put(site.ordinal(),wrapper);
            }
            ClassDesc caller=ClassDesc.of(owner.replace('/','.'));
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                    if(element instanceof MethodModel m){
                        var selected=wrappers.get(new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue()));
                        if(selected!=null){builder.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                            int ordinal;
                            public void accept(CodeBuilder c,CodeElement e){
                                if(e instanceof Instruction instruction){var w=selected.get(ordinal++);if(w!=null){
                                    boolean store=w.site.name().equals("AASTORE");
                                    if(store?instruction.opcode()!=Opcode.AASTORE:!(e instanceof InvokeInstruction call)||call.opcode()!=Opcode.INVOKESTATIC
                                        ||!call.owner().asInternalName().equals(w.site.owner())||!call.name().equalsString(w.site.name())||!call.type().equalsString(w.site.descriptor()))
                                        throw new IllegalArgumentException("FMC route array proof/site mismatch");
                                    c.invokestatic(caller,w.name,w.type);return;
                                }}c.with(e);
                            }
                        }));return;}
                    }builder.with(element);
                }
                public void atEnd(ClassBuilder builder){for(var wrapper:ordered)add(builder,wrapper);}
            });
            if(!data.cf.verify(output).isEmpty())throw new IllegalArgumentException("FMC route array generated wrapper verification");changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static TypeKind kind(ClassDesc type){return switch(type.descriptorString().charAt(0)){
        case 'V'->TypeKind.VOID;case 'J'->TypeKind.LONG;case 'D'->TypeKind.DOUBLE;case 'F'->TypeKind.FLOAT;
        case 'L','['->TypeKind.REFERENCE;default->TypeKind.INT;};}
    private static int width(TypeKind kind){return kind==TypeKind.LONG||kind==TypeKind.DOUBLE?2:1;}
    private static void add(ClassBuilder b,Wrapper wrapper){
        b.withMethodBody(wrapper.name,wrapper.type,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            int ticket=c.allocateLocal(TypeKind.INT),original=c.allocateLocal(TypeKind.REFERENCE);var resultKind=kind(wrapper.type.returnType());int result=resultKind==TypeKind.VOID?-1:c.allocateLocal(resultKind);
            c.iconst_0().istore(ticket);guard(c,()->c.aload(wrapper.site.name().equals("AASTORE")?0:2).invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int,CD_Object)).istore(ticket));
            var begin=c.newLabel();var end=c.newLabel();var handler=c.newLabel();c.labelBinding(begin);int slot=0;
            for(var argument:wrapper.type.parameterList()){var k=kind(argument);c.loadLocal(k,slot);slot+=width(k);}
            var site=wrapper.site;
            if(site.name().equals("AASTORE"))c.aastore();else c.invokestatic(ClassDesc.of("java.lang.System"),"arraycopy",MethodTypeDesc.ofDescriptor(FmcRouteArrayFlowAudit.COPY));
            if(result>=0)c.storeLocal(resultKind,result);
            c.labelBinding(end);guard(c,()->c.iload(ticket).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)));
            if(result>=0)c.loadLocal(resultKind,result);c.return_(resultKind);
            c.exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(original);
            guard(c,()->c.iload(ticket).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)));c.aload(original).athrow();
        });
    }
    private static void guard(CodeBuilder c,Runnable injected){
        var begin=c.newLabel();var end=c.newLabel();var failure=c.newLabel();var done=c.newLabel();
        c.labelBinding(begin);injected.run();c.labelBinding(end).goto_(done).exceptionCatchAll(begin,end,failure).labelBinding(failure).pop();
        var report=c.newLabel();var reportEnd=c.newLabel();var reportFailure=c.newLabel();
        c.labelBinding(report).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reportEnd).goto_(done)
            .exceptionCatchAll(report,reportEnd,reportFailure).labelBinding(reportFailure).pop().labelBinding(done);
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=2)throw new IllegalArgumentException("installed JAR and owned report");var jar=Path.of(args[0]);
        if(!Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR))throw new IllegalArgumentException("FMC installed identity");
        var data=new Prepare(Prepare.readJar(jar));var proof=FmcRouteArrayFlowAudit.audit(data,Map.of());var output=transform(data,Map.of());
        var ordered=new TreeMap<>(output);var text=new StringBuilder("{\"schema\":1,\"state\":\"route_array_mutator_component\",\"captureQualified\":false,\"recallQualified\":false,\"provedMutationSites\":").append(proof.mutations().size()).append(",\"classes\":[");boolean comma=false;
        for(var entry:ordered.entrySet()){if(comma)text.append(',');comma=true;text.append("{\"name\":").append(FmcQualification.quote(entry.getKey())).append(",\"inputSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(entry.getKey())))).append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(entry.getValue()))).append('}');}
        text.append("],\"limit\":\"Guarded call-site component only. Actual destination and native exception semantics retained; full field/alias profile, capture, exclusion and recall remain unavailable. No installed classes defined/initialized/invoked.\"}\n");
        Files.writeString(Path.of(args[1]),text,StandardOpenOption.CREATE_NEW);System.out.println("Route array call-site witnesses verify as data: "+output.size()+" classes; native FMC capability unavailable");
    }
}
