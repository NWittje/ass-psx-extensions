package psx.exporter;

import java.util.concurrent.atomic.AtomicLong;
import java.lang.invoke.VarHandle;

/** Fixed receiver selection for FMC input-bank witnesses. Banks are live
 * bindings, not owned artifact nodes. The installed profile must independently
 * qualify their identities and all writers before using this helper. */
public final class FmcBankCoherence {
    private record Scope(long lifecycle,Object[] banks){}
    private static volatile Scope scope;
    private static final long STEP=1L<<16,COUNT=STEP-1;
    private static final AtomicLong bindingWriters=new AtomicLong();
    private static volatile boolean bindingFailed;
    private FmcBankCoherence(){}
    // Worker/qualified dispatcher only. No callback allocates a scope or waits.
    static void bind(long lifecycle,Object[] banks,Object excludedNav){
        long bindingBefore=bindingWriters.get(),before=FmcCoherence.beginCopy();
        if(bindingFailed||bindingBefore<0||(bindingBefore&COUNT)!=0||lifecycle<=0||lifecycle!=Observer.terrainEpoch()||banks==null||banks.length==0||banks.length>32||before<0||excludedNav==null||!excludedNav.getClass().getName().equals("Qs"))
            throw new IllegalStateException("FMC bank binding unavailable");
        Object[] bound=banks.clone();
        for(int i=0;i<bound.length;i++){
            String name=bound[i]==null?"":bound[i].getClass().getName();
            if(bound[i]==excludedNav||!name.equals("Qi")&&!name.equals("Qs"))throw new IllegalArgumentException("FMC bank receiver binding");
            for(int j=0;j<i;j++)if(bound[j]==bound[i])throw new IllegalArgumentException("FMC bank binding alias");
        }
        scope=new Scope(lifecycle,bound);
        VarHandle.acquireFence();
        if(bindingFailed||bindingWriters.get()!=bindingBefore||!FmcCoherence.endCopy(before,lifecycle,Observer.terrainEpoch(),1,1)){scope=null;throw new IllegalStateException("FMC bank binding changed during publication");}
    }
    static boolean ready(){Scope current=scope;return !bindingFailed&&current!=null&&current.lifecycle==Observer.terrainEpoch()&&FmcCoherence.beginCopy()>=0;}
    static void clear(){scope=null;}
    public static int enter(Object receiver){
        // All Q/Qi/Qs writer bodies count during binding, even when the old
        // receiver scope is absent or different. Once bound, unrelated banks
        // do not invalidate the ordinary FMC copied-state witness.
        long old=bindingWriters.getAndAdd(STEP+1);
        if(old<0||(old&COUNT)==COUNT)bindingFailed=true;
        try{
            Scope current=scope;
            if(current!=null&&current.lifecycle==Observer.terrainEpoch())
                for(Object bank:current.banks)if(bank==receiver){FmcCoherence.enter();return 3;}
            return 1;
        }catch(Throwable failure){retireBinding();throw failure;}
    }
    public static void leave(int ticket){
        if(ticket==0)return;
        if(ticket!=1&&ticket!=3){failed();return;}
        try{if(ticket==3)FmcCoherence.leave();}finally{retireBinding();}
    }
    private static void retireBinding(){long old=bindingWriters.getAndAdd(STEP-1);if(old<0||(old&COUNT)==0)bindingFailed=true;}
    public static void failed(){bindingFailed=true;FmcCoherence.failed();}
}
