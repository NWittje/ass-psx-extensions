package psx.exporter;
/** Independent observational ABI; no recorder control or command writer. */
final class DuTimingNative {
    static native int abiVersion();
    static native boolean prepare();
    static native void disable();
    static native boolean initialize(String source);
    static native void event(int producer,int stage,int unit,long generation,long frame,long paint,long value);
}
