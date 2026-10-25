package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Generated FMC copy witnesses. Inputs must come from the completed field and
 * alias proof; discovery alone must never choose the production method list. */
final class FmcWriterPrepare {
    private static final ClassDesc GUARD=ClassDesc.of("psx.exporter.FmcCoherence");
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    private FmcWriterPrepare(){}

    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,
                                        Set<Prepare.MethodId> qualifiedMethods) {
        return transform(data,lower,qualifiedMethods,GUARD);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,
                                        Set<Prepare.MethodId> qualifiedMethods,ClassDesc guard) {
        return transform(data,lower,qualifiedMethods,guard,Set.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,
                                        Set<Prepare.MethodId> qualifiedMethods,ClassDesc guard,Set<Prepare.MethodId> nullGetters) {
        return transform(data,lower,qualifiedMethods,guard,nullGetters,Map.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,
                                        Set<Prepare.MethodId> qualifiedMethods,ClassDesc guard,Set<Prepare.MethodId> nullGetters,
                                        Map<Prepare.MethodId,Set<FmcQualification.Member>> changedFields) {
        for(var entry:changedFields.entrySet()){
            var id=entry.getKey();var method=data.methods.get(id);
            if(!qualifiedMethods.contains(id)||!id.equals(new Prepare.MethodId("Option","monitor","()V"))
                ||!guard.equals(ClassDesc.of("psx.exporter.FmcCompatibilityWitness"))||method==null||(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)
                throw new IllegalArgumentException("unqualified FMC compatibility polling writer");
            var expected=Set.of(new FmcQualification.Member("Option","ngFmc","Z"),new FmcQualification.Member("Option","engineModel","I"));
            if(!entry.getValue().equals(expected))throw new IllegalArgumentException("FMC compatibility polling fields");
            var found=new HashSet<FmcQualification.Member>();
            for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC){
                var member=new FmcQualification.Member(f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(expected.contains(member))found.add(member);
            }
            if(!found.equals(expected))throw new IllegalArgumentException("FMC compatibility polling store coverage");
        }
        for(var method:nullGetters){
            if(!qualifiedMethods.contains(method)||!method.equals(new Prepare.MethodId("Core","navFold","(Ljava/lang/String;)Ljava/lang/String;"))
                ||!guard.equals(ClassDesc.of("psx.exporter.FmcCompatibilityWitness")))throw new IllegalArgumentException("unqualified FMC null getter");
            NavigationSelection.qualify(data.inputs.get(method.owner()));
            NavigationSelection.qualify(lower.getOrDefault(method.owner(),data.inputs.get(method.owner())));
        }
        var owners=new TreeSet<String>();
        for(var id:qualifiedMethods) {
            var method=data.methods.get(id);
            if(method==null||method.code().isEmpty()||id.name().startsWith("<"))
                throw new IllegalArgumentException("unqualified FMC writer body "+id);
            // Constructors/class initializers need separate initialized-this,
            // execution and helper-loading proof; do not silently wrap them.
            owners.add(id.owner());
        }
        var before=new HashMap<String,byte[]>(lower);var changed=new HashMap<String,byte[]>();
        for(String owner:owners) {
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);
            var model=data.cf.parse(input);
            byte[] result=data.cf.transformClass(model,(builder,element)->{
                if(!(element instanceof MethodModel method)){builder.with(element);return;}
                var id=new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());
                if(!qualifiedMethods.contains(id)){builder.with(element);return;}
                boolean nullGetter=nullGetters.contains(id);
                var watched=changedFields.getOrDefault(id,Set.of());
                builder.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                    Label begin,end,handler;int throwable,ngBefore,engineBefore;
                    List<ExceptionCatch> nativeHandlers;
                    public void atStart(CodeBuilder b){
                        nativeHandlers=new ArrayList<>();
                        if(watched.isEmpty())guarded(b,guard,"enter",nullGetter);
                        else{ngBefore=b.allocateLocal(TypeKind.INT);engineBefore=b.allocateLocal(TypeKind.INT);b.iconst_0().istore(ngBefore).iconst_0().istore(engineBefore);monitor(b,true);}
                        begin=b.newLabel();end=b.newLabel();handler=b.newLabel();
                        throwable=b.allocateLocal(TypeKind.REFERENCE);b.labelBinding(begin);
                    }
                    private void retire(CodeBuilder b){
                        if(watched.isEmpty()){guarded(b,guard,"leave",nullGetter);return;}
                        monitor(b,false);
                    }
                    private void monitor(CodeBuilder b,boolean enter){
                        guarded(b,guard,c->{
                            ClassDesc option=ClassDesc.of("Option");
                            if(enter){
                                c.getstatic(option,"ngFmc",CD_boolean).istore(ngBefore).getstatic(option,"engineModel",CD_int).istore(engineBefore)
                                    .invokestatic(guard,"monitorEnter",VOID);
                            }else{
                                c.iload(ngBefore).iload(engineBefore).getstatic(option,"ngFmc",CD_boolean).getstatic(option,"engineModel",CD_int)
                                    .invokestatic(guard,"monitorLeave",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_int));
                            }
                        });
                    }
                    public void accept(CodeBuilder b,CodeElement element){
                        if(element instanceof ExceptionCatch nativeHandler){nativeHandlers.add(nativeHandler);return;}
                        if(element instanceof ReturnInstruction result){
                            // An exception handler discards the operand stack.
                            // Preserve every native return value across optional
                            // helper/reporting failures, including two-slot values.
                            TypeKind kind=result.typeKind();int value=-1;
                            if(kind!=TypeKind.VOID){value=b.allocateLocal(kind);b.storeLocal(kind,value);}
                            retire(b);if(value>=0)b.loadLocal(kind,value);
                        }
                        b.with(element);
                    }
                    public void atEnd(CodeBuilder b){
                        // Optional helper handlers take precedence solely over
                        // injected spans. Preserve native-handler order before
                        // the outer witness retirement for uncaught exceptions.
                        for(var nativeHandler:nativeHandlers)b.with(nativeHandler);
                        b.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable);
                        retire(b);b.aload(throwable).athrow();
                    }
                }));
            });
            var errors=data.cf.verify(result);if(!errors.isEmpty())throw new IllegalArgumentException("FMC writer verify "+errors.getFirst());
            changed.put(owner,result);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        return Map.copyOf(changed);
    }
    private static void guarded(CodeBuilder b,ClassDesc guard,String method,boolean nullGetter) {
        guarded(b,guard,c->{if(nullGetter)c.aload(0);c.invokestatic(guard,method,nullGetter?MethodTypeDesc.of(CD_void,CD_String):VOID);});
    }
    private static void guarded(CodeBuilder b,ClassDesc guard,java.util.function.Consumer<CodeBuilder> action) {
        var start=b.newLabel();var end=b.newLabel();var handler=b.newLabel();var done=b.newLabel();
        b.labelBinding(start);action.accept(b);
        b.labelBinding(end).goto_(done)
            .exceptionCatchAll(start,end,handler).labelBinding(handler).pop();
        // Failure reporting has its own linkage guard. It cannot replace a
        // native return value or the original native exception object.
        var report=b.newLabel();var reportEnd=b.newLabel();var reportFail=b.newLabel();
        b.labelBinding(report).invokestatic(guard,"failed",VOID).labelBinding(reportEnd).goto_(done)
            .exceptionCatchAll(report,reportEnd,reportFail).labelBinding(reportFail).pop().labelBinding(done);
    }
}
