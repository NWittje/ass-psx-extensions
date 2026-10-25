package psx.exporter;

import java.util.concurrent.atomic.*;

/** A worker-owned, complete FMC input view for the exact native AFDS bank
 * consumer. Original bank parsing and every guidance consequence continue.
 * Only the qualified three FMC reads can use this view while publication is
 * closed. A final native calculation is owed if an old view was consumed. */
public final class FmcBackgroundAdmissionAfdsSnapshot {
    private static final long PRESENT=1L<<32;
    static final long FAILED=Long.MIN_VALUE;
    private static final class View {
        final long lifecycle,token;final int cruise,active,available;
        // 0 unused, 1 old view observed, 2 final calculation in progress,
        // 3 final calculation retired. Allocated only on the original worker.
        final AtomicInteger pending=new AtomicInteger();
        final AtomicReferenceArray<Thread> threads=new AtomicReferenceArray<>(64);
        final AtomicIntegerArray depth=new AtomicIntegerArray(64);
        final AtomicInteger readers=new AtomicInteger();
        volatile boolean complete;
        View(long lifecycle,long token,int cruise,int active,int available){this.lifecycle=lifecycle;this.token=token;this.cruise=cruise;this.active=active;this.available=available;}
    }
    private static volatile View view;private static long sequence;
    private FmcBackgroundAdmissionAfdsSnapshot(){}
    static synchronized long arm(long lifecycle,long[] values){
        require(lifecycle>0&&lifecycle==Observer.terrainEpoch()&&!FmcExclusion.admissionClosed(),"open current lifecycle");
        require(values!=null&&values.length==3&&values[0]==(int)values[0]&&(values[1]==0||values[1]==1)&&(values[2]==0||values[2]==1),"complete typed FMC view");
        require(view==null&&sequence<Long.MAX_VALUE,"one bound snapshot");long token=++sequence;view=new View(lifecycle,token,(int)values[0],(int)values[1],(int)values[2]);return token;
    }
    static void verify(long token,long[] values){var current=owned(token);require(values!=null&&values.length==3&&current.cruise==values[0]&&current.active==values[1]&&current.available==values[2],"prepublication FMC view changed");}
    /** Zero means the original native read must run. A present value keeps all
     * signed cruise-altitude bits, including the native unset sentinel. */
    public static long read(int slot){
        var current=view;boolean admitted=false;Thread thread=Thread.currentThread();if(current!=null)for(int i=0;i<64;i++)if(current.threads.get(i)==thread&&current.depth.get(i)>0){admitted=true;break;}
        if(!admitted&&(!FmcExclusion.admissionClosed()||FmcExclusion.currentAdmitted()))return 0;
        if(!admitted||current.lifecycle!=Observer.terrainEpoch()||slot<0||slot>2){FmcExclusion.failed();return FAILED;}
        current.pending.compareAndSet(0,1);
        int value=slot==0?current.cruise:slot==1?current.active:current.available;return PRESENT|(value&0xffffffffL);
    }
    /** A complete original readGlob lifetime may consume the immutable OLD
     * view while closed. It does not acquire permission to read live content.
     * Counter publication precedes the final gate check, sealing reopen races. */
    public static int enterConsumer(){
        if(!FmcExclusion.admissionClosed()||FmcExclusion.currentAdmitted())return 0;var current=view;
        if(current==null||current.lifecycle!=Observer.terrainEpoch()){FmcExclusion.failed();return 0;}
        Thread thread=Thread.currentThread();for(int i=0;i<64;i++)if(current.threads.get(i)==thread){int depth=current.depth.get(i);if(depth<=0||depth==Integer.MAX_VALUE){FmcExclusion.failed();return 0;}current.depth.set(i,depth+1);return i+1;}
        for(int i=0;i<64;i++)if(current.threads.compareAndSet(i,null,thread)){
            current.depth.set(i,1);current.readers.incrementAndGet();
            if(!FmcExclusion.admissionClosed()||current.lifecycle!=Observer.terrainEpoch()||current.pending.get()==4){leaveConsumer(current,i+1);return 0;}
            return i+1;
        }
        FmcExclusion.failed();return 0;
    }
    public static void leaveConsumer(int ticket){
        leaveConsumer(view,ticket);
    }
    private static void leaveConsumer(View current,int ticket){
        if(ticket<=0)return;int slot=ticket-1;
        if(current==null||slot>=64||current.threads.get(slot)!=Thread.currentThread()){FmcExclusion.failed();return;}int count=current.depth.get(slot);
        if(count<=0){FmcExclusion.failed();return;}if(count>1){current.depth.set(slot,count-1);return;}
        current.depth.set(slot,0);current.threads.set(slot,null);if(current.readers.decrementAndGet()<0)FmcExclusion.failed();
    }
    /** Marks coherent NEW reconstruction/ordinary EXEC completion. Admission
     * must still reopen before the original AFDS cadence can reconcile it. */
    static void complete(long token){var current=owned(token);require(FmcExclusion.admissionClosed(),"complete under closed publication");current.complete=true;}
    public static int claim(){
        if(FmcExclusion.admissionClosed())return 0;var current=view;if(current==null||!current.complete)return 0;
        if(current.lifecycle!=Observer.terrainEpoch()){FmcExclusion.failed();return 0;}if(current.readers.get()!=0)return 0;
        int ticket=FmcExclusion.enter();if(ticket<=0)return 0;
        if(current.pending.compareAndSet(1,2))return ticket;FmcExclusion.leave(ticket);return 0;
    }
    public static void retire(int ticket,boolean success){
        if(ticket<=0)return;try{var current=view;if(current==null||current.pending.get()!=2){FmcExclusion.failed();return;}
            if(!success||current.lifecycle!=Observer.terrainEpoch())FmcExclusion.failed();current.pending.set(3);
        }finally{FmcExclusion.leave(ticket);}
    }
    static boolean reconciled(long token){var current=owned(token);return !FmcExclusion.admissionClosed()&&current.complete&&current.readers.get()==0&&(current.pending.get()==0||current.pending.get()==3)&&FmcExclusion.available();}
    /** Seals retirement before either immutable fallback is cleared. A late
     * entry which had observed the old closed gate rechecks this state and
     * retires through its captured View without consuming OLD values. */
    static void prepareRelease(long token){var current=view;require(current!=null&&current.token==token&&!FmcExclusion.admissionClosed()&&current.readers.get()==0,"open retired consumers");for(;;){int before=current.pending.get();require(before!=2,"final calculation must retire");if(before==4||current.pending.compareAndSet(before,4))return;}}
    static void clear(long token){var current=view;require(current!=null&&current.token==token&&!FmcExclusion.admissionClosed(),"open snapshot owner");if(current.pending.get()!=4)prepareRelease(token);view=null;}
    public static void failed(){FmcExclusion.failed();}
    private static View owned(long token){var current=view;require(current!=null&&current.token==token&&current.lifecycle==Observer.terrainEpoch(),"same snapshot lifecycle/token");return current;}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC AFDS snapshot "+why);}
}
