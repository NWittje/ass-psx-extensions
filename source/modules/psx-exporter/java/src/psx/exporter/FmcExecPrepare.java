package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;
import static java.lang.constant.ConstantDescs.*;

/** Data-only qualification of PSX's ordinary EXEC launchers. Their native
 * source-selection branch, original Thread/Runnable construction and priority
 * remain intact. The returned actual Thread is completion evidence, not proof
 * that content or guidance is ready. Only the qualified command authority may
 * launch it, after NEW content/selection banks have been loaded and validated.
 * No installed class is defined or initialized during preparation. */
final class FmcExecPrepare {
    static final String LEFT="$psxExporter$fmcRecallExecL",RIGHT="$psxExporter$fmcRecallExecR";
    static final String TOKEN="$psxExporter$fmcRecallOperation",FAULT="$psxExporter$fmcRecallHookFailure";
    private FmcExecPrepare(){}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException("FMC native EXEC "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower)throws Exception{
        var original=data.models.get("FmcRte");require(original!=null,"owner");
        var input=lower.getOrDefault("FmcRte",data.inputs.get("FmcRte"));var model=data.cf.parse(input);
        var nativeMethods=new ArrayList<MethodModel>();
        for(String name:List.of("execL","execR")){
            var method=original.methods().stream().filter(m->m.methodName().equalsString(name)&&m.methodType().equalsString("()V")).findFirst().orElseThrow();
            var ops=Prepare.instructions(method);
            require(method.code().isPresent()&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)!=0&&ops.size()==(name.equals("execL")?20:19),"ordinary launcher shape "+name);
            require(ops.getLast().opcode()==Opcode.RETURN,"ordinary return");
            int starts=0,priority=0,constructors=0;
            for(var op:ops)if(op instanceof InvokeInstruction call){
                if(Prepare.call(call,"java/lang/Thread","start","()V")){require(call.opcode()==Opcode.INVOKEVIRTUAL,"start opcode");starts++;}
                else if(Prepare.call(call,"java/lang/Thread","setPriority","(I)V"))priority++;
                else if(call.name().equalsString("<init>")&&Set.of("java/lang/Thread","FmcRte$ExecL","FmcRte$ExecR").contains(call.owner().asInternalName()))constructors++;
                else throw new IllegalArgumentException("FMC EXEC unexpected native call "+call);
            }
            require(starts==1&&priority==1&&constructors==3&&method.code().orElseThrow().exceptionHandlers().isEmpty(),"complete constructor/start boundary");
            // Full original method bytes, including branch targets and nav-side
            // selection, must survive lower-layer composition unchanged.
            var current=model.methods().stream().filter(m->m.methodName().equalsString(name)&&m.methodType().equalsString("()V")).findFirst().orElseThrow();
            require(methodBytes(data,method).equals(methodBytes(data,current)),"lower launcher drift");nativeMethods.add(method);
        }
        require(model.methods().stream().noneMatch(m->Set.of(LEFT,RIGHT).contains(m.methodName().stringValue())),"bridge collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){for(var nativeMethod:nativeMethods){
                String bridge=nativeMethod.methodName().equalsString("execL")?LEFT:RIGHT;
                b.withMethodBody(bridge,MethodTypeDesc.of(ClassDesc.of("java.lang.Thread"),CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.lload(0).lstore(2);
                    var valid=c.newLabel();c.lload(2).invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"commandActive",MethodTypeDesc.of(CD_boolean,CD_long)).ifne(valid);
                    c.new_(ClassDesc.of("java.lang.IllegalStateException")).dup().loadConstant("FMC EXEC mutation authority")
                        .invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow().labelBinding(valid);
                    for(var element:nativeMethod.code().orElseThrow()){
                        if(element instanceof ReturnInstruction)c.aload(0).areturn();
                        else{
                            if(element instanceof InvokeInstruction start&&Prepare.call(start,"java/lang/Thread","start","()V"))
                                c.dup().lload(2).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),"launching",MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"),CD_long));
                            c.with(element);
                            if(element instanceof InvokeInstruction call&&call.name().equalsString("<init>")&&Set.of("FmcRte$ExecL","FmcRte$ExecR").contains(call.owner().asInternalName()))
                                c.dup().lload(2).putfield(call.owner().asSymbol(),TOKEN,CD_long);
                        }
                    }
                });
            }}
        });
        require(data.cf.verify(output).isEmpty(),"bridge verification");var before=new HashMap<>(lower);before.put("FmcRte",input);
        var result=new HashMap<>(lower);result.put("FmcRte",output);
        var metadata=FmcExecMutationPrepare.metadataCalls(data,lower);
        for(String owner:List.of("FmcRte$ExecL","FmcRte$ExecR")){
            byte[] nativeInput=lower.getOrDefault(owner,data.inputs.get(owner));require(nativeInput!=null,"native job owner");before.put(owner,nativeInput);
            result.put(owner,job(data,owner,nativeInput,metadata));
        }
        TerrainAccess.verifyAdded(before,result,data.inputs);return Map.copyOf(result);
    }
    private static byte[] job(Prepare data,String owner,byte[] input,Set<Prepare.MethodId> metadata){
        ClassDesc nativeClass=ClassDesc.of(owner),seam=ClassDesc.of("psx.exporter.FmcExecSeam");var model=data.cf.parse(input);
        require(model.fields().stream().noneMatch(f->Set.of(TOKEN,FAULT).contains(f.fieldName().stringValue())),"job field collision");
        require(model.methods().stream().filter(m->m.methodName().equalsString("run")&&m.methodType().equalsString("()V")&&m.code().isPresent()).count()==1,"native job body");
        byte[] result=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder builder,ClassElement element){
                if(!(element instanceof MethodModel method)||!method.methodName().equalsString("run")||!method.methodType().equalsString("()V")){builder.with(element);return;}
                builder.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                    List<ExceptionCatch> handlers;Label begin,end,handler;int original;
                    public void atStart(CodeBuilder c){
                        handlers=new ArrayList<>();original=c.allocateLocal(TypeKind.REFERENCE);
                        var manual=c.newLabel();var started=c.newLabel();var accepted=c.newLabel();var failed=c.newLabel();
                        c.aload(0).getfield(nativeClass,TOKEN,CD_long).lconst_0().lcmp().ifle(manual);
                        c.labelBinding(started).aload(0).getfield(nativeClass,TOKEN,CD_long).loadConstant(owner.equals("FmcRte$ExecL")?0:1).invokestatic(seam,"beforeNative",MethodTypeDesc.of(CD_void,CD_long,CD_int))
                            .labelBinding(accepted).goto_(manual).exceptionCatchAll(started,accepted,failed).labelBinding(failed).astore(original);
                        report(c,original);c.return_().labelBinding(manual);
                        begin=c.newLabel();end=c.newLabel();handler=c.newLabel();c.labelBinding(begin);
                    }
                    public void accept(CodeBuilder c,CodeElement element){
                        if(element instanceof ExceptionCatch caught){handlers.add(caught);return;}
                        if(element instanceof InvokeInstruction call&&!call.name().equalsString("<init>")&&!metadata.contains(new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue()))){
                            // Keep the native call/arguments exactly as emitted.
                            // Only an operation-owned job checks authority; a
                            // manual job's token0 branch never calls the helper.
                            var manual=c.newLabel();var start=c.newLabel();var end=c.newLabel();var failed=c.newLabel();
                            c.aload(0).getfield(nativeClass,TOKEN,CD_long).lconst_0().lcmp().ifle(manual);
                            c.labelBinding(start).aload(0).getfield(nativeClass,TOKEN,CD_long)
                                .invokestatic(seam,"requireContinuation",MethodTypeDesc.of(CD_void,CD_long)).labelBinding(end).goto_(manual);
                            // Unwind through the lower native handlers rather
                            // than inserting a RETURN that bypasses their
                            // lifetime/capture finally paths.
                            c.exceptionCatchAll(start,end,failed).labelBinding(failed).astore(original);report(c,original);c.aload(original).athrow().labelBinding(manual);
                        }
                        if(element instanceof ReturnInstruction){
                            var manual=c.newLabel();c.aload(0).getfield(nativeClass,TOKEN,CD_long).lconst_0().lcmp().ifle(manual);
                            var start=c.newLabel();var finish=c.newLabel();var failed=c.newLabel();var done=c.newLabel();
                            c.labelBinding(start).aload(0).getfield(nativeClass,TOKEN,CD_long).invokestatic(seam,"returned",MethodTypeDesc.of(CD_void,CD_long))
                                .labelBinding(finish).goto_(done).exceptionCatchAll(start,finish,failed).labelBinding(failed).astore(original);
                            report(c,original);c.labelBinding(done).labelBinding(manual);
                        }
                        c.with(element);
                    }
                    public void atEnd(CodeBuilder c){
                        for(var nativeHandler:handlers)c.with(nativeHandler);
                        c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(original);
                        var manual=c.newLabel();c.aload(0).getfield(nativeClass,TOKEN,CD_long).lconst_0().lcmp().ifle(manual);
                        report(c,original);c.labelBinding(manual).aload(original).athrow();
                    }
                    void report(CodeBuilder c,int failure){
                        c.iconst_1().putstatic(nativeClass,FAULT,CD_boolean);
                        var start=c.newLabel();var finish=c.newLabel();var failed=c.newLabel();var done=c.newLabel();
                        c.labelBinding(start).aload(0).getfield(nativeClass,TOKEN,CD_long).aload(failure).invokestatic(seam,"failed",MethodTypeDesc.of(CD_void,CD_long,CD_Throwable))
                            .labelBinding(finish).goto_(done).exceptionCatchAll(start,finish,failed).labelBinding(failed).pop().labelBinding(done);
                    }
                }));
            }
            public void atEnd(ClassBuilder b){
                b.withField(TOKEN,CD_long,ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC);
                b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
            }
        });require(data.cf.verify(result).isEmpty(),"native seam verification");return result;
    }
    private static String methodBytes(Prepare data,MethodModel method)throws Exception{
        return Identity.hash(data.cf.build(ClassDesc.of("FmcRte"),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e)))));
    }
}
