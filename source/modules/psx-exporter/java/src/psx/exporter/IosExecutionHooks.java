package psx.exporter;

import psx.ios.*;

/** Unactivated native-boundary facade. No production installation calls bind.
 * Callbacks allocate nothing, never calculate weather or invoke a scheduler.
 * Ordinary native exceptions belong to the original body, outside these helpers. */
public final class IosExecutionHooks {
    private record Runtime(IosNativeBindingState state,IosRelocationNativeAdapter adapter,IosTwoStageRelocation transaction){}
    private static volatile Runtime runtime;
    private IosExecutionHooks(){}
    /** Future ordinary-startup binding, not a fixture/default authority path. */
    public static void bind(IosNativeBindingState state,IosRelocationNativeAdapter adapter,IosTwoStageRelocation transaction){
        if(state==null||adapter==null||transaction==null||runtime!=null||state.gate!=IosExclusion.gate())
            throw new IllegalArgumentException("Invalid native hook association");
        runtime=new Runtime(state,adapter,transaction);
    }
    public static void ownerPoll(){
        var r=runtime;if(r==null||!r.state.startupAssociated)return;
        // The engine preserves the original fault and owns pause-only recovery.
        // A binding/notification fault must not falsely poison intact exclusion
        // guards and prevent that recovery on a later ordinary owner callback.
        try{r.transaction.poll(BridgeNative.qpc());}catch(Throwable optional){r.state.hooksReady=false;}
    }
    public static void selected(long epoch,long qpc,double lat,double lon,float altitude){
        var r=runtime;if(r==null||!r.state.runtimeReady())return;
        r.adapter.destinationSelected(epoch,r.state.weatherConfiguration,qpc,lat,lon,altitude);
    }
    public static void applied(long epoch,long qpc,double lat,double lon,float altitude,float oat,float qnh,float shear,float rollback){
        var r=runtime;if(r==null||!r.state.runtimeReady())return;
        r.adapter.destinationWeatherApplied(epoch,r.state.weatherConfiguration,qpc,lat,lon,altitude,oat,qnh,shear,rollback);
    }
    public static void dynamics(){var r=runtime;if(r!=null&&r.state.startupAssociated)r.state.integrated();}
    public static void fault(){var r=runtime;if(r!=null){r.state.hooksReady=false;r.state.gate.disable();}IosScopeFaults.failed=true;}
}
