package psx.exporter;

/** Independently resolved environment JNI1. No PSX command entry point. */
final class EnvironmentNative {
    private EnvironmentNative() {}
    static native int abiVersion();
    static native long intervalTicks();
    static native void capabilities(int groups);
    static native void state(long lifecycle,boolean loading);
    static native void publish(int flags,int valid,int configured,long sequence,long lifecycle,long start,long version,long frame,long utc,
        double v0,double v1,double v2,double v3,double v4,double v5,double v6,double v7,double v8,double v9,double v10,double v11,double v12,double v13,double v14,double v15,double v16,double v17,double v18,double v19,
        double c0,double c1,double c2,double c3,double c4,double c5,double c6,double c7,double c8,double c9,double c10,double c11,double c12,double c13,double c14,double c15,double c16,double c17,double c18,double c19,double c22);
}
