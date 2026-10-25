package psx.exporter;

import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLong;

/** Applied model/database selection witness, separate from fast route edits.
 * A long filesystem hash must not require ordinary FMC calculations to stop.
 * Native callbacks allocate nothing, never wait and cannot inhibit an edit. */
public final class FmcCompatibilityWitness {
    private static final long STEP=1L<<16,COUNT=STEP-1;
    private static final AtomicLong state=new AtomicLong();
    private static volatile boolean failed;
    private FmcCompatibilityWitness(){}
    public static void enter(){long old=state.getAndAdd(STEP+1);if(old<0||(old&COUNT)==COUNT)failed=true;}
    public static void leave(){long old=state.getAndAdd(STEP-1);if(old<0||(old&COUNT)==0)failed=true;}
    // The installed synchronized Core.navFold(null) branch is a qualified
    // pure getter. Polling it must not impersonate a database replacement.
    public static void enter(String selection){if(selection!=null)enter();}
    public static void leave(String selection){if(selection!=null)leave();}
    // Option.monitor can transiently assign NG and then apply its native
    // availability restriction. Its final applied model is authoritative.
    // Readers refuse an active monitor; a completed unchanged poll is not a
    // configuration replacement, including an intermediate restricted value.
    public static void monitorEnter(){long old=state.getAndIncrement();if(old<0||(old&COUNT)==COUNT)failed=true;}
    public static void monitorLeave(int ngBefore,int engineBefore,int ngAfter,int engineAfter){
        boolean changed=ngBefore!=ngAfter||engineBefore!=engineAfter;
        long old=state.getAndAdd(changed?STEP-1:-1);if(old<0||(old&COUNT)==0)failed=true;
    }
    public static void failed(){failed=true;}
    static long begin(){long value=state.get();return failed||value<0||(value&COUNT)!=0?-1:value;}
    static boolean end(long before){VarHandle.acquireFence();return before>=0&&!failed&&state.get()==before;}
}
