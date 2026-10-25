package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Source-only integer argument branch proof. Only an adjacent native load of
 * the known argument, a literal and a comparison may remove an existing edge.
 * Other conditions and handlers remain conservative; opaque reassignment or
 * an entry into the operand block immediately loses the constant proof. */
final class FmcBackgroundAdmissionArgumentFlow {
    static final int UNKNOWN=Integer.MIN_VALUE;
    private FmcBackgroundAdmissionArgumentFlow(){}
    static Set<Integer> reachable(MethodModel method,int slot,int initial){
        var ops=Prepare.instructions(method);var code=method.code().orElseThrow();var labels=new HashMap<Label,Integer>();int ordinal=0;for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ordinal);if(e instanceof Instruction)ordinal++;}
        var entries=new HashSet<Integer>();for(var op:ops){if(op instanceof BranchInstruction b)entries.add(labels.get(b.target()));if(op instanceof LookupSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}if(op instanceof TableSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}}for(var h:code.exceptionHandlers())entries.add(labels.get(h.handler()));
        var values=new HashMap<Integer,Integer>();var pending=new ArrayDeque<Integer>();offer(0,initial,values,pending,ops.size());
        while(!pending.isEmpty()){
            int at=pending.removeFirst(),before=values.get(at),after=before;var op=ops.get(at);
            if(op instanceof StoreInstruction store&&store.slot()==slot)after=store.typeKind()==TypeKind.INT&&at>0&&!entries.contains(at)&&ops.get(at-1) instanceof ConstantInstruction c&&c.constantValue() instanceof Integer number?number:UNKNOWN;
            if(op instanceof IncrementInstruction inc&&inc.slot()==slot)after=UNKNOWN;
            for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd()))offer(labels.get(h.handler()),before,values,pending,ops.size());
            if(op instanceof ReturnInstruction||op.opcode()==Opcode.ATHROW)continue;
            if(op instanceof BranchInstruction branch){
                Boolean taken=condition(ops,at,slot,before,entries);
                if(taken==null||taken)offer(labels.get(branch.target()),after,values,pending,ops.size());
                if(op.opcode()==Opcode.GOTO||op.opcode()==Opcode.GOTO_W)continue;if(taken==null||!taken)offer(at+1,after,values,pending,ops.size());
            }else if(op instanceof LookupSwitchInstruction s){offer(labels.get(s.defaultTarget()),after,values,pending,ops.size());for(var c:s.cases())offer(labels.get(c.target()),after,values,pending,ops.size());}
            else if(op instanceof TableSwitchInstruction s){offer(labels.get(s.defaultTarget()),after,values,pending,ops.size());for(var c:s.cases())offer(labels.get(c.target()),after,values,pending,ops.size());}
            else offer(at+1,after,values,pending,ops.size());
        }
        return Set.copyOf(values.keySet());
    }
    private static void offer(int at,int value,Map<Integer,Integer> values,ArrayDeque<Integer> pending,int size){if(at<0||at>=size)throw new IllegalArgumentException("FMC argument flow native target");Integer old=values.get(at);int joined=old==null?value:old==value?old:UNKNOWN;if(old==null||old!=joined){values.put(at,joined);pending.add(at);}}
    private static boolean load(Instruction op,int slot){return op instanceof LoadInstruction l&&l.typeKind()==TypeKind.INT&&l.slot()==slot;}
    private static Boolean condition(List<Instruction> ops,int at,int slot,int value,Set<Integer> entries){
        if(value==UNKNOWN||entries.contains(at))return null;var opcode=ops.get(at).opcode();
        if(at>0&&load(ops.get(at-1),slot)){return switch(opcode){case IFEQ->value==0;case IFNE->value!=0;case IFLT->value<0;case IFLE->value<=0;case IFGT->value>0;case IFGE->value>=0;default->null;};}
        if(at<2||entries.contains(at-1))return null;Integer literal=null;boolean normal=true;
        if(load(ops.get(at-2),slot)&&ops.get(at-1) instanceof ConstantInstruction c&&c.constantValue() instanceof Integer n)literal=n;
        else if(ops.get(at-2) instanceof ConstantInstruction c&&c.constantValue() instanceof Integer n&&load(ops.get(at-1),slot)){literal=n;normal=false;}
        if(literal==null)return null;int left=normal?value:literal,right=normal?literal:value;
        return switch(opcode){case IF_ICMPEQ->left==right;case IF_ICMPNE->left!=right;case IF_ICMPLT->left<right;case IF_ICMPLE->left<=right;case IF_ICMPGT->left>right;case IF_ICMPGE->left>=right;default->null;};
    }
}