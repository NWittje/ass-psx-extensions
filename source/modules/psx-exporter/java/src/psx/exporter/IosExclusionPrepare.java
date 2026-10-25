package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Candidate scope weaving; not called by ordinary exporter preparation.
 * Local bytecode qualification does not establish outer monitor/array/writer coverage. */
public final class IosExclusionPrepare {
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.IosExclusion");
    private static final MethodTypeDesc ENTER=MethodTypeDesc.of(CD_boolean,CD_int),
        LEAVE=MethodTypeDesc.of(CD_void,CD_boolean,CD_int);
    public record Scope(String name,String descriptor,int kind,String beforeEnter){
        public Scope(String name,String descriptor,int kind){this(name,descriptor,kind,null);}
        public Scope{
            if(name==null||descriptor==null||name.startsWith("<")||kind<0||kind>1)
                throw new IllegalArgumentException("Invalid bounded IOS scope");
            MethodTypeDesc.ofDescriptor(descriptor);
            if(beforeEnter!=null&&(kind!=IosExclusion.ACTIVITY||!beforeEnter.equals(IosNativeHooksPrepare.POLL)))
                throw new IllegalArgumentException("Only qualified owner poll may precede activity entry");
        }
    }
    public static String localProblem(MethodModel method){
        if(method.methodName().stringValue().startsWith("<"))return "initialization is not a bounded writer scope";
        if((method.flags().flagsMask()&(ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))!=0||method.code().isEmpty())
            return "implicit monitor or unavailable method body";
        var labels=new HashMap<Label,Integer>();var sites=new ArrayList<Instruction>();
        for(var element:method.code().orElseThrow()){
            if(element instanceof LabelTarget l)labels.put(l.label(),sites.size());
            if(element instanceof Instruction i)sites.add(i);
        }
        for(int n=0;n<sites.size();n++){
            var i=sites.get(n);
            if(i.opcode()==Opcode.JSR||i.opcode()==Opcode.JSR_W||i.opcode()==Opcode.RET||i.opcode()==Opcode.RET_W)return "legacy subroutine needs separate qualification";
            var targets=new ArrayList<Label>();
            if(i instanceof BranchInstruction b)targets.add(b.target());
            if(i instanceof TableSwitchInstruction b){targets.add(b.defaultTarget());for(var c:b.cases())targets.add(c.target());}
            if(i instanceof LookupSwitchInstruction b){targets.add(b.defaultTarget());for(var c:b.cases())targets.add(c.target());}
            for(Label target:targets)if(!labels.containsKey(target)||labels.get(target)<=n)return "backward scope needs bounded per-iteration qualification";
        }
        return "";
    }
    public static byte[] transform(ClassFile parser,ClassModel input,List<Scope> scopes){
        var selected=new HashMap<String,Scope>();
        for(var scope:scopes){
            String key=scope.name()+scope.descriptor();
            if(selected.put(key,scope)!=null)throw new IllegalArgumentException("Duplicate IOS scope");
            var method=input.methods().stream().filter(m->m.methodName().equalsString(scope.name())&&m.methodType().equalsString(scope.descriptor())).findFirst()
                .orElseThrow(()->new IllegalArgumentException("Missing IOS scope "+key));
            String problem=localProblem(method);if(!problem.isEmpty())throw new IllegalArgumentException(key+": "+problem);
            if(scope.beforeEnter()!=null){
                var first=method.code().orElseThrow().elementList().stream().filter(e->e instanceof Instruction).findFirst().orElseThrow();
                if(!(first instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKESTATIC&&c.owner().equals(input.thisClass())&&
                    c.name().equalsString(scope.beforeEnter())&&c.type().equalsString("()V")))
                    throw new IllegalArgumentException("Missing leading owner poll before activity scope");
            }
            for(var element:method.code().orElseThrow())if(element instanceof InvokeInstruction call&&call.owner().asInternalName().equals("psx/exporter/IosExclusion")&&
                    (call.name().equalsString("enter")||call.name().equalsString("leave")))
                throw new IllegalArgumentException("Already woven IOS scope");
        }
        byte[] bytes=parser.transformClass(input,(builder,element)->{
            if(element instanceof MethodModel method){
                Scope scope=selected.get(method.methodName().stringValue()+method.methodType().stringValue());
                if(scope!=null){builder.transformMethod(method,(mb,me)->{
                    if(me instanceof CodeModel code)mb.transformCode(code,guard(scope,input.thisClass().asSymbol()));else mb.with(me);
                });return;}
            }
            builder.with(element);
        });
        var errors=parser.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("IOS scope verification: "+errors.getFirst());
        return bytes;
    }
    public static void verifyPrepared(ClassFile parser,ClassModel before,List<Scope> scopes,ClassModel after){
        var reference=parser.parse(transform(parser,before,scopes));
        if(!before.thisClass().equals(after.thisClass())||reference.methods().size()!=after.methods().size()||
            !reference.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList().equals(
                after.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList()))
            throw new IllegalArgumentException("Changed bounded-scope class structure");
        for(var m:reference.methods()){
            var actual=after.methods().stream().filter(x->x.methodName().equals(m.methodName())&&x.methodType().equals(m.methodType())).findFirst().orElseThrow();
            if(!IosMethodIdentity.of(m,i->false).equals(IosMethodIdentity.of(actual,i->false)))
                throw new IllegalArgumentException("Changed bounded scope body/return/exception cleanup "+m.methodName());
        }
    }
    private static CodeTransform guard(Scope scope,ClassDesc owner){return new CodeTransform(){
        final int kind=scope.kind();
        boolean leading=true;
        Label begin,end,handler;int entered,originalException;
        final List<ExceptionCatch> nativeHandlers=new ArrayList<>();
        private void poison(CodeBuilder b){
            // Even a missing optional failure latch cannot replace the original body.
            var start=b.newLabel();var end=b.newLabel();var fail=b.newLabel();var done=b.newLabel();
            b.labelBinding(start).iconst_1().putstatic(ClassDesc.of("psx.exporter.IosScopeFaults"),"failed",CD_boolean)
                .labelBinding(end).goto_(done).labelBinding(fail).pop().labelBinding(done);
            b.exceptionCatchAll(start,end,fail);
        }
        private void leave(CodeBuilder b){
            var start=b.newLabel();var end=b.newLabel();var fail=b.newLabel();var done=b.newLabel();
            b.labelBinding(start).iload(entered).loadConstant(kind).invokestatic(HELPER,"leave",LEAVE)
                .labelBinding(end).goto_(done).labelBinding(fail).pop();poison(b);b.labelBinding(done);
            b.exceptionCatchAll(start,end,fail);
        }
        public void atStart(CodeBuilder b){
            if(scope.beforeEnter()!=null)b.invokestatic(owner,scope.beforeEnter(),MethodTypeDesc.of(CD_void));
            entered=b.allocateLocal(TypeKind.INT);originalException=b.allocateLocal(TypeKind.REFERENCE);
            begin=b.newLabel();end=b.newLabel();handler=b.newLabel();
            var start=b.newLabel();var finish=b.newLabel();var fail=b.newLabel();var done=b.newLabel();
            b.iconst_0().istore(entered).labelBinding(start).loadConstant(kind).invokestatic(HELPER,"enter",ENTER)
                .istore(entered).labelBinding(finish).goto_(done).labelBinding(fail).pop();poison(b);
            b.labelBinding(done).labelBinding(begin);b.exceptionCatchAll(start,finish,fail);
        }
        public void accept(CodeBuilder b,CodeElement e){
            if(e instanceof Instruction&&leading){leading=false;if(scope.beforeEnter()!=null)return;}
            // Optional-call catches must precede overlapping native catch ranges;
            // preserve native precedence among themselves, after the narrow guards.
            if(e instanceof ExceptionCatch h){nativeHandlers.add(h);return;}
            if(e instanceof ReturnInstruction r){
                // Spill the native result before optional cleanup: a linkage handler
                // clears the operand stack and must not lose a wide/reference return.
                int result=-1;if(r.typeKind()!=TypeKind.VOID){result=b.allocateLocal(r.typeKind());b.storeLocal(r.typeKind(),result);}
                leave(b);if(result>=0)b.loadLocal(r.typeKind(),result);
            }
            b.with(e);
        }
        public void atEnd(CodeBuilder b){
            b.labelBinding(end).labelBinding(handler).astore(originalException);leave(b);b.aload(originalException).athrow();
            for(var nativeHandler:nativeHandlers)b.with(nativeHandler);
            // Appended after original handlers: native catches run first; only escaping
            // exceptions reach this cleanup. leave is failure-isolated and never throws.
            b.exceptionCatchAll(begin,end,handler);
        }
    };}
}
