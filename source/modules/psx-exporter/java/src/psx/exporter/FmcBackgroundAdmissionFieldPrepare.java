package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** A native field's actual receiver acquires the method-lifetime witness.
 * This covers source/destination aliases in copy/geometry routines without
 * assuming that `this` or a same-typed parameter owns every access. All native
 * operands, return values, handlers and exceptions retain their original path.
 * Unexpected closed access invalidates optional recall and still executes. */
final class FmcBackgroundAdmissionFieldPrepare {
    static final String OWNED="$psxExporter$fmcAccessOwned",GLOBAL="$psxExporter$fmcAccessGlobal",PAUSED="$psxExporter$fmcAccessPaused",LEAVE="$psxExporter$fmcAccessLeave";
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBackgroundAdmission");
    private FmcBackgroundAdmissionFieldPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> fields,Set<Prepare.MethodId> methods){
        return transform(data,lower,fields,methods,Set.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> fields,Set<Prepare.MethodId> methods,Set<Prepare.MethodId> paused){
        return transform(data,lower,fields,methods,paused,Set.of());
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> fields,Set<Prepare.MethodId> methods,Set<Prepare.MethodId> paused,Set<FmcBackgroundAdmissionProjection.Site> dispatchOnly){
        require(methods.containsAll(paused),"paused method roster");
        for(var id:paused)require(FmcExclusionAudit.entry(data.methods.get(id)).earlyPauseReturn(),"original first Pause return");
        var grouped=new TreeMap<String,Set<Prepare.MethodId>>();
        for(var id:methods){require(data.methods.containsKey(id)&&!id.name().equals("<clinit>"),"original instance/body method");grouped.computeIfAbsent(id.owner(),k->new HashSet<>()).add(id);}
        var before=new HashMap<>(lower);var output=new HashMap<String,byte[]>();
        var projections=new HashSet<>(FmcBackgroundAdmissionProjection.qualify(data,lower));projections.addAll(dispatchOnly);
        for(var group:grouped.entrySet()){
            String owner=group.getKey();ClassDesc self=ClassDesc.of(owner);byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);
            var faults=model.fields().stream().filter(f->f.fieldName().equalsString(FmcBackgroundAdmissionPrepare.FAULT)).toList();
            boolean qualifiedShared=false;if(!faults.isEmpty()){boolean option=model.methods().stream().anyMatch(m->FmcBackgroundAdmissionOptionPrepare.generated(data,owner,m)),packet=FmcBackgroundAdmissionPacketPrepare.generatedFamily(data,owner,model);if(option)FmcBackgroundAdmissionOptionPrepare.qualify(data);if(packet&&FmcBackgroundAdmissionPacketPrepare.nativeOwner(owner))FmcBackgroundAdmissionPacketPrepare.qualify(data);qualifiedShared=(option||packet)&&data.models.get(owner).fields().stream().noneMatch(f->f.fieldName().equalsString(FmcBackgroundAdmissionPrepare.FAULT))&&faults.size()==1&&faults.getFirst().fieldType().equalsString("Z")&&faults.getFirst().flags().flagsMask()==(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);}
            require((faults.isEmpty()||qualifiedShared)
                &&model.methods().stream().noneMatch(m->m.methodName().stringValue().startsWith(OWNED)||Set.of(GLOBAL,PAUSED,LEAVE).contains(m.methodName().stringValue())),"generated member collision");
            var receiverOwners=new TreeSet<String>();
            for(var method:model.methods())if(group.getValue().contains(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue())))
                for(var op:Prepare.instructions(method))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.PUTFIELD)){
                    var member=FmcQualification.resolve(data,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());if(member!=null)receiverOwners.add(member.owner());
                }
            var receiverHelpers=new TreeMap<String,String>();for(String nativeOwner:receiverOwners)receiverHelpers.put(nativeOwner,OWNED+receiverHelpers.size());
            byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement element){
                    if(!(element instanceof MethodModel m)){b.with(element);return;}
                    var id=new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue());
                    if(!group.getValue().contains(id)){b.with(element);return;}
                    require(m.code().isPresent(),"final method body");
                    boolean constructor=id.name().equals("<init>");String superclass=model.superclass().orElseThrow().asInternalName();
                    if(constructor){
                        boolean initialized=false;var nativeOps=Prepare.instructions(m);
                        for(int at=0;at<nativeOps.size();at++){var op=nativeOps.get(at);
                            if(op instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals(superclass)&&call.name().equalsString("<init>"))initialized=true;
                            if(op instanceof FieldInstruction f&&FmcQualification.resolve(data,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null)
                                require(initialized||privateOuterPrefix(model,m,nativeOps,at,f),"selected live field before constructor superclass initialization: "+id+" "+f);
                        }
                        require(initialized,"native constructor superclass initialization");
                    }
                    b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                        int ticket,throwable;Label begin,end,handler;List<ExceptionCatch> nativeHandlers;boolean begun;
                        int ordinal;
                        public void atStart(CodeBuilder c){ticket=c.allocateLocal(TypeKind.INT);throwable=c.allocateLocal(TypeKind.REFERENCE);c.iconst_m1().istore(ticket);
                            if(paused.contains(id)){var body=c.newLabel();c.invokestatic(self,PAUSED,MethodTypeDesc.of(CD_int)).istore(ticket).iload(ticket).ifne(body).return_().labelBinding(body);}
                            nativeHandlers=new ArrayList<>();begin=c.newLabel();end=c.newLabel();handler=c.newLabel();if(!constructor){c.labelBinding(begin);begun=true;}}
                        public void accept(CodeBuilder c,CodeElement e){
                            if(e instanceof ExceptionCatch caught){nativeHandlers.add(caught);return;}
                            boolean projected=e instanceof Instruction&&projections.contains(new FmcBackgroundAdmissionProjection.Site(id,ordinal));if(e instanceof Instruction)ordinal++;
                            if(constructor&&!begun&&e instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals(superclass)&&call.name().equalsString("<init>")){
                                c.with(e);c.labelBinding(begin);begun=true;return;
                            }
                            // The exact native synthetic-final outer link is
                            // initialized on fresh uninitialized-this. It can
                            // never be an already bound recall receiver, and
                            // passing it to a helper before super is illegal.
                            if(constructor&&!begun){c.with(e);return;}
                            if(!projected&&e instanceof FieldInstruction f&&FmcQualification.resolve(data,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){
                                var member=FmcQualification.resolve(data,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                                var done=c.newLabel();
                                boolean put=f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC;
                                boolean stat=f.opcode()==Opcode.GETSTATIC||f.opcode()==Opcode.PUTSTATIC;
                                TypeKind value=kind(f.type().stringValue());int saved=put?c.allocateLocal(value):-1;
                                if(put)c.storeLocal(value,saved);
                                c.iload(ticket).ifgt(done);
                                if(stat)c.invokestatic(self,GLOBAL,MethodTypeDesc.of(CD_int));
                                else c.dup().invokestatic(self,receiverHelpers.get(member.owner()),MethodTypeDesc.of(CD_int,CD_Object));
                                c.istore(ticket).labelBinding(done);if(put)c.loadLocal(value,saved);
                            }
                            if(e instanceof ReturnInstruction)c.iload(ticket).invokestatic(self,LEAVE,MethodTypeDesc.of(CD_void,CD_int));
                            c.with(e);
                        }
                        public void atEnd(CodeBuilder c){
                            for(var h:nativeHandlers)c.with(h);
                            c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable)
                                .iload(ticket).invokestatic(self,LEAVE,MethodTypeDesc.of(CD_void,CD_int)).aload(throwable).athrow();
                        }
                    }));
                }
                public void atEnd(ClassBuilder b){
                    if(faults.isEmpty())b.withField(FmcBackgroundAdmissionPrepare.FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
                    for(var receiver:receiverHelpers.entrySet())owned(b,self,receiver.getValue(),ClassDesc.of(receiver.getKey()));
                    enter(b,self,GLOBAL,false);
                    if(group.getValue().stream().anyMatch(paused::contains))b.withMethodBody(PAUSED,MethodTypeDesc.of(CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                        int result=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(result);
                        guard(c,self,()->c.invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"enter",MethodTypeDesc.of(CD_int)).getstatic(ClassDesc.of("Core"),"paused",CD_boolean)
                            .invokestatic(HELPER,"pauseTicket",MethodTypeDesc.of(CD_int,CD_int,CD_boolean)).istore(result),()->{});c.iload(result).ireturn();
                    });
                    b.withMethodBody(LEAVE,MethodTypeDesc.of(CD_void,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                        var done=c.newLabel();c.iload(0).ifle(done);guard(c,self,()->c.iload(0).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)),()->{});c.labelBinding(done).return_();
                    });
                }
            });
            var errors=data.cf.verify(bytes);require(errors.isEmpty(),"final bytecode verifier "+errors);output.put(owner,bytes);
        }
        var legalFinal=new HashMap<>(lower);legalFinal.putAll(output);TerrainAccess.verifyAdded(before,legalFinal,data.inputs);return Map.copyOf(output);
    }
    static boolean privateOuterPrefix(ClassModel owner,MethodModel constructor,List<Instruction> ops,int at,FieldInstruction field){
        if(at!=2||field.opcode()!=Opcode.PUTFIELD||!field.owner().asInternalName().equals(owner.thisClass().asInternalName())||!field.name().equalsString("this$0")
            ||!(ops.get(0) instanceof LoadInstruction self)||self.typeKind()!=TypeKind.REFERENCE||self.slot()!=0
            ||!(ops.get(1) instanceof LoadInstruction input)||input.typeKind()!=TypeKind.REFERENCE||input.slot()!=1)return false;
        var parameters=constructor.methodTypeSymbol().parameterList();if(parameters.isEmpty()||!parameters.getFirst().descriptorString().equals(field.type().stringValue()))return false;
        var declared=owner.fields().stream().filter(f->f.fieldName().equals(field.name())&&f.fieldType().equalsString(field.type().stringValue())).toList();
        return declared.size()==1&&declared.getFirst().flags().flagsMask()==(ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC);
    }
    private static void owned(ClassBuilder b,ClassDesc self,String name,ClassDesc receiverOwner){
        b.withMethodBody(name,MethodTypeDesc.of(CD_int,CD_Object),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            int result=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(result);var done=c.newLabel();c.aload(0).ifnull(done);
            guard(c,self,()->{
                int tag=c.allocateLocal(TypeKind.LONG);
                c.aload(0).invokestatic(receiverOwner,FmcBackgroundAdmissionOwnership.READ,MethodTypeDesc.of(CD_long,CD_Object)).lstore(tag);
                c.lload(tag).invokestatic(HELPER,"enterTag",MethodTypeDesc.of(CD_int,CD_long)).istore(result);
            },()->{});
            c.labelBinding(done).iload(result).ireturn();
        });
    }
    private static void enter(ClassBuilder b,ClassDesc self,String name,boolean owned){
        b.withMethodBody(name,owned?MethodTypeDesc.of(CD_int,CD_Object):MethodTypeDesc.of(CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            int result=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(result);
            guard(c,self,()->{if(owned)c.aload(0).invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int,CD_Object));else c.invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int));c.istore(result);},()->{});
            c.iload(result).ireturn();
        });
    }
    private static void guard(CodeBuilder c,ClassDesc self,Runnable work,Runnable fallback){
        var begin=c.newLabel();var end=c.newLabel();var bad=c.newLabel();var done=c.newLabel();c.labelBinding(begin);work.run();c.labelBinding(end).goto_(done)
            .exceptionCatchAll(begin,end,bad).labelBinding(bad).pop().iconst_1().putstatic(self,FmcBackgroundAdmissionPrepare.FAULT,CD_boolean);fallback.run();
        var report=c.newLabel();var reported=c.newLabel();var missing=c.newLabel();c.labelBinding(report).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reported).goto_(done)
            .exceptionCatchAll(report,reported,missing).labelBinding(missing).pop().labelBinding(done);
    }
    private static TypeKind kind(String d){return switch(d.charAt(0)){case 'J'->TypeKind.LONG;case 'D'->TypeKind.DOUBLE;case 'F'->TypeKind.FLOAT;case 'L','['->TypeKind.REFERENCE;default->TypeKind.INT;};}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC actual field receiver "+why);}
}
