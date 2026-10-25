package psx.exporter;

import java.util.concurrent.atomic.*;

/** Fixed pending/running-job witnesses. A reservation starts before the unchanged
 * native Thread.start and lasts through its entire qualified Runnable, including
 * sleeps. This rejects incomplete captures; it does not exclude future jobs or
 * authorize recall. Startup must initialize this storage before installing hooks. */
public final class FmcJobs {
    private static final int SLOTS=64;
    private static final AtomicLong sequence=new AtomicLong();
    private static final AtomicLongArray tokens=new AtomicLongArray(SLOTS);
    private static final AtomicIntegerArray depth=new AtomicIntegerArray(SLOTS);
    private static final AtomicLongArray admission=new AtomicLongArray(SLOTS);
    private static final AtomicReferenceArray<Thread> threads=new AtomicReferenceArray<>(SLOTS);
    private FmcJobs(){}
    static void initialize(){FmcExecSeam.initialize();} // Ordinary bootstrap/worker initialization.
    public static long reserve(Thread thread){
        return reserve(thread,0);
    }
    static long reserve(Thread thread,long jobAdmission){
        if(thread==null){FmcCoherence.failed();return 0;}
        long generation=sequence.incrementAndGet();
        if(generation<=0||generation>Long.MAX_VALUE>>>8){FmcCoherence.failed();return 0;}
        for(int index=0;index<SLOTS;index++){
            long token=(generation<<8)|(index+1);
            if(tokens.compareAndSet(index,0,token)){
                depth.set(index,0);admission.set(index,jobAdmission);threads.set(index,thread);FmcCoherence.enter();return token;
            }
        }
        FmcCoherence.failed();return 0;
    }
    public static long begin(){
        Thread current=Thread.currentThread();
        for(int index=0;index<SLOTS;index++)if(threads.get(index)==current){long token=tokens.get(index);if(token>0){int entered=depth.incrementAndGet(index);if(entered<=0||entered>65535)FmcCoherence.failed();return token;}}
        FmcCoherence.failed();return 0;
    }
    public static void release(long token){
        if(token<=0)return;int index=(int)(token&255)-1;
        if(index<0||index>=SLOTS){FmcCoherence.failed();return;}
        if(tokens.get(index)!=token)return;
        int entered=depth.get(index);
        if(entered>0){
            // Synchronous native run-to-run calls share the outer Thread's
            // reservation. A nested return must not expose a still-running
            // outer job as a quiet copy interval.
            if(threads.get(index)!=Thread.currentThread()||!depth.compareAndSet(index,entered,entered-1)){FmcCoherence.failed();return;}
            if(entered>1)return;
        }
        // Start failure and job completion may race; exactly one retires the
        // witness. An old token can never clear a reused slot's new reservation.
        if(tokens.compareAndSet(index,token,-token)){
            long gate=admission.getAndSet(index,0);
            Thread original=threads.get(index);
            threads.set(index,null);depth.set(index,0);FmcCoherence.leave();
            if(gate>0)FmcExclusion.releaseJob(gate);tokens.set(index,0);
            FmcExecSeam.jobRetired(original);
        }
    }
}
