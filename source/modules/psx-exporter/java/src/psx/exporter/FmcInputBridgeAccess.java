package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.function.BooleanSupplier;

/** Worker-only resolution of prepared input replay bridges. Reuses the existing
 * initialized, loader/lifecycle-bound context. It never reflects native fields,
 * defines a class, opens a module or initializes a vendor class during inspection.
 * Thread/exception/lifecycle disposition remains installed-profile authority. */
final class FmcInputBridgeAccess implements FmcJobAdmission.InputReplay {
    private final FmcInitialization.Context context;
    private final BooleanSupplier qualification;
    private final Class<?> qi,qs;
    private final MethodHandle integers,strings;
    private FmcInputBridgeAccess(FmcInitialization.Context context,BooleanSupplier qualification,Class<?> qi,Class<?> qs,MethodHandle integers,MethodHandle strings){
        this.context=context;this.qualification=qualification;this.qi=qi;this.qs=qs;this.integers=integers;this.strings=strings;
    }
    private static MethodHandle bridge(Class<?> owner,Class<?> value)throws ReflectiveOperationException{
        var type=MethodType.methodType(void.class,Object.class,value,char.class,int.class);
        var method=owner.getDeclaredMethod(FmcInputPrepare.REPLAY,type.parameterArray());
        if(method.getReturnType()!=void.class||!Modifier.isPublic(method.getModifiers())||!Modifier.isStatic(method.getModifiers())||!method.isSynthetic())
            throw new IllegalAccessException("FMC retained input bridge flags/descriptor");
        return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,FmcInputPrepare.REPLAY,type);
    }
    static FmcInputBridgeAccess bind(FmcInitialization.Context context,BooleanSupplier qualification)throws ReflectiveOperationException{
        if(context==null||qualification==null||!context.current()||!qualification.getAsBoolean())throw new IllegalStateException("FMC input replay qualification");
        Class<?> qi=context.owner("Qi"),qs=context.owner("Qs");
        var result=new FmcInputBridgeAccess(context,qualification,qi,qs,bridge(qi,int.class),bridge(qs,String.class));
        if(!context.current()||!qualification.getAsBoolean())throw new IllegalStateException("FMC input replay binding replaced");
        return result;
    }
    void receivers(Object[] integers,Object[] strings){
        for(Object receiver:integers)if(receiver==null||receiver.getClass()!=qi)throw new IllegalArgumentException("FMC integer input receiver identity/type");
        for(Object receiver:strings)if(receiver==null||receiver.getClass()!=qs)throw new IllegalArgumentException("FMC string input receiver identity/type");
    }
    private void current(){if(!context.current()||!qualification.getAsBoolean())throw new IllegalStateException("FMC input replay authority replaced");}
    public void qi(Object receiver,int value,char mode,int origin)throws Throwable{current();integers.invokeExact(receiver,value,mode,origin);}
    public void qs(Object receiver,String value,char mode,int origin)throws Throwable{current();strings.invokeExact(receiver,value,mode,origin);}
}
