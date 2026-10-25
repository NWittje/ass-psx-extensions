package psx.exporter;
/** Read-only tap of the already calculated local target, before native smoothing. */
public final class EnvironmentVisibilityObserver {
 static volatile boolean qualified;
 private static volatile boolean ready;
 private EnvironmentVisibilityObserver() {}
 static boolean available(){try{if(EnvironmentVisibilityNative.abiVersion()!=1)return false;EnvironmentVisibilityNative.capabilities(false);EnvironmentVisibilityNative.record(-1,0);return true;}catch(Throwable failure){return false;}}
 static void prepare(){try{ready=qualified&&available();EnvironmentVisibilityNative.capabilities(ready);}catch(Throwable failure){ready=false;}}
 public static void disable(){ready=false;try{EnvironmentVisibilityNative.capabilities(false);}catch(Throwable ignored){}}
 public static void record(int metres){if(ready)try{EnvironmentVisibilityNative.record(metres,Observer.epochForTest());}catch(Throwable failure){disable();}}
}
