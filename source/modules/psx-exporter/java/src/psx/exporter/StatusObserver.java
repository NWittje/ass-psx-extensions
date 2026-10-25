package psx.exporter;
import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReferenceArray;

/** Bounded multi-writer qualification guards; no waits, formatting or IPC. */
public final class StatusObserver {
    static volatile boolean nativeReady,qualified;
    private static volatile boolean failed;
    private static final AtomicLong writers=new AtomicLong(),versions=new AtomicLong(),copyOwner=new AtomicLong();
    private static final AtomicLongArray threads=new AtomicLongArray(32);
    private static final AtomicReferenceArray<Thread> lifetimes=new AtomicReferenceArray<>(32);
    private static final long[] depth=new long[32],changes=new long[32];
    private static long before,epoch,start,sequence,next,interval;
    private static int copySlot,lastFlags=-1;
    private StatusObserver() {}
    static void prepare(){writers.incrementAndGet();writers.decrementAndGet();versions.incrementAndGet();versions.decrementAndGet();copyOwner.compareAndSet(0,0);threads.compareAndSet(0,0,0);lifetimes.set(0,lifetimes.get(0));VarHandle.fullFence();}
    static void prepareNative(){try{if(StatusNative.abiVersion()!=1)throw new LinkageError("status ABI");interval=StatusNative.intervalTicks();if(interval<=0)throw new LinkageError("status clock");nativeReady=true;StatusNative.capabilities(qualified&&!failed?1:0);}catch(Throwable unavailable){disable();}}
    public static void disable(){failed=true;nativeReady=false;try{StatusNative.failed();}catch(Throwable ignored){}}
    private static int slot(){
        Thread current=Thread.currentThread();long id=current.threadId();
        // Preserve this live owner before considering any lower retired slot.
        for(int i=0;i<32;i++)if(threads.get(i)==id)return i;
        for(int i=0;i<32;i++){
            long found=threads.get(i);if(found==id)return i;
            if(found==0&&threads.compareAndSet(i,0,-id)){lifetimes.set(i,current);threads.set(i,id);return i;}
            Thread previous=lifetimes.get(i);
            if(found>0&&previous!=null&&previous.threadId()==found&&!previous.isAlive()&&threads.compareAndSet(i,found,-id)){
                if(depth[i]!=0){threads.set(i,found);disable();return -1;}
                changes[i]=0;lifetimes.set(i,current);threads.set(i,id);return i;
            }
        }
        disable();return -1;
    }
    public static void enter(){int i=slot();if(i<0)return;writers.incrementAndGet();depth[i]++;versions.incrementAndGet();changes[i]++;VarHandle.fullFence();}
    public static void leave(){VarHandle.fullFence();int i=slot();if(i<0)return;versions.incrementAndGet();changes[i]++;depth[i]--;writers.decrementAndGet();}
    public static boolean begin(){
        if(!nativeReady||failed||!qualified)return false;
        int i=slot();if(i<0||writers.get()!=depth[i])return false;
        long id=Thread.currentThread().threadId();if(!copyOwner.compareAndSet(0,id))return false;
        copySlot=i;epoch=Observer.epochForTest();before=versions.get()-changes[i];start=BridgeNative.qpc();VarHandle.fullFence();return true;
    }
    public static void publish(boolean active,boolean requested,boolean paused,int pauseMask,boolean loading){
        VarHandle.fullFence();int i=copySlot;
        if(copyOwner.get()!=Thread.currentThread().threadId()||writers.get()!=depth[i]||versions.get()-changes[i]!=before||epoch!=Observer.epochForTest()||failed)return;
        int flags=loading?0:1|512|4096|(active?1024:0)|(requested?2048:0)|(paused?8192:0)|((pauseMask&2)!=0?16384:0);
        if(start>=next||flags!=lastFlags){StatusNative.publish(++sequence,epoch,start,flags);next=start+interval;lastFlags=flags;}
    }
    public static void end(){if(copyOwner.get()==Thread.currentThread().threadId())copyOwner.set(0);}
}
