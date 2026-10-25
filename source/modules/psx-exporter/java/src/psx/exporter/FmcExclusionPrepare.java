package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Admission at an explicitly qualified void callback frontier. Deferral must
 * precede native input consumption; arbitrary discovered readers/writers and
 * continuous frame/network loops are not eligible. Original exceptions retain
 * their native handlers and identity. Helper failures disable optional recall. */
final class FmcExclusionPrepare {
    static final String FAULT="$psxExporter$fmcBoundaryFailure";
    record Receiver(int slot,String descriptor,String tagOwner){Receiver(int slot,String descriptor){this(slot,descriptor,null);}}
    private static final ClassDesc GATE=ClassDesc.of("psx.exporter.FmcExclusion");
    private FmcExclusionPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> qualified){
        return transform(data,lower,qualified,Map.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> qualified,
                                        Map<Prepare.MethodId,Receiver> receivers){
        return transform(data,lower,qualified,receivers,Set.of(),Map.of());
    }
    /** An exact producer may retain its physical prefix while its FMC tail
     * defers. Admission begins at entry, so work which passed a native Pause
     * test before closure remains in the drain until its original retirement. */
    static Map<String,byte[]> deferredPrefix(Prepare data,Map<String,byte[]> lower,Prepare.MethodId method,Receiver receiver,int ordinal){
        if(ordinal<=0||ordinal>=Prepare.instructions(data.methods.get(method)).size())throw new IllegalArgumentException("FMC producer tail boundary");
        return transform(data,lower,Set.of(method),Map.of(method,receiver),Set.of(),Map.of(method,ordinal));
    }
    /** The sole extracted terrain route region has an independent original
     * instruction/branch proof. It is not reintroduced as an original vendor
     * input to Prepare, whose instrumentation-preimage rejection stays intact. */
    static Map<String,byte[]> terrainRegion(Prepare data,Map<String,byte[]> lower){
        var id=FmcTerrainRoutePrepare.REGION;
        return transform(data,lower,Set.of(id),Map.of(id,new Receiver(2,"LRoutes$RteBody;")),Set.of(id),Map.of());
    }
    private static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> qualified,
                                        Map<Prepare.MethodId,Receiver> receivers,Set<Prepare.MethodId> generated,Map<Prepare.MethodId,Integer> delayed){
        var models=FmcQualification.finalModels(data,lower);var methods=new HashMap<Prepare.MethodId,MethodModel>();
        var owners=new TreeSet<String>();
        for(var id:qualified){
            var owner=models.get(id.owner());var matches=owner==null?List.<MethodModel>of():owner.methods().stream()
                .filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).toList();
            var method=matches.size()==1?matches.getFirst():null;
            if(method==null||method.code().isEmpty()||id.name().startsWith("<")||!id.descriptor().endsWith(")V"))
                throw new IllegalArgumentException("FMC deferrable void frontier missing "+id);
            if(generated.contains(id)){
                if(!id.equals(FmcTerrainRoutePrepare.REGION)||data.methods.containsKey(id)
                    ||(method.flags().flagsMask()&(ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC|ClassFile.ACC_STATIC))!=(ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC))
                    throw new IllegalArgumentException("FMC unqualified generated region");
            }else if(!data.methods.containsKey(id))throw new IllegalArgumentException("FMC original frontier missing");
            methods.put(id,method);
            owners.add(id.owner());
        }
        for(var entry:receivers.entrySet()){
            var id=entry.getKey();var receiver=entry.getValue();
            if(!qualified.contains(id))throw new IllegalArgumentException("FMC receiver frontier not selected");
            var method=methods.get(id);var parameters=new HashMap<Integer,String>();int slot=0;
            if((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0){parameters.put(0,"L"+id.owner()+";");slot=1;}
            for(var parameter:method.methodTypeSymbol().parameterList()){
                parameters.put(slot,parameter.descriptorString());slot+=parameter.equals(CD_long)||parameter.equals(CD_double)?2:1;
            }
            if(!Objects.equals(parameters.get(receiver.slot),receiver.descriptor)||!receiver.descriptor.startsWith("L"))
                throw new IllegalArgumentException("FMC receiver is not the exact reference parameter "+id);
            if(receiver.tagOwner!=null){
                if(receiver.slot!=0||(method.flags().flagsMask()&ClassFile.ACC_STATIC)!=0||!receiver.descriptor.equals("L"+id.owner()+";")||!ancestor(models,id.owner(),receiver.tagOwner,new HashSet<>()))throw new IllegalArgumentException("FMC tagged poll is not exact this ancestry "+id);
                var tagModel=models.get(receiver.tagOwner);if(tagModel==null)throw new IllegalArgumentException("FMC tagged poll owner missing "+receiver.tagOwner);FmcBackgroundAdmissionOwnership.qualify(data,receiver.tagOwner,tagModel);
            }
        }
        var before=new HashMap<String,byte[]>(lower);var changed=new HashMap<String,byte[]>();
        for(String owner:owners){
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);
            ClassDesc nativeOwner=ClassDesc.of(owner);var model=data.cf.parse(input);
            if(model.fields().stream().anyMatch(f->f.fieldName().equalsString(FAULT)))throw new IllegalArgumentException("FMC boundary fault-field collision");
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                    if(!(element instanceof MethodModel method)){builder.with(element);return;}
                    var id=new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());
                    if(!qualified.contains(id)){builder.with(element);return;}
                    if(method.code().isEmpty()||!generated.contains(id)&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)!=(data.methods.get(id).flags().flagsMask()&ClassFile.ACC_STATIC))
                        throw new IllegalArgumentException("FMC final frontier descriptor/staticness drift "+id);
                    Receiver receiver=receivers.get(id);
                    builder.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                    Label begin,end,handler;int ticket,throwable,ordinal;List<ExceptionCatch> nativeHandlers;
                    public void atStart(CodeBuilder b){
                        nativeHandlers=new ArrayList<>();ticket=b.allocateLocal(TypeKind.INT);throwable=b.allocateLocal(TypeKind.REFERENCE);
                        b.iconst_m1().istore(ticket);guardedEntry(b,ticket,nativeOwner,receiver);
                        if(!delayed.containsKey(id)){var admitted=b.newLabel();b.iload(ticket).ifne(admitted).return_().labelBinding(admitted);}
                        begin=b.newLabel();end=b.newLabel();handler=b.newLabel();b.labelBinding(begin);
                    }
                    public void accept(CodeBuilder b,CodeElement element){
                        if(element instanceof ExceptionCatch nativeHandler){nativeHandlers.add(nativeHandler);return;}
                        if(element instanceof Instruction&&ordinal++==delayed.getOrDefault(id,-1)){var admitted=b.newLabel();b.iload(ticket).ifne(admitted).return_().labelBinding(admitted);}
                        if(element instanceof ReturnInstruction)guardedLeave(b,ticket,nativeOwner);
                        b.with(element);
                    }
                    public void atEnd(CodeBuilder b){
                        for(var nativeHandler:nativeHandlers)b.with(nativeHandler);
                        b.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable);
                        guardedLeave(b,ticket,nativeOwner);b.aload(throwable).athrow();
                    }
                    }));
                }
                public void atEnd(ClassBuilder builder){builder.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);}
            });
            var errors=data.cf.verify(output);if(!errors.isEmpty())throw new IllegalArgumentException("FMC exclusion verification "+errors.getFirst());changed.put(owner,output);
        }
        var legalFinal=new HashMap<>(lower);legalFinal.putAll(changed);TerrainAccess.verifyAdded(before,legalFinal,data.inputs);return Map.copyOf(changed);
    }
    private static boolean ancestor(Map<String,ClassModel> models,String owner,String target,Set<String> seen){if(owner.equals(target))return true;if(!seen.add(owner))return false;var model=models.get(owner);return model!=null&&model.superclass().isPresent()&&ancestor(models,model.superclass().get().asInternalName(),target,seen);}
    private static void guardedEntry(CodeBuilder b,int ticket,ClassDesc nativeOwner,Receiver receiver){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start);
        if(receiver==null)b.invokestatic(GATE,"enter",MethodTypeDesc.of(CD_int));
        else if(receiver.tagOwner!=null)b.aload(receiver.slot).invokestatic(ClassDesc.of(receiver.tagOwner),FmcBackgroundAdmissionOwnership.READ,MethodTypeDesc.of(CD_long,CD_Object)).invokestatic(ClassDesc.of("psx.exporter.FmcBackgroundAdmission"),"pollTag",MethodTypeDesc.of(CD_int,CD_long));
        else b.aload(receiver.slot).invokestatic(ClassDesc.of("psx.exporter.FmcAdmissionReceivers"),"enter",MethodTypeDesc.of(CD_int,CD_Object));
        b.istore(ticket).labelBinding(end).goto_(done)
            .exceptionCatchAll(start,end,failure).labelBinding(failure).pop();report(b,nativeOwner);b.labelBinding(done);
    }
    private static void guardedLeave(CodeBuilder b,int ticket,ClassDesc nativeOwner){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.iload(ticket).ifle(done).labelBinding(start).iload(ticket).invokestatic(GATE,"leave",MethodTypeDesc.of(CD_void,CD_int)).labelBinding(end).goto_(done)
            .exceptionCatchAll(start,end,failure).labelBinding(failure).pop();report(b,nativeOwner);b.labelBinding(done);
    }
    private static void report(CodeBuilder b,ClassDesc nativeOwner){
        b.iconst_1().putstatic(nativeOwner,FAULT,CD_boolean);
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start).invokestatic(GATE,"failed",MethodTypeDesc.of(CD_void)).labelBinding(end).goto_(done)
            .exceptionCatchAll(start,end,failure).labelBinding(failure).pop().labelBinding(done);
    }
}
