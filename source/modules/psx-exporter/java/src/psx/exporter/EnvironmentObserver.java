package psx.exporter;

import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReferenceArray;

/** Fixed coherent copies only. Conversion, transport and persistence belong to the worker. */
public final class EnvironmentObserver {
    static volatile int capabilities;
    static volatile boolean nativeReady;
    private static volatile boolean failed;
    private static final AtomicLong writers=new AtomicLong(),versions=new AtomicLong(),copyOwner=new AtomicLong();
    private static final AtomicLongArray threadIds=new AtomicLongArray(32);
    private static final AtomicReferenceArray<Thread> lifetimes=new AtomicReferenceArray<>(32);
    private static final long[] depth=new long[32],changes=new long[32];
    private static final double[] values=new double[32],configured=new double[32];
    private static long before,epoch,start,sequence,utc,next,interval;
    private static int flags,valid,configuredMask,copySlot;
    private EnvironmentObserver() {}
    static void prepare(){writers.incrementAndGet();writers.decrementAndGet();versions.incrementAndGet();versions.decrementAndGet();copyOwner.compareAndSet(0,0);threadIds.compareAndSet(0,0,0);lifetimes.set(0,lifetimes.get(0));VarHandle.fullFence();}
    public static void disable(){failed=true;}
    static volatile boolean manualWeatherFailed;
    static int observations(){return failed?0:capabilities&(manualWeatherFailed?1:3);}
    static void manualWeatherFailure(boolean value){manualWeatherFailed=value;updateNativeCapabilities();}
    static void prepareNative(){try{if(EnvironmentNative.abiVersion()!=1)throw new LinkageError("environment ABI");interval=EnvironmentNative.intervalTicks();if(interval<=0)throw new LinkageError("environment clock");EnvironmentNative.capabilities(observations());EnvironmentNative.state(0,false);nativeReady=true;}catch(Throwable failure){disable();nativeReady=false;}try{EnvironmentContextObserver.prepare();}catch(Throwable ignored){}try{WeatherChangeObserver.prepare();}catch(Throwable ignored){}try{EnvironmentVisibilityObserver.prepare();}catch(Throwable ignored){}}
    static void updateNativeCapabilities(){if(nativeReady)try{EnvironmentNative.capabilities(observations());}catch(Throwable failure){disable();nativeReady=false;}}
    private static int slot(){
        Thread current=Thread.currentThread();long id=current.threadId();
        // Preserve this live owner before considering any lower retired slot.
        for(int i=0;i<32;i++)if(threadIds.get(i)==id)return i;
        for(int i=0;i<32;i++){
            long found=threadIds.get(i);if(found==id)return i;
            if(found==0&&threadIds.compareAndSet(i,0,-id)){lifetimes.set(i,current);threadIds.set(i,id);return i;}
            Thread previous=lifetimes.get(i);
            // Termination establishes the previous writer's completed lifetime.
            // Reserve the slot before resetting its thread-owned counters.
            if(found>0&&previous!=null&&previous.threadId()==found&&!previous.isAlive()&&threadIds.compareAndSet(i,found,-id)){
                if(depth[i]!=0){threadIds.set(i,found);disable();return -1;}
                changes[i]=0;lifetimes.set(i,current);threadIds.set(i,id);return i;
            }
        }
        disable();return -1;
    }
    public static void enter(){int i=slot();if(i<0)return;writers.incrementAndGet();depth[i]++;versions.incrementAndGet();changes[i]++;VarHandle.fullFence();}
    public static void leave(){VarHandle.fullFence();int i=slot();if(i<0)return;versions.incrementAndGet();changes[i]++;depth[i]--;writers.decrementAndGet();}
    public static void systemState(boolean loading){if(nativeReady)EnvironmentNative.state(Observer.epochForTest(),loading);}
    public static void begin(){
        // Pose-subscription enablement is independent of environment observation.
        if(!nativeReady||!Observer.firstFrame||observations()==0)return;
        int i=slot();if(i<0||writers.get()!=depth[i])return;
        long id=Thread.currentThread().threadId();if(!copyOwner.compareAndSet(0,id))return;
        long now=BridgeNative.qpc();if(now<next){copyOwner.set(0);return;}
        copySlot=i;epoch=Observer.epochForTest();before=versions.get()-changes[i];start=now;valid=0;configuredMask=0;sequence++;try{EnvironmentContextObserver.reset();}catch(Throwable ignored){}VarHandle.fullFence();
    }
    public static boolean copy(){int i=copySlot;return copyOwner.get()==Thread.currentThread().threadId()&&writers.get()==depth[i]&&versions.get()-changes[i]==before&&epoch==Observer.epochForTest()&&observations()!=0;}
    public static void value(int slot,double value){values[slot]=value;valid|=1<<slot;}
    static long phaseStart(){return start;}
    public static void configured(int slot,double value){configured[slot]=value;configuredMask|=1<<slot;}
    public static void clock(long millis,boolean paused,int freeze){utc=millis;flags=1|16|(paused?2:0)|(freeze!=0?4:0);if(paused){values[0]=0;valid|=1;}}
    public static void publish(){
        VarHandle.fullFence();int i=copySlot;
        if(writers.get()!=depth[i]||versions.get()-changes[i]!=before||epoch!=Observer.epochForTest()||observations()==0)return;
        if((valid&(1<<18))!=0&&values[18]==9999)flags|=8;
        double[] v=values,c=configured;
        try{EnvironmentContextObserver.publish(sequence,epoch,start,versions.get());}catch(Throwable ignored){}
        EnvironmentNative.publish(flags,valid,configuredMask,sequence,epoch,start,versions.get(),Observer.terrainFrame(),utc,
            v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15],v[16],v[17],v[18],v[19],
            c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],c[8],c[9],c[10],c[11],c[12],c[13],c[14],c[15],c[16],c[17],c[18],c[19],c[22]);
        next=BridgeNative.qpc()+interval;
    }
    public static void end(){if(copyOwner.get()==Thread.currentThread().threadId())copyOwner.set(0);}
}
