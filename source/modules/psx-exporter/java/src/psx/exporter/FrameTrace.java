package psx.exporter;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Capture-only native frame stages; buffers are initialized before simulator main. */
public final class FrameTrace {
    private static final ByteBuffer detail=ByteBuffer.allocateDirect(1024).order(ByteOrder.LITTLE_ENDIAN);
    private static long generation,epoch,frame,thread;
    private static int mask,osThread;
    private FrameTrace() {}
    static void prepare() {} // Agent calls this before installing the startup transformer.
    public static void failed(){generation=0;}
    public static void enter(){
        try{
            generation=0;mask=0;
            if(!Terrain.nativeReady||!Observer.firstFrame)return;
            long capture=BridgeNative.terrainCaptureGeneration();if(capture==0)return;
            generation=capture;epoch=Observer.terrainEpoch();frame=Observer.terrainFrame()+1;
            long current=Thread.currentThread().threadId();if(thread!=current){thread=current;osThread=BridgeNative.terrainThreadId();}
            stage(0);
        }catch(Throwable ignored){generation=0;}
    }
    public static void stage(int site){
        if(generation==0)return;
        try{if(Thread.currentThread().threadId()!=thread||site<0||site>=8){generation=0;return;}detail.putLong(72+site*8,BridgeNative.qpc());mask|=1<<site;}
        catch(Throwable ignored){generation=0;}
    }
    public static void exit(double delta,double deltaAcc,boolean failed){
        if(generation==0)return;
        try{
            stage(7);if(generation==0||epoch!=Observer.terrainEpoch())return;
            detail.putInt(0,3);detail.putInt(4,136);detail.putLong(8,epoch);detail.putLong(16,frame);detail.putLong(24,generation);detail.putLong(32,thread);
            detail.putInt(40,osThread);detail.putInt(44,mask);detail.putInt(48,failed?1:0);detail.putInt(52,0);detail.putDouble(56,delta);detail.putDouble(64,deltaAcc);
            BridgeNative.terrainDetailPublish(detail);
        }catch(Throwable ignored){}finally{generation=0;}
    }
}
