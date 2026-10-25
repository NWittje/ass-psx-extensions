package psx.exporter;
import java.nio.ByteBuffer;
/** Fixed ResetMalfunctions boundary; no arbitrary method/argument service. */
final class ResetNative {
 static native int abiVersion();
 static native boolean initialize(boolean startup);
 static native void capabilities(boolean available);
 static native boolean poll(ByteBuffer request,long lifecycle);
 static native boolean begin(long token,long lifecycle);
 static native void complete(long token,boolean returned);
 private ResetNative() {}
}
