package psx.exporter;
/** Independent read-only PXS1 primitive boundary. */
final class StatusNative {
    private StatusNative() {}
    static native int abiVersion();
    static native long intervalTicks();
    static native void capabilities(int mask);
    static native void failed();
    static native void publish(long sequence,long lifecycle,long observed,int flags);
}
