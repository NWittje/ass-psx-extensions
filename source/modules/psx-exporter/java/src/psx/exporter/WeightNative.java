package psx.exporter;
/** Readouts and independently probed action broker. Preparation never starts IPC. */
final class WeightNative {
 static native int abiVersion();
 static native int actionAbiVersion();
 static native boolean initialize(boolean startup);
 static native void sample(int capabilities,long lifecycle,double zfwCg,double fuelMax,int zfwMin,boolean loading);
 static native boolean poll(java.nio.ByteBuffer request,long lifecycle);
 static native boolean begin(long token,long lifecycle);
 static native void complete(long token,boolean returned);
 static native void reject(long token);
 static native void uncertain(long token);
 private WeightNative() {}
}
