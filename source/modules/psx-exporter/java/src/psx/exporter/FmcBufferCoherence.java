package psx.exporter;

import java.lang.invoke.VarHandle;
import java.util.concurrent.atomic.AtomicLong;

/** Witness for the three entered header buffers. Tracking assignments alone
 * misses StringBuilder mutations. Every qualified mutator must bracket its
 * actual receiver, including before first binding; this helper supplies no
 * writer instrumentation or capability by itself. Callbacks never allocate,
 * wait, or consult a lifecycle provider. Binding/copy checks belong to workers. */
public final class FmcBufferCoherence {
    private record Scope(long lifecycle,StringBuilder[] buffers){}
    private static volatile Scope scope;
    private static final long STEP=1L<<16,COUNT=STEP-1;
    private static final AtomicLong bindingWriters=new AtomicLong();
    private static volatile boolean failed;
    private FmcBufferCoherence(){}
    static void bind(FmcInitialization.Source source,StringBuilder[] buffers){
        long lifecycle=source.qualifiedLifecycle(),bindingBefore=bindingWriters.get(),before=FmcCoherence.beginCopy();
        if(failed||lifecycle<=0||bindingBefore<0||(bindingBefore&COUNT)!=0||before<0||buffers==null||buffers.length!=3)
            throw new IllegalStateException("FMC header buffer binding unavailable");
        StringBuilder[] bound=buffers.clone();
        for(int i=0;i<bound.length;i++){
            if(bound[i]==null)throw new IllegalArgumentException("FMC header buffer absent");
            for(int j=0;j<i;j++)if(bound[i]==bound[j])throw new IllegalArgumentException("FMC header buffer alias");
        }
        scope=new Scope(lifecycle,bound);VarHandle.acquireFence();
        long after;
        try{after=source.qualifiedLifecycle();}catch(RuntimeException|Error original){scope=null;throw original;}
        if(failed||bindingWriters.get()!=bindingBefore||!FmcCoherence.endCopy(before,lifecycle,after,1,1)){
            scope=null;throw new IllegalStateException("FMC header buffer changed during binding");
        }
    }
    static boolean ready(long lifecycle){Scope current=scope;return !failed&&lifecycle>0&&current!=null&&current.lifecycle==lifecycle&&FmcCoherence.beginCopy()>=0;}
    static boolean matches(long lifecycle,StringBuilder[] actual){
        Scope current=scope;
        if(!ready(lifecycle)||current==null||actual==null||actual.length!=3)return false;
        for(int i=0;i<3;i++)if(current.buffers[i]!=actual[i])return false;
        return true;
    }
    static void clear(){scope=null;}
    public static int enter(Object receiver){
        long old=bindingWriters.getAndAdd(STEP+1);if(old<0||(old&COUNT)==COUNT)failed=true;
        try{
            Scope current=scope;
            if(current!=null)for(StringBuilder buffer:current.buffers)if(buffer==receiver){FmcCoherence.enter();return 3;}
            return 1;
        }catch(Throwable original){retireBinding();throw original;}
    }
    public static void leave(int ticket){
        if(ticket==0)return;
        if(ticket!=1&&ticket!=3){failed();return;}
        try{if(ticket==3)FmcCoherence.leave();}finally{retireBinding();}
    }
    private static void retireBinding(){long old=bindingWriters.getAndAdd(STEP-1);if(old<0||(old&COUNT)==0)failed=true;}
    public static void failed(){failed=true;FmcCoherence.failed();}
}
