package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Current-state inputs are never recipe outputs. A native store of identical
 * primitive bits/reference identity may continue while recall is closed; a
 * change acquires its entire original method lifetime before the native write.
 * Closed changes revoke recall and preserve native behavior. The original
 * operation's store checkpoints then reject the stale reconstruction authority.
 * No physical/input reader, whole dynamics frame or native return is skipped. */
final class FmcBackgroundAdmissionInputPrepare {
    static final String FAULT="$psxExporter$fmcCurrentInputFailure",STORE="$psxExporter$fmcInputStore",LEAVE="$psxExporter$fmcInputLeave";
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBackgroundAdmission");
    record Site(FmcQualification.Member field,boolean stat,String helper){}
    record Result(Map<String,byte[]> changed,Set<Prepare.MethodId> methods,int stores){}
    private FmcBackgroundAdmissionInputPrepare(){}
    static Result transform(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> inputs){
        if(inputs.isEmpty())return new Result(Map.of(),Set.of(),0);
        var grouped=new TreeMap<String,Set<Prepare.MethodId>>();int total=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()&&!entry.getKey().name().equals("<clinit>"))
            for(var op:Prepare.instructions(entry.getValue()))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)
                &&FmcQualification.resolve(data,inputs,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){
                grouped.computeIfAbsent(entry.getKey().owner(),k->new HashSet<>()).add(entry.getKey());total++;
            }
        var before=new HashMap<>(lower);var output=new HashMap<String,byte[]>();var allMethods=new HashSet<Prepare.MethodId>();
        for(var group:grouped.entrySet()){
            String owner=group.getKey();var self=ClassDesc.of(owner);byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);
            require(model.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT))&&model.methods().stream().noneMatch(m->m.methodName().stringValue().startsWith(STORE)||m.methodName().equalsString(LEAVE)),"generated input collision");
            var sites=new LinkedHashMap<FmcQualification.Member,Site>();
            for(var id:group.getValue())for(var op:Prepare.instructions(data.methods.get(id)))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                var field=FmcQualification.resolve(data,inputs,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(field!=null)sites.computeIfAbsent(field,k->new Site(k,f.opcode()==Opcode.PUTSTATIC,STORE+sites.size()));
            }
            byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)){b.with(e);return;}var id=new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue());
                    if(!group.getValue().contains(id)){b.with(e);return;}allMethods.add(id);boolean constructor=id.name().equals("<init>");String superclass=model.superclass().orElseThrow().asInternalName();
                    b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                        int ticket,thrown;Label begin,end,handler;boolean begun;List<ExceptionCatch> nativeHandlers;
                        public void atStart(CodeBuilder c){ticket=c.allocateLocal(TypeKind.INT);thrown=c.allocateLocal(TypeKind.REFERENCE);c.iconst_m1().istore(ticket);nativeHandlers=new ArrayList<>();begin=c.newLabel();end=c.newLabel();handler=c.newLabel();if(!constructor){c.labelBinding(begin);begun=true;}}
                        public void accept(CodeBuilder c,CodeElement element){
                            if(element instanceof ExceptionCatch h){nativeHandlers.add(h);return;}
                            if(constructor&&!begun&&element instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals(superclass)&&call.name().equalsString("<init>")){c.with(element);c.labelBinding(begin);begun=true;return;}
                            if(element instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                                var field=FmcQualification.resolve(data,inputs,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                                if(field!=null){
                                    if(!begun&&constructor){var original=data.methods.get(id);var nativeOps=Prepare.instructions(original);
                                        if(nativeOps.size()>2&&nativeOps.get(2) instanceof FieldInstruction prefix&&prefix.owner().equals(f.owner())&&prefix.name().equals(f.name())&&prefix.type().equals(f.type())&&FmcBackgroundAdmissionFieldPrepare.privateOuterPrefix(data.models.get(owner),original,nativeOps,2,prefix)){c.with(element);return;}}
                                    require(begun,"input constructor store before superclass: "+id+" "+f);var site=sites.get(field);TypeKind value=kind(f.type().stringValue());int saved=c.allocateLocal(value);c.storeLocal(value,saved);
                                    var done=c.newLabel();c.iload(ticket).ifgt(done);if(!site.stat)c.dup();c.loadLocal(value,saved).invokestatic(self,site.helper,type(site)).istore(ticket).labelBinding(done).loadLocal(value,saved);}
                            }
                            if(element instanceof ReturnInstruction)c.iload(ticket).invokestatic(self,LEAVE,MethodTypeDesc.of(CD_void,CD_int));c.with(element);
                        }
                        public void atEnd(CodeBuilder c){require(begun,"input method scope");for(var h:nativeHandlers)c.with(h);c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(thrown).iload(ticket).invokestatic(self,LEAVE,MethodTypeDesc.of(CD_void,CD_int)).aload(thrown).athrow();}
                    }));
                }
                public void atEnd(ClassBuilder b){
                    b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
                    for(var site:sites.values())store(b,self,site);
                    b.withMethodBody(LEAVE,MethodTypeDesc.of(CD_void,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var done=c.newLabel();c.iload(0).ifle(done);guard(c,self,()->c.iload(0).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)));c.labelBinding(done).return_();});
                }
            });
            var errors=data.cf.verify(bytes);require(errors.isEmpty(),"input final verifier "+errors);output.put(owner,bytes);
        }
        var legalFinal=new HashMap<>(lower);legalFinal.putAll(output);TerrainAccess.verifyAdded(before,legalFinal,data.inputs);return new Result(Map.copyOf(output),Set.copyOf(allMethods),total);
    }
    private static MethodTypeDesc type(Site site){var value=ClassDesc.ofDescriptor(site.field.descriptor());return site.stat?MethodTypeDesc.of(CD_int,value):MethodTypeDesc.of(CD_int,CD_Object,value);}
    private static void store(ClassBuilder b,ClassDesc self,Site site){
        b.withMethodBody(site.helper,type(site),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            int result=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(result);var done=c.newLabel();if(!site.stat)c.aload(0).ifnull(done);
            guard(c,self,()->{
                var owner=ClassDesc.of(site.field.owner());var value=ClassDesc.ofDescriptor(site.field.descriptor());TypeKind kind=kind(site.field.descriptor());
                if(site.stat)c.getstatic(owner,site.field.name(),value);else c.aload(0).checkcast(owner).getfield(owner,site.field.name(),value);
                raw(c,kind);c.loadLocal(kind,site.stat?0:1);raw(c,kind);
                var same=c.newLabel();if(kind==TypeKind.REFERENCE)c.if_acmpeq(same);else if(kind==TypeKind.LONG||kind==TypeKind.DOUBLE)c.lcmp().ifeq(same);else c.if_icmpeq(same);
                if(site.stat)c.invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int));
                else{int tag=c.allocateLocal(TypeKind.LONG);c.aload(0).invokestatic(owner,FmcBackgroundAdmissionOwnership.READ,MethodTypeDesc.of(CD_long,CD_Object)).lstore(tag).lload(tag).invokestatic(HELPER,"enterTag",MethodTypeDesc.of(CD_int,CD_long));}
                c.istore(result).labelBinding(same);
            });c.labelBinding(done).iload(result).ireturn();
        });
    }
    private static void raw(CodeBuilder c,TypeKind kind){if(kind==TypeKind.FLOAT)c.invokestatic(ClassDesc.of("java.lang.Float"),"floatToRawIntBits",MethodTypeDesc.of(CD_int,CD_float));else if(kind==TypeKind.DOUBLE)c.invokestatic(ClassDesc.of("java.lang.Double"),"doubleToRawLongBits",MethodTypeDesc.of(CD_long,CD_double));}
    private static void guard(CodeBuilder c,ClassDesc self,Runnable work){var begin=c.newLabel();var end=c.newLabel();var bad=c.newLabel();var done=c.newLabel();c.labelBinding(begin);work.run();c.labelBinding(end).goto_(done).exceptionCatchAll(begin,end,bad).labelBinding(bad).pop().iconst_1().putstatic(self,FAULT,CD_boolean);
        var report=c.newLabel();var reported=c.newLabel();var missing=c.newLabel();c.labelBinding(report).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reported).goto_(done).exceptionCatchAll(report,reported,missing).labelBinding(missing).pop().labelBinding(done);}
    private static TypeKind kind(String d){return switch(d.charAt(0)){case 'J'->TypeKind.LONG;case 'D'->TypeKind.DOUBLE;case 'F'->TypeKind.FLOAT;case 'L','['->TypeKind.REFERENCE;default->TypeKind.INT;};}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC current input "+why);}
}
