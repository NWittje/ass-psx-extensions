package psx.exporter;
import java.nio.ByteBuffer;

/** Independent FMC JNI1 worker boundary. These initial operations only inspect
 * VM initialization status: they advertise no capture/recall, start no service,
 * resolve no native members and never initialize PSX classes. Future producer
 * activation still requires the complete installed profile and command path.
 * GetClassStatus can require a VM safepoint; no native observer calls it. */
final class FmcNative {
    static native int abiVersion();
    static native boolean classStates(Class<?>[] owners,int[] states);
    // Worker-only runtime extensions of ABI1. Probes use null storage and
    // cannot start transport or make an optional capability available.
    static native boolean identity(ByteBuffer identity);
    static native boolean configure(ByteBuffer compatibility,long generation);
    static native void complete(long token,int reason,long lifecycle,long generation,long observationQpc,ByteBuffer artifact,int length);
    static final int INITIALIZED=4,ERROR=8,ARRAY=16,PRIMITIVE=32,KNOWN=63;
    static boolean initialized(int status){
        return (status&~KNOWN)==0&&(status&INITIALIZED)!=0&&(status&(ERROR|ARRAY|PRIMITIVE))==0;
    }
    private FmcNative(){}
}
