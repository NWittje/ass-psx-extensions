package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound native timer/cache subset. The actual Qi265 value must already be
 * independently proved zero under complete exclusion; this does not set it,
 * consume its flags, publish a value or qualify a complete FMC relocation. */
public final class IosFmcBookkeepingPrepare {
    public static final String RESET="psxExporterIosResetFmcBookkeeping";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    public record Layout(ClassDesc global,ClassDesc position,ClassDesc glob,ClassDesc qi){}
    public record Selection(Layout layout,int timerWrite,int cacheWrite){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static MethodModel method(ClassModel c,String name,MethodTypeDesc type){
        var all=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/overloaded FMC bookkeeping method "+name);
        var m=all.getFirst();require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&
            (m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,
            "Changed FMC bookkeeping context "+name);return m;
    }
    private static void field(ClassModel c,String name,ClassDesc type){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing FMC bookkeeping field "+name);
        var f=all.getFirst();require(f.fieldTypeSymbol().equals(type)&&(f.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,"Changed FMC bookkeeping storage "+name);
    }
    private static List<Instruction> instructions(CodeModel code){var result=new ArrayList<Instruction>();for(var e:code)if(e instanceof Instruction i)result.add(i);return result;}
    private static Map<Label,Integer> labels(CodeModel code){var map=new HashMap<Label,Integer>();int n=0;for(var e:code){if(e instanceof LabelTarget t)map.put(t.label(),n);if(e instanceof Instruction)n++;}return map;}
    private static List<String> identity(CodeModel code,int start,int end,boolean handlers){
        var map=labels(code);var list=instructions(code);var result=new ArrayList<String>();
        for(int n=start;n<end;n++){var i=list.get(n);
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)result.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof StoreInstruction s)result.add("store:"+s.typeKind()+":"+s.slot());
            else if(i instanceof BranchInstruction b)result.add(i.opcode()+":"+(map.get(b.target())-start));
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else if(i instanceof InvokeInstruction call)result.add(i.opcode()+":"+call.owner().asInternalName()+":"+call.name()+":"+call.type());
            else result.add(i.opcode().name());
        }
        if(handlers)for(var h:code.exceptionHandlers())result.add("handler:"+map.get(h.tryStart())+":"+map.get(h.tryEnd())+":"+map.get(h.handler())+":"+h.catchType().map(x->x.asInternalName()).orElse("all"));
        return List.copyOf(result);
    }
    /** Independent authored reference for complete reset monitor/finally order. */
    public static byte[] resetReference(ClassFile cf,Layout l){
        return cf.build(l.global,b->b.withMethodBody("resetWashout",VOID,ClassFile.ACC_STATIC,c->{
            var leftStart=c.newLabel();var rightStart=c.newLabel();var rightEnd=c.newLabel();var rightHandler=c.newLabel();var rightCleanupEnd=c.newLabel();var rightDone=c.newLabel();
            var leftEnd=c.newLabel();var leftHandler=c.newLabel();var leftCleanupEnd=c.newLabel();var done=c.newLabel();
            c.iconst_0().putstatic(l.global,"situInitStabilizeTimer",CD_int)
                .getstatic(l.global,"posL",l.position).dup().astore(0).monitorenter().labelBinding(leftStart)
                .getstatic(l.global,"posR",l.position).dup().astore(1).monitorenter().labelBinding(rightStart)
                .getstatic(l.global,"posL",l.position).invokevirtual(l.position,"resetWashout",VOID)
                .getstatic(l.global,"posR",l.position).invokevirtual(l.position,"resetWashout",VOID)
                .iconst_1().invokestatic(l.global,"checkIrsOpsAndFmcBias",MethodTypeDesc.of(CD_void,CD_boolean))
                .aload(1).monitorexit().labelBinding(rightEnd).goto_(rightDone)
                .labelBinding(rightHandler).astore(2).aload(1).monitorexit().labelBinding(rightCleanupEnd).aload(2).athrow()
                .labelBinding(rightDone).aload(0).monitorexit().labelBinding(leftEnd).goto_(done)
                .labelBinding(leftHandler).astore(3).aload(0).monitorexit().labelBinding(leftCleanupEnd).aload(3).athrow().labelBinding(done).return_()
                .exceptionCatchAll(rightStart,rightEnd,rightHandler).exceptionCatchAll(rightHandler,rightCleanupEnd,rightHandler)
                .exceptionCatchAll(leftStart,leftEnd,leftHandler).exceptionCatchAll(leftHandler,leftCleanupEnd,leftHandler);
        }));
    }
    public static byte[] cacheReference(ClassFile cf,Layout l){
        return cf.build(l.global,b->b.withMethodBody("consume",VOID,ClassFile.ACC_STATIC,c->{
            var done=c.newLabel();
            c.getstatic(l.glob,"qi",l.qi.arrayType()).getstatic(l.glob,"qiFmcPosBiasMode",CD_int).aaload().invokevirtual(l.qi,"isUpdated",MethodTypeDesc.of(CD_boolean)).ifeq(done)
                .getstatic(l.glob,"qi",l.qi.arrayType()).getstatic(l.glob,"qiFmcPosBiasMode",CD_int).aaload()
                .iconst_0().bipush(114).iconst_0().invokevirtual(l.qi,"transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int))
                .putstatic(l.global,"fmcPosBiasMode",CD_int).labelBinding(done).return_();
        }));
    }
    public static Selection select(ClassModel global,ClassModel position,ClassModel aero,ClassModel glob,ClassModel qi){
        // Complete position reset must qualify first, not merely its four fields.
        IosFmcPositionPrepare.select(position,aero,glob,qi);
        var l=new Layout(global.thisClass().asSymbol(),position.thisClass().asSymbol(),glob.thisClass().asSymbol(),qi.thisClass().asSymbol());
        require(pkg(l.global).equals(pkg(l.position)),"Changed global FMC package");
        for(var m:global.methods())require(!m.methodName().equalsString(RESET),"Existing FMC bookkeeping binding");
        field(global,"situInitStabilizeTimer",CD_int);field(global,"fmcPosBiasMode",CD_int);field(global,"posL",l.position);field(global,"posR",l.position);
        method(global,"checkIrsOpsAndFmcBias",MethodTypeDesc.of(CD_void,CD_boolean));
        var reset=method(global,"resetWashout",VOID).code().orElseThrow();var reference=ClassFile.of().parse(resetReference(ClassFile.of(),l)).methods().getFirst().code().orElseThrow();
        require(identity(reset,0,instructions(reset).size(),true).equals(identity(reference,0,instructions(reference).size(),true)),"Changed complete native FMC reset/monitor/exception ordering");
        var lento=method(global,"runLento",VOID).code().orElseThrow();require(lento.exceptionHandlers().isEmpty(),"Changed FMC cache outer exceptional context");var all=instructions(lento);int call=-1;
        for(int n=0;n<all.size();n++)if(all.get(n) instanceof InvokeInstruction i&&i.owner().asSymbol().equals(l.qi)&&i.name().equalsString("isUpdated")){
            require(call<0,"Ambiguous FMC register cache consumption");call=n;
        }
        require(call>=3&&call+10<=all.size(),"Missing FMC cache consumption");
        var cache=ClassFile.of().parse(cacheReference(ClassFile.of(),l)).methods().getFirst().code().orElseThrow();
        require(identity(lento,call-3,call+10,false).equals(identity(cache,0,13,false)),"Changed FMC cache source/descriptor/flags/order/branch");
        // No branch/handler can enter the middle of the selected consumption window.
        var targets=labels(lento);for(int n=0;n<all.size();n++)if(n<call-3||n>=call+10){
            var i=all.get(n);if(i instanceof BranchInstruction branch){int t=targets.get(branch.target());require(t<=call-3||t>=call+10,"Branch enters FMC cache consumption mid-sequence");}
            if(i instanceof TableSwitchInstruction||i instanceof LookupSwitchInstruction)throw new IllegalArgumentException("FMC cache switch entry needs explicit qualification");
        }
        int offset=0,write=-1;for(var i:all){if(i instanceof FieldInstruction f&&i.opcode()==Opcode.PUTSTATIC&&f.owner().asSymbol().equals(l.global)&&f.name().equalsString("fmcPosBiasMode")){require(write<0,"Multiple cached-mode writes");write=offset;}offset+=i.sizeInBytes();}
        return new Selection(l,1,write);
    }
    public static byte[] prepare(ClassFile cf,ClassModel global,ClassModel position,ClassModel aero,ClassModel glob,ClassModel qi,Selection s){
        require(select(global,position,aero,glob,qi).equals(s),"FMC bookkeeping preparation differs from qualification");
        return cf.transformClass(global,ClassTransform.endHandler(b->b.withMethodBody(RESET,VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->
            c.iconst_0().putstatic(s.layout.global,"situInitStabilizeTimer",CD_int).iconst_0().putstatic(s.layout.global,"fmcPosBiasMode",CD_int).return_())));
    }
    private IosFmcBookkeepingPrepare(){}
}
