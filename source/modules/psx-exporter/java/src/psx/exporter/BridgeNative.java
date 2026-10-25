package psx.exporter;

/** Native bounded SPSC ingress. Only qpc/publish are called from the frame thread. */
public final class BridgeNative {
    /** Independent DPI1 instrument recorder. Initialize is worker-only. */
    public static native int instrumentRecorderAbiVersion();
    public static native boolean instrumentRecorderInitialize(String metadata);
    public static native void instrumentRecorderPoll(java.nio.ByteBuffer target);
    public static native boolean instrumentRecorderPublish(java.nio.ByteBuffer data,int length);
    private BridgeNative() {}
    /** Independent DPF1 recorder worker boundary; never transports on the paint thread. */
    public static native int recorderAbiVersion();
    public static native boolean recorderInitialize(String metadata);
    public static native void recorderPoll(java.nio.ByteBuffer destination);
    public static native boolean recorderPublish(java.nio.ByteBuffer chunk,int length);
    /** Independent read-only physical aircraft copies, JNI ABI1. */
    public static native int aircraftAbiVersion();
    public static native void aircraftCapabilities(int groups);
    public static native void aircraftState(long lifecycle,boolean loading);
    public static native void aircraftPublish(int group,int family,int variant,int flags,int valid,
        long sequence,long lifecycle,long start,long writerVersion,long frame,
        float v0,float v1,float v2,float v3,float v4,float v5,float v6,float v7,
        float v8,float v9,float v10,float v11,float v12,float v13,float v14,float v15,
        float v16,float v17,float v18,float v19,float v20,float v21,float v22,float v23,
        float v24,float v25,float v26,float v27,float v28,float v29,float v30,float v31);
    /** Pure ABI probe; must not initialize transport or retain process resources. */
    public static native int abiVersion();
    /** Pure support probe; ownership is acquired only by ordinary startup. */
    public static native int runtimeInstanceSupport();
    public static native boolean claimRuntime();
    public static native boolean initialize();
    public static native boolean active();
    public static native int terrainAbiVersion();
    public static native void terrainCapabilities(int capabilities);
    public static native int terrainPoll(java.nio.ByteBuffer destination,long lifecycle);
    public static native boolean terrainConnected();
    public static native long terrainConnection();
    public static native long terrainFrequency();
    public static native long terrainCaptureGeneration();
    public static native int terrainThreadId();
    /** Capture-only, bounded direct-buffer snapshot; schema1/464 bytes, little endian. */
    public static native void terrainDetailPublish(java.nio.ByteBuffer detail);
    public static native void terrainApplied(long operation,int phase,int reason);
    public static native void terrainPublish(long poseEpoch,long terrainEpoch,long frame,long query,long owner,long consumedQpc,long surfaceQpc,int phase,int reason,
        double latitude,double longitude,double height,double normalEast,double normalNorth,double normalUp,double pitch,double bank,float strut,float osc,float compress,int configuration,int contacts,long applied,long rejected,long hookTicks,long sourceSampleQpc);
    public static native long qpc();
    /** Independent scalar display diagnostics, ABI1; no simulator commands or graphics. */
    public static native int displayAbiVersion();
    public static native boolean displayInitialize(String metadata);
    public static native long displayFrequency();
    public static native int displayThreadId();
    public static native void displayPoll(java.nio.ByteBuffer destination);
    public static native boolean displayPublish(java.nio.ByteBuffer batch,int length);
    public static native void audioCapabilities(int observations, int suppression);
    /** Packed monotonically increasing control ticket and eight group bits. */
    public static native long audioControl();
    public static native void audioApplied(long control);
    public static native void audioPaused(boolean paused);
    public static native void audioPublish(int group,int family,int variant,int flags,int valid,
        long sequence,long lifecycle,long start,
        float v0,float v1,float v2,float v3,float v4,float v5,float v6,float v7,
        float v8,float v9,float v10,float v11,float v12,float v13,float v14,float v15,
        float v16,float v17,float v18,float v19,float v20,float v21,float v22,float v23,
        float v24,float v25,float v26,float v27,float v28,float v29,float v30,float v31);
    public static native void state(long attempted, long rejected, long poseEpoch, int flags);
    public static native void publish(long sampleStartQpc, long frameSequence, long poseEpoch,
        long schedulerWallMillis, double deltaMs, double deltaAccMs, double simRate, int flags,
        double latitudeRad, double longitudeRad, double headingRad, float totalPitchRad,
        float bankRad, float altitudeFeet, float mainStrutFeet, float basePitchRad,
        float noseOscRad, float noseCompressRad, float cachedSinPitch, float stockEyeAltitudeFeet,
        float longitudinalHeadingRad, float fpvRad, float slopeFade,
        double stockEyeLatitudeRad, double stockEyeLongitudeRad);
    public static native int duRecorderAbiVersion();
    public static native boolean duRecorderInitialize(int unit,String metadata);
    public static native void duRecorderPoll(int unit,java.nio.ByteBuffer target);
    public static native boolean duRecorderPublish(int unit,java.nio.ByteBuffer data,int length);
}
