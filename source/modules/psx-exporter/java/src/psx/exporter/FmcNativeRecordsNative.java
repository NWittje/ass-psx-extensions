package psx.exporter;
import java.nio.ByteBuffer;
/** Independent service plumbing ABI. These calls never initialize PSX owners. */
final class FmcNativeRecordsNative {
    static native int abiVersion();
    static native boolean configure(ByteBuffer profile,long lifecycle,long generation);
    static native boolean begin(long token,long lifecycle,long generation);
    static native long remainingNanos(long token);
    static native void complete(long token,int phase,int reason,long lifecycle,long generation,
                                long observation,ByteBuffer artifact,int length,boolean paused,boolean began);
    private FmcNativeRecordsNative(){}
}
