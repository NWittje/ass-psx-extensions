package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;

/** Worker-resolved current Pause proof; no mutation or initialization request. */
final class FmcReconstructionPauseAccess {
    private final FmcInitialization.Context context;private final MethodHandle state;
    private FmcReconstructionPauseAccess(FmcInitialization.Context context,MethodHandle state){this.context=context;this.state=state;}
    static FmcReconstructionPauseAccess bind(FmcInitialization.Context context)throws ReflectiveOperationException{
        var owner=context.owner("Core");var method=owner.getDeclaredMethod(FmcReconstructionPausePrepare.STATE);
        if(method.getModifiers()!=(Modifier.PUBLIC|Modifier.STATIC|0x1000)||!method.isSynthetic()||method.getReturnType()!=int.class)throw new IllegalStateException("FMC Pause exact getter");
        var state=MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,FmcReconstructionPausePrepare.STATE,MethodType.methodType(int.class));
        if(!context.current())throw new IllegalStateException("FMC Pause binding lifecycle");return new FmcReconstructionPauseAccess(context,state);
    }
    int ordinaryMask()throws Throwable{
        if(!context.current())throw new IllegalStateException("FMC Pause lifecycle");int mask=(int)state.invokeExact();
        if(!context.current()||mask!=1&&mask!=3)throw new IllegalStateException("FMC recall requires current ordinary Pause; Motion Freeze may remain set");return mask;
    }
    void requireMask(int expected)throws Throwable{if(ordinaryMask()!=expected)throw new IllegalStateException("FMC Pause/Freeze changed during recall");}
}
