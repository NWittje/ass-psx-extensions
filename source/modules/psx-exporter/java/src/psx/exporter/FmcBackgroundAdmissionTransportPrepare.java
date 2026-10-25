package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Observe original PrintWriter errors without interrupting native transport
 * shutdown. The boolean and every native handler remain unchanged; optional
 * recall is invalidated only after the full original return/throw path. */
final class FmcBackgroundAdmissionTransportPrepare {
    static final String FAULT="$psxExporter$fmcTransportFailure",REPORT="$psxExporter$fmcTransportReport",BEGIN="$psxExporter$fmcTransportBegin",END="$psxExporter$fmcTransportEnd";
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBackgroundAdmission");
    private static final ClassDesc AUTHORITY=ClassDesc.of("psx.exporter.FmcExecSeam");
    private FmcBackgroundAdmissionTransportPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        // The independent original partition is mandatory even when this
        // layer is applied onto already prepared native methods.
        FmcBackgroundAdmissionTransport.inspect(data);
        return transform(data,lower,2);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,int expectedChecks){
        String owner="Network";ClassDesc self=ClassDesc.of(owner);byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));var model=data.cf.parse(input);
        require(model.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT))&&model.methods().stream().noneMatch(m->Set.of(REPORT,BEGIN,END).contains(m.methodName().stringValue())),"generated member collision");
        var original=data.methods.get(FmcBackgroundAdmissionTransport.FRONTIER);require(original!=null&&original.code().isPresent(),"original body");
        require((original.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED))==(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED),"original serialized output lifetime");
        int checks=0;for(var op:Prepare.instructions(original))if(Prepare.call(op,"java/io/PrintWriter","checkError","()Z"))checks++;
        require(checks==expectedChecks&&checks>0,"original error-site vector");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement element){
                if(!(element instanceof MethodModel m)||!m.methodName().equalsString("toPrint")||!m.methodType().equalsString(FmcBackgroundAdmissionTransport.FRONTIER.descriptor())){b.with(element);return;}
                b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                    int failure,thrown,seen,ticket;Label begin,end,handler;List<ExceptionCatch> nativeHandlers;
                    public void atStart(CodeBuilder c){failure=c.allocateLocal(TypeKind.INT);thrown=c.allocateLocal(TypeKind.REFERENCE);ticket=c.allocateLocal(TypeKind.INT);begin=c.newLabel();end=c.newLabel();handler=c.newLabel();nativeHandlers=new ArrayList<>();c.iconst_0().istore(failure).iconst_0().istore(ticket).labelBinding(begin);}
                    public void accept(CodeBuilder c,CodeElement e){
                        if(e instanceof ExceptionCatch h){nativeHandlers.add(h);return;}
                        if(e instanceof ReturnInstruction){c.iload(ticket).invokestatic(self,END,MethodTypeDesc.of(CD_void,CD_int)).iload(failure).invokestatic(self,REPORT,MethodTypeDesc.of(CD_void,CD_boolean));}
                        c.with(e);
                        if(e instanceof Instruction op&&Prepare.call(op,"java/io/PrintWriter","checkError","()Z")){seen++;var keep=c.newLabel();c.dup().iload(failure).ior().istore(failure).dup().ifeq(keep).iload(ticket).ifne(keep).invokestatic(self,BEGIN,MethodTypeDesc.of(CD_int)).istore(ticket).labelBinding(keep);}
                    }
                    public void atEnd(CodeBuilder c){require(seen==expectedChecks,"final error-site drift");for(var h:nativeHandlers)c.with(h);
                        c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(thrown).iload(ticket).invokestatic(self,END,MethodTypeDesc.of(CD_void,CD_int)).iconst_1().invokestatic(self,REPORT,MethodTypeDesc.of(CD_void,CD_boolean)).aload(thrown).athrow();}
                }));
            }
            public void atEnd(ClassBuilder b){
                b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
                b.withMethodBody(BEGIN,MethodTypeDesc.of(CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var start=c.newLabel();var end=c.newLabel();var absent=c.newLabel();c.labelBinding(start).invokestatic(AUTHORITY,"enterTransportContinuation",MethodTypeDesc.of(CD_int)).labelBinding(end).ireturn().exceptionCatchAll(start,end,absent).labelBinding(absent).pop().iconst_1().putstatic(self,FAULT,CD_boolean).iconst_0().ireturn();});
                b.withMethodBody(END,MethodTypeDesc.of(CD_void,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var start=c.newLabel();var end=c.newLabel();var absent=c.newLabel();var done=c.newLabel();c.labelBinding(start).iload(0).invokestatic(AUTHORITY,"leaveTransportContinuation",MethodTypeDesc.of(CD_void,CD_int)).labelBinding(end).goto_(done).exceptionCatchAll(start,end,absent).labelBinding(absent).pop().iconst_1().putstatic(self,FAULT,CD_boolean).labelBinding(done).return_();});
                b.withMethodBody(REPORT,MethodTypeDesc.of(CD_void,CD_boolean),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var done=c.newLabel();var start=c.newLabel();var end=c.newLabel();var absent=c.newLabel();
                    c.iload(0).ifeq(done).labelBinding(start).invokestatic(HELPER,"operationScope",MethodTypeDesc.of(CD_long)).lconst_0().lcmp().ifeq(end).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void))
                        .labelBinding(end).goto_(done).exceptionCatchAll(start,end,absent).labelBinding(absent).pop().iconst_1().putstatic(self,FAULT,CD_boolean).labelBinding(done).return_();
                });
            }
        });
        require(data.cf.verify(output).isEmpty(),"final verifier");var before=new HashMap<>(lower);before.put(owner,input);var finalClasses=new HashMap<>(lower);finalClasses.put(owner,output);TerrainAccess.verifyAdded(before,finalClasses,data.inputs);return Map.of(owner,output);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native transport continuation "+why);}
}
