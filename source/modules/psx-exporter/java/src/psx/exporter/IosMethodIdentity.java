package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;
import java.util.function.Predicate;

/** Structural identity of instructions and ordered catches. Labels use retained
 * instruction ordinals; constant-pool numbering and instruction widening do not
 * conceal a changed branch target, local, member, constant or cleanup range. */
public final class IosMethodIdentity {
    public static List<String> of(MethodModel method,Predicate<Instruction> omit){
        var out=new ArrayList<String>();out.add("flags:"+method.flags().flagsMask());
        if(method.code().isEmpty()){out.add("no-code");return List.copyOf(out);}
        var code=method.code().get();var labels=new HashMap<Label,Integer>();int retained=0;
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),retained);if(e instanceof Instruction i&&!omit.test(i))retained++;}
        for(var e:code)if(e instanceof Instruction i&&!omit.test(i)){
            String detail="";
            if(i instanceof FieldInstruction f)detail=f.owner().asInternalName()+"."+f.name()+":"+f.type();
            else if(i instanceof InvokeInstruction c)detail=c.owner().asInternalName()+"."+c.name()+c.type()+":"+c.isInterface();
            else if(i instanceof BranchInstruction b)detail="target:"+labels.get(b.target());
            else if(i instanceof LoadInstruction l)detail="local:"+l.slot()+":"+l.typeKind();
            else if(i instanceof StoreInstruction s)detail="local:"+s.slot()+":"+s.typeKind();
            else if(i instanceof IncrementInstruction n)detail="local:"+n.slot()+":"+n.constant();
            else if(i instanceof ConstantInstruction c)detail="constant:"+c.constantValue().getClass().getName()+":"+c.constantValue();
            else if(i instanceof TypeCheckInstruction t)detail="type:"+t.type().asInternalName();
            else if(i instanceof NewObjectInstruction n)detail="new:"+n.className().asInternalName();
            else if(i instanceof NewReferenceArrayInstruction n)detail="array:"+n.componentType().asInternalName();
            else if(i instanceof NewPrimitiveArrayInstruction n)detail="array:"+n.typeKind();
            else if(i instanceof NewMultiArrayInstruction n)detail="array:"+n.arrayType().asInternalName()+":"+n.dimensions();
            else if(i instanceof TableSwitchInstruction s)detail="switch:"+labels.get(s.defaultTarget())+":"+s.cases().stream().map(c->c.caseValue()+"="+labels.get(c.target())).toList();
            else if(i instanceof LookupSwitchInstruction s)detail="switch:"+labels.get(s.defaultTarget())+":"+s.cases().stream().map(c->c.caseValue()+"="+labels.get(c.target())).toList();
            else if(i instanceof InvokeDynamicInstruction d)detail="dynamic:"+d.name()+d.type()+":"+d.bootstrapMethod()+":"+d.bootstrapArgs();
            out.add((i instanceof ConstantInstruction?"CONSTANT":i.opcode().name())+":"+detail);
        }
        for(var h:code.exceptionHandlers())out.add("catch:"+labels.get(h.tryStart())+":"+labels.get(h.tryEnd())+":"+labels.get(h.handler())+":"+h.catchType().map(x->x.asInternalName()).orElse("all"));
        return List.copyOf(out);
    }
    private IosMethodIdentity(){}
}
