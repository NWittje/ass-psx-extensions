package psx.exporter;
import java.nio.ByteBuffer;
/** Independent situation JNI1. Used by workers/EDT commands, never physics callbacks. */
final class SituationNative {
 static native int abiVersion();
 static native boolean initialize(boolean startup);
 static native void capabilities(int mask);
 static native boolean poll(ByteBuffer request,long lifecycle);
 static native boolean begin(long token,long lifecycle);
 static native void complete(long token,int reason,int evidence,long lifecycle,long observationQpc,ByteBuffer artifact,int length,double[] readback);
 static native void sourceHash(ByteBuffer hash);
 private SituationNative(){}
}
