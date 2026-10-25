package psx.exporter;

import java.util.concurrent.atomic.AtomicLong;
import java.lang.invoke.VarHandle;

/** FMC-only copy witness. Every qualified write/alias mutation must be bracketed.
 * This does not exclude writers and is never, by itself, permission to recall.
 * Atomic entry/exit orders native field writes with the worker's copy checks.
 * It neither changes aircraft lifecycle nor adds Pause to a running capture. */
public final class FmcCoherence {
    private static final long STEP=1L<<16, COUNT=STEP-1;
    private static final AtomicLong state=new AtomicLong();
    private static volatile boolean failed;
    private FmcCoherence(){}

    public static void enter() {
        long old=state.getAndAdd(STEP+1);
        // Overflow is a capability failure; original native work still executes.
        if((old&COUNT)==COUNT||old<0)failed=true;
    }
    public static void leave() {
        long old=state.getAndAdd(STEP-1);
        if((old&COUNT)==0||old<0)failed=true;
    }
    public static void failed(){failed=true;}
    static long beginCopy() {
        long observed=state.get();
        return failed||(observed&COUNT)!=0||observed<0?-1:observed;
    }
    static boolean endCopy(long observed,long lifecycleBefore,long lifecycleAfter,
                           long generationBefore,long generationAfter) {
        // Optimistic copied loads must complete before checking the sequence.
        // A trailing acquire read alone does not supply this load-load fence.
        VarHandle.acquireFence();
        return observed>=0&&!failed&&lifecycleBefore>0&&lifecycleBefore==lifecycleAfter
            &&generationBefore>0&&generationBefore==generationAfter&&state.get()==observed;
    }
    static int active(){return (int)(state.get()&COUNT);}
}
