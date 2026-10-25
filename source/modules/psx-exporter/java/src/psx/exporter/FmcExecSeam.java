package psx.exporter;

import java.util.concurrent.atomic.*;

/** One prepared FMC action on the ORIGINAL ordinary native EXEC job, just
 * before its original run body. Content publication precedes native EXEC;
 * its delayed calculations and activation remain normal PSX execution.
 * This is neither the transaction journal nor evidence of successful recall.
 * The concrete adapter must validate lifecycle and complete write authority
 * inside its preallocated action and require later fresh restored readback.
 * The currently installed source layer does not arm or install this seam. */
public final class FmcExecSeam {
    enum Phase { PREPARED, LOADING, NATIVE_EXEC, RETURNED, FAILED }
    enum FinishStage { PROPAGATE, RECONCILE }
    interface LoadAction {void run(int logicalSide)throws Throwable;}
    static final class Request implements FmcExclusion.CommandAction {
        final long token,deadline,lifecycle;final LoadAction action;
        final AtomicReference<Phase> phase=new AtomicReference<>(Phase.PREPARED);
        volatile Throwable failure;
        volatile Thread nativeThread;
        volatile int logicalSide=-1;
        final AtomicReferenceArray<Thread> transportThreads=new AtomicReferenceArray<>(64);
        final AtomicIntegerArray transportDepth=new AtomicIntegerArray(64);
        final AtomicReferenceArray<Thread> children=new AtomicReferenceArray<>(64);
        final AtomicIntegerArray retiredChildren=new AtomicIntegerArray(64);
        volatile boolean hasTransport,requiresFinishing;
        final AtomicReference<Thread> finisher=new AtomicReference<>();
        final AtomicInteger finishStage=new AtomicInteger();
        Request(long token,long deadline,LoadAction action){this.token=token;this.deadline=deadline;this.action=action;this.lifecycle=Observer.terrainEpoch();FinishStage.PROPAGATE.ordinal();Thread.State.TERMINATED.ordinal();}
        public void run()throws Throwable{action.run(logicalSide);}
    }
    public static void launching(Thread original,long token){
        var request=pending.get();
        if(request==null||request.token!=token||request.phase.get()!=Phase.PREPARED||request.nativeThread!=null||!FmcExclusion.commandActive(token))
            throw new IllegalStateException("FMC native EXEC launch identity");
        request.nativeThread=original;
    }
    public static void handoffLauncher(Thread original){
        var request=pending.get();
        if(request!=null&&request.nativeThread==original&&!FmcExclusion.handoffCommand(request.token,original))FmcExclusion.failed();
    }
    public static void startFailed(Thread original,Throwable failure){
        var request=pending.get();
        if(request!=null&&request.nativeThread==original){request.failure=failure;request.phase.set(Phase.FAILED);}
    }
    private static final AtomicReference<Request> pending=new AtomicReference<>();
    private FmcExecSeam(){}
    static void initialize(){} // Ordinary bootstrap/worker class initialization.
    static Request prepare(long token,long deadline,LoadAction action){
        if(action==null||deadline-System.nanoTime()<=0||!FmcExclusion.quiescent(token))throw new IllegalStateException("FMC EXEC preparation authority");
        var request=new Request(token,deadline,action);
        if(!pending.compareAndSet(null,request))throw new IllegalStateException("FMC EXEC still outstanding");return request;
    }
    /** Mark the staged completion obligation on the original LOAD job before
     * its first native publication. Request/enum storage was prepared by the
     * worker; this callback only validates current primitive/identity state. */
    static void requireFinishing(long token){
        var request=pending.get();
        if(request==null||request.token!=token||request.nativeThread!=Thread.currentThread()
            ||request.phase.get()!=Phase.LOADING||!live(request)||!FmcExclusion.commandActive(token)
            ||request.finishStage.get()!=0||request.finisher.get()!=null){failed(token,ABORT);throw ABORT;}
        request.requiresFinishing=true;
    }
    /** Both stages run on the actual EDT under a separately claimed ordinary
     * dispatcher command permit. Parent EXEC and prior original children must
     * already have retired; the callback never waits for them. */
    static Request enterFinisher(long token,FinishStage stage){
        var request=pending.get();Thread current=Thread.currentThread();
        if(request!=null&&request.token==token&&request.requiresFinishing&&request.phase.get()==Phase.RETURNED&&!live(request)){failed(token,ABORT);throw ABORT;}
        if(stage==null||request==null||request.token!=token||!request.requiresFinishing
            ||request.phase.get()!=Phase.RETURNED||!live(request)||!java.awt.EventQueue.isDispatchThread()
            ||!FmcExclusion.commandQuiescent(token)||request.nativeThread==null||request.nativeThread.isAlive()
            ||!childrenRetired(request)||request.finisher.get()!=null)throw FINISH_DENIED;
        int before=stage==FinishStage.PROPAGATE?0:2,active=before+1;
        if(request.finishStage.get()!=before||!request.finisher.compareAndSet(null,current))throw FINISH_DENIED;
        if(!request.finishStage.compareAndSet(before,active)){
            request.finisher.compareAndSet(current,null);throw FINISH_DENIED;
        }
        if(!live(request)||request.phase.get()!=Phase.RETURNED||!FmcExclusion.commandActive(token)){
            request.finisher.compareAndSet(current,null);failed(token,ABORT);throw ABORT;
        }
        return request;
    }
    /** Retirement needs the recorded identity, even after deadline/lifecycle
     * loss. A primary native throwable keeps its exact identity; cleanup must
     * not replace it. Only a successful stage advances its monotonic marker. */
    static void leaveFinisher(Request request,FinishStage stage,Throwable primary){
        Thread current=Thread.currentThread();int active=stage==FinishStage.PROPAGATE?1:stage==FinishStage.RECONCILE?3:-1;
        boolean owned=request!=null&&pending.get()==request&&request.finisher.get()==current
            &&request.finishStage.get()==active;
        if(!owned){if(request!=null)failed(request.token,primary==null?ABORT:primary);if(primary==null)throw ABORT;return;}
        boolean valid=primary==null&&request.phase.get()==Phase.RETURNED&&live(request)&&FmcExclusion.commandActive(request.token);
        if(valid)valid=request.finishStage.compareAndSet(active,active+1);
        request.finisher.compareAndSet(current,null);
        if(!valid){failed(request.token,primary==null?ABORT:primary);if(primary==null)throw ABORT;}
    }
    private static boolean live(Request request){return request.deadline-System.nanoTime()>0&&request.lifecycle==Observer.terrainEpoch();}
    private static boolean finisherIdentity(Request request,Thread current){return request.finisher.get()==current;}
    private static boolean finisherAuthority(Request request){
        int stage=request.finishStage.get();return request.requiresFinishing&&(stage==1||stage==3)
            &&request.phase.get()==Phase.RETURNED&&live(request)&&finisherIdentity(request,Thread.currentThread())
            &&FmcExclusion.commandActive(request.token);
    }
    private static boolean childrenRetired(Request request){
        for(int i=0;i<64;i++){Thread original=request.children.get(i);if(original==null)continue;if(original.isAlive())return false;if(request.retiredChildren.get(i)==0&&original.getState()!=Thread.State.TERMINATED)return false;}return true;
    }
    /** Exact new-content load interval, for qualified owned-bank output only.
     * Existing/manual transfers and ordinary EXEC publication keep native
     * output. This predicate never grants mutation authority by itself. */
    static boolean loadingOwned(){
        var request=pending.get();
        if(request==null||request.phase.get()!=Phase.LOADING||request.deadline-System.nanoTime()<=0)return false;
        if(request.nativeThread==Thread.currentThread())return FmcExclusion.commandActive(request.token);
        return FmcExclusion.jobAuthority(request.token);
    }
    /** Current navigation output remains private until its exact preimage has
     * been restored after EXEC. Only the original operation and reserved
     * children can withhold the current radio/IRS banks' intermediate native
     * publication. */
    static boolean preservingOwned(){
        var request=pending.get();if(request==null||request.deadline-System.nanoTime()<=0)return false;
        var phase=request.phase.get();if(phase!=Phase.LOADING&&phase!=Phase.NATIVE_EXEC&&phase!=Phase.RETURNED)return false;
        if(phase==Phase.LOADING&&request.nativeThread==Thread.currentThread())return FmcExclusion.commandActive(request.token);
        return finisherAuthority(request)||FmcExclusion.jobAuthority(request.token);
    }
    public static void beforeNative(long token,int logicalSide)throws Throwable{
        var request=pending.get();
        if(logicalSide<0||logicalSide>1||request==null||request.token!=token||request.nativeThread!=Thread.currentThread()
            ||!request.phase.compareAndSet(Phase.PREPARED,Phase.LOADING))throw new IllegalStateException("FMC EXEC request identity");
        try{
            // PSX can route a physical CDU EXEC to the opposite logical FMC.
            // The ACT/MOD planner must use the actual dispatched job, not an
            // earlier navigation-selection sample or the button's side.
            request.logicalSide=logicalSide;
            if(request.deadline-System.nanoTime()<=0)throw new IllegalStateException("FMC recall expired before native publication");
            FmcExclusion.runReservedCommand(token,request);request.phase.set(Phase.NATIVE_EXEC);
        }
        catch(Throwable original){request.failure=original;request.phase.set(Phase.FAILED);throw original;}
    }
    public static void returned(long token){
        var request=pending.get();
        if(!continuationAllowed(token))return;
        if(request==null||request.token!=token||!request.phase.compareAndSet(Phase.NATIVE_EXEC,Phase.RETURNED))FmcExclusion.failed();
    }
    /** Ordinary operation-owned Thread only. Manual token0 jobs bypass this
     * helper in their generated code. A lost/expired operation cannot continue
     * into native activation after the original job's delayed work. */
    public static boolean continuationAllowed(long token){
        var request=pending.get();
        boolean valid=request!=null&&request.token==token&&request.nativeThread==Thread.currentThread()
            &&request.phase.get()==Phase.NATIVE_EXEC&&request.deadline-System.nanoTime()>0&&FmcExclusion.jobAuthority(token);
        if(valid)return true;
        if(request!=null&&request.token==token){
            if(request.failure==null)request.failure=new IllegalStateException("FMC native EXEC continuation lost operation/lifecycle authority");
            request.phase.set(Phase.FAILED);
        }
        FmcExclusion.failed();return false;
    }
    public static void requireContinuation(long token){if(!continuationAllowed(token))throw ABORT;}
    /** Checks inside native callees cannot use their parameter list to recover
     * the operation token. Exact original Thread identity and its reserved
     * children supply the scope. Quiescence before prepare excludes older jobs;
     * outside closed-window starts cannot obtain these reservations. */
    public static void mutationCheckpoint(){
        var request=pending.get();Thread current=Thread.currentThread();
        // Generated reconstruction cannot borrow the ordinary native fallback.
        // Identity alone grants no permit: the existing phase/deadline/lifecycle
        // check below still requires the exact reserved job or staged command.
        if(request==null||request.nativeThread!=current&&!child(request,current)
            &&!FmcExclusion.reservedJob(request.token)&&!finisherIdentity(request,current))throw ABORT;
        mutationCheckpoint(request);
    }
    /** Only original native bodies use this checkpoint. A source-qualified
     * transport error continuation retains its own work even after recall
     * authority expires. Exporter reconstruction kernels use the strict check. */
    public static void nativeMutationCheckpoint(){
        var request=pending.get();
        if(request!=null&&request.hasTransport){Thread current=Thread.currentThread();for(int i=0;i<64;i++)if(request.transportThreads.get(i)==current&&request.transportDepth.get(i)>0)return;}
        mutationCheckpoint(request);
    }
    /** Exact Network.toPrint error branches only. Slots are allocated with the
     * request on the worker; native entry performs bounded identity/CAS work. */
    public static int enterTransportContinuation(){
        var request=pending.get();Thread current=Thread.currentThread();
        if(request==null||request.nativeThread!=current&&!child(request,current)&&!FmcExclusion.reservedJob(request.token)&&!finisherAuthority(request))return 0;
        for(int i=0;i<64;i++)if(request.transportThreads.get(i)==current){int depth=request.transportDepth.get(i);if(depth==Integer.MAX_VALUE)throw new IllegalStateException("FMC transport continuation depth");request.transportDepth.set(i,depth+1);request.hasTransport=true;return i+1;}
        for(int i=0;i<64;i++)if(request.transportThreads.compareAndSet(i,null,current)){request.transportDepth.set(i,1);request.hasTransport=true;return i+1;}
        throw new IllegalStateException("FMC transport continuation capacity");
    }
    public static void leaveTransportContinuation(int ticket){
        if(ticket==0)return;var request=pending.get();int at=ticket-1;
        if(request==null||at<0||at>=64||request.transportThreads.get(at)!=Thread.currentThread()||request.transportDepth.get(at)<=0)throw new IllegalStateException("FMC transport continuation identity");
        int depth=request.transportDepth.decrementAndGet(at);if(depth==0)request.transportThreads.set(at,null);
        // This is reached only after the original native return/throw path.
        // Even expired/lifecycle-lost scopes record failure; no fresh permit is
        // required to retire original transport work or mark it uncertain.
        failed(request.token,TRANSPORT_FAILURE);
    }
    private static void mutationCheckpoint(Request request){
        if(request==null||request.nativeThread!=Thread.currentThread()&&!child(request,Thread.currentThread())&&!FmcExclusion.reservedJob(request.token)&&!finisherIdentity(request,Thread.currentThread()))return;
        if(request.phase.get()==Phase.PREPARED){
            // The run-entry check precedes its load seam. Only this already
            // reserved original job may reach beforeNative; it has not started
            // its original native body yet.
            if(request.deadline-System.nanoTime()>0&&FmcExclusion.jobAuthority(request.token))return;
        }else if(request.phase.get()==Phase.LOADING){
            if(request.deadline-System.nanoTime()>0&&FmcExclusion.commandActive(request.token))return;
        }else if((request.phase.get()==Phase.NATIVE_EXEC||request.phase.get()==Phase.RETURNED)
            &&request.deadline-System.nanoTime()>0&&(FmcExclusion.jobAuthority(request.token)||finisherAuthority(request)))return;
        failed(request.token,ABORT);throw ABORT;
    }
    private static final Error FINISH_DENIED=new Error("FMC staged finisher identity/phase not admitted",null,false,false){};
    private static final Error ABORT=new Error("FMC native mutation lost operation authority",null,false,false){};
    private static final Error TRANSPORT_FAILURE=new Error("FMC native transport error continuation completed",null,false,false){};
    public static void failed(long token,Throwable original){
        var request=pending.get();
        if(request!=null&&request.token==token){if(request.failure==null||request.failure==TRANSPORT_FAILURE&&original!=ABORT&&original!=TRANSPORT_FAILURE)request.failure=original;request.phase.set(Phase.FAILED);}
        FmcExclusion.failed();
    }
    /** Record an original child before start. Its identity must survive a
     * terminal admission reopen while delayed native work is still retiring. */
    static void jobAdmitted(Thread original){
        var request=pending.get();if(request==null||original==request.nativeThread)return;Thread current=Thread.currentThread();
        // Identity retention is separate from mutation authority. A deadline
        // or situation can change between checkpoint/reservation/start; even
        // that already admitted child must remain recognizable after reopen.
        if(request.nativeThread!=current&&!child(request,current)&&!finisherIdentity(request,current)||!FmcExclusion.currentAdmitted())return;
        if(request.hasTransport)for(int i=0;i<64;i++)if(request.transportThreads.get(i)==current&&request.transportDepth.get(i)>0)return;
        for(int i=0;i<64;i++)if(request.children.get(i)==original){request.retiredChildren.set(i,0);return;}
        for(int i=0;i<64;i++){
            Thread previous=request.children.get(i);if(previous!=null&&(request.retiredChildren.get(i)==0||previous.isAlive()))continue;
            if(request.children.compareAndSet(i,previous,original)){request.retiredChildren.set(i,0);return;}
        }
        failed(request.token,ABORT);
    }
    /** Generated original start paths register identity after reservation,
     * including a lower witness fallback. This grants no late scope/authority. */
    public static void jobStarting(Thread original){jobAdmitted(original);}
    /** Only the exact original Thread.start Throwable handler calls this.
     * A still-NEW thread is retired because its original start failed; mere
     * !isAlive before start is never a retirement proof. */
    public static void jobStartFailed(Thread original){jobRetired(original);}
    static void jobRetired(Thread original){var request=pending.get();if(request==null)return;for(int i=0;i<64;i++)if(request.children.get(i)==original){request.retiredChildren.set(i,1);return;}}
    private static boolean child(Request request,Thread thread){for(int i=0;i<64;i++)if(request.children.get(i)==thread)return true;return false;}
    static void release(Request request){
        Phase phase=request.phase.get();
        boolean childrenRetired=childrenRetired(request);
        if(phase==Phase.LOADING||phase==Phase.NATIVE_EXEC||request.finisher.get()!=null||request.requiresFinishing&&phase==Phase.RETURNED&&request.finishStage.get()!=4||!childrenRetired||request.nativeThread!=null&&(request.nativeThread.isAlive()||FmcExclusion.admissionClosed()&&!FmcExclusion.closedAndRetired(request.token))
            ||!pending.compareAndSet(request,null))throw new IllegalStateException("FMC EXEC still running or identity changed");
    }
}
