package psx.exporter;

import java.lang.invoke.VarHandle;
import java.nio.*;
import java.util.concurrent.atomic.*;

/** Native callbacks only copy primitives. All storage is prepared before PSX classes load. */
public final class TrafficObserver {
    static volatile boolean qualified;
    private static volatile boolean failed,nativeReady;
    private static ByteBuffer buffer;
    private static final AtomicLong writers=new AtomicLong(),versions=new AtomicLong(),copyOwner=new AtomicLong();
    private static final AtomicLongArray threads=new AtomicLongArray(32),generations=new AtomicLongArray(7);
    private static final long[] depths=new long[32],changes=new long[32];
    private static final boolean[] active=new boolean[7];
    private static long before,epoch,start,frame,sequence,next,interval,lastEpoch;
    private static int copySlot,seen,lastMode=Integer.MIN_VALUE;
    private TrafficObserver() {}
    static void prepare(){
        buffer=ByteBuffer.allocateDirect(TrafficSnapshot.BYTES).order(ByteOrder.LITTLE_ENDIAN);
        for(int i=0;i<TrafficSnapshot.BYTES;i++)buffer.put(i,(byte)0);
        buffer.putInt(0,1).putInt(4,TrafficSnapshot.BYTES).putInt(8,7).putInt(12,1);
        for(int i=0;i<7;i++)generations.set(i,1);
        writers.incrementAndGet();writers.decrementAndGet();versions.incrementAndGet();versions.decrementAndGet();
        threads.compareAndSet(0,0,0);copyOwner.compareAndSet(0,0);VarHandle.fullFence();
    }
    static void prepareNative(){try{
        if(!qualified||failed||buffer==null)return;
        if(!TrafficLayer.available()||(interval=TrafficNative.intervalTicks())<=0||!TrafficNative.initialize(buffer))throw new LinkageError("traffic JNI1");
        TrafficNative.capabilities(1);nativeReady=true;
    }catch(Throwable unavailable){disable();}}
    public static void disable(){failed=true;nativeReady=false;try{TrafficNative.failed();}catch(Throwable ignored){}}
    private static int slot(){
        long id=Thread.currentThread().threadId();
        // A lower slot can retire while this thread still owns a higher slot.
        for(int i=0;i<32;i++)if(threads.get(i)==id)return i;
        for(int i=0;i<32;i++)if(threads.get(i)==0&&threads.compareAndSet(i,0,id))return i;
        disable();return -1;
    }
    public static void enter(){if(!qualified||failed)return;int i=slot();if(i<0)return;writers.incrementAndGet();depths[i]++;versions.incrementAndGet();changes[i]++;VarHandle.fullFence();}
    public static void leave(){if(!qualified||failed)return;VarHandle.fullFence();int i=slot();if(i<0)return;versions.incrementAndGet();changes[i]++;depths[i]--;writers.decrementAndGet();if(depths[i]<0)disable();else if(depths[i]==0&&copyOwner.get()!=Thread.currentThread().threadId())threads.set(i,0);}
    public static void changed(int id){if(!qualified||failed)return;if(id<0||id>=7){disable();return;}if(generations.incrementAndGet(id)==0)disable();}
    public static void replaced(){if(!qualified||failed)return;for(int i=0;i<7;i++)changed(i);}
    public static boolean begin(){
        if(!nativeReady||failed||!qualified||!Observer.trafficFrameReady())return false;
        int i=slot();if(i<0||writers.get()!=depths[i])return false;
        long id=Thread.currentThread().threadId();if(!copyOwner.compareAndSet(0,id))return false;
        copySlot=i;before=versions.get()-changes[i];epoch=Observer.terrainEpoch();frame=Observer.terrainFrame();start=BridgeNative.qpc();seen=0;
        if(start<=0||start<next){end();return false;}
        if(lastEpoch!=epoch){replaced();for(int n=0;n<7;n++)active[n]=false;lastEpoch=epoch;}
        VarHandle.fullFence();return true;
    }
    public static void header(long delta,float rate,boolean paused,int pauseMask,boolean loading,int mode){
        if(lastMode!=mode){replaced();lastMode=mode;for(int n=0;n<7;n++)active[n]=false;}
        int flags=(loading?TrafficSnapshot.LOADING:TrafficSnapshot.COHERENT)|(paused?TrafficSnapshot.PAUSED:0)|((pauseMask&2)!=0?TrafficSnapshot.FROZEN:0);
        buffer.putLong(24,epoch).putLong(32,frame).putLong(40,start).putLong(56,before);
        buffer.putInt(64,flags).putInt(68,mode).putLong(104,delta).putDouble(112,rate);
        buffer.putInt(120,Float.isFinite(rate)&&rate>=0&&delta>=0?1:0);
    }
    public static void aircraft(int index,int id,boolean on,boolean screen,boolean altitudeReport,boolean airbus,
        double lat,double lon,double altitude,double track,double pitch,double bank,double gs,float actualFpm,double requestedFpm,double trackRate){
        if(index<0||index>=7||id!=index||(seen&(1<<index))!=0){disable();return;}
        seen|=1<<index;int o=TrafficSnapshot.HEADER+index*TrafficSnapshot.SLOT;
        boolean now=on&&lastMode!=0&&(buffer.getInt(64)&TrafficSnapshot.LOADING)==0;
        if(now&&!active[index])changed(index);active[index]=now;
        int valid=0;if(Double.isFinite(lat)&&Math.abs(lat)<=Math.PI/2)valid|=1;
        if(Double.isFinite(lon)&&Math.abs(lon)<=2*Math.PI)valid|=2;
        if(Double.isFinite(altitude))valid|=4;if(Double.isFinite(track))valid|=8;
        if(Double.isFinite(pitch))valid|=16;if(Double.isFinite(bank))valid|=32;
        if(Double.isFinite(gs)&&gs>=0)valid|=64;if(Float.isFinite(actualFpm))valid|=128;
        if(Double.isFinite(requestedFpm))valid|=256;if(Double.isFinite(trackRate))valid|=512;
        buffer.putLong(o,generations.get(index)).putInt(o+8,id).putInt(o+12,valid);
        buffer.putInt(o+16,(on?1:0)|(screen?2:0)|(altitudeReport?4:0)|(airbus?8:0)|(now?16:0));
        buffer.putDouble(o+32,lat).putDouble(o+40,lon).putDouble(o+48,altitude).putDouble(o+56,track);
        buffer.putDouble(o+64,pitch).putDouble(o+72,bank).putDouble(o+80,gs).putDouble(o+88,actualFpm);
        buffer.putDouble(o+96,requestedFpm).putDouble(o+104,trackRate);
    }
    public static void publish(){
        buffer.putLong(48,BridgeNative.qpc());
        VarHandle.fullFence();int i=copySlot;
        if(failed||seen!=127||copyOwner.get()!=Thread.currentThread().threadId()||writers.get()!=depths[i]||versions.get()-changes[i]!=before||epoch!=Observer.terrainEpoch()||frame!=Observer.terrainFrame())return;
        buffer.putLong(16,++sequence);
        if(!TrafficNative.publish(buffer)){disable();return;}next=start+interval;
    }
    public static void end(){if(copyOwner.get()==Thread.currentThread().threadId())copyOwner.set(0);}
}
