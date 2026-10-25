package psx.exporter;

import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReferenceArray;

/** Accepted manual configuration changes only; no weather writes or native calls. */
public final class WeatherChangeObserver {
    static volatile boolean qualified;
    private static volatile boolean ready;
    private static final AtomicLong edits=new AtomicLong();
    private static volatile long consumed;
    private static final long[] cycles=new long[32];
    private static final int[] cycleDepth=new int[32],uiDepth=new int[32];
    private static final boolean[] dirty=new boolean[32];
    private static final AtomicInteger uiWriters=new AtomicInteger();
    private static final AtomicLongArray threadIds=new AtomicLongArray(32);
    private static final AtomicReferenceArray<Thread> lifetimes=new AtomicReferenceArray<>(32);
    private WeatherChangeObserver() {}
    static boolean available() {
        try { if(WeatherChangeNative.abiVersion()!=1)return false;WeatherChangeNative.capabilities(false);
            WeatherChangeNative.mark(0,0,-1);WeatherChangeNative.consumed(0,0);return true; }
        catch(Throwable unavailable) { return false; }
    }
    static void prepare() {
        try { ready=qualified&&available();WeatherChangeNative.capabilities(ready);if(ready)EnvironmentObserver.manualWeatherFailure(false); }
        catch(Throwable unavailable) { ready=false; }
    }
    public static void disable() {
        ready=false;
        // Use the already qualified base status path even if the optional JNI
        // failure handler itself no longer resolves. Time remains available.
        try { EnvironmentObserver.manualWeatherFailure(true); } catch(Throwable ignored) {}
        try { WeatherChangeNative.capabilities(false); } catch(Throwable ignored) {}
    }
    public static void changed(int reason) {
        if(!ready)return;
        try { int slot=slot();if(slot<0)return;WeatherChangeNative.mark(reason,Observer.epochForTest(),slot);edits.incrementAndGet(); }
        catch(Throwable failure) { disable(); }
    }
    public static void consumed() {
        if(!ready)return;long version=edits.get();if(version==consumed)return;
        try { int slot=slot();if(slot<0)return;long cycle=cycles[slot];
            if(cycle>0&&WeatherChangeNative.consumed(Observer.epochForTest(),Math.min(cycle,EnvironmentObserver.phaseStart())))consumed=version; }
        catch(Throwable failure) { disable(); }
    }
    public static void cycle() {
        if(!ready)return;
        try { int slot=slot();if(slot>=0&&cycleDepth[slot]++==0)cycles[slot]=BridgeNative.qpc(); }
        catch(Throwable failure) { disable(); }
    }
    public static void cycleEnd() {
        try { int slot=slot();if(slot>=0&&cycleDepth[slot]>0)--cycleDepth[slot]; }
        catch(Throwable failure) { disable(); }
    }
    public static boolean uiEnter() {
        if(!ready)return false;
        try { int slot=slot();if(slot<0||cycleDepth[slot]!=0)return false;
            // Programmatic Swing refreshes can run the same listeners as input.
            // Accept the native listener only while actual input/focus is being
            // dispatched; Main commands have their separate negative origin.
            var event=java.awt.EventQueue.getCurrentEvent();
            if(!(event instanceof java.awt.event.InputEvent)&&!(event instanceof java.awt.event.FocusEvent))return false;
            ++uiDepth[slot];uiWriters.incrementAndGet();return true;
        } catch(Throwable failure) { disable();return false; }
    }
    public static void uiEnd(boolean success) {
        try { int slot=slot();if(slot<0||uiDepth[slot]<=0){disable();return;}
            --uiDepth[slot];uiWriters.decrementAndGet();
            if(!success&&dirty[slot]){dirty[slot]=false;disable();return;}
            if(uiDepth[slot]==0&&dirty[slot]){dirty[slot]=false;changed(1);}
        } catch(Throwable failure) { disable(); }
    }
    public static boolean interested(int origin) {
        if(!ready)return false;if(origin<0)return true;if(origin!=1||uiWriters.get()==0)return false;
        try { int slot=slot();return slot>=0&&uiDepth[slot]>0&&cycleDepth[slot]==0; }
        catch(Throwable failure) { disable();return false; }
    }
    public static void packet(int origin) {
        if(origin<0)changed(2);else direct();
    }
    public static void direct() {
        if(!ready||uiWriters.get()==0)return;
        try { int slot=slot();if(slot>=0&&uiDepth[slot]>0&&cycleDepth[slot]==0)dirty[slot]=true; }
        catch(Throwable failure) { disable(); }
    }
    private static int slot() {
        Thread thread=Thread.currentThread();long id=thread.threadId();
        for(int i=0;i<32;i++)if(threadIds.get(i)==id)return i;
        for(int i=0;i<32;i++){
            long old=threadIds.get(i);if(old==id)return i;
            if(old==0&&threadIds.compareAndSet(i,0,-id)){lifetimes.set(i,thread);threadIds.set(i,id);return i;}
            Thread previous=lifetimes.get(i);
            if(old>0&&previous!=null&&previous.threadId()==old&&!previous.isAlive()&&threadIds.compareAndSet(i,old,-id)){
                if(cycleDepth[i]!=0||uiDepth[i]!=0){threadIds.set(i,old);disable();return -1;}
                cycles[i]=0;dirty[i]=false;lifetimes.set(i,thread);threadIds.set(i,id);return i;
            }
        }
        disable();return -1;
    }
}
