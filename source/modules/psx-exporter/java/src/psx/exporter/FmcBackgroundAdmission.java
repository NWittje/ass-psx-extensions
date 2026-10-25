package psx.exporter;

/** Access witnesses complement deferrable native poll entries. An unexpected
 * reader or writer must retain its return/exception/continuation, while making
 * an optional recall uncertain before it observes or changes live content.
 * Positive tickets participate in worker quiescence. This helper never waits,
 * allocates, reads a native bank, or substitutes a native return value. */
public final class FmcBackgroundAdmission {
    private record Scope(long lifecycle,long identity,long previous,boolean binding){}
    private static volatile Scope scope;
    private static long scopeSequence;
    private FmcBackgroundAdmission(){}
    public static int enter(){return witness(FmcExclusion.enter());}
    public static int enter(Object receiver){return witness(FmcAdmissionReceivers.enter(receiver));}
    static synchronized long nextScope(){
        if(FmcExclusion.admissionClosed()||scopeSequence==Long.MAX_VALUE)throw new IllegalStateException("FMC ownership sequence/frozen binding");
        return ++scopeSequence;
    }
    static void publishScope(long lifecycle,long identity){
        if(lifecycle<=0||lifecycle!=Observer.terrainEpoch()||identity<=0||FmcExclusion.admissionClosed())throw new IllegalStateException("FMC ownership publication lifecycle");
        scope=new Scope(lifecycle,identity,0,false);
    }
    static void beginScope(long lifecycle,long identity){
        if(lifecycle<=0||lifecycle!=Observer.terrainEpoch()||identity<=0||FmcExclusion.admissionClosed())throw new IllegalStateException("FMC ownership binding lifecycle");
        var before=scope;scope=new Scope(lifecycle,identity,before!=null&&before.lifecycle==lifecycle?before.identity:0,true);
    }
    static long currentScope(){var current=scope;return current==null?0:current.identity;}
    /** Native child metadata inherits only current recall authority. Unrelated
     * launches keep their original path; this primitive check allocates nothing. */
    public static long operationScope(){var current=scope;return current!=null&&current.lifecycle==Observer.terrainEpoch()&&FmcExecSeam.preservingOwned()?current.identity:0;}
    public static boolean suppressBankOutput(long identity){
        var current=scope;
        return identity>0&&current!=null&&identity==current.identity&&current.lifecycle==Observer.terrainEpoch()&&FmcExecSeam.loadingOwned();
    }
    public static boolean suppressBankOutput(long identity,Object receiver){
        return FmcNavigationOutputs.suppress(receiver)||suppressBankOutput(identity);
    }
    public static int enterTag(long identity){
        var current=scope;
        if(current==null)return witness(FmcExclusion.enter());
        if(current.lifecycle!=Observer.terrainEpoch()){
            if(FmcExclusion.admissionClosed())FmcExclusion.failed();return -1;
        }
        if(identity==0&&current.binding)return witness(FmcExclusion.enter());
        if(identity<=0||identity!=current.identity&&identity!=current.previous)return -1;
        return witness(FmcExclusion.enter());
    }
    /** Deferrable receiver polls retain their existing cadence. The exact
     * ownership identity decides scope; closed admission takes the qualified
     * void poll return instead of revoking recall as an unexpected access. */
    public static int pollTag(long identity){
        var current=scope;if(current==null)return FmcExclusion.enter();
        if(current.lifecycle!=Observer.terrainEpoch()){if(FmcExclusion.admissionClosed())FmcExclusion.failed();return -1;}
        if(identity==0&&current.binding)return FmcExclusion.enter();
        if(identity<=0||identity!=current.identity&&identity!=current.previous)return -1;
        return FmcExclusion.enter();
    }
    /** Only original methods with an exact first native Pause return use this
     * decision. A running system keeps its native body and revokes optional
     * recall; a paused system takes its own already-qualified return path. */
    public static int pauseTicket(int ticket,boolean nativePaused){
        if(ticket==0&&!nativePaused){FmcExclusion.failed();return -1;}
        return ticket;
    }
    private static int witness(int ticket){
        if(ticket==0){FmcExclusion.failed();return -1;}
        return ticket;
    }
    public static void leave(int ticket){if(ticket>0)FmcExclusion.leave(ticket);}
    public static void failed(){FmcExclusion.failed();}
}
