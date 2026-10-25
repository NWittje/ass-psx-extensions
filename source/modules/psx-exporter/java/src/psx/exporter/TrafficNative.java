package psx.exporter;
import java.nio.ByteBuffer;

/** Independent bounded traffic JNI1; no network, command or simulation writer. */
final class TrafficNative {
    private TrafficNative() {}
    static native int abiVersion();
    static native long intervalTicks();
    static native boolean initialize(ByteBuffer source);
    static native void capabilities(int mask);
    static native void failed();
    static native boolean publish(ByteBuffer source);
    /** Single worker/test-reader drain. Returns bytes, zero if no current snapshot. */
    static native int poll(ByteBuffer destination);
    static native void stats(ByteBuffer destination);
}
