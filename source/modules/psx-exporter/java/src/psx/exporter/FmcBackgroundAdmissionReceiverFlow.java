package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Source-only proof of the actual receiver at an original call. Only bound
 * static roots and bound child edges establish ownership. A native this edge
 * carries an obligation to its caller; class/type equality alone proves none.
 * Stack copies, local aliases and entries into operand expressions remain
 * unresolved rather than silently treating every instance as owned. */
final class FmcBackgroundAdmissionReceiverFlow {
    enum Origin { OWNED, THIS, UNKNOWN }
    private record Value(int before,Origin origin){}
    private final Prepare data;private final Set<FmcQualification.Member> roots,children;
    private final Map<Prepare.MethodId,Map<Integer,Origin>> calls=new HashMap<>();
    private final Map<Prepare.MethodId,List<Instruction>> instructions=new HashMap<>();
    private final Map<Prepare.MethodId,Set<Integer>> methodEntries=new HashMap<>();
    FmcBackgroundAdmissionReceiverFlow(Prepare data,Set<FmcQualification.Member> roots,Set<FmcQualification.Member> children){this.data=data;this.roots=roots;this.children=children;}
    Origin receiver(Prepare.MethodId caller,int ordinal){
        return calls.computeIfAbsent(caller,k->new HashMap<>()).computeIfAbsent(ordinal,k->compute(caller,k));
    }
    private Origin compute(Prepare.MethodId caller,int ordinal){
        var method=data.methods.get(caller);var ops=instructions.computeIfAbsent(caller,k->Prepare.instructions(method));
        if(ordinal<0||ordinal>=ops.size()||!(ops.get(ordinal) instanceof InvokeInstruction call)||call.opcode()==Opcode.INVOKESTATIC)return Origin.UNKNOWN;
        var entries=methodEntries.computeIfAbsent(caller,k->entries(method));int before=ordinal-1;
        for(int i=0;i<call.typeSymbol().parameterCount();i++){var value=value(method,ops,before,entries,0);if(value==null)return Origin.UNKNOWN;before=value.before();}
        var receiver=value(method,ops,before,entries,0);return receiver==null?Origin.UNKNOWN:receiver.origin();
    }
    Origin fieldReceiver(Prepare.MethodId caller,int ordinal){
        var method=data.methods.get(caller);var ops=instructions.computeIfAbsent(caller,k->Prepare.instructions(method));
        if(!(ops.get(ordinal) instanceof FieldInstruction f)||(f.opcode()!=Opcode.GETFIELD&&f.opcode()!=Opcode.PUTFIELD))return Origin.UNKNOWN;
        var incoming=methodEntries.computeIfAbsent(caller,k->entries(method));int before=ordinal-1;
        if(f.opcode()==Opcode.PUTFIELD){var stored=value(method,ops,before,incoming,0);if(stored==null)return Origin.UNKNOWN;before=stored.before();}
        var receiver=value(method,ops,before,incoming,0);return receiver==null?Origin.UNKNOWN:receiver.origin();
    }
    private Value value(MethodModel method,List<Instruction> ops,int at,Set<Integer> entries,int depth){
        if(at<0||depth>128)return null;var op=ops.get(at);
        // An entry at the first instruction is legal, but an entry after any
        // consumed producer could have supplied a different operand stack.
        Value result;
        if(op instanceof ConstantInstruction)result=new Value(at-1,Origin.UNKNOWN);
        else if(op instanceof LoadInstruction load)result=new Value(at-1,load.typeKind()==TypeKind.REFERENCE&&load.slot()==0&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0?Origin.THIS:Origin.UNKNOWN);
        else if(op instanceof FieldInstruction field&&(op.opcode()==Opcode.GETSTATIC||op.opcode()==Opcode.GETFIELD)){
            var member=FmcQualification.resolveField(data.models,field.owner().asInternalName(),field.name().stringValue(),field.type().stringValue(),new HashSet<>());
            if(op.opcode()==Opcode.GETSTATIC)result=new Value(at-1,member!=null&&roots.contains(member)?Origin.OWNED:Origin.UNKNOWN);
            else{var base=value(method,ops,at-1,entries,depth+1);if(base==null)return null;result=new Value(base.before(),member!=null&&children.contains(member)?base.origin():Origin.UNKNOWN);}
        }else if(op.opcode()==Opcode.CHECKCAST)result=value(method,ops,at-1,entries,depth+1);
        else if(op.opcode()==Opcode.AALOAD){var index=value(method,ops,at-1,entries,depth+1);if(index==null)return null;var array=value(method,ops,index.before(),entries,depth+1);if(array==null)return null;result=new Value(array.before(),array.origin());}
        else if(op instanceof InvokeInstruction call&&!call.typeSymbol().returnType().descriptorString().equals("V")){
            int before=at-1;for(int i=0;i<call.typeSymbol().parameterCount();i++){var argument=value(method,ops,before,entries,depth+1);if(argument==null)return null;before=argument.before();}
            if(call.opcode()!=Opcode.INVOKESTATIC){var receiver=value(method,ops,before,entries,depth+1);if(receiver==null)return null;before=receiver.before();}result=new Value(before,Origin.UNKNOWN);
        }else return null;
        if(result==null)return null;for(int i=result.before()+2;i<=at;i++)if(entries.contains(i))return null;return result;
    }
    private static Set<Integer> entries(MethodModel method){
        var code=method.code().orElseThrow();var labels=new HashMap<Label,Integer>();int at=0;for(var element:code){if(element instanceof LabelTarget target)labels.put(target.label(),at);if(element instanceof Instruction)at++;}
        var entries=new HashSet<Integer>();for(var op:Prepare.instructions(method)){if(op instanceof BranchInstruction b)entries.add(labels.get(b.target()));if(op instanceof LookupSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}if(op instanceof TableSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}}for(var h:code.exceptionHandlers())entries.add(labels.get(h.handler()));return entries;
    }
}
