package psx.exporter;

/** Independent optional metadata ABI. Does not change environment JNI1. */
final class WeatherChangeNative {
    static native int abiVersion();
    static native void capabilities(boolean qualified);
    static native void mark(int reason,long lifecycle,int slot);
    static native boolean consumed(long lifecycle,long phaseStart);
    private WeatherChangeNative() {}
}
