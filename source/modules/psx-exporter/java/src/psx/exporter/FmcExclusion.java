package psx.exporter;

import java.util.concurrent.atomic.*;

/** Admission gate for explicitly qualified, deferrable FMC callbacks. Closing
 * admission never waits on a native thread. The command worker waits for the
 * already admitted callbacks and pending native jobs before committing.
 *
 * A callback may be deferred only before it consumes its native input. This
 * helper supplies no such qualification and must not guard whole frame loops.
 * Nested calls already admitted before closure continue to their ordinary end.
 * Storage is allocated at startup, not on callback entry. */
public final class FmcExclusion {
    private static final int SLOTS=64;
    private static final long CLOSED=Long.MIN_VALUE;
    private static final AtomicLong state=new AtomicLong();
    private static final AtomicLong sequence=new AtomicLong();
    private static final AtomicLong owner=new AtomicLong();
    private static volatile long ownerLifecycle;
    private static final AtomicReferenceArray<Thread> threads=new AtomicReferenceArray<>(SLOTS);
    private static final AtomicIntegerArray depth=new AtomicIntegerArray(SLOTS);
    private static final AtomicLongArray jobs=new AtomicLongArray(SLOTS);
    private static final int COMMAND_TICKET=Integer.MAX_VALUE;
    private static final AtomicReference<Thread> command=new AtomicReference<>();
    private static final AtomicReference<Thread> launcher=new AtomicReference<>();
    private static final AtomicLong handoff=new AtomicLong();
    private static final AtomicInteger commandDepth=new AtomicInteger();
    private static volatile boolean failed;
    interface ReleasePrerequisite {void verify()throws Throwable;}
    private record ReleaseCheck(long token,long lifecycle,ReleasePrerequisite prerequisite){}
    private static final AtomicReference<ReleaseCheck> releaseCheck=new AtomicReference<>();
    private FmcExclusion(){}
    static void initialize(){} // Ordinary bootstrap/worker class initialization.

    /** Positive ticket admits work; zero defers it. Negative means the optional
     * gate failed while open: native work continues, but recall is unavailable.
     * A closed gate always defers new work, including after a helper fault. */
    public static int enter(){
        Thread current=Thread.currentThread();
        // A replaced situation is outside the old operation. Its native polls
        // must not be deferred by that operation's still-closed gate. The
        // worker loses mutation/rollback authority and releases its own token.
        if((state.get()&CLOSED)!=0&&ownerLifecycle!=Observer.terrainEpoch()){
            failed();return -1;
        }
        if(command.get()==current){
            if(owner.get()<=0||ownerLifecycle!=Observer.terrainEpoch()||(state.get()&CLOSED)==0||failed){failed();return 0;}
            int old=commandDepth.getAndIncrement();
            if(old<0||old==Integer.MAX_VALUE){commandDepth.decrementAndGet();failed();return 0;}
            return COMMAND_TICKET;
        }
        for(int i=0;i<SLOTS;i++)if(threads.get(i)==current){
            int nesting=depth.get(i);
            if(nesting<0||nesting==0&&jobs.get(i)==0||nesting==Integer.MAX_VALUE){failed();return (state.get()&CLOSED)!=0?0:-1;}
            depth.set(i,nesting+1);return i+1;
        }
        if((state.get()&CLOSED)!=0)return 0;
        if(failed)return -1;
        for(int i=0;i<SLOTS;i++)if(threads.compareAndSet(i,null,current)){
            // The atomic CAS linearizes admission against request(). A
            // slot claimed just before closure cannot slip into the commit.
            int result=addAdmission(false);
            if(result!=1){threads.set(i,null);return result;}
            depth.set(i,1);return i+1;
        }
        failed();return (state.get()&CLOSED)!=0?0:-1;
    }
    public static void leave(int ticket){
        if(ticket<=0)return;
        if(ticket==COMMAND_TICKET){
            if(command.get()!=Thread.currentThread()||commandDepth.getAndDecrement()<=0){failed();}
            return;
        }
        int i=ticket-1;
        if(i>=SLOTS||threads.get(i)!=Thread.currentThread()){failed();return;}
        int nesting=depth.get(i);
        if(nesting<=0){failed();return;}
        if(nesting>1){depth.set(i,nesting-1);return;}
        depth.set(i,0);
        // A started or reserved native job owns its slot through its complete
        // run, including sleeps. A nested writer return does not retire it.
        if(jobs.get(i)!=0)return;
        threads.set(i,null);
        if((state.getAndDecrement()&Long.MAX_VALUE)==0)failed();
    }
    public static void failed(){failed=true;}
    static boolean available(){return !failed;}
    /** Bound native input producers can retain their complete raw arguments.
     * Already admitted jobs and the command's own bank writes continue normally.
     * A replacement lifecycle has priority over an old operation's gate. */
    static boolean deferInput(){
        if((state.get()&CLOSED)!=0&&ownerLifecycle!=Observer.terrainEpoch())return false;
        return ((state.get()&CLOSED)!=0||FmcJobAdmission.replayPending())&&!currentAdmitted();
    }
    static boolean currentAdmitted(){
        Thread current=Thread.currentThread();if(command.get()==current)return true;
        for(int i=0;i<SLOTS;i++)if(threads.get(i)==current&&(depth.get(i)>0||jobs.get(i)>0))return true;
        return false;
    }
    static boolean admissionClosed(){return (state.get()&CLOSED)!=0;}
    static int active(){return (int)(state.get()&Long.MAX_VALUE);}
    private static int addAdmission(boolean alreadyAdmitted){
        for(int attempt=0;attempt<SLOTS;attempt++){
            long current=state.get();
            if((current&CLOSED)!=0&&!alreadyAdmitted)return 0;
            if((current&Long.MAX_VALUE)>=SLOTS)break;
            if(state.compareAndSet(current,current+1))return 1;
        }
        failed();return (state.get()&CLOSED)!=0?0:-1;
    }

    /** Reserve the ORIGINAL native Thread before start. A job already admitted
     * through its parent may start after closure and is drained, not deferred.
     * Zero requires retained admission before input consumption. The caller
     * must not start a zero-ticket job during publication. */
    static long reserveJob(Thread thread){
        if(thread==null){failed();return -1;}
        Thread current=Thread.currentThread();boolean admitted=command.get()==current;
        for(int i=0;i<SLOTS;i++){
            if(threads.get(i)==thread){failed();return -1;}
            if(threads.get(i)==current&&(depth.get(i)>0||jobs.get(i)>0))admitted=true;
        }
        if((state.get()&CLOSED)!=0&&!admitted)return 0;
        // A helper fault invalidates recall authority, but cannot reopen the
        // closed window by letting a new native job start during publication.
        // Retain it until the operation worker releases admission.
        if(failed)return -1;
        long serial=sequence.incrementAndGet();
        if(serial<=0||serial>Long.MAX_VALUE>>>8){failed();return -1;}
        for(int i=0;i<SLOTS;i++)if(threads.compareAndSet(i,null,thread)){
            int result=addAdmission(admitted);
            if(result!=1){threads.set(i,null);return result;}
            long token=(serial<<8)|(i+1);depth.set(i,0);jobs.set(i,token);return token;
        }
        failed();return -1;
    }
    static void releaseJob(long token){
        if(token<=0)return;int i=(int)(token&255)-1;
        if(i<0||i>=SLOTS||jobs.get(i)!=token||depth.get(i)!=0){failed();return;}
        if(!jobs.compareAndSet(i,token,-token)){failed();return;}
        jobs.set(i,0);threads.set(i,null);
        if((state.getAndDecrement()&Long.MAX_VALUE)==0)failed();
    }

    /** Serialized worker use only. No injected callback requests ownership. */
    static long request(){
        if(failed||releaseCheck.get()!=null)throw new IllegalStateException("FMC exclusion unavailable or prior release obligation retained");
        long token=sequence.incrementAndGet();
        if(token<=0||!owner.compareAndSet(0,token))throw new IllegalStateException("FMC exclusion already owned");
        ownerLifecycle=Observer.terrainEpoch();
        long previous=state.getAndUpdate(value->value|CLOSED);
        if((previous&CLOSED)!=0){owner.compareAndSet(token,0);failed();throw new IllegalStateException("FMC exclusion ownership incoherent");}
        return token;
    }
    static boolean quiescent(long token){
        // Job reservations and every qualified mutation remain part of the
        // separate copy witness. Closing callback admission alone is not enough.
        return token>0&&owner.get()==token&&ownerLifecycle==Observer.terrainEpoch()&&!failed&&state.get()==CLOSED&&command.get()==null&&FmcCoherence.beginCopy()>=0;
    }
    static boolean closedAndRetired(long token){
        return token>0&&owner.get()==token&&state.get()==CLOSED&&command.get()==null&&FmcCoherence.active()==0;
    }
    /** The same retired gate after its qualified dispatcher claims the
     * command permit; no original job/callback may have become active. */
    static boolean commandQuiescent(long token){
        return commandActive(token)&&state.get()==CLOSED&&commandDepth.get()==0&&FmcCoherence.active()==0;
    }
    interface CommandAction {void run()throws Throwable;}
    /** Called only by the qualified ordinary-thread dispatcher, after native
     * callbacks and jobs have retired. The token does not choose a native
     * method. It allows that dispatch's nested qualified callbacks while all
     * other threads still defer before consuming their inputs. Worker-only
     * action is allocated/prepared on the worker; the command seam itself
     * allocates no permit. Callback enter/leave remain bounded and allocation-free.
     * This mechanism does not qualify a dispatcher or a callback frontier. */
    static void runCommand(long token,CommandAction action)throws Throwable{
        if(action==null||!quiescent(token))throw new IllegalStateException("FMC command exclusion authority");
        Thread current=Thread.currentThread();
        if(!command.compareAndSet(null,current))throw new IllegalStateException("FMC command already executing");
        Throwable primary=null;
        try{
            if(owner.get()!=token||ownerLifecycle!=Observer.terrainEpoch()||state.get()!=CLOSED||failed||commandDepth.get()!=0||FmcCoherence.beginCopy()<0)
                throw new IllegalStateException("FMC command admission changed");
            action.run();
            if(ownerLifecycle!=Observer.terrainEpoch())throw new IllegalStateException("FMC lifecycle changed inside command");
        }catch(Throwable original){primary=original;throw original;}
        finally{
            // The native job can begin before Thread.start returns. An explicit
            // handoff retires the launcher's permit before start; its finally
            // must not clear/check the child's now independent command scope.
            boolean transferred=handoff.get()==token&&launcher.get()==current;
            boolean coherent=transferred||commandDepth.get()==0&&command.compareAndSet(current,null);
            if(transferred){handoff.set(0);launcher.set(null);}
            if(!coherent){failed();command.compareAndSet(current,null);if(primary==null)throw new IllegalStateException("FMC command callback closure");}
        }
    }
    static boolean handoffCommand(long token,Thread original){
        Thread current=Thread.currentThread();boolean reserved=false;
        for(int i=0;i<SLOTS;i++)if(threads.get(i)==original&&jobs.get(i)>0)reserved=true;
        if(!reserved||!commandActive(token)||commandDepth.get()!=0||state.get()!=(CLOSED|1L)||handoff.get()!=0)return false;
        launcher.set(current);handoff.set(token);
        if(command.compareAndSet(current,null))return true;
        handoff.set(0);launcher.set(null);failed();return false;
    }
    /** The qualified ordinary EXEC job is the mutation seam. Its original
     * Thread was reserved before start; only that one reservation may remain.
     * Other readers/writers stay outside admission. No callback waits and no
     * permit object is allocated here. The worker prepares the action. */
    static void runReservedCommand(long token,CommandAction action)throws Throwable{
        Thread current=Thread.currentThread();boolean ownJob=false;
        for(int i=0;i<SLOTS;i++)if(threads.get(i)==current&&jobs.get(i)>0&&depth.get(i)==0)ownJob=true;
        if(action==null||!ownJob||token<=0||owner.get()!=token||ownerLifecycle!=Observer.terrainEpoch()||failed||state.get()!=(CLOSED|1L)||FmcCoherence.active()!=1
            ||!command.compareAndSet(null,current))throw new IllegalStateException("FMC ordinary EXEC job authority");
        Throwable primary=null;
        try{
            if(owner.get()!=token||ownerLifecycle!=Observer.terrainEpoch()||failed||state.get()!=(CLOSED|1L)||commandDepth.get()!=0)
                throw new IllegalStateException("FMC EXEC job admission changed");
            action.run();
            if(ownerLifecycle!=Observer.terrainEpoch())throw new IllegalStateException("FMC lifecycle changed inside EXEC publication");
        }catch(Throwable original){primary=original;throw original;}
        finally{
            boolean coherent=commandDepth.get()==0&&command.compareAndSet(current,null);
            if(!coherent){failed();command.compareAndSet(current,null);if(primary==null)throw new IllegalStateException("FMC EXEC job closure");}
        }
    }
    public static boolean commandActive(long token){
        return command.get()==Thread.currentThread()&&owner.get()==token&&ownerLifecycle==Observer.terrainEpoch()&&(state.get()&CLOSED)!=0&&!failed;
    }
    /** A native EXEC job retains its reservation after the initial load permit
     * retires. Recheck that actual job, operation and situation at continuation
     * boundaries; elapsed native sleeps do not renew old mutation authority. */
    static boolean jobAuthority(long token){
        if(token<=0||owner.get()!=token||ownerLifecycle!=Observer.terrainEpoch()||failed||(state.get()&CLOSED)==0)return false;
        return reservedJob(token);
    }
    /** Identity survives lifecycle invalidation until the original job retires.
     * It distinguishes a stale operation child from unrelated native work. */
    static boolean reservedJob(long token){
        if(token<=0||owner.get()!=token||(state.get()&CLOSED)==0)return false;
        Thread current=Thread.currentThread();
        for(int i=0;i<SLOTS;i++)if(threads.get(i)==current&&jobs.get(i)>0)return true;
        return false;
    }
    /** Worker-only registration before the first mutation. This grants no
     * command or rollback permit. A failed same-lifecycle operation retains
     * its captured prerequisite until raw current preimages are again exact. */
    static void installReleasePrerequisite(long token,ReleasePrerequisite prerequisite){
        if(prerequisite==null||!available()||!closedAndRetired(token)||ownerLifecycle!=Observer.terrainEpoch())throw new IllegalStateException("FMC release prerequisite registration authority");
        long lifecycle=ownerLifecycle;var check=new ReleaseCheck(token,lifecycle,prerequisite);
        if(!releaseCheck.compareAndSet(null,check))throw new IllegalStateException("FMC release prerequisite already installed");
        if(!available()||!closedAndRetired(token)||ownerLifecycle!=lifecycle||lifecycle!=Observer.terrainEpoch()){failed();throw new IllegalStateException("FMC release prerequisite registration changed");}
    }
    static void release(long token){
        if(token<=0||owner.get()!=token||command.get()!=null)throw new IllegalStateException("FMC exclusion release identity");
        var check=releaseCheck.get();
        if(check!=null){
            if(check.token()!=token||check.lifecycle()!=ownerLifecycle)throw new IllegalStateException("FMC release prerequisite token/lifecycle identity");
            // A new situation must not be forced back to the old navigation
            // preimage. Same-life release, including failure, first drains every
            // old content job and validates without normal mutation authority.
            if(check.lifecycle()==Observer.terrainEpoch()){
                if(!closedAndRetired(token))throw new IllegalStateException("FMC release prerequisite old work still active");
                try{check.prerequisite().verify();}catch(Throwable cause){failed();throw new IllegalStateException("FMC captured preservation prerequisite prevents reopen",cause);}
                if(owner.get()!=token||command.get()!=null||ownerLifecycle!=check.lifecycle()||!closedAndRetired(token))throw new IllegalStateException("FMC preservation release identity/drain changed");
            }

        }
        state.getAndUpdate(value->value&Long.MAX_VALUE);
        if(!owner.compareAndSet(token,0))throw new IllegalStateException("FMC exclusion release ownership");
        if(check!=null&&!releaseCheck.compareAndSet(check,null)){failed();throw new IllegalStateException("FMC preservation release prerequisite changed");}
    }
}


