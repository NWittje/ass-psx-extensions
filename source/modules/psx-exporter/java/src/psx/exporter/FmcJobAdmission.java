package psx.exporter;

import java.util.concurrent.atomic.*;

/** Optional recall admission for finite native jobs. The ORIGINAL Thread,
 * Runnable, construction context and priority survive deferral. Qualified
 * native input producers publish into fixed MPSC storage; the service
 * worker resumes start through Thread.start, never by invoking run directly.
 * Already admitted jobs remain in exclusion/copy witnesses for their entire
 * lifetime. Deferred jobs are not counted as work to drain.
 *
 * This helper is NOT installed by the current read-only source layer. Before
 * using it the profile must qualify pre-consumption admission, producer
 * ordering, queue capacity and delayed-start error handling for every caller. */
public final class FmcJobAdmission {
    static final int CAPACITY=64;
    private static final Thread[] deferred=new Thread[CAPACITY];
    private static final int JOB=1,QI=2,QS=3;
    private static final int[] kinds=new int[CAPACITY],values=new int[CAPACITY],origins=new int[CAPACITY];
    private static final char[] modes=new char[CAPACITY];
    private static final Object[] receivers=new Object[CAPACITY];
    private static final String[] texts=new String[CAPACITY];
    private static final Inputs[] inputScopes=new Inputs[CAPACITY];
    private static final AtomicLongArray ready=new AtomicLongArray(CAPACITY);
    private static final AtomicLong tail=new AtomicLong();
    private static volatile long head;
    private static volatile Throwable startFailure;
    private static volatile Thread resuming;
    interface InputReplay {
        void qi(Object original,int value,char mode,int origin)throws Throwable;
        void qs(Object original,String value,char mode,int origin)throws Throwable;
    }
    private record Inputs(long lifecycle,Object[] qi,Object[] qs,InputReplay replay){}
    private static volatile Inputs inputs;
    private FmcJobAdmission(){}
    /** Complete helper storage initialization before optional native hooks can
     * run. This starts no worker, transport, lease or installed PSX class. */
    static void initialize(){FmcExclusion.initialize();FmcJobs.initialize();FmcExecSeam.initialize();}
    /** Selected recall contract: the caller supplies a quiet edit interval.
     * There is no bank-write replay and no changed native input consumption.
     * Existing jobs are reserved and drained at their original lifetime. A
     * new outside job during closure violates that interval and revokes recall
     * authority before its original native start; it is neither swallowed nor
     * queued with invented input/exception semantics. Late violation requires
     * the transaction's rejection/uncertain-completion path, never success. */
    public static long prepareQuietStart(Thread original){
        long gate=FmcExclusion.reserveJob(original);
        if(gate<=0){FmcExclusion.failed();return nativeFallback(original);}
        long witness=FmcJobs.reserve(original,gate);
        if(witness==0){FmcExclusion.failed();FmcExclusion.releaseJob(gate);return -1;}
        FmcExecSeam.jobAdmitted(original);
        return witness;
    }
    public static long prepareStart(Thread original){
        if(original==null){FmcExclusion.failed();return -1;}
        // Reopening the mutation gate is not completion of ordered replay.
        // New outside jobs must join the retained stream. A running original
        // job may start its own child normally; otherwise lifetime ordering
        // could deadlock a native parent waiting for that child.
        if(replayPending()&&!FmcExclusion.currentAdmitted()){
            if(enqueue(JOB,original,null,null,0,'\0',0,null))return 0;
            FmcExclusion.failed();return nativeFallback(original);
        }
        long gate=FmcExclusion.reserveJob(original);
        if(gate!=0){
            if(gate<0)return nativeFallback(original);
            long witness=FmcJobs.reserve(original,gate);
            if(witness==0){FmcExclusion.failed();FmcExclusion.releaseJob(gate);return -1;}
            return witness;
        }
        if(enqueue(JOB,original,null,null,0,'\0',0,null))return 0;
        // Capacity/CAS-budget violation invalidates optional recall. It must
        // never pretend that an original Thread was retained when it was not.
        FmcExclusion.failed();return nativeFallback(original);
    }
    private static boolean enqueue(int kind,Thread thread,Object receiver,String text,int value,char mode,int origin,Inputs scope){
        for(int attempt=0;attempt<CAPACITY;attempt++){
            long next=tail.get();
            if(next<0||next==Long.MAX_VALUE||next-head>=CAPACITY)break;
            if(!tail.compareAndSet(next,next+1))continue;
            int slot=(int)(next%CAPACITY);deferred[slot]=thread;kinds[slot]=kind;
            receivers[slot]=receiver;texts[slot]=text;values[slot]=value;modes[slot]=mode;origins[slot]=origin;
            inputScopes[slot]=scope;
            // A stalled earlier producer cannot be overtaken by a later
            // completed reservation. The worker checks this exact ticket.
            ready.set(slot,next+1);return true;
        }
        return false;
    }
    /** Worker binds receiver identities only after the installed profile proves
     * types, aliases, writer coverage and legal ordinary native replay bridges.
     * This mechanism by itself neither installs a hook nor qualifies recall. */
    static void bindInputs(long lifecycle,Object[] qi,Object[] qs,InputReplay replay){
        if(lifecycle<=0||lifecycle!=Observer.terrainEpoch()||qi==null||qs==null||replay==null
            ||qi.length+qs.length==0||qi.length+qs.length>CAPACITY||deferredCount()!=0||resuming!=null)
            throw new IllegalArgumentException("FMC native input binding");
        Object[] integers=qi.clone(),strings=qs.clone();
        var seen=new java.util.IdentityHashMap<Object,Boolean>();
        for(Object bank:integers)if(bank==null||seen.put(bank,true)!=null)throw new IllegalArgumentException("FMC input receiver alias");
        for(Object bank:strings)if(bank==null||seen.put(bank,true)!=null)throw new IllegalArgumentException("FMC input receiver alias");
        if(replay instanceof FmcInputBridgeAccess binding)binding.receivers(integers,strings);
        if(lifecycle!=Observer.terrainEpoch())throw new IllegalStateException("FMC input binding lifecycle replaced");
        inputs=new Inputs(lifecycle,integers,strings,replay);
    }
    private static Inputs selected(Object receiver,boolean string){
        Inputs current=inputs;
        if(current==null||current.lifecycle!=Observer.terrainEpoch()||!FmcExclusion.deferInput())return null;
        for(Object bank:string?current.qs:current.qi)if(bank==receiver)return current;
        return null;
    }
    /** Eligible native call sites discard the ORIGINAL write return value.
     * Reads, used return values and unresolved receivers must never use these
     * void-event APIs. No freshness is consumed, result synthesized or value
     * normalized; the original native write validates retained arguments. */
    public static boolean retainQi(Object original,int value,char mode,int origin){
        if(mode!='w')return false;Inputs scope=selected(original,false);if(scope==null)return false;
        if(enqueue(QI,null,original,null,value,mode,origin,scope))return true;
        FmcExclusion.failed();return false;
    }
    public static boolean retainQs(Object original,String value,char mode,int origin){
        if(mode!='w')return false;Inputs scope=selected(original,true);if(scope==null)return false;
        if(enqueue(QS,null,original,value,0,mode,origin,scope))return true;
        FmcExclusion.failed();return false;
    }
    private static long nativeFallback(Thread original){
        long witness=FmcJobs.reserve(original,0);
        // Identity survives failed authority; it grants no mutation permission.
        FmcExecSeam.jobAdmitted(original);
        // Zero has one exclusive meaning at generated start sites: the
        // ORIGINAL Thread is retained in deferred[]. A failed source witness
        // has retained nothing and must not accidentally suppress native work.
        return witness==0?-1:witness;
    }
    /** Authored worker convenience; generated native callers keep their original
     * start instruction and exception propagation outside exporter guards. */
    static void start(Thread original){
        long witness=prepareStart(original);if(witness==0)return;
        try{original.start();}catch(Throwable failure){if(witness>0)FmcJobs.release(witness);throw failure;}
    }
    private static void startNow(Thread original,long gate){
        long witness=0;
        try{
            if(gate<0)FmcExclusion.failed();
            witness=FmcJobs.reserve(original,Math.max(0,gate));
            if(witness==0){FmcExclusion.failed();FmcExclusion.releaseJob(gate);gate=0;}
            original.start();
        }catch(Throwable nativeFailure){
            if(witness>0)FmcJobs.release(witness);
            else if(gate>0)FmcExclusion.releaseJob(gate);
            // Exact exception identity survives immediate start and worker
            // resumption. The operation ledger must retain delayed failures.
            throw nativeFailure;
        }
    }
    /** Sole service worker, at most one original start per call. Queue remains
     * retained while admission is closed; no native callback waits or drains. */
    static boolean drainOne(){
        // Starting a retained Thread and immediately applying a later bank
        // write could change the scratchpad before that Thread consumes it.
        // Only the worker waits for its complete ordinary lifetime. Profiles
        // must prove that this ordering cannot wait for another queued job.
        if(resuming!=null){if(resuming.isAlive())return false;resuming=null;}
        if(FmcExclusion.admissionClosed())return false;
        long current=head;int slot=(int)(current%CAPACITY);
        if(current==tail.get()||ready.get(slot)!=current+1)return false;
        Thread original=deferred[slot];
        int kind=kinds[slot];
        long gate=0;if(kind==JOB){gate=FmcExclusion.reserveJob(original);if(gate==0)return false;}
        Object receiver=receivers[slot];String text=texts[slot];int value=values[slot],origin=origins[slot];char mode=modes[slot];
        Inputs bound=inputScopes[slot];
        if(kind!=JOB&&(bound==null||bound.lifecycle!=Observer.terrainEpoch())){
            // Preserve the event; a worker cannot apply old bound receivers to
            // a replacement situation by inference. Qualification must supply
            // its exact ordinary lifecycle disposition before enabling recall.
            FmcExclusion.failed();throw new IllegalStateException("FMC retained input lifecycle disposition required");
        }
        // Publish the in-flight original before retiring its queue ticket.
        // Producers cannot observe an empty stream between dequeue and start.
        if(kind==JOB)resuming=original;
        deferred[slot]=null;receivers[slot]=null;texts[slot]=null;inputScopes[slot]=null;kinds[slot]=0;ready.set(slot,0);head=current+1;
        try{
            if(kind==JOB)startNow(original,gate);
            else if(kind==QI)bound.replay.qi(receiver,value,mode,origin);
            else if(kind==QS)bound.replay.qs(receiver,text,mode,origin);
            else throw new IllegalStateException("FMC retained input kind");
        }catch(Throwable nativeFailure){if(kind==JOB&&!original.isAlive())resuming=null;startFailure=nativeFailure;FmcExclusion.failed();FmcJobAdmission.<RuntimeException>propagate(nativeFailure);}
        return true;
    }
    @SuppressWarnings("unchecked") private static <T extends Throwable> void propagate(Throwable original)throws T{throw (T)original;}
    static int deferredCount(){return (int)(tail.get()-head);}
    static boolean replayPending(){return head!=tail.get()||resuming!=null;}
    static Throwable startFailure(){return startFailure;}
}
