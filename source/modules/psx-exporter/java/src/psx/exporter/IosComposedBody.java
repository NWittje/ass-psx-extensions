package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Recognizes only complete existing optional enter/leave/rethrow envelopes.
 * No handler is ignored merely because it belongs to an exporter class. */
public final class IosComposedBody {
    public static List<Instruction> nativeInstructions(MethodModel method){
        var code=method.code().orElseThrow();var labels=new HashMap<Label,Integer>();var ops=new ArrayList<Instruction>();
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ops.size());if(e instanceof Instruction i)ops.add(i);}
        var layers=new ArrayList<String>();int prefix=0;
        while(prefix<ops.size()&&ops.get(prefix) instanceof InvokeInstruction c&&c.name().stringValue().startsWith("$psx")&&c.name().stringValue().endsWith("$enter")){
            require(c.opcode()==Opcode.INVOKESTATIC&&c.type().equalsString("()V"),"Changed composed entry");layers.add(c.owner().asInternalName()+"."+c.name().stringValue().replace("$enter",""));prefix++;
        }
        var omitted=new HashSet<Integer>();for(int n=0;n<prefix;n++)omitted.add(n);
        require(code.exceptionHandlers().size()==layers.size(),"Unqualified native/composed exception handler");
        int nesting=layers.size();
        for(var h:code.exceptionHandlers()){
            int begin=labels.get(h.tryStart()),end=labels.get(h.tryEnd()),handler=labels.get(h.handler());
            require(h.catchType().isEmpty()&&begin>0&&begin<=prefix&&end==handler&&handler+1<ops.size()&&ops.get(handler+1).opcode()==Opcode.ATHROW,
                "Changed composed exception cleanup");
            require(leave(ops.get(handler),layers.get(begin-1)),"Changed composed leave/rethrow identity");
            require(begin==nesting--,"Changed nested composed catch precedence");
            omitted.add(handler);omitted.add(handler+1);
        }
        for(int n=0;n<ops.size();n++)if(ops.get(n) instanceof ReturnInstruction){
            require(n>=prefix,"Return before composed cleanup");
            for(int k=0;k<prefix;k++){int at=n-prefix+k;require(leave(ops.get(at),layers.get(prefix-1-k)),"Changed normal composed leave order");omitted.add(at);}
        }
        var result=new ArrayList<Instruction>();for(int n=0;n<ops.size();n++)if(!omitted.contains(n)){
            var i=ops.get(n);if(i instanceof InvokeInstruction c)require(!c.name().stringValue().matches("\\$psx.*\\$(enter|leave)"),"Unexpected composed scope call");result.add(i);
        }
        return List.copyOf(result);
    }
    /** Complete void native fragments with only the qualified optional envelopes.
     * Explicit native throws require their own qualification, never blanket removal. */
    public static List<String> nativeIdentity(MethodModel method){
        require(nativeInstructions(method).stream().noneMatch(i->i.opcode()==Opcode.ATHROW),"Native explicit throw needs separate identity");
        return IosMethodIdentity.of(method,i->i.opcode()==Opcode.ATHROW||i instanceof InvokeInstruction c&&c.name().stringValue().matches("\\$psx.*\\$(enter|leave)"))
            .stream().filter(s->!s.startsWith("catch:")).toList();
    }
    private static boolean leave(Instruction i,String layer){return i instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKESTATIC&&
        c.type().equalsString("()V")&&(c.owner().asInternalName()+"."+c.name()).equals(layer+"$leave");}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private IosComposedBody(){}
}
