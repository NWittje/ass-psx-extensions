package psx.exporter;

import java.util.*;

/** Exact identity routing for qualified FMC poll frontiers. A same-typed
 * inactive, safety or private staging object is not an owned destination.
 * Binding builds the identity table on a worker; callbacks only probe it.
 * There is no field reflection, allocation, native input read or callback wait.
 * This table cannot qualify a method's deferral or its receiver aliases. */
public final class FmcAdmissionReceivers {
    private static final int MAX_RECEIVERS=768;
    private record Binding(long lifecycle,Object[] slots,int count){}
    private static volatile Binding binding;
    private FmcAdmissionReceivers(){}
    static void bind(long lifecycle,Collection<?> receivers){
        if(FmcExclusion.admissionClosed())throw new IllegalStateException("FMC admission receiver binding is frozen during closure");
        if(lifecycle<=0||lifecycle!=Observer.terrainEpoch())throw new IllegalStateException("FMC admission receiver lifecycle");
        Objects.requireNonNull(receivers);if(receivers.isEmpty()||receivers.size()>MAX_RECEIVERS)throw new IllegalArgumentException("FMC admission receiver capacity");
        var slots=new Object[receivers.size()];int count=0,examined=0;
        for(Object receiver:receivers){
            if(++examined>MAX_RECEIVERS)throw new IllegalArgumentException("FMC admission receiver enumeration capacity");
            if(receiver==null)throw new IllegalArgumentException("FMC admission null receiver");
            boolean duplicate=false;for(int at=0;at<count;at++)if(slots[at]==receiver){duplicate=true;break;}
            if(!duplicate)slots[count++]=receiver;
        }
        if(lifecycle!=Observer.terrainEpoch()||FmcExclusion.admissionClosed())throw new IllegalStateException("FMC admission receiver changed during binding");
        binding=new Binding(lifecycle,slots,count);
    }
    static boolean current(){var b=binding;return b!=null&&b.lifecycle==Observer.terrainEpoch();}
    static int count(){var b=binding;return b==null?0:b.count;}
    static void clear(){if(FmcExclusion.admissionClosed())throw new IllegalStateException("FMC admission receiver binding is frozen during closure");binding=null;}
    public static int enter(Object receiver){
        var b=binding;
        if(b==null||b.lifecycle!=Observer.terrainEpoch()){
            if(FmcExclusion.admissionClosed())FmcExclusion.failed();return -1;
        }
        if(receiver==null)return -1; // Keep the original native null behavior.
        // Poll roots and mutable buffer/array aliases form a small list. Hot
        // native instance-cell accesses use direct ownership tags separately.
        // Identity comparison does not materialize an unowned object's VM hash.
        for(int at=0;at<b.count;at++)if(b.slots[at]==receiver)return FmcExclusion.enter();
        return -1;
    }
}
