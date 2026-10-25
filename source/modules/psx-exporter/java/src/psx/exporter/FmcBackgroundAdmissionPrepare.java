package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Non-deferring witnesses at an exact native access roster. These preserve
 * every native method result and exception, including nonvoid writers. They
 * are supplementary violation detection, never a substitute for the narrow
 * recurrent poll/render cuts required for a usable closed recall interval. */
final class FmcBackgroundAdmissionPrepare {
    static final String FAULT="$psxExporter$fmcUnexpectedAccess";
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBackgroundAdmission");
    private FmcBackgroundAdmissionPrepare(){}
    record Profile(Set<Prepare.MethodId> accesses,Set<Prepare.MethodId> initializers,
                   Set<Prepare.MethodId> nonvoidWriters,int bufferSites,int arraySites){}
    static Profile inspect(Prepare data,Set<FmcQualification.Member> selected){
        var evidence=FmcRecallBoundary.inspect(data,selected);
        var accesses=new HashSet<Prepare.MethodId>(evidence.readers());accesses.addAll(evidence.writers());
        var buffers=FmcBufferFlowAudit.audit(data,Map.of());
        for(var site:buffers.mutations())if(!FmcBufferFlowAudit.paddingHelper(site.method()))accesses.add(site.method());
        var arrays=FmcRouteArrayFlowAudit.audit(data,Map.of());
        for(var site:arrays.mutations())accesses.add(site.method());
        var constructors=new HashSet<Prepare.MethodId>();
        for(var id:List.copyOf(accesses))if(id.name().startsWith("<")){constructors.add(id);accesses.remove(id);}
        return new Profile(Set.copyOf(accesses),Set.copyOf(constructors),evidence.nonvoidWriters(),buffers.mutations().size(),arrays.mutations().size());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> accesses,
                                       Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers){
        return transform(data,lower,accesses,receivers,Set.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> accesses,
                                       Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers,Set<Prepare.MethodId> nativePaused){
        var grouped=new TreeMap<String,Set<Prepare.MethodId>>();
        for(var id:accesses){
            var original=data.methods.get(id);
            require(original!=null&&original.code().isPresent()&&!id.name().startsWith("<"),"original access body "+id);
            grouped.computeIfAbsent(id.owner(),k->new HashSet<>()).add(id);
        }
        require(accesses.containsAll(receivers.keySet()),"unselected receiver boundary");
        require(accesses.containsAll(nativePaused),"unselected native Pause boundary");
        for(var id:nativePaused)require(id.descriptor().endsWith(")V")&&!receivers.containsKey(id)&&FmcExclusionAudit.entry(data.methods.get(id)).earlyPauseReturn(),"exact original first native Pause return "+id);
        var before=new HashMap<>(lower);var changed=new HashMap<String,byte[]>();
        for(var group:grouped.entrySet()){
            String owner=group.getKey();byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);
            var model=data.cf.parse(input);ClassDesc nativeOwner=ClassDesc.of(owner);
            var faults=model.fields().stream().filter(f->f.fieldName().equalsString(FAULT)).toList();
            int faultFlags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC;
            require(data.models.get(owner).fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT)),"original fault-field collision");
            require(faults.isEmpty()||faults.size()==1&&faults.getFirst().fieldType().equalsString("Z")&&faults.getFirst().flags().flagsMask()==faultFlags,"generated shared fault-field shape");
            var seen=new HashSet<Prepare.MethodId>();
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                    if(!(element instanceof MethodModel method)){builder.with(element);return;}
                    var id=new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());
                    if(!group.getValue().contains(id)){builder.with(element);return;}
                    seen.add(id);require(method.code().isPresent()&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==(data.methods.get(id).flags().flagsMask()&ClassFile.ACC_STATIC),"final access staticness/body");
                    var receiver=receivers.get(id);if(receiver!=null)qualifyReceiver(id,method,receiver);
                    builder.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                        int ticket,throwable;Label begin,end,handler;List<ExceptionCatch> original;
                        public void atStart(CodeBuilder b){
                            original=new ArrayList<>();ticket=b.allocateLocal(TypeKind.INT);throwable=b.allocateLocal(TypeKind.REFERENCE);
                            b.iconst_m1().istore(ticket);
                            guard(b,nativeOwner,()->{if(nativePaused.contains(id))b.invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"enter",MethodTypeDesc.of(CD_int))
                                    .getstatic(ClassDesc.of("Core"),"paused",CD_boolean).invokestatic(HELPER,"pauseTicket",MethodTypeDesc.of(CD_int,CD_int,CD_boolean));
                                else if(receiver==null)b.invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int));
                                else b.aload(receiver.slot()).invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int,CD_Object));b.istore(ticket);});
                            if(nativePaused.contains(id)){var original=b.newLabel();b.iload(ticket).ifne(original).return_().labelBinding(original);}
                            begin=b.newLabel();end=b.newLabel();handler=b.newLabel();b.labelBinding(begin);
                        }
                        public void accept(CodeBuilder b,CodeElement e){
                            if(e instanceof ExceptionCatch caught){original.add(caught);return;}
                            if(e instanceof ReturnInstruction ret){
                                // Empty the operand stack around helper handlers. A
                                // native wide/reference result keeps its exact bits.
                                TypeKind kind=ret.typeKind();int result=kind==TypeKind.VOID?-1:b.allocateLocal(kind);
                                if(result>=0)b.storeLocal(kind,result);release(b,nativeOwner,ticket);
                                if(result>=0)b.loadLocal(kind,result);
                            }
                            b.with(e);
                        }
                        public void atEnd(CodeBuilder b){
                            for(var caught:original)b.with(caught);
                            b.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable);
                            release(b,nativeOwner,ticket);b.aload(throwable).athrow();
                        }
                    }));
                }
                public void atEnd(ClassBuilder builder){if(faults.isEmpty())builder.withField(FAULT,CD_boolean,faultFlags);}
            });
            require(seen.equals(group.getValue()),"final method roster drift");
            var errors=data.cf.verify(output);require(errors.isEmpty(),"final verifier "+errors);
            changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static void qualifyReceiver(Prepare.MethodId id,MethodModel method,FmcExclusionPrepare.Receiver receiver){
        var slots=new HashMap<Integer,String>();int slot=0;
        if((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0){slots.put(0,"L"+id.owner()+";");slot=1;}
        for(var p:method.methodTypeSymbol().parameterList()){slots.put(slot,p.descriptorString());slot+=p.equals(CD_long)||p.equals(CD_double)?2:1;}
        require(receiver.descriptor().startsWith("L")&&Objects.equals(slots.get(receiver.slot()),receiver.descriptor()),"exact receiver parameter");
    }
    private static void release(CodeBuilder b,ClassDesc owner,int ticket){
        var done=b.newLabel();b.iload(ticket).ifle(done);
        guard(b,owner,()->b.iload(ticket).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)));b.labelBinding(done);
    }
    private static void guard(CodeBuilder b,ClassDesc owner,Runnable work){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();
        b.labelBinding(start);work.run();b.labelBinding(end).goto_(done).exceptionCatchAll(start,end,failure).labelBinding(failure).pop();
        b.iconst_1().putstatic(owner,FAULT,CD_boolean);
        var report=b.newLabel();var reported=b.newLabel();var absent=b.newLabel();
        b.labelBinding(report).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reported).goto_(done)
            .exceptionCatchAll(report,reported,absent).labelBinding(absent).pop().labelBinding(done);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC background access "+why);}
}
