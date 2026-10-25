package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Exact read-only admission helper bodies may be excluded from operation
 * checkpoint rewriting. Their callers still carry every native store/call
 * checkpoint. An original method, arbitrary synthetic prefix or helper which
 * calls a native content/output mutator never gets this exemption. */
final class FmcBackgroundAdmissionHelpers {
    private FmcBackgroundAdmissionHelpers(){}
    static boolean readOnly(Prepare data,String owner,MethodModel method,Map<String,ClassModel> models){
        String name=method.methodName().stringValue(),descriptor=method.methodType().stringValue();
        boolean field=name.matches("\\Q"+FmcBackgroundAdmissionFieldPrepare.OWNED+"\\E[0-9]+")&&descriptor.equals("(Ljava/lang/Object;)I")
            ||Set.of(FmcBackgroundAdmissionFieldPrepare.GLOBAL,FmcBackgroundAdmissionFieldPrepare.PAUSED).contains(name)&&descriptor.equals("()I")
            ||name.equals(FmcBackgroundAdmissionFieldPrepare.LEAVE)&&descriptor.equals("(I)V");
        boolean input=name.matches("\\Q"+FmcBackgroundAdmissionInputPrepare.STORE+"\\E[0-9]+")&&method.methodTypeSymbol().returnType().descriptorString().equals("I")
            &&(method.methodTypeSymbol().parameterCount()==1||method.methodTypeSymbol().parameterCount()==2&&method.methodTypeSymbol().parameterType(0).descriptorString().equals("Ljava/lang/Object;"))
            ||name.equals(FmcBackgroundAdmissionInputPrepare.LEAVE)&&descriptor.equals("(I)V");
        boolean transport=owner.equals("Network")&&(name.equals(FmcBackgroundAdmissionTransportPrepare.REPORT)&&descriptor.equals("(Z)V")||name.equals(FmcBackgroundAdmissionTransportPrepare.BEGIN)&&descriptor.equals("()I")||name.equals(FmcBackgroundAdmissionTransportPrepare.END)&&descriptor.equals("(I)V"));
        boolean afds=owner.equals("AfdsHub")&&(Set.of(FmcBackgroundAdmissionAfdsPrepare.CLAIM,FmcBackgroundAdmissionAfdsPrepare.ENTER,FmcBackgroundAdmissionAfdsPrepare.FALLBACK_ENTER,FmcBackgroundAdmissionAfdsPrepare.DRAIN_ENTER).contains(name)&&descriptor.equals("()I")||name.equals(FmcBackgroundAdmissionAfdsPrepare.RETIRE)&&descriptor.equals("(IZ)V")||Set.of(FmcBackgroundAdmissionAfdsPrepare.LEAVE,FmcBackgroundAdmissionAfdsPrepare.DRAIN_LEAVE).contains(name)&&descriptor.equals("(I)V"));
        boolean rendering=owner.equals("NavigationDisplay")&&(name.equals(FmcRenderingPrepare.ENTRY)&&descriptor.equals("()I")||name.equals(FmcRenderingPrepare.LEAVE)&&descriptor.equals("(I)V"));
        boolean option=owner.equals("Option")&&FmcBackgroundAdmissionOptionPrepare.generated(data,owner,method);
        boolean packet=FmcBackgroundAdmissionPacketPrepare.generated(data,owner,method);
        if(!field&&!input&&!transport&&!afds&&!rendering&&!option&&!packet||data.methods.containsKey(new Prepare.MethodId(owner,name,descriptor))||method.code().isEmpty()
            ||method.flags().flagsMask()!=(ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC))return false;
        var fault=rendering?FmcRenderingPrepare.FAULT:afds?FmcBackgroundAdmissionAfdsPrepare.FAULT:transport?FmcBackgroundAdmissionTransportPrepare.FAULT:input?FmcBackgroundAdmissionInputPrepare.FAULT:FmcBackgroundAdmissionPrepare.FAULT;
        var ops=Prepare.instructions(method);
        for(int at=0;at<ops.size();at++){
            var op=ops.get(at);
            if(op instanceof NewObjectInstruction||op instanceof NewReferenceArrayInstruction||op instanceof NewPrimitiveArrayInstruction||op instanceof NewMultiArrayInstruction||op instanceof InvokeDynamicInstruction)return false;
            if(Set.of(Opcode.IASTORE,Opcode.LASTORE,Opcode.FASTORE,Opcode.DASTORE,Opcode.AASTORE,Opcode.BASTORE,Opcode.CASTORE,Opcode.SASTORE).contains(op.opcode())||op.opcode()==Opcode.MONITORENTER||op.opcode()==Opcode.MONITOREXIT)return false;
            if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                if(f.opcode()!=Opcode.PUTSTATIC||!f.owner().asInternalName().equals(owner)||!f.name().equalsString(fault)||!f.type().equalsString("Z")||at==0||ops.get(at-1).opcode()!=Opcode.ICONST_1)return false;
                var declaration=models.get(owner).fields().stream().filter(x->x.fieldName().equalsString(fault)).toList();
                if(declaration.size()!=1||!declaration.getFirst().fieldType().equalsString("Z")||declaration.getFirst().flags().flagsMask()!=(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC))return false;
            }
            if(op instanceof InvokeInstruction c){
                String target=c.owner().asInternalName(),signature=c.name().stringValue()+c.type().stringValue();
                if(afds&&Set.of(FmcBackgroundAdmissionAfdsPrepare.LEAVE,FmcBackgroundAdmissionAfdsPrepare.FALLBACK_ENTER).contains(name)&&c.opcode()==Opcode.INVOKEVIRTUAL&&target.equals("java/util/concurrent/atomic/AtomicInteger")&&Set.of("get()I","compareAndSet(II)Z","set(I)V","decrementAndGet()I").contains(signature)){
                    var declarations=models.get(owner).fields().stream().filter(f->f.fieldName().equalsString(FmcBackgroundAdmissionAfdsPrepare.SHADOW+"Readers")).toList();if(declarations.size()!=1||!declarations.getFirst().fieldType().equalsString("Ljava/util/concurrent/atomic/AtomicInteger;")||declarations.getFirst().flags().flagsMask()!=(ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC))return false;continue;
                }
                if(c.opcode()!=Opcode.INVOKESTATIC)return false;
                if(afds&&name.equals(FmcBackgroundAdmissionAfdsPrepare.ENTER)&&target.equals(owner)&&signature.equals(FmcBackgroundAdmissionAfdsPrepare.FALLBACK_ENTER+"()I")){var methods=models.get(owner).methods().stream().filter(m->m.methodName().equalsString(FmcBackgroundAdmissionAfdsPrepare.FALLBACK_ENTER)&&m.methodType().equalsString("()I")).toList();if(methods.size()!=1||!readOnly(data,owner,methods.getFirst(),models))return false;continue;}
                if(target.equals("psx/exporter/FmcBackgroundAdmission")&&Set.of("enter()I","enterTag(J)I","leave(I)V","failed()V","pauseTicket(IZ)I").contains(signature))continue;
                if(transport&&target.equals("psx/exporter/FmcBackgroundAdmission")&&signature.equals("operationScope()J"))continue;
                if(transport&&target.equals("psx/exporter/FmcExecSeam")&&Set.of("enterTransportContinuation()I","leaveTransportContinuation(I)V").contains(signature))continue;
                if(afds&&target.equals("psx/exporter/FmcBackgroundAdmissionAfdsSnapshot")&&Set.of("claim()I","retire(IZ)V","enterConsumer()I","leaveConsumer(I)V","failed()V").contains(signature))continue;
                if((afds||rendering||option||packet)&&target.equals("psx/exporter/FmcExclusion")&&signature.equals("leave(I)V"))continue;
                if(target.equals("psx/exporter/FmcExclusion")&&signature.equals("enter()I"))continue;
                if(target.equals("java/lang/Float")&&signature.equals("floatToRawIntBits(F)I")||target.equals("java/lang/Double")&&signature.equals("doubleToRawLongBits(D)J"))continue;
                if(c.name().equalsString(FmcBackgroundAdmissionOwnership.READ)&&c.type().equalsString("(Ljava/lang/Object;)J")){
                    var nativeOwner=models.get(target);if(nativeOwner==null)return false;try{FmcBackgroundAdmissionOwnership.qualify(data,target,nativeOwner);}catch(IllegalArgumentException invalid){return false;}continue;
                }
                return false;
            }
        }
        return true;
    }
}
