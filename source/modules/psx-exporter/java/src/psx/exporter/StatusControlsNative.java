package psx.exporter;
/** Independent PIS1 observer handoff; preparation never starts its pipe worker. */
final class StatusControlsNative {
    static native int abiVersion();
    static native long frequency();
    static native long qpc();
    static native boolean initialize(boolean startup);
    static native void state(int capabilities,long lifecycle,boolean loading);
    static native void publish(int group,int model,int valid,long sequence,long lifecycle,long observed,long frame,
        float v0,float v1,float v2,float v3,float v4,float v5,float v6,float v7,
        float v8,float v9,float v10,float v11,float v12,float v13,float v14,float v15,
        float v16,float v17,float v18,float v19,float v20,float v21,float v22,float v23,
        float v24,float v25,float v26,float v27,float v28,float v29,float v30,float v31);
    private StatusControlsNative(){}
}
