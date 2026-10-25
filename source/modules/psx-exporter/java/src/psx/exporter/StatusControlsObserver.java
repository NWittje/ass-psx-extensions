package psx.exporter;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLongArray;

/** Four independently coherent groups; all callback storage is initialized at startup. */
public final class StatusControlsObserver {
    static volatile int capabilities;
    static volatile boolean nativeReady;
    private static volatile int failed;
    private static final VarHandle FAILED;
    static {try{FAILED=MethodHandles.lookup().findStaticVarHandle(StatusControlsObserver.class,"failed",int.class);}
        catch(ReflectiveOperationException error){throw new ExceptionInInitializerError(error);}}
    private static final AtomicLongArray writers=new AtomicLongArray(4),versions=new AtomicLongArray(4),owners=new AtomicLongArray(4),ids=new AtomicLongArray(32);
    private static final ThreadState[] threads=new ThreadState[32];
    private static final long[] before=new long[4],epochs=new long[4],sequences=new long[4],observed=new long[4],last=new long[4];
    private static final int[] models=new int[4],valid=new int[4];
    private static final boolean[] inside=new boolean[4],candidate=new boolean[4];
    private static final float[][] values=new float[4][32];
    private static long interval;
    private static final class ThreadState {final long[] depth=new long[4],changes=new long[4];volatile Thread thread;}
    static {for(int i=0;i<threads.length;i++)threads[i]=new ThreadState();}
    private StatusControlsObserver(){}
    static boolean available(){try{
        if(StatusControlsNative.abiVersion()!=1||StatusControlsNative.frequency()<=0||StatusControlsNative.qpc()<=0||StatusControlsNative.initialize(false))return false;
        StatusControlsNative.state(0,0,true);StatusControlsNative.publish(0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);return true;
    }catch(Throwable unavailable){return false;}}
    static void prepare(){
        disable(0);for(int g=0;g<4;g++){writers.incrementAndGet(g);writers.decrementAndGet(g);versions.incrementAndGet(g);versions.decrementAndGet(g);owners.compareAndSet(g,0,0);}
        ids.compareAndSet(0,0,0);VarHandle.fullFence();
    }
    static void prepareNative(){try{
        if(capabilities==0)return;
        if(!available())throw new LinkageError("IOS Status JNI1 unavailable");
        long frequency=StatusControlsNative.frequency();interval=frequency/15+(frequency%15==0?0:1);
        if(!StatusControlsNative.initialize(true))throw new LinkageError("IOS Status startup unavailable");nativeReady=true;
    }catch(Throwable unavailable){disable(15);nativeReady=false;}}
    public static void disable(int mask){FAILED.getAndBitwiseOr(mask);}
    private static ThreadState current(){
        Thread current=Thread.currentThread();long id=current.threadId();
        // Preserve this live owner before considering any lower retired slot.
        for(int i=0;i<32;i++)if(ids.get(i)==id)return threads[i];
        for(int i=0;i<32;i++){
            long found=ids.get(i);ThreadState state=threads[i];
            if(found==id)return state;
            if(found==0&&ids.compareAndSet(i,0,-id)){state.thread=current;ids.set(i,id);return state;}
            Thread previous=state.thread;
            // Termination detection establishes the old writer's completed lifetime.
            // A negative identity reserves the slot until its primitive state is reset.
            if(found>0&&previous!=null&&previous.threadId()==found&&!previous.isAlive()&&ids.compareAndSet(i,found,-id)){
                for(int g=0;g<4;g++)if(state.depth[g]!=0){ids.set(i,found);disable(15);return null;}
                for(int g=0;g<4;g++)state.changes[g]=0;
                state.thread=current;ids.set(i,id);return state;
            }
        }
        disable(15);return null;
    }
    public static void enter(int mask){ThreadState state=current();if(state==null)return;
        for(int g=0;g<4;g++)if((mask&(1<<g))!=0){writers.incrementAndGet(g);state.depth[g]++;versions.incrementAndGet(g);state.changes[g]++;}VarHandle.fullFence();}
    public static void leave(int mask){VarHandle.fullFence();ThreadState state=current();if(state==null)return;
        for(int g=0;g<4;g++)if((mask&(1<<g))!=0){versions.incrementAndGet(g);state.changes[g]++;writers.decrementAndGet(g);state.depth[g]--;}}
    public static void begin(int g,int family,int variant,boolean paused){
        if(!nativeReady||!Observer.metadataReady()||((capabilities&~failed)&(1<<g))==0)return;
        long thread=Thread.currentThread().threadId();if(owners.get(g)==0)owners.compareAndSet(g,0,thread);
        if(owners.get(g)!=thread)return;if(inside[g]){disable(1<<g);return;}
        ThreadState state=current();if(state==null)return;
        inside[g]=true;before[g]=versions.get(g)-state.changes[g];epochs[g]=Observer.terrainEpoch();models[g]=family-1;
        candidate[g]=writers.get(g)==state.depth[g];VarHandle.fullFence();
    }
    public static boolean copy(int g){
        if(!nativeReady||!inside[g]||!candidate[g]||owners.get(g)!=Thread.currentThread().threadId()||((capabilities&~failed)&(1<<g))==0)return false;
        long now=StatusControlsNative.qpc();if(now<=0||now<last[g]||now-last[g]<interval)return false;
        observed[g]=now;valid[g]=0;return true;
    }
    public static void value(int g,int slot,float value){values[g][slot]=value;valid[g]|=1<<slot;}
    public static void configuration(int g,int family,int variant,boolean paused){models[g]=family-1;}
    public static void publish(int g){
        VarHandle.fullFence();ThreadState state=current();if(state==null)return;
        if(!candidate[g]||writers.get(g)!=state.depth[g]||versions.get(g)-state.changes[g]!=before[g]||epochs[g]!=Observer.terrainEpoch()||((capabilities&~failed)&(1<<g))==0)return;
        if(sequences[g]==Long.MAX_VALUE){disable(1<<g);return;}
        float[] v=values[g];StatusControlsNative.publish(g,models[g],valid[g],++sequences[g],epochs[g],observed[g],Observer.terrainFrame(),
            v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],
            v[16],v[17],v[18],v[19],v[20],v[21],v[22],v[23],v[24],v[25],v[26],v[27],v[28],v[29],v[30],v[31]);last[g]=observed[g];
    }
    public static void end(int g){if(owners.get(g)==Thread.currentThread().threadId()){candidate[g]=false;inside[g]=false;}}
    public static void systemState(boolean loading){if(nativeReady)StatusControlsNative.state(capabilities&~failed,Observer.terrainEpoch(),loading);}
}
