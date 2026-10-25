package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional, unbound native arithmetic preparation. Never defines installed classes.
 * Extracts only the reviewed float arithmetic; no native diagnostic, state or UI
 * path is invoked. Callers must validate inputs/results on a worker. This does
 * not qualify destination weather capture or the mutation adapter. */
public final class IosAirDataPrepare {
    public static final String MACH="psxExporterIosMach",IAS="psxExporterIosIas",INDICATION="psxExporterIosIndication";
    private static final MethodTypeDesc TWO=MethodTypeDesc.of(CD_float,CD_float,CD_float),
        THREE=MethodTypeDesc.of(CD_float,CD_float,CD_float,CD_float);
    private static final ClassDesc FLOAT=ClassDesc.of("java.lang.Float");
    private static final MethodTypeDesc FINITE=MethodTypeDesc.of(CD_boolean,CD_float);
    private static final ClassDesc MATH=ClassDesc.of("java.lang.Math");
    private static final MethodTypeDesc UNARY=MethodTypeDesc.of(CD_double,CD_double);
    public record Site(int offset,Instruction instruction) {}
    public record Selection(List<Instruction> mach,List<Instruction> ias,int machBegin,int machEnd,int iasBegin,int iasEnd) {}
    private record Body(List<Site> sites,Map<Label,Integer> labels) {}
    private static Body body(MethodModel m){
        if((m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))!=ClassFile.ACC_STATIC||m.code().isEmpty())
            throw new IllegalArgumentException("Changed native air-data binding");
        if(!m.code().orElseThrow().exceptionHandlers().isEmpty())throw new IllegalArgumentException("Air-data handler changed");
        var sites=new ArrayList<Site>();var labels=new HashMap<Label,Integer>();int offset=0;
        for(var e:m.code().orElseThrow()){
            if(e instanceof LabelTarget t)labels.put(t.label(),sites.size());
            if(e instanceof Instruction i){sites.add(new Site(offset,i));offset+=i.sizeInBytes();}
        }
        return new Body(List.copyOf(sites),Map.copyOf(labels));
    }
    private static MethodModel method(ClassModel c,String name){
        return c.methods().stream().filter(m->m.methodName().equalsString(name)&&m.methodType().equalsString("(FF)F")).findFirst()
            .orElseThrow(()->new IllegalArgumentException("Missing native float air-data descriptor: "+name));
    }
    private static Instruction at(Body b,int n){if(n<0||n>=b.sites.size())throw new IllegalArgumentException("Truncated air-data path");return b.sites.get(n).instruction;}
    private static boolean load(Instruction i,int slot){return i instanceof LoadInstruction l&&l.typeKind()==TypeKind.FLOAT&&l.slot()==slot;}
    private static boolean constant(Instruction i,Object value){return i instanceof ConstantInstruction c&&value.equals(c.constantValue());}
    private static boolean call(Instruction i,String owner,String name,String type){return i instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKESTATIC&&c.owner().asInternalName().equals(owner)&&c.name().equalsString(name)&&c.type().equalsString(type);}
    private static void require(boolean valid){if(!valid)throw new IllegalArgumentException("Native float air-data path changed");}
    private static int jump(Body b,int n,Opcode opcode){
        var instruction=at(b,n);require(instruction.opcode()==opcode&&instruction instanceof BranchInstruction);
        Integer destination=b.labels.get(((BranchInstruction)instruction).target());
        require(destination!=null&&destination>n);return destination;
    }
    // Mathematical templates are authored expressions, not recovered source.
    private static void machExpression(CodeBuilder b){
        b.fload(0).loadConstant(38.97d).fload(1).f2d().loadConstant(273.2d).dadd()
            .invokestatic(MATH,"sqrt",UNARY).dmul().d2f().fdiv().freturn();
    }
    private static void iasExpression(CodeBuilder b){
        b.loadConstant(5.256d).dconst_1().loadConstant(6.876E-6d).fload(0).f2d().dmul().dsub()
            .invokestatic(MATH,"log",UNARY).dmul().invokestatic(MATH,"exp",UNARY).d2f().fstore(2);
        b.loadConstant(3.5d).fconst_1().fload(1).fload(1).fmul().loadConstant(5f).fdiv().fadd().f2d()
            .invokestatic(MATH,"log",UNARY).dmul().invokestatic(MATH,"exp",UNARY).d2f().fconst_1().fsub().fstore(3);
        b.loadConstant(.28571429d).fconst_1().fload(2).fload(3).fmul().fadd().f2d()
            .invokestatic(MATH,"log",UNARY).dmul().invokestatic(MATH,"exp",UNARY).d2f().fconst_1().fsub().fstore(3);
        b.loadConstant(661.48f).loadConstant(5f).fload(3).fmul().f2d().invokestatic(MATH,"sqrt",UNARY).d2f().fmul().fstore(3);
    }
    private static List<Instruction> template(boolean mach){
        var cf=ClassFile.of();byte[] bytes=cf.build(ClassDesc.of("IosAuthoredAirDataExpression"),c->
            c.withMethodBody("expression",TWO,ClassFile.ACC_STATIC,b->{if(mach)machExpression(b);else{iasExpression(b);b.fload(3).freturn();}}));
        return cf.parse(bytes).methods().getFirst().code().orElseThrow().elementStream().filter(e->e instanceof Instruction).map(e->(Instruction)e).toList();
    }
    private static String identity(Instruction i){
        if(i instanceof ConstantInstruction c)return "constant:"+c.typeKind()+":"+c.constantValue();
        if(i instanceof LoadInstruction l)return "load:"+l.typeKind()+":"+l.slot();
        if(i instanceof StoreInstruction s)return "store:"+s.typeKind()+":"+s.slot();
        if(i instanceof InvokeInstruction c)return "call:"+c.opcode()+":"+c.owner().asInternalName()+":"+c.name().stringValue()+c.type().stringValue();
        return i.opcode().name();
    }
    private static List<Instruction> match(Body body,int start,List<Instruction> expected){
        var result=new ArrayList<Instruction>();
        for(int n=0;n<expected.size();n++){
            var i=at(body,start+n);require(identity(i).equals(identity(expected.get(n))));result.add(i);
        }
        return List.copyOf(result);
    }
    public static Selection select(ClassModel input){
        var mach=body(method(input,"getMach"));var ias=body(method(input,"getIas"));
        require(load(at(mach,0),0)&&call(at(mach,1),"java/lang/Float","isNaN","(F)Z"));
        int oat=jump(mach,2,Opcode.IFEQ);
        require(load(at(mach,oat),1)&&call(at(mach,oat+1),"java/lang/Float","isNaN","(F)Z"));
        int temperature=jump(mach,oat+2,Opcode.IFEQ);
        require(load(at(mach,temperature),1)&&at(mach,temperature+1).opcode()==Opcode.F2D&&
            constant(at(mach,temperature+2),-273.1d)&&at(mach,temperature+3).opcode()==Opcode.DCMPG);
        int mBegin=jump(mach,temperature+4,Opcode.IFGE);var mExpected=template(true);var mBody=match(mach,mBegin,mExpected);
        require(mBegin+mExpected.size()==mach.sites.size());
        require(load(at(ias,0),1)&&constant(at(ias,1),.001f)&&at(ias,2).opcode()==Opcode.FCMPL);
        int zero=jump(ias,3,Opcode.IFLE);var iExpected=template(false);
        var arithmetic=iExpected.subList(0,iExpected.size()-2);var iBody=match(ias,4,arithmetic);
        int check=4+iBody.size();
        require(load(at(ias,check),3)&&call(at(ias,check+1),"java/lang/Float","isNaN","(F)Z"));
        int success=jump(ias,check+2,Opcode.IFEQ);
        require(success>check+2&&load(at(ias,success),3)&&at(ias,success+1).opcode()==Opcode.FRETURN&&
            zero==success+2&&constant(at(ias,zero),0f)&&at(ias,zero+1).opcode()==Opcode.FRETURN&&zero+2==ias.sites.size());
        return new Selection(mBody,iBody,mach.sites.get(mBegin).offset,mach.sites.getLast().offset+at(mach,mach.sites.size()-1).sizeInBytes(),
            ias.sites.get(4).offset,ias.sites.get(check).offset);
    }
    public static byte[] transform(ClassFile parser,ClassModel input){
        for(var m:input.methods())if(Set.of(MACH,IAS,INDICATION).contains(m.methodName().stringValue()))
            throw new IllegalArgumentException("IOS air-data accessor collision");
        var selected=select(input);ClassDesc owner=input.thisClass().asSymbol();
        byte[] output=parser.transformClass(input,ClassTransform.endHandler(c->{
            int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            c.withMethodBody(MACH,TWO,flags,b->{for(var i:selected.mach)b.with(i);});
            c.withMethodBody(IAS,TWO,flags,b->{var zero=b.newLabel();b.fload(1).loadConstant(.001f).fcmpl().ifle(zero);
                for(var i:selected.ias)b.with(i);b.fload(3).freturn().labelBinding(zero).fconst_0().freturn();});
            c.withMethodBody(INDICATION,THREE,flags,b->{
                var bad=b.newLabel();
                for(int slot=0;slot<3;slot++)b.fload(slot).invokestatic(FLOAT,"isFinite",FINITE).ifeq(bad);
                b.fload(2).f2d().loadConstant(-273.1d).dcmpg().ifle(bad);
                b.fload(1).f2d().loadConstant(6.876E-6d).dmul().dconst_1().dcmpg().ifge(bad);
                b.fload(0).fload(2).invokestatic(owner,MACH,TWO).fstore(3);
                b.fload(3).invokestatic(FLOAT,"isFinite",FINITE).ifeq(bad);
                b.fload(1).fload(3).invokestatic(owner,IAS,TWO).fstore(3);
                b.fload(3).invokestatic(FLOAT,"isFinite",FINITE).ifeq(bad);
                b.fload(3).freturn().labelBinding(bad).loadConstant(Float.NaN).freturn();
            });
        }));
        if(!parser.verify(output).isEmpty())throw new IllegalArgumentException("Generated air-data verification failed");
        return output;
    }
}
