package psx.exporter;

import java.util.*;
import java.util.concurrent.locks.LockSupport;

/** Worker-side command sequencing. A native adapter must separately qualify
 * its exact field profile, pure factories, legal ordinary-thread bridges, alias publication,
 * reader/writer frontier and unrelated-state witness. Implementing this interface
 * or setting an artifact flag cannot qualify that adapter.
 *
 * Only staging/copying/validation and waiting occur here. Native callbacks never
 * enter this class. A command is executed once; reconnect lookup belongs to the
 * authenticated operation ledger, never to retry of this transaction. */
final class FmcTransaction {
    enum Phase {REJECTED,OBSERVED,ROLLED_BACK,INDETERMINATE}
    record Identity(long lifecycle,long generation,byte[] compatibility){
        Identity{if(lifecycle<=0||generation<=0||compatibility.length!=128)throw new IllegalArgumentException("FMC transaction identity");compatibility=compatibility.clone();}
    }
    record Outcome(Phase phase,long generation,int entryPause,byte[] memory,byte[] preserved,Throwable failure,Throwable rollbackFailure){}
    static final class PauseEntry {
        int mask=-1;boolean began;
        // Qualified native adapter records this after preflight and before its
        // first ordinary Pause write, so a native setter exception cannot lose
        // the entry Freeze selection. A rejected preflight leaves began false.
        void accepted(int entryMask){require(entryMask>=0&&entryMask<=3&&!began,"FMC Pause entry receipt");mask=entryMask;began=true;}
    }
    interface Journal {
        // Capture actual old references/scalars, not reconstructed lookalikes.
        // The journal includes every qualified external FMC alias as well as
        // owned roots. apply may fail after a partial write; rollback must cover it.
        void apply(Object[] stagedRoots)throws Throwable;
        void rollback()throws Throwable;
    }
    interface Native {
        boolean captureQualified();boolean recallQualified();
        FmcGraph graph();FmcGraph.Access access();List<FmcFactoryOrder.Rule> factories();
        Identity identity();
        // Native operations run on their qualified ordinary execution thread.
        void enterPause(PauseEntry receipt)throws Throwable;
        void finishPause(int entryMask)throws Throwable;
        long closeAdmission()throws Throwable;
        boolean quiescent(long token)throws Throwable;
        void openAdmission(long token)throws Throwable;
        // Physical/native navigation position, IRS, fuel, flight number and
        // inactive content are preservation inputs. Native guidance effects
        // of restored route/PERF plus EXEC are intentionally not equality
        // targets; no saved guidance/progress is imported.
        byte[] preservedState()throws Throwable;
        Journal preimage()throws Throwable;
        long advanceGeneration(long expected)throws Throwable;
    }
    private final Native nativeState;
    FmcTransaction(Native nativeState){this.nativeState=Objects.requireNonNull(nativeState);}
    private static void require(boolean value,String reason){if(!value)throw new IllegalStateException(reason);}
    private static boolean same(Identity a,Identity b){return a.lifecycle==b.lifecycle&&a.generation==b.generation&&Arrays.equals(a.compatibility,b.compatibility);}
    private static boolean sameLifecycle(Identity a,Identity b){return a.lifecycle==b.lifecycle&&Arrays.equals(a.compatibility,b.compatibility);}
    private byte[] memory()throws Exception{return nativeState.graph().encode(nativeState.graph().capture(nativeState.access()));}

    byte[] capture(Identity expected)throws Exception{
        require(nativeState.captureQualified(),"native FMC capture unavailable");
        Identity before=nativeState.identity();require(same(expected,before),"FMC source replaced before capture");
        long witness=FmcCoherence.beginCopy();require(witness>=0,"concurrent FMC writer");
        byte[] copied=memory();Identity after=nativeState.identity();
        require(same(before,after)&&FmcCoherence.endCopy(witness,before.lifecycle,after.lifecycle,before.generation,after.generation),"FMC changed during capture");
        return copied;
    }
    private void awaitQuiescence(long token,long deadline)throws Throwable{
        while(!nativeState.quiescent(token)){
            long remaining=deadline-System.nanoTime();require(remaining>0,"FMC exclusion deadline");
            // Worker only. No input, render, physics or JNI callback waits.
            LockSupport.parkNanos(Math.min(remaining,1_000_000));
            require(!Thread.currentThread().isInterrupted(),"FMC command worker interrupted");
        }
        require(deadline-System.nanoTime()>0,"FMC exclusion deadline");
    }
    Outcome recall(FmcGraph.Graph requested,Identity expected,long deadline){
        int entry=-1;long token=0,generation=expected.generation;Journal journal=null;
        byte[] beforeMemory=new byte[0],beforePreserved=new byte[0],accepted=new byte[0];
        boolean touched=false,gateQuiet=false,pauseFinished=false;var pauseEntry=new PauseEntry();Throwable failure=null,rollbackFailure=null;Phase phase=Phase.REJECTED;
        try{
            require(nativeState.recallQualified()&&nativeState.captureQualified(),"native FMC recall unavailable");
            require(deadline-System.nanoTime()>0,"FMC command expired");require(same(expected,nativeState.identity()),"FMC source replaced before staging");
            var graph=nativeState.graph();graph.validate(requested);byte[] wanted=graph.encode(requested);
            Object[] staged=graph.prepare(requested,nativeState.access(),nativeState.factories());
            require(same(expected,nativeState.identity()),"FMC source replaced during staging");
            nativeState.enterPause(pauseEntry);require(pauseEntry.began,"unqualified entry Pause/Freeze");entry=pauseEntry.mask;
            token=nativeState.closeAdmission();awaitQuiescence(token,deadline);gateQuiet=true;
            require(same(expected,nativeState.identity()),"FMC source replaced before commit");
            beforeMemory=memory();beforePreserved=nativeState.preservedState();require(beforePreserved.length>0,"preservation witness unavailable");
            journal=nativeState.preimage();require(journal!=null,"FMC preimage unavailable");
            // Revalidate after preimage creation. Artifacts never supply this
            // authority, and a quiet copy witness alone cannot authorize recall.
            require(nativeState.quiescent(token)&&same(expected,nativeState.identity()),"FMC exclusion changed before commit");
            touched=true;journal.apply(staged);
            require(same(expected,nativeState.identity())&&nativeState.quiescent(token),"FMC source replaced during publication");
            accepted=memory();require(Arrays.equals(wanted,accepted),"FMC restored memory mismatch");
            nativeState.finishPause(entry);pauseFinished=true;
            require(same(expected,nativeState.identity())&&nativeState.quiescent(token),"FMC lifecycle/gate changed during commit");
            require(Arrays.equals(beforePreserved,nativeState.preservedState()),"unrelated native state changed");
            generation=nativeState.advanceGeneration(expected.generation);require(generation==expected.generation+1&&generation>0,"FMC generation advance mismatch");
            phase=Phase.OBSERVED;
        }catch(Throwable original){
            failure=original;
            if(touched){
                phase=Phase.INDETERMINATE;
                try{
                    // A replaced lifecycle or lost exclusion cannot authorize
                    // rollback into a new native situation. Leave it paused and
                    // report uncertainty, rather than replaying any command.
                    require(gateQuiet&&nativeState.quiescent(token)&&sameLifecycle(expected,nativeState.identity()),"FMC rollback authority unavailable");
                    journal.rollback();if(!pauseFinished){nativeState.finishPause(entry);pauseFinished=true;}
                    require(Arrays.equals(beforeMemory,memory())&&Arrays.equals(beforePreserved,nativeState.preservedState()),"FMC rollback readback mismatch");
                    Identity current=nativeState.identity();require(sameLifecycle(expected,current)&&nativeState.quiescent(token),"FMC rollback source replaced");
                    // Even certain rollback is a discontinuity: a momentarily
                    // published target invalidates earlier FMC observations.
                    generation=nativeState.advanceGeneration(current.generation);require(generation==current.generation+1&&generation>0,"FMC rollback generation mismatch");phase=Phase.ROLLED_BACK;
                }catch(Throwable rollback){rollbackFailure=rollback;}
            }
        }finally{
            if(pauseEntry.began&&!pauseFinished){entry=pauseEntry.mask;try{
                // An old command has no authority over the Pause/Freeze choice
                // of a newly loaded situation. The native adapter must also
                // recheck this binding on its ordinary execution thread before
                // either Pause writer, closing the worker-to-EDT race.
                require(sameLifecycle(expected,nativeState.identity()),"FMC Pause finalization source replaced");
                nativeState.finishPause(entry);
            }catch(Throwable finish){if(failure==null)failure=finish;else if(rollbackFailure==null)rollbackFailure=finish;phase=Phase.INDETERMINATE;}}
            if(token>0){try{nativeState.openAdmission(token);}catch(Throwable release){if(failure==null)failure=release;else if(rollbackFailure==null)rollbackFailure=release;phase=Phase.INDETERMINATE;}}
        }
        return new Outcome(phase,generation,entry,accepted,beforePreserved,failure,rollbackFailure);
    }
}
