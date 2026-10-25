package psx.exporter;

import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicInteger;

/** Independent per-producer audio observations. All arrays are allocated before PSX starts. */
public final class AudioObserver {
    static volatile int capabilities, suppressionCapabilities;
    private static final AtomicInteger failed = new AtomicInteger();
    private static final AtomicLongArray writers = new AtomicLongArray(8);
    private static final AtomicLongArray versions = new AtomicLongArray(8);
    private static final AtomicLongArray owners = new AtomicLongArray(8);
    private static final boolean[] inside = new boolean[8];
    private static final boolean[] candidate = new boolean[8];
    private static final long[] before = new long[8], epochs = new long[8], sequences = new long[8], start = new long[8];
    private static final int[] families = new int[8], variants = new int[8], flags = new int[8];
    private static final float[][] values = new float[8][32];
    private static final int[] valid = new int[8];
    private AudioObserver() {}
    public static void disable(int mask) {failed.getAndAccumulate(mask,(a,b)->a|b);}
    static int observations() {return capabilities & ~failed.get();}
    static int suppression() {return suppressionCapabilities & observations();}
    private static final class ThreadState {
        final long[] depth=new long[8],changes=new long[8];
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
                for(int g=0;g<8;g++)if(state.depth[g]!=0){threadIds.set(i,found);disable(255);return null;}
                for(int g=0;g<8;g++)state.changes[g]=0;
                state.thread=current;threadIds.set(i,id);return state;
            }
        }
        disable(255);return null;
    }
    public static void enter(int mask) {
        ThreadState state=current();if(state==null)return;
        for(int g=0;g<8;g++)if((mask&(1<<g))!=0){writers.incrementAndGet(g);state.depth[g]++;versions.incrementAndGet(g);state.changes[g]++;}
        VarHandle.fullFence();
    }
    public static void leave(int mask) {
        VarHandle.fullFence();ThreadState state=current();if(state==null)return;
        for(int g=0;g<8;g++)if((mask&(1<<g))!=0){versions.incrementAndGet(g);state.changes[g]++;writers.decrementAndGet(g);state.depth[g]--;}
    }
    public static void begin(int g,int family,int variant,boolean paused) {
        if(!Observer.enabled||(observations()&(1<<g))==0)return;
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
    public static void systemState(boolean paused){if(Observer.enabled)BridgeNative.audioPaused(paused);}
    public static void publish(int g) {
        VarHandle.fullFence();
        ThreadState state=current();if(state==null)return;
        if(candidate[g] && writers.get(g)==state.depth[g] && versions.get(g)-state.changes[g]==before[g] && epochs[g]==Observer.epochForTest() && (observations()&(1<<g))!=0) {
            float[] v=values[g];
            // Primitive arguments avoid JNI array pinning/copying and per-observation allocation.
            BridgeNative.audioPublish(g,families[g],variants[g],flags[g],valid[g],sequences[g],epochs[g],start[g],
                v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],
                v[16],v[17],v[18],v[19],v[20],v[21],v[22],v[23],v[24],v[25],v[26],v[27],v[28],v[29],v[30],v[31]);
        }
    }
    public static void end(int g) {
        if(owners.get(g)==Thread.currentThread().threadId()){candidate[g]=false;inside[g]=false;}
    }
    // Pure scalar interpretation of an actual producer value, never a simulator method call.
    public static float positive(float value) {return value>0?1:0;}
}
