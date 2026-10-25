package psx.exporter;

import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicReferenceArray;

/** Narrow output-only lease filter. No PSX preference or simulation field is assigned. */
public final class AudioOutput {
    private static final AtomicIntegerArray gains=new AtomicIntegerArray(12);
    private static final AtomicIntegerArray known=new AtomicIntegerArray(12);
    private static final AtomicReferenceArray<Object> identities=new AtomicReferenceArray<>(12);
    private static final AtomicIntegerArray sourceIds=new AtomicIntegerArray(12);
    private static long previous;
    private static volatile boolean gainsSuppressed;
    static volatile boolean ready;
    private AudioOutput() {}
    public static float gain(int index,Object identity,int source,float gain) {
        if(!ready)return gain;
        if(identities.get(index)!=identity||sourceIds.get(index)!=source){known.set(index,0);identities.set(index,identity);sourceIds.set(index,source);}
        gains.set(index,Float.floatToRawIntBits(gain));known.set(index,1);
        if((BridgeNative.audioControl()&(index==11?128:1))!=0){gainsSuppressed=true;return 0;}return gain;
    }
    public static long control(){return ready?BridgeNative.audioControl():0;}
    public static boolean needsService(){return ready||gainsSuppressed;}
    public static boolean changed(long token){return (token&255)!=(previous&255);}
    public static boolean missing(int index,Object identity,int source) {
        if(identities.get(index)!=identity||sourceIds.get(index)!=source){known.set(index,0);identities.set(index,identity);sourceIds.set(index,source);}
        return known.get(index)==0;
    }
    public static void remember(int index,float gain){gains.set(index,Float.floatToRawIntBits(gain));known.set(index,1);}
    public static float restored(int index,long token){return (token&(index==11?128:1))!=0?0:Float.intBitsToFloat(gains.get(index));}
    public static void completed(long token){previous=token;gainsSuppressed=(token&255)!=0;if(ready)BridgeNative.audioApplied(token);}
    public static void failed(){AudioObserver.suppressionCapabilities=0;ready=false;}
}
