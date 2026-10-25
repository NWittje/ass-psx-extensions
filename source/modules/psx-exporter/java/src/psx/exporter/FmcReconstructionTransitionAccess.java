package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Pilot transition statuses and their non-consuming bank are current inputs.
 * Verify before mutation; ordinary EXEC may publish fresh automatic values.
 * Never restore an old bank or consume that native output's freshness. */
final class FmcReconstructionTransitionAccess {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;
    private final MethodHandle status,bank,value,pending;private final Object receiver;
    private final int[] before;private final String text;
    private FmcReconstructionTransitionAccess(FmcInitialization.Context context,BooleanSupplier qualified)throws Throwable{
        this.context=context;this.qualified=qualified;var owner=context.owner("FmcVnav");context.owner("Glob");
        status=method(owner,FmcReconstructionTransitionEffects.STATUS,int[].class);bank=method(owner,FmcReconstructionTransitionEffects.BANK,Object.class);
        value=method(context.owner("Qs"),FmcReconstructionBanks.VALUE,String.class,Object.class);pending=method(context.owner("Q"),FmcReconstructionBanks.PENDING,boolean.class,Object.class);
        receiver=(Object)bank.invokeExact();require(context.owner("Qs").isInstance(receiver),"current transition bank type");before=(int[])status.invokeExact();require(before.length==2,"both pilot statuses");text=(String)value.invokeExact(receiver);require(text!=null,"transition bank text");verifyPreimage();
    }
    static FmcReconstructionTransitionAccess bind(FmcInitialization.Context context,BooleanSupplier qualified)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(qualified);require(context.current()&&qualified.getAsBoolean()&&!FmcExclusion.admissionClosed(),"open worker binding");return new FmcReconstructionTransitionAccess(context,qualified);
    }
    private static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>... args)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,args);require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==result,"exact bridge");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result,args));
    }
    void verifyPreimage()throws Throwable{
        require(context.current()&&qualified.getAsBoolean()&&(Object)bank.invokeExact()==receiver,"current bank identity/lifecycle");require(Arrays.equals(before,(int[])status.invokeExact())&&text.equals((String)value.invokeExact(receiver))&&!(boolean)pending.invokeExact(receiver),"pilot transition input changed or pending");require(context.current()&&qualified.getAsBoolean(),"preimage lifecycle changed");
    }
    Object receiver(){return receiver;}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC transition input "+why);}
}
