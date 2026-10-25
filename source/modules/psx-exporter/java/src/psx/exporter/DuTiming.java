package psx.exporter;
/** Optional diagnostics. No Java allocation in callbacks; failure is isolated. */
final class DuTiming {
    private static volatile boolean available;
    private static void failed(){available=false;try{DuTimingNative.disable();}catch(Throwable ignored){}}
    static void prepare(){available=false;try{available=DuTimingNative.abiVersion()==1&&DuTimingNative.prepare();if(available)DuTimingNative.event(0,0,0,0,0,0,0);}catch(Throwable ignored){failed();}}
    static void initialize(String source){if(available)try{if(!DuTimingNative.initialize(source))failed();}catch(Throwable ignored){failed();}}
    static void event(int producer,int stage,int unit,long generation,long frame,long paint,long value){if(available)try{DuTimingNative.event(producer,stage,unit,generation,frame,paint,value);}catch(Throwable ignored){failed();}}
    static void frame(int producer,int stage,int unit,PfdFrame f,long value){if(available)try{DuTimingNative.event(producer,stage,unit,f.generation,f.bytes.getLong(16),f.bytes.getLong(24),stage==25?f.bytes.getLong(48):stage==24?(f.valid?1:0):value);}catch(Throwable ignored){failed();}}
    static long clock(){if(available)try{return BridgeNative.qpc();}catch(Throwable ignored){failed();}return 0;}
    static void chunk(int producer,int stage,int unit,long generation,long sequence,java.nio.ByteBuffer payload,int kind,long value){if(available)try{DuTimingNative.event(producer,stage,unit,generation,sequence,kind==1?payload.getLong(24):0,value);}catch(Throwable ignored){failed();}}
}
