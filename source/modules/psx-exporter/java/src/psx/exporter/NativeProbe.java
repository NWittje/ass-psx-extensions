package psx.exporter;

import java.io.IOException;
import java.nio.file.Path;

/** Disposable helper entry point, also used on immutable preparation-cache hits. */
public final class NativeProbe {
    private NativeProbe() {}
    static void verify(Path nativePath) throws IOException {
        System.load(nativePath.toString());
        if (BridgeNative.abiVersion() != 2) throw new IOException("native ABI revision");
        if (BridgeNative.runtimeInstanceSupport() != 1) throw new IOException("native runtime instance support");
        long first = BridgeNative.qpc(), second = BridgeNative.qpc();
        if (first <= 0 || second < first) throw new IOException("native QPC probe");
    }
    static boolean terrainAvailable(){try{
        if(BridgeNative.terrainAbiVersion()!=3)return false;
        // Resolve the entire independent primitive JNI boundary without initialize/threads/IPC/PSX.
        BridgeNative.terrainCapabilities(0);BridgeNative.terrainFrequency();BridgeNative.terrainConnection();BridgeNative.terrainConnected();
        BridgeNative.terrainPoll(java.nio.ByteBuffer.allocateDirect(160),1);BridgeNative.terrainApplied(0,1,0);
        BridgeNative.terrainPublish(1,0,1,0,0,1,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0);
        BridgeNative.terrainCaptureGeneration();BridgeNative.terrainThreadId();BridgeNative.terrainDetailPublish(java.nio.ByteBuffer.allocateDirect(1024));
        return true;
    }catch(UnsatisfiedLinkError unavailable){return false;}}
    static boolean instrumentAvailable(){return InstrumentLayer.available();}
    static boolean recorderAvailable(){try{
        if(BridgeNative.recorderAbiVersion()!=1)return false;
        BridgeNative.recorderPoll(java.nio.ByteBuffer.allocateDirect(16));
        BridgeNative.recorderPublish(java.nio.ByteBuffer.allocateDirect(64),0);
        return !BridgeNative.recorderInitialize("");
    }catch(Throwable unavailable){return false;}}
    static boolean duRecorderAvailable(){try{
        if(BridgeNative.duRecorderAbiVersion()!=1)return false;
        for(int unit=0;unit<6;unit++){
            BridgeNative.duRecorderPoll(unit,java.nio.ByteBuffer.allocateDirect(48));
            BridgeNative.duRecorderPublish(unit,java.nio.ByteBuffer.allocateDirect(64),0);
            if(BridgeNative.duRecorderInitialize(unit,""))return false;
        }
        return true;
    }catch(Throwable unavailable){return false;}}
    static boolean displayAvailable(){try{
        if(BridgeNative.displayAbiVersion()!=1)return false;
        BridgeNative.displayFrequency();BridgeNative.displayThreadId();
        BridgeNative.displayPoll(java.nio.ByteBuffer.allocateDirect(16));
        BridgeNative.displayPublish(java.nio.ByteBuffer.allocateDirect(2048),2048);
        if(BridgeNative.displayInitialize(""))return false; // empty metadata must be a pure rejection
        return true;
    }catch(Throwable unavailable){return false;}}
    static boolean aircraftAvailable(){try{
        if(BridgeNative.aircraftAbiVersion()!=1)return false;
        BridgeNative.aircraftCapabilities(0);BridgeNative.aircraftState(0,false);
        BridgeNative.aircraftPublish(0,0,0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
        return true;
    }catch(Throwable unavailable){return false;}}
    static boolean environmentAvailable(){return EnvironmentLayer.available();}
    static boolean statusAvailable(){return StatusLayer.available();}
    static boolean trafficAvailable(){return TrafficLayer.available();}
    static boolean resetAvailable(){try{
        if(ResetNative.abiVersion()!=1)return false;
        if(ResetNative.initialize(false))return false;
        ResetNative.capabilities(false);ResetNative.poll(java.nio.ByteBuffer.allocateDirect(40),0);
        ResetNative.begin(0,0);ResetNative.complete(0,false);return true;
    }catch(Throwable unavailable){return false;}}
    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("expected absolute native DLL path");
            Path path = Path.of(args[0]);
            if (!path.isAbsolute()) throw new IllegalArgumentException("native path must be absolute");
            verify(path.toRealPath());
            System.out.println("{\"supported\":true,\"native_abi\":2,\"terrain_abi\":"+(terrainAvailable()?3:0)+",\"display_abi\":"+(displayAvailable()?1:0)+",\"recorder_abi\":"+(recorderAvailable()?1:0)+",\"du_recorder_abi\":"+(duRecorderAvailable()?1:0)+",\"aircraft_abi\":"+(aircraftAvailable()?1:0)+",\"environment_abi\":"+(environmentAvailable()?1:0)+",\"status_abi\":"+(statusAvailable()?1:0)+",\"traffic_abi\":"+(trafficAvailable()?1:0)+",\"situation_abi\":"+(SituationLayer.available()?1:0)+"}");
        } catch (Throwable failure) {
            System.out.println("{\"supported\":false,\"reason\":" + Identity.json(failure.toString()) + "}");
            System.exit(2);
        }
    }
}
