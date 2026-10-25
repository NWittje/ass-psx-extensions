package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound Qi265-only physical write plus immediate native flag consumption.
 * Requires complete Qi/Q reader-writer and outer lock exclusion. Return1 records
 * a changed value for a future current-state publisher, never a saved-value replay.
 * This helper does not publish or by itself prove completed FMC application. */
public final class IosFmcModePrepare {
    public static final String RESET="psxExporterIosResetFmcMode";
    public record Layout(ClassDesc qi,ClassDesc q,ClassDesc glob,ClassDesc network){}
    public record Selection(Layout layout,List<Integer> transferWrites){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean external){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing/duplicate FMC mode field "+name);var f=all.getFirst();
        require(f.fieldTypeSymbol().equals(type)&&((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==statik&&(f.flags().flagsMask()&ClassFile.ACC_FINAL)==0&&(!external||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible FMC mode field "+name);
    }
    private static MethodModel method(ClassModel c,String name,MethodTypeDesc type,boolean statik){
        var all=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/overloaded FMC mode method "+name);var m=all.getFirst();
        require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_ABSTRACT|ClassFile.ACC_NATIVE|ClassFile.ACC_PRIVATE))==(statik?ClassFile.ACC_STATIC:0),"Changed/inaccessible FMC mode method "+name);return m;
    }
    private static List<String> identity(CodeModel code){
        var map=new HashMap<Label,Integer>();int count=0;for(var e:code){if(e instanceof LabelTarget t)map.put(t.label(),count);if(e instanceof Instruction)count++;}
        var result=new ArrayList<String>();for(var e:code)if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)result.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof StoreInstruction s)result.add("store:"+s.typeKind()+":"+s.slot());
            else if(i instanceof BranchInstruction b)result.add(i.opcode()+":"+map.get(b.target()));
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else if(i instanceof InvokeInstruction call)result.add(i.opcode()+":"+call.owner().asInternalName()+":"+call.name()+":"+call.type());
            else result.add(i.opcode().name());
        }
        for(var h:code.exceptionHandlers())result.add("handler:"+map.get(h.tryStart())+":"+map.get(h.tryEnd())+":"+map.get(h.handler())+":"+h.catchType().map(x->x.asInternalName()).orElse("all"));return List.copyOf(result);
    }
    /** Independently authored reference for complete transfer branch/monitor/exit semantics. */
    public static byte[] transferReference(ClassFile cf,Layout l){
        return cf.build(l.qi,b->b.withMethodBody("transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int),0,c->{
            var start=c.newLabel();var notFresh=c.newLabel();var freshGate=c.newLabel();var write=c.newLabel();var upper=c.newLabel();var clamped=c.newLabel();var publish=c.newLabel();var updated=c.newLabel();var read=c.newLabel();var readValue=c.newLabel();var readReturn=c.newLabel();var result=c.newLabel();var writeReturn=c.newLabel();var handler=c.newLabel();var cleanupEnd=c.newLabel();
            c.aload(0).dup().astore(4).monitorenter().labelBinding(start)
                .iload(3).ifeq(notFresh).aload(0).iconst_1().putfield(l.qi,"fresh",CD_boolean).labelBinding(notFresh)
                .iload(2).bipush(119).if_icmpne(read)
                .iload(3).ifne(freshGate).aload(0).getfield(l.qi,"fresh",CD_boolean).ifeq(write)
                .labelBinding(freshGate).iload(3).ifeq(result).labelBinding(write)
                .iload(1).aload(0).getfield(l.qi,"min",CD_int).if_icmpge(upper)
                .aload(0).getfield(l.qi,"min",CD_int).istore(1).goto_(clamped).labelBinding(upper)
                .iload(1).aload(0).getfield(l.qi,"max",CD_int).if_icmple(clamped).aload(0).getfield(l.qi,"max",CD_int).istore(1)
                .labelBinding(clamped).aload(0).getfield(l.qi,"iVar",CD_int).iload(1).if_icmpeq(result)
                .aload(0).iload(1).putfield(l.qi,"iVar",CD_int)
                .iload(3).iconst_1().if_icmpeq(publish).iload(3).ifge(updated).labelBinding(publish);
            publication(c,l);
            c.labelBinding(updated).aload(0).iconst_1().putfield(l.qi,"updated",CD_boolean).goto_(result)
                .labelBinding(read).aload(0).iconst_0().putfield(l.qi,"fresh",CD_boolean).iload(3).iconst_1().if_icmpne(readValue);
            publication(c,l);
            c.labelBinding(readValue).aload(0).getfield(l.qi,"iVar",CD_int).aload(4).monitorexit().labelBinding(readReturn).ireturn()
                .labelBinding(result).iload(1).aload(4).monitorexit().labelBinding(writeReturn).ireturn()
                .labelBinding(handler).astore(5).aload(4).monitorexit().labelBinding(cleanupEnd).aload(5).athrow()
                .exceptionCatchAll(start,readReturn,handler).exceptionCatchAll(result,writeReturn,handler).exceptionCatchAll(handler,cleanupEnd,handler);
        }));
    }
    private static void publication(CodeBuilder c,Layout l){c.iload(3).bipush(105).aload(0).getfield(l.qi,"serialNr",CD_int).aconst_null().aload(0).getfield(l.qi,"iVar",CD_int).invokestatic(l.network,"toPrint",MethodTypeDesc.of(CD_void,CD_int,CD_char,CD_int,CD_String,CD_int));}
    private static byte[] updateReference(ClassFile cf,ClassDesc q){return cf.build(q,b->b.withMethodBody("isUpdated",MethodTypeDesc.of(CD_boolean),0,c->{var no=c.newLabel();c.aload(0).getfield(q,"updated",CD_boolean).ifeq(no).aload(0).iconst_0().putfield(q,"updated",CD_boolean).iconst_1().ireturn().labelBinding(no).iconst_0().ireturn();}));}
    public static Selection select(ClassModel qi,ClassModel q,ClassModel glob,ClassModel network){
        var l=new Layout(qi.thisClass().asSymbol(),q.thisClass().asSymbol(),glob.thisClass().asSymbol(),network.thisClass().asSymbol());
        for(var type:List.of(l.q,l.glob,l.network))require(pkg(type).equals(pkg(l.qi)),"Changed FMC mode dependency package");
        require(qi.superclass().orElseThrow().asSymbol().equals(l.q),"Changed register superclass");
        for(var m:qi.methods())require(!m.methodName().equalsString(RESET),"Existing FMC mode binding");
        for(String name:List.of("fresh","updated","serialNr"))require(qi.fields().stream().noneMatch(f->f.fieldName().equalsString(name)),"Shadowed register storage "+name);
        field(qi,"iVar",CD_int,false,false);field(qi,"min",CD_int,false,false);field(qi,"max",CD_int,false,false);
        field(q,"fresh",CD_boolean,false,true);field(q,"updated",CD_boolean,false,true);field(q,"serialNr",CD_int,false,true);
        field(glob,"qi",l.qi.arrayType(),true,true);field(glob,"qiFmcPosBiasMode",CD_int,true,true);
        var transfer=method(qi,"transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int),false);
        require((transfer.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)==0,"Changed transfer implicit monitor");
        var reference=ClassFile.of().parse(transferReference(ClassFile.of(),l)).methods().getFirst();
        require(identity(transfer.code().orElseThrow()).equals(identity(reference.code().orElseThrow())),"Changed complete Qi transfer/clamp/flags/publication/exit ordering");
        var query=method(q,"isUpdated",MethodTypeDesc.of(CD_boolean),false);require((query.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)==0,"Changed flag-consumption monitor");
        require(identity(query.code().orElseThrow()).equals(identity(ClassFile.of().parse(updateReference(ClassFile.of(),l.q)).methods().getFirst().code().orElseThrow())),"Changed complete Q update consumption");
        var print=method(network,"toPrint",MethodTypeDesc.of(CD_void,CD_int,CD_char,CD_int,CD_String,CD_int),true);
        require((print.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)!=0,"Changed native publication lock contract");
        var writes=new ArrayList<Integer>();int offset=0;for(var e:transfer.code().orElseThrow())if(e instanceof Instruction i){if(i.opcode()==Opcode.PUTFIELD)writes.add(offset);offset+=i.sizeInBytes();}
        return new Selection(l,List.copyOf(writes));
    }
    public static byte[] prepare(ClassFile cf,ClassModel qi,ClassModel q,ClassModel glob,ClassModel network,Selection s){
        require(select(qi,q,glob,network).equals(s),"FMC mode preparation differs from qualification");var l=s.layout;
        return cf.transformClass(qi,ClassTransform.endHandler(b->b.withMethodBody(RESET,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->{
            var invalid=c.newLabel();var unchanged=c.newLabel();var consume=c.newLabel();
            c.aload(0).getfield(l.qi,"serialNr",CD_int).sipush(265).if_icmpne(invalid)
                .aload(0).getfield(l.qi,"min",CD_int).ifne(invalid).aload(0).getfield(l.qi,"max",CD_int).iconst_1().if_icmpne(invalid)
                .aload(0).getfield(l.qi,"iVar",CD_int).iflt(invalid).aload(0).getfield(l.qi,"iVar",CD_int).iconst_1().if_icmpgt(invalid)
                .getstatic(l.glob,"qi",l.qi.arrayType()).ifnull(invalid).getstatic(l.glob,"qiFmcPosBiasMode",CD_int).iflt(invalid)
                .getstatic(l.glob,"qiFmcPosBiasMode",CD_int).getstatic(l.glob,"qi",l.qi.arrayType()).arraylength().if_icmpge(invalid)
                .getstatic(l.glob,"qi",l.qi.arrayType()).getstatic(l.glob,"qiFmcPosBiasMode",CD_int).aaload().aload(0).if_acmpne(invalid)
                .aload(0).getfield(l.qi,"iVar",CD_int).ifeq(unchanged)
                .aload(0).iconst_0().putfield(l.qi,"iVar",CD_int).iconst_1().istore(1).goto_(consume)
                .labelBinding(unchanged).iconst_0().istore(1).labelBinding(consume)
                // Equivalent final flag state of write(0,'w',1), isUpdated(), read(0,'r',0).
                .aload(0).iconst_0().putfield(l.qi,"updated",CD_boolean).aload(0).iconst_0().putfield(l.qi,"fresh",CD_boolean).iload(1).ireturn()
                .labelBinding(invalid).iconst_m1().ireturn();
        })));
    }
    private IosFmcModePrepare(){}
}
