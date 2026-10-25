package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Closed-quiescent preservation preimages, followed by non-consuming checks.
 * A pre-existing pending flight-number edit rejects before publication. No
 * native fuel/history/flight-number field is restored over newer inputs. */
final class FmcReconstructionPreservationAccess {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final MethodHandle flight,fuel,value,pending;
    private final Object bank;private String number,bankText;private int[] fuelBits;
    private FmcReconstructionPreservationAccess(FmcInitialization.Context context,BooleanSupplier qualified)throws Throwable{
        this.context=context;this.qualified=qualified;flight=method(context.owner("FmcRte"),FmcReconstructionPreservationPrepare.FLIGHT,Object[].class);fuel=method(context.owner("FmcPerf"),FmcReconstructionPreservationPrepare.FUEL,int[].class);value=method(context.owner("Qs"),FmcReconstructionBanks.VALUE,String.class,Object.class);pending=method(context.owner("Q"),FmcReconstructionBanks.PENDING,boolean.class,Object.class);
        Object[] roots=(Object[])flight.invokeExact();require(roots.length==2&&context.owner("Qs").isInstance(roots[1]),"current flight-number bank");bank=roots[1];
    }
    static FmcReconstructionPreservationAccess bind(FmcInitialization.Context context,BooleanSupplier qualified)throws Throwable{Objects.requireNonNull(context);Objects.requireNonNull(qualified);require(context.current()&&qualified.getAsBoolean(),"worker lifecycle");return new FmcReconstructionPreservationAccess(context,qualified);}
    Object[] receivers(){return new Object[]{bank};}
    void capture()throws Throwable{
        require(fuelBits==null&&context.current()&&qualified.getAsBoolean(),"closed preimage order/lifecycle");Object[] roots=(Object[])flight.invokeExact();require(roots.length==2&&roots[1]==bank&&roots[0] instanceof String,"current flight-number roots");number=(String)roots[0];bankText=(String)value.invokeExact(bank);require(!(boolean)pending.invokeExact(bank)&&number.equals(bankText.equals("-")?"":bankText),"pending/incoherent current flight-number bank");fuelBits=(int[])fuel.invokeExact();require(fuelBits.length==4,"complete manual FMC fuel history");verify();
    }
    void verify()throws Throwable{
        require(fuelBits!=null&&context.current()&&qualified.getAsBoolean(),"preservation lifecycle");Object[] roots=(Object[])flight.invokeExact();require(roots.length==2&&roots[1]==bank&&number.equals(roots[0])&&bankText.equals((String)value.invokeExact(bank))&&!(boolean)pending.invokeExact(bank),"current global flight number changed");require(Arrays.equals(fuelBits,(int[])fuel.invokeExact()),"manual FMC fuel/history changed");require(context.current()&&qualified.getAsBoolean(),"preservation lifecycle changed");
    }
    private static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>... args)throws ReflectiveOperationException{var m=owner.getDeclaredMethod(name,args);require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==result,"exact source bridge");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result,args));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC current preservation "+why);}
}
