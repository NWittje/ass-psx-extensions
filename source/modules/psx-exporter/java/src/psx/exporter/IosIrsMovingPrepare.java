package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound native near-pole IRU movement component. No global IRS publication,
 * alignment reset or FMC washout. Qualified scratch ownership, finite geometry,
 * established owner/exclusion and preinitialized dependencies are mandatory. */
public final class IosIrsMovingPrepare {
    public static final String MOVE="psxExporterIosIrsMoving";
    private static final MethodTypeDesc MOVING=MethodTypeDesc.of(CD_void,CD_double,CD_double),FOUR=MethodTypeDesc.of(CD_double,CD_double,CD_double,CD_double,CD_double);
    private static final ClassDesc MATH=ClassDesc.of("java.lang.Math");
    public record Layout(ClassDesc unit,ClassDesc aero,ClassDesc calc,ClassDesc point,ClassDesc result){}
    public record Selection(Layout layout,IosIrsTransferPrepare.Selection transfer,List<Integer> physicalWrites){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc d){String s=d.descriptorString();return s.substring(0,s.lastIndexOf('/')+1);}
    private static MethodModel method(ClassModel c,String name,MethodTypeDesc type,boolean statik){
        var found=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(found.size()==1,"Missing/overloaded IRS geometry method "+name);var m=found.getFirst();
        require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&((m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==statik&&
            (m.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_PRIVATE))==0,"Changed/inaccessible IRS geometry method "+name);
        require(m.code().get().exceptionHandlers().isEmpty(),"IRS geometry handlers changed");return m;
    }
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean foreign){
        var found=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(found.size()==1,"Missing/duplicate IRS geometry field "+name);var f=found.getFirst();
        require(f.fieldTypeSymbol().equals(type)&&((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==statik&&(f.flags().flagsMask()&ClassFile.ACC_FINAL)==0&&
            (!foreign||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible IRS geometry field "+name);
    }
    private static List<String> identity(MethodModel m){
        var code=m.code().orElseThrow();var labels=new HashMap<Label,Integer>();int index=0;
        for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),index);if(e instanceof Instruction)index++;}
        var out=new ArrayList<String>();for(var e:code)if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)out.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)out.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof InvokeInstruction c)out.add(i.opcode()+":"+c.owner().asInternalName()+":"+c.name()+":"+c.type());
            else if(i instanceof BranchInstruction b)out.add(i.opcode()+":"+labels.get(b.target()));
            else if(i instanceof ConstantInstruction c)out.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else out.add(i.opcode().name());
        }return List.copyOf(out);
    }
    private static void body(CodeBuilder c,Layout l,boolean bridge){
        var done=c.newLabel();
        c.aload(0).getfield(l.unit,"lat",CD_double).invokestatic(MATH,"abs",MethodTypeDesc.of(CD_double,CD_double))
            .loadConstant(1.560324339196086d).dcmpl().ifle(done)
            .aload(0).getfield(l.unit,"pbdM",l.point).aload(0).getfield(l.unit,"lat",CD_double)
            .aload(0).getfield(l.unit,"lon",CD_double).dload(1).dload(3).invokevirtual(l.point,"precise",MethodTypeDesc.of(CD_void,CD_double,CD_double,CD_double,CD_double))
            .aload(0).aload(0).getfield(l.unit,"pbdM",l.point).getfield(l.point,"lat",CD_double).putfield(l.unit,"lat",CD_double)
            .aload(0).aload(0).getfield(l.unit,"pbdM",l.point).getfield(l.point,"lon",CD_double).putfield(l.unit,"lon",CD_double).aload(0);
        if(!bridge)c.iconst_1();
        c.getstatic(l.aero,"realLat",CD_double).getstatic(l.aero,"realLon",CD_double)
            .aload(0).getfield(l.unit,"lat",CD_double).aload(0).getfield(l.unit,"lon",CD_double).invokestatic(l.calc,"courseTo",FOUR)
            .getstatic(l.aero,"realLat",CD_double).getstatic(l.aero,"realLon",CD_double)
            .aload(0).getfield(l.unit,"lat",CD_double).aload(0).getfield(l.unit,"lon",CD_double).invokestatic(l.calc,"orthodromicDist",FOUR);
        if(bridge)c.invokevirtual(l.unit,IosIrsTransferPrepare.TRANSFER,MOVING);
        else c.invokevirtual(l.unit,"trans",MethodTypeDesc.of(l.result,CD_boolean,CD_double,CD_double)).pop();
        c.labelBinding(done).return_();
    }
    /** Qualifies bounded native geometry effects, not its numerical accuracy. */
    private static void geometry(MethodModel m,ClassDesc point){
        var code=m.code().orElseThrow();var labels=new HashMap<Label,Integer>();int offset=0;
        for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),offset);if(e instanceof Instruction i)offset+=i.sizeInBytes();}
        offset=0;for(var e:code)if(e instanceof Instruction i){
            require(!Set.of(Opcode.NEW,Opcode.NEWARRAY,Opcode.ANEWARRAY,Opcode.MULTIANEWARRAY,Opcode.MONITORENTER,Opcode.MONITOREXIT,Opcode.INVOKEDYNAMIC,Opcode.ATHROW,Opcode.JSR,Opcode.RET,Opcode.TABLESWITCH,Opcode.LOOKUPSWITCH).contains(i.opcode()),"Unbounded/stateful geometry action "+i.opcode());
            if(i instanceof BranchInstruction b)require(labels.getOrDefault(b.target(),-1)>offset,"Geometry backward branch");
            if(i instanceof FieldInstruction f)require(point!=null&&f.owner().asSymbol().equals(point)&&Set.of("lat","lon").contains(f.name().stringValue())&&f.type().equalsString("D")&&
                Set.of(Opcode.GETFIELD,Opcode.PUTFIELD).contains(i.opcode()),"Foreign geometry field effect");
            if(i instanceof InvokeInstruction call)require(i.opcode()==Opcode.INVOKESTATIC&&call.owner().asSymbol().equals(MATH)&&
                Set.of("sin","cos","asin","atan2","sqrt").contains(call.name().stringValue())&&
                call.type().equalsString(call.name().equalsString("atan2")?"(DD)D":"(D)D"),"Unqualified geometry callee/descriptor");
            if(i instanceof LoadInstruction load&&load.typeKind()==TypeKind.REFERENCE)require(point!=null&&load.slot()==0,"Foreign geometry receiver");
            if(i instanceof StoreInstruction store)require(store.typeKind()!=TypeKind.REFERENCE,"Geometry aliases a reference");
            require(!i.opcode().name().endsWith("ASTORE")||i instanceof StoreInstruction,"Geometry array write");
            offset+=i.sizeInBytes();
        }
    }
    public static Selection select(ClassModel unit,ClassModel aero,ClassModel calc,ClassModel point,ClassDesc result){
        var l=new Layout(unit.thisClass().asSymbol(),aero.thisClass().asSymbol(),calc.thisClass().asSymbol(),point.thisClass().asSymbol(),result);
        for(var d:List.of(l.aero,l.calc,l.point,l.result))require(pkg(l.unit).equals(pkg(d)),"Changed IRS geometry dependency package");
        for(var m:unit.methods())require(!m.methodName().equalsString(MOVE),"Existing IOS IRS movement binding");
        var transfer=IosIrsTransferPrepare.select(unit,aero,result);field(unit,"pbdM",l.point,false,false);
        for(String name:List.of("lat","lon"))field(point,name,CD_double,false,true);
        var moving=method(unit,"instructorMoving",MOVING,false);
        byte[] reference=ClassFile.of().build(l.unit,b->b.withMethodBody("instructorMoving",MOVING,0,c->body(c,l,false)));
        require(identity(moving).equals(identity(ClassFile.of().parse(reference).methods().getFirst())),"Changed complete native IRS movement branch/order/threshold");
        geometry(method(point,"precise",MethodTypeDesc.of(CD_void,CD_double,CD_double,CD_double,CD_double),false),l.point);
        for(String name:List.of("courseTo","orthodromicDist"))geometry(method(calc,name,FOUR,true),null);
        var writes=new ArrayList<Integer>();int offset=0;for(var e:moving.code().get())if(e instanceof Instruction i){if(i.opcode()==Opcode.PUTFIELD)writes.add(offset);offset+=i.sizeInBytes();}
        return new Selection(l,transfer,List.copyOf(writes));
    }
    public static byte[] prepare(ClassFile cf,ClassModel unit,ClassModel aero,ClassModel calc,ClassModel point,Selection s){
        require(select(unit,aero,calc,point,s.layout.result).equals(s),"IRS movement input differs from qualification");
        var withTransfer=cf.parse(IosIrsTransferPrepare.prepare(cf,unit,aero,s.transfer));
        return cf.transformClass(withTransfer,ClassTransform.endHandler(b->b.withMethodBody(MOVE,MOVING,ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->body(c,s.layout,true))));
    }
    private IosIrsMovingPrepare(){}
}
