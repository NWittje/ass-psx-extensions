package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound actual air-data copy. Requires complete owner/input exclusion.
 * The generated member reads static native fields, never a requested target. */
public final class IosAirDataReadbackPrepare {
    public static final String OBSERVE="psxExporterIosObserveRelocationAirData";
    public record Field(String name,ClassDesc type,boolean weather){}
    public record Selection(IosAirDataFinalizerPrepare.Selection finalizer,ClassDesc output,List<Field> inputs,List<Field> caches){}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException(why);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean external){var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing readback storage "+name);var f=all.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(!external||(flags&ClassFile.ACC_PUBLIC)!=0),"Changed/inaccessible air-data readback storage "+name);
        if(!statik)require((flags&ClassFile.ACC_FINAL)==0,"Immutable readback output storage");}
    public static Selection select(ClassModel aero,ClassModel weather,ClassModel calc,ClassModel output){
        var finalizer=IosAirDataFinalizerPrepare.select(aero,weather,calc);
        require((output.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_INTERFACE))==(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL),"Readback must be public/final concrete class");
        for(var m:aero.methods())require(!m.methodName().equalsString(OBSERVE),"Existing actual air-data observation");field(output,"availability",CD_int,false,true);
        var inputs=new ArrayList<Field>();for(String name:List.of("altitudeReal","tasReal","gsReal","longiHdg","smoothImpulseTas","actHeadShear","rollBack"))inputs.add(new Field(name,CD_float,false));
        inputs.add(new Field("ground",CD_boolean,false));inputs.add(new Field("driftAngByWind",CD_double,false));
        for(String name:List.of("fHpa","fTemp","fWindDir","totalWindSpd"))inputs.add(new Field(name,CD_float,true));
        var caches=finalizer.outputs().stream().map(n->new Field(n,n.equals("movingBackwards")?CD_boolean:CD_float,false)).toList();
        for(var f:inputs){field(f.weather?weather:aero,f.name,f.type,true,false);field(output,f.name,f.type,false,true);}
        for(var f:caches){field(aero,f.name,f.type,true,false);field(output,f.name,f.type,false,true);}
        return new Selection(finalizer,output.thisClass().asSymbol(),List.copyOf(inputs),caches);
    }
    public static byte[] prepare(ClassFile cf,ClassModel aero,ClassModel weather,ClassModel calc,ClassModel output,Selection s){
        require(select(aero,weather,calc,output).equals(s),"Air-data observation differs from qualification");var l=s.finalizer.layout();
        return cf.transformClass(aero,ClassTransform.endHandler(b->b.withMethodBody(OBSERVE,MethodTypeDesc.of(CD_void,s.output),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var valid=c.newLabel();c.aload(0).ifnonnull(valid).return_().labelBinding(valid).aload(0).iconst_0().putfield(s.output,"availability",CD_int);
            for(var f:s.inputs)c.aload(0).getstatic(f.weather?l.weather():l.aero(),f.name,f.type).putfield(s.output,f.name,f.type);
            c.aload(0).iconst_1().putfield(s.output,"availability",CD_int);
            for(var f:s.caches)c.aload(0).getstatic(l.aero(),f.name,f.type).putfield(s.output,f.name,f.type);
            c.aload(0).iconst_3().putfield(s.output,"availability",CD_int).return_();
        })));
    }
    private IosAirDataReadbackPrepare(){}
}
