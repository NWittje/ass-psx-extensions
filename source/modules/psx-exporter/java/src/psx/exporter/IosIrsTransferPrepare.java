package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound forced-write branch of native IRU transfer, without allocating its
 * return container or taking a foreign monitor. Complete exclusion/receiver
 * qualification is mandatory before use; this is not an IRS relocation service. */
public final class IosIrsTransferPrepare {
    public static final String TRANSFER="psxExporterIosIrsForcedTransfer";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void,CD_double,CD_double);
    public record Layout(ClassDesc unit,ClassDesc aerodynamics,ClassDesc result){}
    public record Selection(Layout layout,List<Integer> nativeForcedWrites){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean external){
        var candidates=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(candidates.size()==1,"Missing/duplicate IRU field "+name);var f=candidates.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&ClassFile.ACC_FINAL)==0&&(!external||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible IRU field "+name);
    }
    private static List<String> identity(MethodModel m){
        var code=m.code().orElseThrow();require(code.exceptionHandlers().isEmpty(),"IRU transfer has exceptional effects");
        var labels=new HashMap<Label,Integer>();int index=0;for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),index);if(e instanceof Instruction)index++;}
        var result=new ArrayList<String>();for(var e:code)if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)result.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof BranchInstruction b)result.add(i.opcode()+":"+labels.get(b.target()));
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else if(i instanceof NewObjectInstruction n)result.add("new:"+n.className().asInternalName());
            else if(i instanceof InvokeInstruction call)result.add(i.opcode()+":"+call.owner().asInternalName()+":"+call.name()+":"+call.type());
            else result.add(i.opcode().name());
        }return List.copyOf(result);
    }
    /** Separately constructed native branch reference; no vendor source emitted. */
    public static byte[] reference(ClassFile cf,Layout l){
        return cf.build(l.unit,b->b.withMethodBody("trans",MethodTypeDesc.of(l.result,CD_boolean,CD_double,CD_double),ClassFile.ACC_SYNCHRONIZED,c->{
            var write=c.newLabel();var unchanged=c.newLabel();var dirty=c.newLabel();var result=c.newLabel();
            c.iload(1).ifne(write).aload(0).getfield(l.unit,"hasChanged",CD_boolean).ifne(dirty).labelBinding(write)
                .aload(0).dload(2).putfield(l.unit,"iruBrg",CD_double)
                .aload(0).dload(4).putfield(l.unit,"iruDist",CD_double)
                .dload(4).dconst_0().dcmpl().ifne(unchanged)
                .aload(0).getstatic(l.aerodynamics,"realLat",CD_double).putfield(l.unit,"lat",CD_double)
                .aload(0).getstatic(l.aerodynamics,"realLon",CD_double).putfield(l.unit,"lon",CD_double).labelBinding(unchanged)
                .aload(0).iload(1).putfield(l.unit,"hasChanged",CD_boolean).goto_(result).labelBinding(dirty)
                .aload(0).getfield(l.unit,"hasChanged",CD_boolean).ifeq(result)
                .aload(0).iconst_0().putfield(l.unit,"hasChanged",CD_boolean).labelBinding(result)
                .new_(l.result).dup().aload(0).getfield(l.unit,"iruBrg",CD_double).aload(0).getfield(l.unit,"iruDist",CD_double)
                .invokespecial(l.result,"<init>",MethodTypeDesc.of(CD_void,CD_double,CD_double)).areturn();
        }));
    }
    public static Selection select(ClassModel unit,ClassModel aero,ClassDesc result){
        var l=new Layout(unit.thisClass().asSymbol(),aero.thisClass().asSymbol(),result);
        require(pkg(l.unit).equals(pkg(l.aerodynamics))&&pkg(l.unit).equals(pkg(l.result)),"Changed IRU dependency package");
        for(var m:unit.methods())require(!m.methodName().equalsString(TRANSFER),"Existing IOS IRU transfer binding");
        for(String name:List.of("iruBrg","iruDist","lat","lon"))field(unit,name,CD_double,false,false);field(unit,"hasChanged",CD_boolean,false,false);
        for(String name:List.of("realLat","realLon"))field(aero,name,CD_double,true,true);
        var methods=unit.methods().stream().filter(m->m.methodName().equalsString("trans")).toList();require(methods.size()==1,"Missing/overloaded IRU transfer");var m=methods.getFirst();
        require(m.methodTypeSymbol().equals(MethodTypeDesc.of(result,CD_boolean,CD_double,CD_double))&&m.code().isPresent()&&
            (m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==ClassFile.ACC_SYNCHRONIZED,"Changed IRU native transfer context");
        require(identity(m).equals(identity(ClassFile.of().parse(reference(ClassFile.of(),l)).methods().getFirst())),"Changed complete native IRU transfer effects/branch/order/return");
        var writes=new ArrayList<Integer>();int offset=0;for(var e:m.code().orElseThrow())if(e instanceof Instruction i){if(i.opcode()==Opcode.PUTFIELD&&writes.size()<5)writes.add(offset);offset+=i.sizeInBytes();}
        return new Selection(l,List.copyOf(writes));
    }
    public static byte[] prepare(ClassFile cf,ClassModel unit,ClassModel aero,Selection s){
        require(select(unit,aero,s.layout.result).equals(s),"IRU preparation differs from qualification");var l=s.layout;
        return cf.transformClass(unit,ClassTransform.endHandler(b->b.withMethodBody(TRANSFER,VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->{
            var unchanged=c.newLabel();
            c.aload(0).dload(1).putfield(l.unit,"iruBrg",CD_double).aload(0).dload(3).putfield(l.unit,"iruDist",CD_double)
                .dload(3).dconst_0().dcmpl().ifne(unchanged)
                .aload(0).getstatic(l.aerodynamics,"realLat",CD_double).putfield(l.unit,"lat",CD_double)
                .aload(0).getstatic(l.aerodynamics,"realLon",CD_double).putfield(l.unit,"lon",CD_double).labelBinding(unchanged)
                .aload(0).iconst_1().putfield(l.unit,"hasChanged",CD_boolean).return_();
        })));
    }
    private IosIrsTransferPrepare(){}
}
