package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Destination heading fragment. No lateral-progress/dynamics call is made.
 * The installed polar reference uses grid-coded trueHeading beyond +/-89 degrees;
 * longiHdg is geographic true heading. Original methods remain unchanged. */
public final class IosHeadingPrepare {
    public static final String APPLY="psxExporterIosDestinationHeading";
    public static final double POLAR=1.5533430342749532, NATIVE_TAU=6.2831854820251465;
    private static final ClassDesc A=ClassDesc.of("Aerodynamics"),R=ClassDesc.of("SmoothRotator");
    public record Selection(List<Integer> referenceSites){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    public static Selection select(ClassModel aero,ClassModel rotator){
        require(aero.thisClass().asInternalName().equals("Aerodynamics")&&rotator.thisClass().asInternalName().equals("SmoothRotator"),"Changed heading owner");
        require(aero.methods().stream().noneMatch(m->m.methodName().equalsString(APPLY)),"Heading already prepared");
        for(String n:List.of("realLat","realLon","trueHeading","targTrueHeading","flexHdg"))field(aero,n,CD_double,true);
        for(String n:List.of("longiHdg","publicLongiHdg","gridHdg"))field(aero,n,CD_float,true);
        for(String n:List.of("gridNorthOn","gridSouthOn"))field(aero,n,CD_boolean,true);
        field(aero,"flexHdgRotator",R,true);field(rotator,"angle",CD_double,false);field(rotator,"angleEx",CD_double,false);
        var nativeMethod=aero.methods().stream().filter(m->m.methodName().equalsString("lateralProgress")&&m.methodType().equalsString("()V")).findFirst().orElseThrow();
        require((nativeMethod.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,"Changed heading context");
        var sites=new ArrayList<Integer>();boolean north=false,south=false,period=false;int offset=0;
        for(var e:nativeMethod.code().orElseThrow())if(e instanceof Instruction i){
            if(i instanceof ConstantInstruction c){north|=Objects.equals(c.constantValue(),POLAR);south|=Objects.equals(c.constantValue(),-POLAR);period|=Objects.equals(c.constantValue(),NATIVE_TAU);}
            if(i instanceof FieldInstruction f&&f.owner().asInternalName().equals("Aerodynamics")&&
                Set.of("gridNorthOn","gridSouthOn","trueHeading","longiHdg","gridHdg").contains(f.name().stringValue()))sites.add(offset);
            offset+=i.sizeInBytes();
        }
        require(north&&south&&period&&sites.size()>12,"Missing native polar heading reference");
        var finalizer=aero.methods().stream().filter(m->m.methodName().equalsString("finalizeInstructorReposition")&&m.methodType().equalsString("()V")).findFirst().orElseThrow();
        var reference=ClassFile.of().parse(finalizerReference(ClassFile.of())).methods().getFirst();
        require(IosComposedBody.nativeIdentity(finalizer).equals(IosMethodIdentity.of(reference,i->false)),"Changed complete polar heading conversion/order");
        return new Selection(List.copyOf(sites));
    }
    private static byte[] finalizerReference(ClassFile cf){return cf.build(A,b->b.withMethodBody("finalizeInstructorReposition",MethodTypeDesc.of(CD_void),ClassFile.ACC_STATIC,c->{
        var south=c.newLabel();var done=c.newLabel();
        c.getstatic(A,"gridNorthOn",CD_boolean).ifeq(south).getstatic(A,"longiHdg",CD_float).getstatic(A,"realLon",CD_double).d2f().fsub()
            .ldc((float)NATIVE_TAU).frem().f2d().putstatic(A,"trueHeading",CD_double)
            .sipush(136).invokestatic(A,"checkNan",MethodTypeDesc.of(CD_void,CD_int))
            .getstatic(A,"trueHeading",CD_double).putstatic(A,"targTrueHeading",CD_double).goto_(done)
            .labelBinding(south).getstatic(A,"gridSouthOn",CD_boolean).ifeq(done).getstatic(A,"longiHdg",CD_float).getstatic(A,"realLon",CD_double).d2f().fadd()
            .ldc((float)NATIVE_TAU).frem().f2d().putstatic(A,"trueHeading",CD_double)
            .sipush(137).invokestatic(A,"checkNan",MethodTypeDesc.of(CD_void,CD_int))
            .getstatic(A,"trueHeading",CD_double).putstatic(A,"targTrueHeading",CD_double).labelBinding(done).return_();
    }));}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik){
        var f=c.fields().stream().filter(x->x.fieldName().equalsString(name)).findFirst().orElseThrow();
        require(f.fieldTypeSymbol().equals(type)&&((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==statik&&
            (f.flags().flagsMask()&ClassFile.ACC_FINAL)==0&&
            (c.thisClass().asInternalName().equals("Aerodynamics")||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0),"Inaccessible heading field "+name);
    }
    private static void positive(CodeBuilder c){
        var nonnegative=c.newLabel();c.ldc(NATIVE_TAU).drem().dstore(2).dload(2).dconst_0().dcmpg().ifge(nonnegative)
            .dload(2).ldc(NATIVE_TAU).dadd().dstore(2).labelBinding(nonnegative);
    }
    public static byte[] prepare(ClassFile cf,ClassModel aero){
        return cf.transformClass(aero,ClassTransform.endHandler(b->b.withMethodBody(APPLY,MethodTypeDesc.of(CD_void,CD_double),
            ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                var south=c.newLabel();var ordinary=c.newLabel();var done=c.newLabel();
                c.iconst_0().putstatic(A,"gridNorthOn",CD_boolean).iconst_0().putstatic(A,"gridSouthOn",CD_boolean);
                c.dload(0).dstore(2).getstatic(A,"realLat",CD_double).ldc(POLAR).dcmpl().ifle(south)
                    .iconst_1().putstatic(A,"gridNorthOn",CD_boolean)
                    .dload(0).getstatic(A,"realLon",CD_double).d2f().f2d().dsub();positive(c);c.goto_(done);
                c.labelBinding(south).getstatic(A,"realLat",CD_double).ldc(-POLAR).dcmpg().ifge(ordinary)
                    .iconst_1().putstatic(A,"gridSouthOn",CD_boolean)
                    .dload(0).getstatic(A,"realLon",CD_double).d2f().f2d().dadd();positive(c);c.goto_(done);
                c.labelBinding(ordinary).dload(0);positive(c);c.labelBinding(done)
                    .dload(2).putstatic(A,"trueHeading",CD_double).dload(2).putstatic(A,"targTrueHeading",CD_double)
                    .dload(2).putstatic(A,"flexHdg",CD_double)
                    .getstatic(A,"flexHdgRotator",R).dload(2).putfield(R,"angle",CD_double)
                    .getstatic(A,"flexHdgRotator",R).dload(2).putfield(R,"angleEx",CD_double)
                    .dload(0).d2f().putstatic(A,"longiHdg",CD_float).dload(0).d2f().putstatic(A,"publicLongiHdg",CD_float);
                var negative=c.newLabel();var grid=c.newLabel();c.getstatic(A,"realLat",CD_double).dconst_0().dcmpl().ifle(negative)
                    .dload(0).getstatic(A,"realLon",CD_double).dsub().goto_(grid)
                    .labelBinding(negative).dload(0).getstatic(A,"realLon",CD_double).dadd().labelBinding(grid)
                    .ldc(NATIVE_TAU).drem().d2f().putstatic(A,"gridHdg",CD_float).return_();
            })));
    }
    private IosHeadingPrepare(){}
}
