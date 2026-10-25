package psx.exporter;

import java.lang.invoke.VarHandle;

/** Worker-only coherent view of the qualified lower hook's native lifecycle.
 * The existing mutation counter/version surround situation/reposition writes;
 * completedFrame establishes ordinary startup. Pose subscriptions and a pose
 * capability failure are independent. This is not FMC field qualification. */
final class FmcCurrentLifecycle implements FmcInitialization.Source {
    /** Worker-only retry after filesystem/VM work. Ordinary pose writers can
     * temporarily withdraw the fast witness without replacing this lifecycle.
     * A real epoch change rejects immediately; callbacks never wait here. */
    long awaitQualifiedLifecycle(long expected){
        if(expected<=0)return 0;
        long deadline=System.nanoTime()+100_000_000L;
        do{
            if(!Observer.firstFrame||Observer.terrainEpoch()!=expected||Thread.currentThread().isInterrupted())return 0;
            if(qualifiedLifecycle()==expected)return expected;
            java.util.concurrent.locks.LockSupport.parkNanos(1_000_000L);
        }while(System.nanoTime()<deadline);
        return 0;
    }
    public long qualifiedLifecycle(){
        if(!Observer.firstFrame)return 0;
        long version=Observer.metadataMutationVersion(),epoch=Observer.terrainEpoch();
        if(version<0||epoch<=0||Observer.metadataMutationActive())return 0;
        VarHandle.acquireFence();
        return Observer.firstFrame&&!Observer.metadataMutationActive()
            &&Observer.metadataMutationVersion()==version&&Observer.terrainEpoch()==epoch?epoch:0;
    }
}
