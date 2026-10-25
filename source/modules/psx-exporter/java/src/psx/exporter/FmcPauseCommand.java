package psx.exporter;

import java.awt.EventQueue;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;

/** Command-only receipt for the separately qualified native Pause bridges.
 * The worker prepares a session; its native dispatcher binds it on the ordinary
 * EDT. No simulation callback, operation retry or native initializer uses this
 * class. A receipt precedes the first setter, including a setter that throws.
 * Actual lifecycle/configuration authority comes from the installed adapter,
 * never from an imported artifact or a retained command result. */
public final class FmcPauseCommand {
    interface Authority {void check() throws Throwable;}
    interface Action {void run() throws Throwable;}
    private static final AtomicReference<Session> ACTIVE=new AtomicReference<>();
    private static volatile boolean disabled;

    static final class Session implements AutoCloseable {
        final Authority authority;
        final FmcTransaction.PauseEntry receipt;
        volatile boolean closed;
        Session(Authority authority,FmcTransaction.PauseEntry receipt) {
            this.authority=Objects.requireNonNull(authority);
            this.receipt=Objects.requireNonNull(receipt);
            if(receipt.began)throw new IllegalArgumentException("FMC Pause receipt already used");
        }
        // The dispatcher itself, native EDT use and complete exclusion frontier
        // must be qualified separately. Binding this session is not a capability.
        void run(Action action)throws Throwable {
            if(!EventQueue.isDispatchThread()||disabled||closed||!ACTIVE.compareAndSet(null,this))
                throw new IllegalStateException("FMC Pause command thread/ownership");
            try {if(closed)throw new IllegalStateException("FMC Pause command closed");authority.check();action.run();}
            // A damaged helper cannot replace an exception thrown by native
            // Pause. Disable only this optional command component; retain the
            // original exception and leave diagnosis to its worker.
            finally {if(!ACTIVE.compareAndSet(this,null))disabled=true;}
        }
        public void close() {
            if(ACTIVE.get()==this)throw new IllegalStateException("FMC Pause command still executing");
            closed=true;
        }
    }
    private FmcPauseCommand(){}
    private static Session current()throws Throwable {
        Session value=ACTIVE.get();
        if(!EventQueue.isDispatchThread()||disabled||value==null||value.closed)
            throw new IllegalStateException("FMC Pause command not bound");
        value.authority.check();return value;
    }
    /** Called after native preflight, immediately before the first Pause setter. */
    public static void acceptEntry(int mask)throws Throwable {current().receipt.accepted(mask);}
    /** Called before EACH native setter, on that setter's ordinary thread. */
    public static void requireAuthority()throws Throwable {
        Session value=current();if(!value.receipt.began)throw new IllegalStateException("FMC Pause entry missing");
    }
    /** Finish uses this command's entry selection, including partial entry failure. */
    public static void acceptFinish(int mask)throws Throwable {
        Session value=current();
        if(!value.receipt.began||mask!=value.receipt.mask||mask<0||mask>3)
            throw new IllegalStateException("FMC Pause finalization receipt mismatch");
    }
}
