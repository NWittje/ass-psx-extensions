package psx.exporter;

import java.util.Arrays;
import java.util.Objects;

/** Ordinary native FMC operations on the EDT. No route repair, preservation,
 * rollback or extra EXEC is performed. The caller decodes on its worker before
 * submission; the native routines retain responsibility for FMC semantics. */
final class FmcNativeRecordsExecutor {
    interface NativeCalls {
        void check() throws Throwable;
        void save(String filename, String description) throws Throwable;
        void pause() throws Throwable;
        void initialize() throws Throwable;
        default void finishReload() throws Throwable {}
        boolean paused() throws Throwable;
    }
    enum Kind { CAPTURE, RELOAD }
    record Completion(long operation, Kind kind, FmcCommandDispatch.Observation nativeAction,
                      boolean pauseConfirmed) {
        boolean succeeded() { return nativeAction.phase()==FmcCommandDispatch.Phase.RETURNED; }
    }
    static final class Receipt {
        final long operation;
        final Kind kind;
        private final byte[] requestHash;
        private final FmcCommandDispatch.Job job;
        private final Evidence evidence;
        private Receipt(long operation, Kind kind, byte[] hash, FmcCommandDispatch.Job job,Evidence evidence) {
            this.operation=operation;this.kind=kind;requestHash=hash;this.job=job;this.evidence=evidence;
        }
    }
    private static final class Evidence { volatile boolean pauseConfirmed; }
    private final NativeCalls nativeCalls;
    private final FmcCommandDispatch dispatcher;
    private final Completion[] history=new Completion[64];
    private int next;
    private long operationFloor;
    private Receipt outstanding;

    FmcNativeRecordsExecutor(NativeCalls nativeCalls) { this(nativeCalls,new FmcCommandDispatch()); }
    FmcNativeRecordsExecutor(NativeCalls nativeCalls,FmcCommandDispatch dispatcher) {
        this.nativeCalls=Objects.requireNonNull(nativeCalls);this.dispatcher=Objects.requireNonNull(dispatcher);
    }
    private static void worker() {
        if(java.awt.EventQueue.isDispatchThread())throw new IllegalStateException("Native FMC worker required");
    }
    Receipt capture(long operation,long deadline,byte[] requestHash,String filename,String description) {
        Objects.requireNonNull(filename);Objects.requireNonNull(description);
        return submit(operation,deadline,requestHash,Kind.CAPTURE,()->nativeCalls.save(filename,description));
    }
    Receipt reload(long operation,long deadline,byte[] requestHash,FmcCommandDispatch.Action nativeImport) {
        Objects.requireNonNull(nativeImport);
        return submit(operation,deadline,requestHash,Kind.RELOAD,()->{
            Throwable original=null;
            try {
                nativeCalls.pause();
                nativeImport.run();
                nativeCalls.initialize();
            }catch(Throwable failure){original=failure;throw failure;}
            finally {
                try{nativeCalls.finishReload();}
                catch(Throwable finishFailure){
                    if(original==null)throw finishFailure;
                    if(original!=finishFailure)try{original.addSuppressed(finishFailure);}catch(Throwable ignored){}
                }
            }
            if(!nativeCalls.paused())throw new IllegalStateException("Native FMC final Pause not observed");
        });
    }
    private Receipt submit(long operation,long deadline,byte[] hash,Kind kind,FmcCommandDispatch.Action action) {
        worker();
        if(hash==null||hash.length!=32)throw new IllegalArgumentException("Native FMC request hash");
        if(outstanding!=null) {
            if(operation==outstanding.operation&&kind==outstanding.kind&&Arrays.equals(hash,outstanding.requestHash))return outstanding;
            throw new IllegalStateException("Native FMC operation already outstanding");
        }
        if(operation<=operationFloor)throw new IllegalStateException("Native FMC retired operation cannot be repeated");
        // The EDT may start before scheduling returns. Its evidence storage
        // exists first and does not depend on the worker's receipt assignment.
        var evidence=new Evidence();
        var job=dispatcher.submit(operation,deadline,nativeCalls::check,()->{
            action.run();if(kind==Kind.RELOAD)evidence.pauseConfirmed=true;
        });
        outstanding=new Receipt(operation,kind,hash.clone(),job,evidence);operationFloor=operation;
        return outstanding;
    }
    Completion await(Receipt receipt,long deadline) {
        worker();require(receipt);
        return new Completion(receipt.operation,receipt.kind,dispatcher.await(receipt.job,deadline),receipt.evidence.pauseConfirmed);
    }
    Completion lookup(long operation) {
        worker();
        if(outstanding!=null&&outstanding.operation==operation)
            return new Completion(operation,outstanding.kind,dispatcher.lookup(outstanding.job),outstanding.evidence.pauseConfirmed);
        for(var item:history)if(item!=null&&item.operation()==operation)return item;
        return null;
    }
    void release(Receipt receipt) {
        worker();require(receipt);
        var completion=lookup(receipt.operation);dispatcher.release(receipt.job);
        history[next]=completion;next=(next+1)%history.length;outstanding=null;
    }
    private void require(Receipt receipt) {
        if(receipt==null||receipt!=outstanding)throw new IllegalArgumentException("Native FMC receipt identity");
    }
}
