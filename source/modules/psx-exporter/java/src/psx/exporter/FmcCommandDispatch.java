package psx.exporter;

import java.awt.EventQueue;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;

/** One outstanding command on a separately qualified ordinary EDT path.
 * This worker-side mechanism neither qualifies an FMC caller nor excludes its
 * native writers. A queued expiry cancels before any action. An action already
 * started keeps its slot and receipt until it genuinely finishes: a worker
 * timeout never authorizes replay, rollback, or disposal of an executing job.
 * No physics/render observer enters this dispatcher or waits here. */
final class FmcCommandDispatch {
    interface Authority {void check()throws Throwable;}
    interface Action {void run()throws Throwable;}
    interface Scheduler {void schedule(Runnable job)throws Throwable;}
    enum Phase {QUEUED,RUNNING,REJECTED,RETURNED,INDETERMINATE}
    record Observation(Phase phase,boolean actionBegan,Throwable failure,Throwable finalCheckFailure,Throwable schedulingFailure){}
    private static final int QUEUED=0,RUNNING=1,DONE=2,CANCELLED=3;
    private final Scheduler scheduler;
    private final AtomicReference<Job> outstanding=new AtomicReference<>();
    private volatile boolean disabled;

    FmcCommandDispatch(){this(EventQueue::invokeLater);}
    FmcCommandDispatch(Scheduler scheduler){this.scheduler=Objects.requireNonNull(scheduler);}

    final class Job implements Runnable {
        final long operation,deadline;
        final Authority authority;
        final Action action;
        final AtomicInteger state=new AtomicInteger(QUEUED);
        volatile boolean began,returned;
        volatile Throwable failure,finalCheckFailure,schedulingFailure;
        private Job(long operation,long deadline,Authority authority,Action action){
            this.operation=operation;this.deadline=deadline;
            this.authority=Objects.requireNonNull(authority);this.action=Objects.requireNonNull(action);
        }
        public void run(){
            if(!state.compareAndSet(QUEUED,RUNNING))return;
            try{
                if(!EventQueue.isDispatchThread()){disabled=true;throw new IllegalStateException("FMC command dispatcher thread");}
                if(disabled)throw new IllegalStateException("FMC command dispatcher unavailable");
                if(deadline-System.nanoTime()<=0)throw new IllegalStateException("FMC command expired before admission");
                authority.check();
                // Recheck after qualification: a slow authority check cannot
                // admit a command whose own execution deadline has elapsed.
                if(deadline-System.nanoTime()<=0)throw new IllegalStateException("FMC command expired before action");
                began=true;action.run();returned=true;
            }catch(Throwable original){failure=original;}
            finally{
                if(began)try{authority.check();}catch(Throwable diagnostic){finalCheckFailure=diagnostic;}
                state.set(DONE);
            }
        }
        Observation observe(){
            return switch(state.get()){
                case QUEUED->new Observation(Phase.QUEUED,false,null,null,schedulingFailure);
                case RUNNING->new Observation(Phase.RUNNING,began,null,null,schedulingFailure);
                case CANCELLED->new Observation(Phase.REJECTED,false,schedulingFailure,null,schedulingFailure);
                case DONE->new Observation(!began?Phase.REJECTED:returned&&finalCheckFailure==null&&schedulingFailure==null?Phase.RETURNED:Phase.INDETERMINATE,began,failure!=null?failure:schedulingFailure,finalCheckFailure,schedulingFailure);
                default->throw new IllegalStateException("FMC command state");
            };
        }
    }

    private static void worker(){if(EventQueue.isDispatchThread())throw new IllegalStateException("FMC command worker required");}
    /** Allocate and schedule from the service worker only. Operation identity
     * and native authority are producer-provided, never imported from a file.
     * This dispatcher supplies bounded execution, not the service's durable
     * operation ledger or native mutation permission. */
    Job submit(long operation,long deadline,Authority authority,Action action){
        worker();
        if(operation<=0||deadline-System.nanoTime()<=0||disabled)throw new IllegalStateException("FMC command admission");
        Job job=new Job(operation,deadline,authority,action);
        if(!outstanding.compareAndSet(null,job))throw new IllegalStateException("FMC command already outstanding");
        try{scheduler.schedule(job);}
        catch(Throwable original){
            // A scheduler can enqueue and then fail. Cancellation wins only
            // while QUEUED; an executing action remains owned and discoverable.
            // Publish the scheduling fault BEFORE terminal cancellation. A
            // concurrently completing action owns its own failure fields, so
            // neither cause can overwrite the native or final-authority cause.
            job.schedulingFailure=original;
            job.state.compareAndSet(QUEUED,CANCELLED);
        }
        return job;
    }
    Observation await(Job job,long waitDeadline){
        worker();requireOwned(job);
        for(;;){
            int state=job.state.get();
            if(state==DONE||state==CANCELLED)return job.observe();
            long remaining=Math.min(waitDeadline-System.nanoTime(),job.deadline-System.nanoTime());
            if(remaining<=0||Thread.currentThread().isInterrupted()){
                if(job.state.compareAndSet(QUEUED,CANCELLED))return job.observe();
                if(job.state.get()==DONE)return job.observe();
                return new Observation(Phase.INDETERMINATE,job.began,null,null,job.schedulingFailure);
            }
            LockSupport.parkNanos(Math.min(remaining,1_000_000));
        }
    }
    Observation lookup(Job job){worker();requireOwned(job);return job.observe();}
    void release(Job job){
        worker();requireOwned(job);
        int state=job.state.get();
        if(state!=DONE&&state!=CANCELLED)throw new IllegalStateException("FMC command still executing");
        if(!outstanding.compareAndSet(job,null))throw new IllegalStateException("FMC command ownership changed");
    }
    private void requireOwned(Job job){if(job==null||outstanding.get()!=job)throw new IllegalStateException("FMC command receipt identity");}
}
