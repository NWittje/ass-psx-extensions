package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound physical subset of the selected native FMC navigation reset.
 * Both receiver identities and complete reader/writer exclusion are prerequisites.
 * Register mode, cached mode, timer and publication are separate obligations. */
public final class IosFmcPositionPrepare {
    public static final String RESET="psxExporterIosResetFmcPosition";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    public record Layout(ClassDesc position,ClassDesc aero,ClassDesc glob,ClassDesc qi){}
    public record Selection(Layout layout,List<Integer> writes,int publication){}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean external){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing/duplicate FMC field "+name);
        var f=all.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&ClassFile.ACC_FINAL)==0&&
            (!external||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible FMC field "+name);
    }
    private static List<String> identity(MethodModel m){
        var code=m.code().orElseThrow();require(code.exceptionHandlers().isEmpty(),"Changed FMC reset exceptional effects");
        var result=new ArrayList<String>();
        for(var e:code)if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)result.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else if(i instanceof InvokeInstruction call)result.add(i.opcode()+":"+call.owner().asInternalName()+":"+call.name()+":"+call.type());
            else {require(!(i instanceof BranchInstruction),"Unexpected FMC reset branch");result.add(i.opcode().name());}
        }return List.copyOf(result);
    }
    /** Authored semantic reference, not a recovered source file. */
    public static byte[] reference(ClassFile cf,Layout l){
        return cf.build(l.position,b->b.withMethodBody("resetWashout",VOID,0,c->{
            c.getstatic(l.glob,"qi",l.qi.arrayType()).getstatic(l.glob,"qiFmcPosBiasMode",CD_int).aaload()
                .iconst_0().bipush(119).iconst_1().invokevirtual(l.qi,"transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int)).pop();
            physical(c,l);
        }));
    }
    private static void physical(CodeBuilder c,Layout l){
        c.aload(0).getstatic(l.aero,"realLat",CD_double).putfield(l.position,"lat",CD_double)
            .aload(0).getstatic(l.aero,"realLon",CD_double).putfield(l.position,"lon",CD_double)
            .aload(0).dconst_0().putfield(l.position,"distForNet",CD_double)
            .aload(0).dconst_0().putfield(l.position,"biasDist",CD_double).return_();
    }
    public static Selection select(ClassModel position,ClassModel aero,ClassModel glob,ClassModel qi){
        var l=new Layout(position.thisClass().asSymbol(),aero.thisClass().asSymbol(),glob.thisClass().asSymbol(),qi.thisClass().asSymbol());
        for(var type:List.of(l.aero,l.glob,l.qi))require(pkg(type).equals(pkg(l.position)),"Changed FMC dependency package");
        for(var c:List.of(aero,glob,qi))require((c.flags().flagsMask()&ClassFile.ACC_INTERFACE)==0,"FMC dependency changed to interface");
        for(var m:position.methods())require(!m.methodName().equalsString(RESET),"Existing FMC reset binding");
        for(String name:List.of("lat","lon","distForNet","biasDist"))field(position,name,CD_double,false,false);
        for(String name:List.of("realLat","realLon"))field(aero,name,CD_double,true,true);
        field(glob,"qi",l.qi.arrayType(),true,true);field(glob,"qiFmcPosBiasMode",CD_int,true,true);
        var transfers=qi.methods().stream().filter(m->m.methodName().equalsString("transfer")&&m.methodTypeSymbol().equals(MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int))).toList();
        require(transfers.size()==1,"Missing/changed FMC register transfer");
        require((transfers.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_NATIVE))==0,
            "Changed/inaccessible FMC register transfer");
        var methods=position.methods().stream().filter(m->m.methodName().equalsString("resetWashout")).toList();require(methods.size()==1,"Missing/overloaded FMC reset");
        var method=methods.getFirst();require(method.methodTypeSymbol().equals(VOID)&&method.code().isPresent()&&
            (method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_ABSTRACT|ClassFile.ACC_NATIVE))==0,"Changed FMC reset descriptor/context");
        require(identity(method).equals(identity(ClassFile.of().parse(reference(ClassFile.of(),l)).methods().getFirst())),"Changed complete FMC reset effects/order");
        var writes=new ArrayList<Integer>();int offset=0,publication=-1;
        for(var e:method.code().orElseThrow())if(e instanceof Instruction i){if(i.opcode()==Opcode.PUTFIELD)writes.add(offset);if(i instanceof InvokeInstruction)publication=offset;offset+=i.sizeInBytes();}
        return new Selection(l,List.copyOf(writes),publication);
    }
    public static byte[] prepare(ClassFile cf,ClassModel position,ClassModel aero,ClassModel glob,ClassModel qi,Selection s){
        require(select(position,aero,glob,qi).equals(s),"FMC preparation differs from qualification");
        return cf.transformClass(position,ClassTransform.endHandler(b->b.withMethodBody(RESET,VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->physical(c,s.layout))));
    }
    private IosFmcPositionPrepare(){}
}
