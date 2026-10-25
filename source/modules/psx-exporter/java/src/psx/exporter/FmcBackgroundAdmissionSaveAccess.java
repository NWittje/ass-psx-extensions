package psx.exporter;
import java.lang.invoke.*;import java.lang.reflect.Modifier;
/** Worker-bound handles retain actual current native save-root identities.
 * Native first-ten-second startup is outside ready recall admission; uptime
 * continues normally and is not an artifact or a changed-store input. */
final class FmcBackgroundAdmissionSaveAccess {
 private final FmcInitialization.Context context;private final MethodHandle getter,ready;private final Class<?>[] types;private final Object[] preimage;
 private FmcBackgroundAdmissionSaveAccess(FmcInitialization.Context context)throws Throwable{this.context=context;if(!context.current())throw new IllegalStateException("FMC save lifecycle");var owner=context.owner("Core");getter=resolve(owner,FmcBackgroundAdmissionSavePrepare.ROOTS,Object[].class);ready=resolve(owner,FmcBackgroundAdmissionSavePrepare.READY,int.class);types=new Class<?>[]{context.owner("Instructor"),context.owner("InstructorSituation"),context.owner("InstructorSaveSitu")};preimage=current();}
 static FmcBackgroundAdmissionSaveAccess bind(FmcInitialization.Context context)throws Throwable{return new FmcBackgroundAdmissionSaveAccess(context);}
 Object[] roots(){return preimage.clone();}
 void verify()throws Throwable{Object[] now=current();for(int i=0;i<3;i++)if(now[i]!=preimage[i])throw new IllegalStateException("FMC save singleton pointer changed");}
 private Object[] current()throws Throwable{if(!context.current())throw new IllegalStateException("FMC save lifecycle");if((int)ready.invokeExact()<10)throw new IllegalStateException("FMC native startup not ready");Object[] result=(Object[])getter.invokeExact();if(result.length!=3)throw new IllegalStateException("FMC save root roster");for(int i=0;i<3;i++)if(result[i]==null||result[i].getClass()!=types[i])throw new IllegalStateException("FMC exact initialized save receiver");if(!context.current())throw new IllegalStateException("FMC save lifecycle changed");return result;}
 static Object[] read(FmcInitialization.Context context)throws Throwable{return bind(context).roots();}
 private static MethodHandle resolve(Class<?> owner,String name,Class<?> result)throws ReflectiveOperationException{var method=owner.getDeclaredMethod(name);if(method.getModifiers()!=(Modifier.PUBLIC|Modifier.STATIC|0x1000)||!method.isSynthetic()||method.getReturnType()!=result)throw new IllegalStateException("FMC exact save readonly bridge");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result));}
}