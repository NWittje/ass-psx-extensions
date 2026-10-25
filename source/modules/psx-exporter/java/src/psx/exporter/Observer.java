package psx.exporter;

import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLong;

/** All callback state belongs to the exporter. No PSX writes, callback allocation or waits. */
public final class Observer {
    private static final AtomicLong active = new AtomicLong();
    private static final AtomicLong version = new AtomicLong();
    private static final AtomicLong epoch = new AtomicLong(1);
    private static final AtomicLong owner = new AtomicLong();
    // Captured only by the ordinary normal-frame callback, never an IOS poller.
    // Keep the existing pose owner's numeric contract unchanged.
    private static volatile Thread nativeOwner;
    private static volatile boolean disabled;
    static volatile boolean enabled;
    static volatile boolean firstFrame;
    private static volatile long attempted, rejected;
    private static volatile int lastFlags;
    // Only the qualified normal frame thread owns these scalars.
    private static long beforeVersion, beforeEpoch, sampleStart;
    private static volatile long frameSequence;
    private static boolean inFrame, candidate;
    private static int producerFlags;

    private Observer() {}
    public static void disable() { disabled = true; enabled = false; }
    public static void enter() {
        try { active.incrementAndGet(); version.incrementAndGet(); VarHandle.fullFence(); }
        catch (Throwable ignored) { disable(); }
    }
    public static void leave() {
        try { VarHandle.fullFence(); version.incrementAndGet(); active.decrementAndGet(); }
        catch (Throwable ignored) { disable(); }
    }
    public static void lifecycle() {
        try { epoch.incrementAndGet(); Terrain.lifecycle(); } catch (Throwable ignored) { disable(); }
    }
    public static void before() {
        try {
            if (disabled) return;
            long thread = Thread.currentThread().threadId();
            if (owner.get() == 0) owner.compareAndSet(0, thread);
            if (owner.get() != thread || inFrame) { disable(); return; }
            if (nativeOwner == null) nativeOwner = Thread.currentThread();
            inFrame = true;
            producerFlags = 0;
            attempted++;
            frameSequence++;
            beforeVersion = version.get();
            beforeEpoch = epoch.get();
            candidate = enabled && firstFrame && active.get() == 0 && version.get() == beforeVersion;
            VarHandle.fullFence();
        } catch (Throwable ignored) { disable(); }
    }
    public static void completedFrame() {
        // Called only after the original NetBooster call has returned, after every copied owner
        // has initialized through its normal PSX execution path.
        firstFrame = true;
        try { NavigationSelection.initialized(); } catch (Throwable ignored) {}
    }
    /** Independent actual normal-frame owner, unavailable until ordinary initialization
     * has completed. Reading this never establishes an owner or requires a pose lease.
     * Full native context/exclusion qualification is separate from this identity. */
    public static Thread iosEstablishedOwnerThread() {
        return !disabled && firstFrame ? nativeOwner : null;
    }
    /** Established ordinary native owner, independent of pose subscriptions.
     * The qualified post-Traffic hook runs after the Aerodynamics-only pose
     * copy interval closes. Traffic retains its own writer/coherence guards. */
    public static boolean trafficFrameReady() {
        return !disabled && firstFrame && Thread.currentThread() == nativeOwner;
    }
    public static boolean startCopy() {
        try {
            if (!candidate || disabled || !enabled) return false;
            sampleStart = BridgeNative.qpc();
            return true;
        } catch (Throwable ignored) { disable(); return false; }
    }
    public static int flags(boolean ground, boolean north, boolean south, boolean loader, boolean nan) {
        return (ground ? 2 : 0) | (north ? 4 : 0) | (south ? 8 : 0) | (loader || nan ? 128 : 0);
    }
    public static void tag(int tag) {
        try {
            if (inFrame && Thread.currentThread().threadId() == owner.get()) producerFlags |= tag;
        } catch (Throwable ignored) { disable(); }
    }
    public static void after() {
        try {
            if (inFrame && !candidate) { rejected++; lastFlags = producerFlags | 256; }
            inFrame = false; candidate = false;
        } catch (Throwable ignored) { disable(); }
    }
    public static void snapshot(long wall, double delta, double deltaAcc, double simRate,
        int flags, double lat, double lon, double heading, float pitch, float bank,
        float altitude, float strut, float basePitch, float noseOsc, float noseCompress,
        float sine, float eyeAltitude, float longiHeading, float fpv, float slopeFade,
        double eyeLat, double eyeLon) {
        try {
            VarHandle.fullFence();
            int tags = producerFlags;
            boolean valid = candidate && enabled && !disabled && active.get() == 0
                && version.get() == beforeVersion && active.get() == 0 && epoch.get() == beforeEpoch
                && (tags == 16 || tags == 32) && (flags & (4 | 8 | 128)) == 0;
            lastFlags = flags | tags | (valid ? 1 : 256);
            if (!valid) { rejected++; return; }
            timingStage(3);
            BridgeNative.publish(sampleStart, frameSequence, beforeEpoch, wall,
                delta, deltaAcc, simRate, lastFlags, lat, lon, heading, pitch, bank,
                altitude, strut, basePitch, noseOsc, noseCompress, sine, eyeAltitude,
                longiHeading, fpv, slopeFade, eyeLat, eyeLon);
            timingStage(4);
        } catch (Throwable ignored) { disable(); }
    }
    private static void timingStage(int site){
        try{FrameTrace.stage(site);}catch(Throwable ignored){
            try{FrameTrace.failed();}catch(Throwable unavailable){}
        }
    }
    static void startWorker(String nativePath) {
        Thread worker = new Thread(() -> {
            try {
                while (!firstFrame && !disabled) Thread.sleep(100);
                if (disabled) return;
                System.load(nativePath);
                try{AircraftObserver.prepareNative();}catch(Throwable unavailable){}
                try{EnvironmentObserver.prepareNative();}catch(Throwable unavailable){}
                try{StatusObserver.prepareNative();}catch(Throwable unavailable){}
                try{StatusControlsObserver.prepareNative();}catch(Throwable unavailable){}
                BridgeNative.audioCapabilities(AudioObserver.observations(),AudioObserver.suppression());
                try { Terrain.nativeReady=BridgeNative.terrainAbiVersion()==3;BridgeNative.terrainCapabilities(Terrain.ready&&Terrain.nativeReady?2:0); } catch(Throwable unavailable){Terrain.nativeReady=false;}
                if (!BridgeNative.initialize()) { disable(); return; }
                AudioOutput.ready=AudioObserver.suppression()!=0;
                while (!disabled) {
                    enabled = BridgeNative.active();
                    BridgeNative.audioCapabilities(AudioObserver.observations(),AudioObserver.suppression());
                    if(Terrain.nativeReady)BridgeNative.terrainCapabilities(Terrain.ready?2:0);
                    try{AircraftObserver.updateNativeCapabilities();}catch(Throwable unavailable){}
                    try{EnvironmentObserver.updateNativeCapabilities();}catch(Throwable unavailable){}
                    long rejectedSnapshot = rejected;
                    long attemptedSnapshot = attempted;
                    BridgeNative.state(attemptedSnapshot, rejectedSnapshot, epoch.get(), lastFlags);
                    Thread.sleep(100);
                }
            } catch (Throwable ignored) { disable(); }
        }, "PSX exporter observer");
        worker.setDaemon(true);
        worker.start();
    }
    static long activeForTest() { return active.get(); }
    static long epochForTest() { return epoch.get(); }
    // Worker-only read access to existing facts. No new callback work or observation state.
    static boolean metadataReady() { return !disabled && firstFrame; }
    static long metadataMutationVersion() { return version.get(); }
    static boolean metadataMutationActive() { return active.get() != 0; }
    public static long terrainEpoch() { return epoch.get(); }
    public static long terrainFrame() { return frameSequence; }
}
