package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Operation-thread authority inside the ordinary EXEC call graph. Other
 * callers keep their native instructions and exception identity. Installed
 * classes are inspected as data; this layer never launches a simulator. */
final class FmcExecMutationPrepare {
    static final String CHECK="$psxExporter$fmcMutationCheckpoint",NATIVE_CHECK="$psxExporter$fmcNativeMutationCheckpoint",FAULT="$psxExporter$fmcMutationFailure";
    record Profile(Set<Prepare.MethodId> methods,int stores,int calls){}
    private FmcExecMutationPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC EXEC mutation "+why);}
    private static Prepare.MethodId resolve(Map<String,ClassModel> models,String owner,String name,String type,Set<String> seen){
        if(!seen.add(owner))return null;var model=models.get(owner);
        if(model==null&&(owner.startsWith("java/")||owner.startsWith("javax/"))){
            try(var stream=ClassLoader.getSystemResourceAsStream(owner+".class")){
                if(stream!=null)model=ClassFile.of().parse(stream.readAllBytes());
            }catch(java.io.IOException failure){throw new IllegalArgumentException("FMC EXEC JDK declaration unavailable",failure);}
        }
        if(model==null)return null;
        if(model.methods().stream().anyMatch(m->m.methodName().equalsString(name)&&m.methodType().equalsString(type)))return new Prepare.MethodId(owner,name,type);
        if(name.equals("<init>"))return null;
        if(model.superclass().isPresent()){var found=resolve(models,model.superclass().get().asInternalName(),name,type,seen);if(found!=null)return found;}
        for(var iface:model.interfaces()){var found=resolve(models,iface.asInternalName(),name,type,seen);if(found!=null)return found;}return null;
    }
    private static boolean subtype(Map<String,ClassModel> models,String owner,String parent,Set<String> seen){
        if(owner.equals(parent))return true;if(!seen.add(owner))return false;var m=models.get(owner);if(m==null)return false;
        if(m.superclass().isPresent()&&subtype(models,m.superclass().get().asInternalName(),parent,seen))return true;
        for(var iface:m.interfaces())if(subtype(models,iface.asInternalName(),parent,seen))return true;return false;
    }
    private static boolean store(Opcode op){return switch(op){case PUTFIELD,PUTSTATIC,IASTORE,LASTORE,FASTORE,DASTORE,AASTORE,BASTORE,CASTORE,SASTORE ->true;default ->false;};}
    static Profile profile(Prepare data){
        return profile(data,Map.of(),Set.of());
    }
    static Profile profile(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> additionalRoots){
        var models=FmcQualification.finalModels(data,lower);var methods=new HashMap<Prepare.MethodId,MethodModel>();
        models.forEach((owner,model)->model.methods().forEach(m->methods.put(new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue()),m)));
        var pending=new ArrayDeque<Prepare.MethodId>();for(String owner:List.of("FmcRte$ExecL","FmcRte$ExecR"))pending.add(new Prepare.MethodId(owner,"run","()V"));
        pending.addAll(additionalRoots);
        var reached=new HashSet<Prepare.MethodId>();int stores=0,calls=0;
        while(!pending.isEmpty()){
            var id=pending.removeFirst();if(!reached.add(id))continue;require(reached.size()<=4096,"call graph capacity");
            var method=methods.get(id);require(method!=null&&method.code().isPresent()&&(method.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"opaque vendor body "+id);
            for(var op:Prepare.instructions(method)){
                if(store(op.opcode()))stores++;
                require(!(op instanceof InvokeDynamicInstruction),"dynamic dispatch "+id);
                if(op instanceof NewObjectInstruction allocation){
                    String allocated=allocation.className().asInternalName();
                    if(models.containsKey(allocated)&&(subtype(models,allocated,"java/lang/Runnable",new HashSet<>())||subtype(models,allocated,"java/lang/Thread",new HashSet<>()))){
                        var run=resolve(models,allocated,"run","()V",new HashSet<>());
                        require(run!=null,"constructed native job body "+allocated);
                        if(models.containsKey(run.owner()))pending.add(run);
                    }
                }
                if(!(op instanceof InvokeInstruction call))continue;calls++;
                String owner=call.owner().asInternalName(),name=call.name().stringValue(),type=call.type().stringValue();
                if(!models.containsKey(owner))continue;
                var target=resolve(models,owner,name,type,new HashSet<>());require(target!=null,"unresolved vendor call "+owner+"."+name+type);
                if(name.equals(FmcBackgroundAdmissionOwnership.READ)||name.equals(FmcBackgroundAdmissionOwnership.WRITE)){
                    // These exact worker metadata bridges neither access nor
                    // mutate native content. Their proven tag shape/body must
                    // remain unchanged for binding; native callers still have
                    // the checkpoint immediately before the bridge call.
                    FmcBackgroundAdmissionOwnership.qualify(data,owner,models.get(owner));continue;
                }
                if(models.containsKey(target.owner())&&FmcBackgroundAdmissionHelpers.readOnly(data,target.owner(),methods.get(target),models))continue;
                if(models.containsKey(target.owner())&&FmcReconstructionExecRoots.metadataOnly(data,target.owner(),methods.get(target)))continue;
                if(models.containsKey(target.owner())){
                    var declared=methods.get(target);
                    if((declared.flags().flagsMask()&ClassFile.ACC_ABSTRACT)==0)pending.add(target);
                    else require(call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE,"abstract direct call "+target);
                }
                if(call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE)
                    for(var entry:models.entrySet())if(!entry.getKey().equals(owner)&&subtype(models,entry.getKey(),owner,new HashSet<>())){
                        var override=entry.getValue().methods().stream().filter(m->m.methodName().equalsString(name)&&m.methodType().equalsString(type)&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_ABSTRACT))==0).findFirst();
                        if(override.isPresent())pending.add(new Prepare.MethodId(entry.getKey(),name,type));
                    }
            }
        }
        return new Profile(Set.copyOf(reached),stores,calls);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        return transform(data,lower,Set.of());
    }
    /** The installed reconstruction layer supplies its exact generated mutator
     * roots after their separate effect proof; artifact names are never roots. */
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> additionalRoots){
        return transform(data,lower,additionalRoots,Set.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> additionalRoots,Set<Prepare.MethodId> transportContinuation){
        if(!transportContinuation.isEmpty())require(FmcBackgroundAdmissionTransport.inspect(data).continuationMethods().equals(transportContinuation),"exact independent native transport continuation proof");
        var whole=profile(data,lower,additionalRoots);var methods=new HashSet<>(whole.methods());methods.removeAll(transportContinuation);
        var profile=new Profile(Set.copyOf(methods),whole.stores(),whole.calls());var result=new HashMap<>(lower);var before=new HashMap<>(lower);var metadata=metadataCalls(data,lower);
        var owners=new TreeSet<String>();profile.methods().forEach(id->owners.add(id.owner()));
        for(String owner:owners){
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);
            var model=data.cf.parse(input);ClassDesc nativeOwner=ClassDesc.of(owner);
            require(model.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT))&&model.methods().stream().noneMatch(m->Set.of(CHECK,NATIVE_CHECK).contains(m.methodName().stringValue())),"checkpoint member collision "+owner);
            byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){
                if(!(e instanceof MethodModel method)||!profile.methods().contains(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue()))){b.with(e);return;}
                require(method.code().isPresent(),"missing final native body");
                String check=data.methods.containsKey(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue()))?NATIVE_CHECK:CHECK;
                b.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                    public void atStart(CodeBuilder c){checkpoint(c,nativeOwner,check);}
                    public void accept(CodeBuilder c,CodeElement element){
                        // No operands are consumed by this void check. In
                        // particular constructor receivers remain uninitialized
                        // until their original invokespecial executes.
                        if(element instanceof Instruction op&&(store(op.opcode())&&!(op instanceof FieldInstruction field&&field.name().stringValue().startsWith("$psxExporter$"))||op instanceof InvokeInstruction call&&!call.owner().asInternalName().startsWith("psx/exporter/")&&!metadata.contains(new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue()))))checkpoint(c,nativeOwner,check);
                        c.with(element);
                    }
                }));
            }
            public void atEnd(ClassBuilder b){
                b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
                // Catch linkage failure in a separate no-argument method so
                // native operands remain on the caller's stack. An authority
                // abort is an Error of another kind and continues unwinding.
                for(String name:List.of(CHECK,NATIVE_CHECK))b.withMethodBody(name,MethodTypeDesc.of(CD_void),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var start=c.newLabel();var end=c.newLabel();var failed=c.newLabel();
                    c.labelBinding(start).invokestatic(ClassDesc.of("psx.exporter.FmcExecSeam"),name.equals(CHECK)?"mutationCheckpoint":"nativeMutationCheckpoint",MethodTypeDesc.of(CD_void)).labelBinding(end).return_()
                        .exceptionCatch(start,end,failed,ClassDesc.of("java.lang.LinkageError")).labelBinding(failed);
                    if(name.equals(CHECK)){
                        // Generated publication has no native fallback: losing
                        // its authority helper must stop before the next write.
                        int original=c.allocateLocal(TypeKind.REFERENCE);c.astore(original).iconst_1().putstatic(nativeOwner,FAULT,CD_boolean).aload(original).athrow();
                    }else c.pop().iconst_1().putstatic(nativeOwner,FAULT,CD_boolean).return_();
                });
            }});
            require(data.cf.verify(output).isEmpty(),"final verifier "+owner);result.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,result,data.inputs);return Map.copyOf(result);
    }
    /** Exact preparation-owned metadata/retirement calls. These cannot be
     * blocked by lost mutation authority: their native callers still unwind
     * and must retire every independent exclusion/capture witness. */
    static Set<Prepare.MethodId> metadataCalls(Prepare data,Map<String,byte[]> lower){
        var finalModels=FmcQualification.finalModels(data,lower);var metadata=new HashSet<Prepare.MethodId>();
        for(var entry:finalModels.entrySet())for(var method:entry.getValue().methods()){
            String name=method.methodName().stringValue();if(!name.startsWith("$psxExporter$"))continue;var id=new Prepare.MethodId(entry.getKey(),name,method.methodType().stringValue());
            if(Set.of(FmcBackgroundAdmissionOwnership.READ,FmcBackgroundAdmissionOwnership.WRITE).contains(name)){FmcBackgroundAdmissionOwnership.qualify(data,entry.getKey(),entry.getValue());metadata.add(id);}
            else if(FmcBackgroundAdmissionHelpers.readOnly(data,entry.getKey(),method,finalModels)||FmcReconstructionExecRoots.metadataOnly(data,entry.getKey(),method))metadata.add(id);
        }
        for(String signature:List.of("psx/exporter/FmcExclusion.enter()I","psx/exporter/FmcExclusion.leave(I)V","psx/exporter/FmcAdmissionReceivers.enter(Ljava/lang/Object;)I","psx/exporter/FmcCoherence.enter()V","psx/exporter/FmcCoherence.leave()V","psx/exporter/FmcCoherence.failed()V","psx/exporter/FmcCompatibilityWitness.enter()V","psx/exporter/FmcCompatibilityWitness.leave()V","psx/exporter/FmcCompatibilityWitness.enter(Ljava/lang/String;)V","psx/exporter/FmcCompatibilityWitness.leave(Ljava/lang/String;)V","psx/exporter/FmcCompatibilityWitness.monitorEnter()V","psx/exporter/FmcCompatibilityWitness.monitorLeave(IIII)V","psx/exporter/FmcCompatibilityWitness.failed()V","psx/exporter/FmcJobs.begin()J","psx/exporter/FmcJobs.release(J)V")){
            int split=signature.indexOf('.'),descriptor=signature.indexOf('(');metadata.add(new Prepare.MethodId(signature.substring(0,split),signature.substring(split+1,descriptor),signature.substring(descriptor)));
        }
        return Set.copyOf(metadata);
    }
    private static void checkpoint(CodeBuilder c,ClassDesc owner,String name){c.invokestatic(owner,name,MethodTypeDesc.of(CD_void));}
}
