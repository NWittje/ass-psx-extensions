package psx.exporter;

import java.lang.invoke.VarHandle;
import java.lang.invoke.MethodHandles;
import java.util.concurrent.atomic.AtomicLongArray;

/** Independent per-producer aircraft observations. All arrays are allocated before PSX starts. */
public final class AircraftObserver {
    static volatile int capabilities; static volatile boolean nativeReady;
    private static volatile int failed;
    private static final VarHandle FAILED;
    static {try{FAILED=MethodHandles.lookup().findStaticVarHandle(AircraftObserver.class,"failed",int.class);}catch(ReflectiveOperationException failure){throw new ExceptionInInitializerError(failure);}}
    private static final AtomicLongArray writers = new AtomicLongArray(6);
    private static final AtomicLongArray versions = new AtomicLongArray(6);
    private static final AtomicLongArray owners = new AtomicLongArray(6);
    private static final boolean[] inside = new boolean[6];
    private static final boolean[] candidate = new boolean[6];
    private static final long[] before = new long[6], epochs = new long[6], sequences = new long[6], start = new long[6];
    private static final int[] families = new int[6], variants = new int[6], flags = new int[6];
    private static final float[][] values = new float[6][32];
    private static final int[] valid = new int[6];
    private AircraftObserver() {}
    /** Resolve first-use atomic operations before any PSX callback can run. */
    static void prepare() {
        disable(0);
        for(int g=0;g<6;g++){
            writers.incrementAndGet(g);writers.decrementAndGet(g);
            versions.incrementAndGet(g);versions.decrementAndGet(g);
            owners.compareAndSet(g,0,0);owners.get(g);
        }
        threadIds.compareAndSet(0,0,0);threadIds.get(0);VarHandle.fullFence();
    }
    public static void disable(int mask) {FAILED.getAndBitwiseOr(mask);}
    static int observations() {return capabilities & ~failed;}
    /** Worker-only linkage resolution; the empty observation cannot supply a baseline. */
    static void prepareNative() {
        try {
            if(BridgeNative.aircraftAbiVersion()!=1)throw new LinkageError("aircraft ABI");
            BridgeNative.qpc();BridgeNative.aircraftState(0,false);
            BridgeNative.aircraftPublish(0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
            BridgeNative.aircraftCapabilities(observations());nativeReady=true;
        } catch(Throwable unavailable) {disable(63);nativeReady=false;}
    }
    static void updateNativeCapabilities() {
        if(nativeReady)try{BridgeNative.aircraftCapabilities(observations());}
        catch(Throwable unavailable){disable(63);nativeReady=false;}
    }
    private static final class ThreadState {
        final long[] depth=new long[6],changes=new long[6];
        volatile Thread thread;
    }
    private static final AtomicLongArray threadIds=new AtomicLongArray(32);
    private static final ThreadState[] threads=new ThreadState[32];
    static {for(int i=0;i<threads.length;i++)threads[i]=new ThreadState();}
    private static ThreadState current() {
        Thread current=Thread.currentThread();long id=current.threadId();
        // Preserve this live owner before considering any lower retired slot.
        for(int i=0;i<32;i++)if(threadIds.get(i)==id)return threads[i];
        for(int i=0;i<32;i++){
            long found=threadIds.get(i);ThreadState state=threads[i];if(found==id)return state;
            if(found==0&&threadIds.compareAndSet(i,0,-id)){state.thread=current;threadIds.set(i,id);return state;}
            Thread previous=state.thread;
            if(found>0&&previous!=null&&previous.threadId()==found&&!previous.isAlive()&&threadIds.compareAndSet(i,found,-id)){
                for(int g=0;g<6;g++)if(state.depth[g]!=0){threadIds.set(i,found);disable(63);return null;}
                for(int g=0;g<6;g++)state.changes[g]=0;
                state.thread=current;threadIds.set(i,id);return state;
            }
        }
        disable(63);return null;
    }
    public static void enter(int mask) {
        ThreadState state=current();if(state==null)return;
        for(int g=0;g<6;g++)if((mask&(1<<g))!=0){writers.incrementAndGet(g);state.depth[g]++;versions.incrementAndGet(g);state.changes[g]++;}
        VarHandle.fullFence();
    }
    public static void leave(int mask) {
        VarHandle.fullFence();ThreadState state=current();if(state==null)return;
        for(int g=0;g<6;g++)if((mask&(1<<g))!=0){versions.incrementAndGet(g);state.changes[g]++;writers.decrementAndGet(g);state.depth[g]--;}
    }
    public static void begin(int g,int family,int variant,boolean paused) {
        if(!nativeReady||!Observer.enabled||!Observer.firstFrame||(observations()&(1<<g))==0)return;
        long thread=Thread.currentThread().threadId();
        if(owners.get(g)==0)owners.compareAndSet(g,0,thread);
        if(owners.get(g)!=thread)return;
        if(inside[g]){disable(1<<g);return;}
        ThreadState state=current();if(state==null)return;
        inside[g]=true;sequences[g]++;before[g]=versions.get(g)-state.changes[g];epochs[g]=Observer.epochForTest();
        families[g]=family;variants[g]=variant;flags[g]=paused?3:1;
        candidate[g]=Observer.enabled && writers.get(g)==state.depth[g] && versions.get(g)-state.changes[g]==before[g];
        VarHandle.fullFence();
    }
    public static boolean copy(int g) {
        if((observations()&(1<<g))==0 || !inside[g] || owners.get(g)!=Thread.currentThread().threadId() || !candidate[g])return false;
        valid[g]=0;start[g]=BridgeNative.qpc();return true;
    }
    public static void value(int g,int slot,float v) {values[g][slot]=v;valid[g]|=1<<slot;}
    public static void configuration(int g,int family,int variant,boolean paused){families[g]=family;variants[g]=variant;flags[g]=paused?3:1;}
    public static void systemState(boolean loading){if(nativeReady)BridgeNative.aircraftState(Observer.epochForTest(),loading);}
    public static void publish(int g) {
        VarHandle.fullFence();
        ThreadState state=current();if(state==null)return;
        if(candidate[g] && writers.get(g)==state.depth[g] && versions.get(g)-state.changes[g]==before[g] && epochs[g]==Observer.epochForTest() && (observations()&(1<<g))!=0) {
            float[] v=values[g];
            // Primitive arguments avoid JNI array pinning/copying and per-observation allocation.
            BridgeNative.aircraftPublish(g,families[g],variants[g],flags[g],valid[g],sequences[g],epochs[g],start[g],versions.get(g),Observer.terrainFrame(),
                v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],v[20],v[21],v[22],v[23],v[24],v[25],v[26],v[27],v[28],v[29],v[30],v[31]);
        }
    }
    public static void end(int g) {
        if(owners.get(g)==Thread.currentThread().threadId()){candidate[g]=false;inside[g]=false;}
    }
    // Pure scalar interpretation of an actual producer value, never a simulator method call.
    public static float positive(float value) {return value>0?1:0;}
}
