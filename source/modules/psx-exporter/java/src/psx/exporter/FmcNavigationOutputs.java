package psx.exporter;

/** Worker-bound CURRENT navigation and IRS banks, never artifact-selected banks.
 * Native callbacks compare identity and lifecycle without hashing/allocation. */
final class FmcNavigationOutputs {
    private record Binding(long lifecycle,Object bank,Object irsBank){}
    private static volatile Binding binding;
    private FmcNavigationOutputs(){}
    static void bind(long lifecycle,Object bank,Object irsBank){if(bank==null||irsBank==null||bank==irsBank||lifecycle<=0||lifecycle!=Observer.terrainEpoch()||FmcExclusion.admissionClosed())throw new IllegalStateException("FMC current navigation output binding");binding=new Binding(lifecycle,bank,irsBank);}
    static boolean suppress(Object receiver){var current=binding;return current!=null&&(current.bank==receiver||current.irsBank==receiver)&&current.lifecycle==Observer.terrainEpoch()&&FmcExecSeam.preservingOwned();}
    static void clear(){if(FmcExclusion.admissionClosed())throw new IllegalStateException("FMC current navigation output still closed");binding=null;}
}
