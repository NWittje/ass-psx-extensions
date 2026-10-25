package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Qualifies the complete lifetime of explicitly audited asynchronous FMC jobs.
 * A fixed reservation precedes the unchanged native start instruction. Native
 * start failures retain their exception and retire only that reservation. This
 * is capture coherence evidence, not recall exclusion or job scheduling. */
final class FmcJobPrepare {
    static final String RECALL_FAULT="$psxExporter$fmcAdmissionFailure";
    record Start(Prepare.MethodId caller,int count,Set<Integer> ordinals){
        Start(Prepare.MethodId caller,int count){this(caller,count,Set.of());}
        Start{
            Objects.requireNonNull(caller);ordinals=Set.copyOf(ordinals);
            if(count<=0||ordinals.stream().anyMatch(i->i<0||i>=count))throw new IllegalArgumentException("FMC native start ordinals");
        }
        boolean selected(int ordinal){return ordinals.isEmpty()||ordinals.contains(ordinal);}
    }
    private static final ClassDesc JOBS=ClassDesc.of("psx.exporter.FmcJobs"),GUARD=ClassDesc.of("psx.exporter.FmcCoherence");
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void),RELEASE=MethodTypeDesc.of(CD_void,CD_long);
    private FmcJobPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs){
        return transform(data,lower,starts,runs,false);
    }
    /** Retaining admission is a separate optional recall transformation. The
     * caller must first complete its reader/writer/frontier/order/error proof;
     * the current source layer deliberately uses the unchanged witness path. */
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs,boolean retaining){
        return transform(data,lower,starts,runs,retaining,false);
    }
    /** User-selected quiet-input contract. Original jobs consume their native
     * inputs on their original Thread. No input-transfer call is transformed.
     * A violated quiet interval invalidates recall, preserving native execution.
     * Whole frame/read/write/alias qualification remains a separate admission
     * requirement; this job component cannot make recall64 available. */
    static Map<String,byte[]> quiet(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs){
        return transform(data,lower,starts,runs,false,true);
    }
    /** Complete assembly supplies the independently qualified metadata-only
     * dispatcher. Component fixtures and capture never acquire this path. */
    static Map<String,byte[]> quietWithScope(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs){
        var bridge=new Prepare.MethodId("FmcRte","$psxExporter$fmcInheritExecJobScope","(Ljava/lang/Thread;J)V");
        body(data,lower,bridge);var method=data.cf.parse(lower.get("FmcRte")).methods().stream().filter(m->id("FmcRte",m).equals(bridge)).findFirst().orElseThrow();
        require(method.flags().flagsMask()==(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC),"qualified operation scope dispatcher flags");
        require(FmcReconstructionExecRoots.metadataOnly(data,"FmcRte",method),"exact source-qualified operation metadata dispatcher body");
        return transform(data,lower,starts,runs,false,true,bridge);
    }
    private static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs,boolean retaining,boolean quiet){
        return transform(data,lower,starts,runs,retaining,quiet,null);
    }
    private static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Start> starts,Set<Prepare.MethodId> runs,boolean retaining,boolean quiet,Prepare.MethodId scopeBridge){
        boolean admission=retaining||quiet;
        if(retaining){
            var liveInputs=FmcRecallBoundary.reachableInputs(data,runs);
            require(liveInputs.isEmpty(),"retained native input recipe missing ("+liveInputs.size()+" sites): "+liveInputs.stream().limit(12).toList());
        }
        var callers=new HashMap<Prepare.MethodId,Start>();var owners=new TreeSet<String>();
        for(Start start:starts){require(callers.put(start.caller,start)==null,"duplicate start caller");owners.add(start.caller.owner());}
        // A finite native run may launch its child before returning. The same
        // method needs both the complete parent lifetime and its start witness.
        for(var run:runs){require(run.name().equals("run")&&run.descriptor().equals("()V"),"job entry");owners.add(run.owner());}
        for(var id:callers.keySet())body(data,lower,id);for(var id:runs)body(data,lower,id);
        var before=new HashMap<>(lower);var changed=new HashMap<String,byte[]>();
        for(String owner:owners){
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);
            var source=data.cf.parse(input);ClassDesc nativeOwner=ClassDesc.of(owner);
            if(admission)require(source.fields().stream().noneMatch(f->f.fieldName().equalsString(RECALL_FAULT)),"recall fault-field collision");
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                if(!(element instanceof MethodModel nativeMethod)||(!callers.containsKey(id(owner,nativeMethod))&&!runs.contains(id(owner,nativeMethod)))){builder.with(element);return;}
                var method=id(owner,nativeMethod);
                builder.transformMethod(nativeMethod,MethodTransform.transformingCode(new CodeTransform(){
                    List<ExceptionCatch> original;int count,token,throwable;Label begin,end,handler;
                    public void atStart(CodeBuilder b){
                        original=new ArrayList<>();count=0;
                        if(runs.contains(method)){
                            token=b.allocateLocal(TypeKind.LONG);b.lconst_0().lstore(token);
                            guarded(b,()->b.invokestatic(JOBS,"begin",MethodTypeDesc.of(CD_long)).lstore(token));
                            begin=b.newLabel();end=b.newLabel();handler=b.newLabel();throwable=b.allocateLocal(TypeKind.REFERENCE);b.labelBinding(begin);
                        }
                    }
                    public void accept(CodeBuilder b,CodeElement element){
                        if(element instanceof ExceptionCatch caught){original.add(caught);return;}
                        if(callers.containsKey(method)&&element instanceof InvokeInstruction call&&Prepare.call(call,"java/lang/Thread","start","()V")){
                            int ordinal=count++;
                            if(!callers.get(method).selected(ordinal)){b.with(element);return;}
                            require(call.opcode()==Opcode.INVOKEVIRTUAL,"native start opcode");int receiver=b.allocateLocal(TypeKind.REFERENCE),reservation=b.allocateLocal(TypeKind.LONG),failure=b.allocateLocal(TypeKind.REFERENCE);
                            b.astore(receiver).loadConstant(admission?-1L:0L).lstore(reservation);
                            if(admission){prepareRecallStart(b,nativeOwner,receiver,reservation,quiet);guardedRecall(b,nativeOwner,()->b.aload(receiver).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),"jobStarting",MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"))));}
                            else guarded(b,()->b.aload(receiver).invokestatic(JOBS,"reserve",MethodTypeDesc.of(CD_long,ClassDesc.of("java.lang.Thread"))).lstore(reservation));
                            if(scopeBridge!=null){var unreserved=b.newLabel();b.lload(reservation).lconst_0().lcmp().ifle(unreserved);
                                guardedRecall(b,nativeOwner,()->b.aload(receiver).invokestatic(ClassDesc.of("psx.exporter.FmcBackgroundAdmission"),"operationScope",MethodTypeDesc.of(CD_long))
                                    .invokestatic(ClassDesc.of(scopeBridge.owner()),scopeBridge.name(),MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"),CD_long)));
                                b.labelBinding(unreserved);
                            }
                            var start=b.newLabel();var finish=b.newLabel();var failed=b.newLabel();var done=b.newLabel();
                            if(retaining)b.lload(reservation).lconst_0().lcmp().ifeq(done);
                            if(admission)guardedRecall(b,nativeOwner,()->b.aload(receiver).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),"handoffLauncher",MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"))));
                            b.labelBinding(start).aload(receiver).with(call).labelBinding(finish).goto_(done)
                                .exceptionCatchAll(start,finish,failed).labelBinding(failed).astore(failure);
                            release(b,reservation);
                            if(admission)guardedRecall(b,nativeOwner,()->b.aload(receiver).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),"jobStartFailed",MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"))));
                            if(admission)guardedRecall(b,nativeOwner,()->b.aload(receiver).aload(failure).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),"startFailed",MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"),CD_Throwable)));
                            b.aload(failure).athrow().labelBinding(done);return;
                        }
                        if(runs.contains(method)&&element instanceof ReturnInstruction){release(b,token);}
                        b.with(element);
                    }
                    public void atEnd(CodeBuilder b){
                        if(callers.containsKey(method))require(count==callers.get(method).count,"changed start count "+method);
                        // Injected handlers precede native handlers only on their
                        // own spans; the native handlers retain their ordering.
                        for(var caught:original)b.with(caught);
                        if(runs.contains(method)){
                            b.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable);
                            release(b,token);b.aload(throwable).athrow();
                        }
                    }
                }));
                }
                public void atEnd(ClassBuilder builder){if(admission)builder.withField(RECALL_FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);}
            });
            var errors=data.cf.verify(output);require(errors.isEmpty(),"bytecode verification "+errors);changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static Prepare.MethodId id(String owner,MethodModel method){return new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());}
    private static void body(Prepare data,Map<String,byte[]> lower,Prepare.MethodId id){
        byte[] input=lower.getOrDefault(id.owner(),data.inputs.get(id.owner()));require(input!=null,"missing qualified owner "+id);
        var method=data.cf.parse(input).methods().stream().filter(m->id(id.owner(),m).equals(id)).findFirst().orElse(null);
        require(method!=null&&method.code().isPresent()&&!id.name().startsWith("<"),"missing qualified body "+id);
    }
    private static void require(boolean value,String reason){if(!value)throw new IllegalArgumentException("FMC job "+reason);}
    private static void recallFailure(CodeBuilder b,ClassDesc nativeOwner){
        // The same-owner primitive survives failure of both optional helpers.
        // Runtime recall binding must include these fields; source binding does
        // not depend on optional admission diagnostics.
        b.iconst_1().putstatic(nativeOwner,RECALL_FAULT,CD_boolean);
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start).invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"failed",VOID).labelBinding(end).goto_(done)
            .exceptionCatchAll(start,end,failure).labelBinding(failure).pop().labelBinding(done);
    }
    private static void guardedRecall(CodeBuilder b,ClassDesc nativeOwner,Runnable work){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start);work.run();b.labelBinding(end).goto_(done).exceptionCatchAll(start,end,failure)
            .labelBinding(failure).pop();recallFailure(b,nativeOwner);b.labelBinding(done);
    }
    private static void prepareRecallStart(CodeBuilder b,ClassDesc nativeOwner,int receiver,int reservation,boolean quiet){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start).aload(receiver).invokestatic(ClassDesc.of("psx.exporter.FmcJobAdmission"),quiet?"prepareQuietStart":"prepareStart",MethodTypeDesc.of(CD_long,ClassDesc.of("java.lang.Thread")))
            .lstore(reservation).labelBinding(end).goto_(done).exceptionCatchAll(start,end,failure).labelBinding(failure).pop();
        recallFailure(b,nativeOwner);
        // An optional linkage failure must not remove the lower source's
        // original job witness. A genuine witness failure still disables source
        // coherence, but it must never skip the original native Thread.start.
        guarded(b,()->b.aload(receiver).invokestatic(JOBS,"reserve",MethodTypeDesc.of(CD_long,ClassDesc.of("java.lang.Thread"))).lstore(reservation));
        var positive=b.newLabel();b.lload(reservation).lconst_0().lcmp().ifgt(positive).loadConstant(-1L).lstore(reservation).labelBinding(positive).labelBinding(done);
    }
    private static void release(CodeBuilder b,int token){guarded(b,()->b.lload(token).invokestatic(JOBS,"release",RELEASE));}
    private static void guarded(CodeBuilder b,Runnable work){
        var start=b.newLabel();var end=b.newLabel();var failed=b.newLabel();var done=b.newLabel();
        b.labelBinding(start);work.run();b.labelBinding(end).goto_(done).exceptionCatchAll(start,end,failed).labelBinding(failed).pop();
        var report=b.newLabel();var reportEnd=b.newLabel();var reportFailed=b.newLabel();
        b.labelBinding(report).invokestatic(GUARD,"failed",VOID).labelBinding(reportEnd).goto_(done)
            .exceptionCatchAll(report,reportEnd,reportFailed).labelBinding(reportFailed).pop().labelBinding(done);
    }
}
