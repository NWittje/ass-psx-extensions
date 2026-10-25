package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unactivated bounded sibling-pass emitter. A native infinite scheduler remains
 * outside the gate: its one forward-only processing pass is protected, while
 * sleep and the original interrupt handler remain outside. No scheduler is
 * invoked by relocation. Outer entry/receiver qualification remains mandatory. */
public final class IosSiblingCyclePrepare {
    public static final String CYCLE="psxExporterIosSiblingCycle";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    public record Selection(int boundary,int sleep,int loop,long delay){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static MethodModel run(ClassModel c){return c.methods().stream().filter(m->m.methodName().equalsString("run")&&m.methodType().equalsString("()V")).findFirst().orElseThrow();}
    public static Selection select(ClassModel c){
        require(c.superclass().isPresent()&&c.superclass().get().asInternalName().equals("java/lang/Thread"),"Changed sibling thread receiver");
        require(c.methods().stream().noneMatch(m->m.methodName().equalsString(CYCLE)),"Sibling cycle already prepared");
        var m=run(c);require(m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"Changed sibling run context");
        var code=m.code().get();var labels=new HashMap<Label,Integer>();var ops=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();int offset=0;
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),offset);if(e instanceof Instruction i){ops.add(i);offsets.add(offset);offset+=i.sizeInBytes();}}
        int cut=-1,sleep=-1,loop=-1;long delay=0;
        for(int n=1;n+1<ops.size();n++)if(ops.get(n) instanceof InvokeInstruction v&&v.opcode()==Opcode.INVOKESTATIC&&v.name().equalsString("sleep")&&v.type().equalsString("(J)V")){
            require(cut<0&&Set.of("java/lang/Thread",c.thisClass().asInternalName()).contains(v.owner().asInternalName()),"Ambiguous/foreign scheduler sleep");
            require(ops.get(n-1) instanceof ConstantInstruction k&&k.constantValue() instanceof Long,"Changed scheduler delay input");
            // Sleep is outside protection: its preserved positive native delay
            // does not determine the processing fragment's scope bound.
            delay=(Long)((ConstantInstruction)ops.get(n-1)).constantValue();require(delay>0,"Invalid native scheduler cadence");
            require(ops.get(n+1) instanceof BranchInstruction b&&b.opcode()==Opcode.GOTO&&Objects.equals(labels.get(b.target()),0),"Missing native single-cycle continuation");
            cut=offsets.get(n-1);sleep=offsets.get(n);loop=offsets.get(n+1);
        }
        require(cut>0,"Missing scheduler pass boundary");
        for(int n=0;n<ops.size()&&offsets.get(n)<cut;n++){
            var i=ops.get(n);require(!(i instanceof ReturnInstruction)&&i.opcode()!=Opcode.JSR&&i.opcode()!=Opcode.RET,"Unqualified sibling pass escape");
            if(i instanceof BranchInstruction b)require(labels.getOrDefault(b.target(),-1)>offsets.get(n)&&labels.get(b.target())<=cut,"Pass must be forward-only, without lifetime/backward escape");
            require(!(i instanceof TableSwitchInstruction)&&!(i instanceof LookupSwitchInstruction),"Sibling switch requires separate bound qualification");
        }
        require(code.exceptionHandlers().size()==1,"Changed scheduler handler set");
        var h=code.exceptionHandlers().getFirst();require(h.catchType().isPresent()&&h.catchType().get().asInternalName().equals("java/lang/InterruptedException")&&
            Objects.equals(labels.get(h.tryStart()),cut)&&Objects.equals(labels.get(h.tryEnd()),loop)&&labels.getOrDefault(h.handler(),-1)>loop,"Native interrupt handler must cover sleep only");
        return new Selection(cut,sleep,loop,delay);
    }
    public static byte[] prepare(ClassFile cf,ClassModel source,Selection s){
        require(select(source).equals(s),"Sibling pass inputs changed");var nativeRun=run(source);var owner=source.thisClass().asSymbol();
        byte[] split=cf.transformClass(source,(b,e)->{
            if(e instanceof MethodModel m&&m.methodName().equalsString("run")&&m.methodType().equalsString("()V"))b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                int at;boolean call;
                public void accept(CodeBuilder cb,CodeElement ce){
                    if(ce instanceof Instruction i){if(at<s.boundary()){
                        if(!call){cb.aload(0).invokespecial(owner,CYCLE,VOID);call=true;}
                        at+=i.sizeInBytes();return;
                    }at+=i.sizeInBytes();cb.with(ce);return;}
                    if(ce instanceof LabelTarget){if(at==0||at>=s.boundary())cb.with(ce);return;}
                    if(ce instanceof ExceptionCatch)cb.with(ce);
                    // Debug local/line ranges are not control-flow evidence and
                    // cannot retain labels removed from the original body.
                }
            }));else b.with(e);
        });
        split=cf.transformClass(cf.parse(split),ClassTransform.endHandler(b->b.withMethodBody(CYCLE,VOID,
            ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC,cb->{
                int at=0;for(var ce:nativeRun.code().get()){
                    if(ce instanceof Instruction i){if(at>=s.boundary())break;cb.with(ce);at+=i.sizeInBytes();}
                    else if(ce instanceof LabelTarget&&at<=s.boundary())cb.with(ce);
                }cb.return_();
            })));
        var unguarded=cf.parse(split);var helper=unguarded.methods().stream().filter(m->m.methodName().equalsString(CYCLE)).findFirst().orElseThrow();
        require(IosExclusionPrepare.localProblem(helper).isEmpty(),"Extracted pass is not bounded");
        byte[] result=IosExclusionPrepare.transform(cf,unguarded,List.of(new IosExclusionPrepare.Scope(CYCLE,"()V",IosExclusion.ACTIVITY)));
        require(cf.verify(result).isEmpty(),"Sibling cycle verifier failed");return result;
    }
    public static void verifyPrepared(ClassFile cf,ClassModel before,Selection selection,ClassModel after){
        var reference=cf.parse(prepare(cf,before,selection));require(reference.methods().size()==after.methods().size(),"Changed sibling member count");
        require(before.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList().equals(
            after.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList()),"Changed sibling storage");
        for(var m:reference.methods()){
            var actual=after.methods().stream().filter(x->x.methodName().equals(m.methodName())&&x.methodType().equals(m.methodType())).findFirst().orElseThrow();
            require(IosMethodIdentity.of(m,i->false).equals(IosMethodIdentity.of(actual,i->false)),"Changed sibling body, sleep, return or exception identity "+m.methodName());
        }
    }
    private IosSiblingCyclePrepare(){}
}
