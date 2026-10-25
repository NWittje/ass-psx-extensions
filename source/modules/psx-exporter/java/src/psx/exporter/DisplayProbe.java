package psx.exporter;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;

/** Scalar diagnostics only. Source callbacks never allocate, wait or perform I/O. */
public final class DisplayProbe {
    static final int WORDS=128, RECORD_BYTES=1024, CAPACITY=4096, HEADER_BYTES=2048, MAX_BATCH=61;
    static final int REQUEST=0, UPDATE_IN=1, UPDATE_OUT=2, PAINT_IN=3, PAINT_OUT=4,
        PRESTO_IN=5, PRESTO_OUT=6, DYNAMICS_IN=7, DYNAMICS_OUT=8, QUARTER_IN=9,
        QUARTER_OUT=10, BUFFER_PASS=11, IDS_IN=12, IDS_OUT=13, UNIT_IN=14,
        UNIT_OUT=15, ROOT_IN=16, ROOT_OUT=17, BODY=18, NATIVE_FAILURE=19;
    private static final AtomicLongArray counters=new AtomicLongArray(128);
    private static final long[] ring=new long[CAPACITY*WORDS], work=new long[WORDS];
    private static final AtomicLong write=new AtomicLong(), read=new AtomicLong(), painting=new AtomicLong();
    private static final ByteBuffer batch=ByteBuffer.allocateDirect(65536).order(ByteOrder.LITTLE_ENDIAN);
    private static final ByteBuffer control=ByteBuffer.allocateDirect(16).order(ByteOrder.LITTLE_ENDIAN);
    private static volatile boolean enabled, nativeReady;
    private static volatile int failure, stage=-1;
    private static volatile long generation, deadline, sequence, thread, osThread, stageQpc, lastRequestQpc;
    private static long recordGeneration, paintStart, rootStart,quarterStart,idsStart;
    private static int unit=-1;
    private DisplayProbe() {}
    static void prepare(){
        // Resolve the JDK's lazy VarHandle call sites before any native callback can execute.
        // JFR showed first-use linkage allocations inside AtomicLongArray.incrementAndGet.
        counters.incrementAndGet(0);counters.set(0,0);counters.get(0);
        painting.compareAndSet(0,0);painting.get();painting.set(0);
        write.get();write.lazySet(write.get());read.get();read.lazySet(read.get());
        Arrays.fill(work,0);System.arraycopy(work,0,ring,0,WORDS);
        boolean timedWarmup=false;
        try{prepareNative();timedWarmup=true;}catch(Throwable unavailable){}
        nativeReady=timedWarmup;generation=timedWarmup?1:0;deadline=Long.MAX_VALUE;
        enabled=true;
        // Resolve the actual callback entry paths as startup work, with no native timing/PSX calls.
        event(REQUEST,0);event(PAINT_IN,0);event(PRESTO_IN,0);event(DYNAMICS_IN,0);
        event(DYNAMICS_OUT,0);event(PRESTO_OUT,0);event(QUARTER_IN,0);event(IDS_IN,0);
        beginUnit(0,0,false);gate(0,0);beginRenderer(false);event(BODY,0);event(ROOT_OUT,0);
        event(UNIT_OUT,0);event(IDS_OUT,0);event(QUARTER_OUT,0);event(PAINT_OUT,0);
        for(int i=0;i<128;i++)counters.set(i,0);
        sequence=0;thread=0;osThread=0;stage=-1;stageQpc=0;lastRequestQpc=0;
        nativeReady=false;generation=0;deadline=0;recordGeneration=0;write.set(0);read.set(0);
    }
    public static void failed(){failure=2;enabled=false;generation=0;}
    static void prepareNative(){BridgeNative.qpc();BridgeNative.displayThreadId();}
    private static boolean owned(){return painting.get()==Thread.currentThread().threadId();}
    private static long stamp(){
        if(recordGeneration==0)return 0;
        long now=BridgeNative.qpc();
        if(now>=deadline||recordGeneration!=generation){recordGeneration=0;counters.incrementAndGet(23);return 0;}
        return now;
    }
    public static void event(int code,int value){
        if(!enabled)return;
        if(value==0||code!=UPDATE_OUT&&code!=PAINT_OUT&&code!=PRESTO_OUT&&code!=DYNAMICS_OUT&&code!=QUARTER_OUT&&code!=IDS_OUT&&code!=UNIT_OUT&&code!=ROOT_OUT)counters.incrementAndGet(code);
        if(code==REQUEST){if(nativeReady&&generation!=0)lastRequestQpc=BridgeNative.qpc();return;}
        if(code==PAINT_IN){
            long current=Thread.currentThread().threadId();
            if(!painting.compareAndSet(0,current)){failure=1;enabled=false;counters.incrementAndGet(20);return;}
            if(thread!=0&&thread!=current){failure=3;enabled=false;painting.set(0);return;}
            thread=current;sequence++;unit=-1;recordGeneration=0;
            if(nativeReady&&generation!=0){long now=BridgeNative.qpc();if(now<deadline){
                recordGeneration=generation;paintStart=now;
                if(osThread==0)osThread=BridgeNative.displayThreadId();
                Arrays.fill(work,0);work[0]=recordGeneration;work[1]=sequence;work[2]=thread;work[3]=osThread;work[4]=now;
            }}
            stage=PAINT_IN;stageQpc=recordGeneration!=0?paintStart:0;return;
        }
        if(!owned())return;
        long now=stamp();stage=switch(code){
            case PRESTO_OUT,QUARTER_OUT -> PAINT_IN;
            case DYNAMICS_OUT -> PRESTO_IN;
            case IDS_OUT -> QUARTER_IN;
            case UNIT_OUT -> IDS_IN;
            case ROOT_OUT -> UNIT_IN;
            default -> code;
        };stageQpc=now;
        switch(code){
            case PAINT_OUT -> {
                if(recordGeneration!=0){work[5]=now;long w=write.get();
                    if(w-read.get()<CAPACITY){System.arraycopy(work,0,ring,(int)(w&(CAPACITY-1))*WORDS,WORDS);write.lazySet(w+1);}
                    else counters.incrementAndGet(21);
                }
                recordGeneration=0;unit=-1;painting.set(0);stage=-1;
            }
            case PRESTO_IN -> {if(recordGeneration!=0){work[10]++;work[16]=now;}}
            case PRESTO_OUT -> {if(recordGeneration!=0){if(value==0)work[11]++;work[17]=now;}}
            case DYNAMICS_IN -> {if(recordGeneration!=0){work[12]++;work[18]=now;}}
            case DYNAMICS_OUT -> {if(recordGeneration!=0){if(value==0)work[13]++;work[19]=now;}}
            case QUARTER_IN -> {if(recordGeneration!=0){work[7]++;quarterStart=now;if(work[21]==0)work[21]=now;}}
            case QUARTER_OUT -> {if(recordGeneration!=0){work[22]=now;work[23]+=now-quarterStart;}}
            case IDS_IN -> {if(recordGeneration!=0){idsStart=now;if(work[24]==0)work[24]=now;}}
            case IDS_OUT -> {if(recordGeneration!=0){work[25]=now;work[26]+=now-idsStart;}}
            case BUFFER_PASS -> {if(recordGeneration!=0){work[8]++;work[27]=now;}}
            case UNIT_OUT -> {if(unit>=0&&value==0){counters.incrementAndGet(33+unit*4);if(recordGeneration!=0)work[32+unit*14+3]++;}unit=-1;}
            case ROOT_IN -> {if(unit>=0){counters.incrementAndGet(34+unit*4);if(recordGeneration!=0){int b=32+unit*14;work[b+4]++;if(work[b+9]==0)work[b+9]=now;rootStart=now;}}}
            case ROOT_OUT -> {if(unit>=0&&recordGeneration!=0){int b=32+unit*14;if(value==0)work[b+5]++;work[b+10]=now;work[b+11]+=now-rootStart;}}
            case BODY -> {if(unit>=0){counters.incrementAndGet(35+unit*4);if(recordGeneration!=0)work[32+unit*14+6]++;}}
            case NATIVE_FAILURE -> {if(recordGeneration!=0){work[6]|=1L<<value;if(unit>=0)work[32+unit*14+(value==14?12:13)]++;}}
            default -> {}
        }
    }
    public static void beginUnit(int id,int assignment,boolean refresh){
        if(!enabled)return;event(UNIT_IN,id);if(!owned())return;
        if(id<0||id>=6||unit!=-1){failed();return;}unit=id;
        counters.incrementAndGet(32+id*4);
        if(recordGeneration!=0){int b=32+id*14;work[b]=assignment;work[b+1]|=refresh?1:0;work[b+2]++;work[9]++;}
    }
    public static void beginRenderer(boolean refresh){
        event(ROOT_IN,0);
        if(enabled&&owned()&&unit>=0&&recordGeneration!=0)work[32+unit*14+1]|=refresh?2:0;
    }
    /** Records the value already consumed by a native predicate; never repeats that predicate. */
    public static void gate(int id,int value){
        if(!enabled)return;
        boolean yes=id==0?value>0:value!=0;if(id<0||id>=12){failed();return;}
        counters.incrementAndGet(64+id*2+(yes?1:0));
        if(id==8&&yes)counters.incrementAndGet(22);
        if(id==8&&owned()&&recordGeneration!=0)work[20]=value;
        if(owned()&&unit>=0&&recordGeneration!=0){int b=32+unit*14;work[b+7]|=1L<<id;
            if(yes)work[b+8]|=1L<<id;else work[b+8]&=~(1L<<id);
        }
    }
    static void startWorker(String nativePath,String metadata){
        Thread worker=new Thread(()->{
            try{
                System.load(nativePath);
                if(BridgeNative.displayAbiVersion()!=1||!BridgeNative.displayInitialize(metadata)){failed();return;}
                prepareNative(); // resolve native clock/thread entries in this holder on the worker
                nativeReady=true;
                for(;;){
                    BridgeNative.displayPoll(control);long nextGeneration=control.getLong(0);
                    if(nextGeneration<0){nativeReady=false;failed();return;}
                    deadline=control.getLong(8);generation=nextGeneration;
                    batch.clear();for(int i=0;i<HEADER_BYTES/8;i++)batch.putLong(i*8,0);
                    batch.putLong(0,1);batch.putLong(8,RECORD_BYTES);batch.putLong(16,write.get());batch.putLong(24,read.get());
                    batch.putLong(32,counters.get(21));batch.putLong(40,failure);batch.putLong(48,generation);
                    batch.putLong(56,BridgeNative.displayFrequency());batch.putLong(64,sequence);batch.putLong(72,thread);
                    batch.putLong(80,osThread);batch.putLong(88,stage);batch.putLong(96,stageQpc);
                    batch.putLong(104,BridgeNative.qpc());batch.putLong(120,lastRequestQpc);
                    for(int i=0;i<128;i++)batch.putLong(512+i*8,counters.get(i));
                    long r=read.get(),w=write.get();int n=(int)Math.min(MAX_BATCH,w-r);
                    for(int j=0;j<n;j++){int slot=(int)((r+j)&(CAPACITY-1))*WORDS;
                        for(int i=0;i<WORDS;i++)batch.putLong(HEADER_BYTES+j*RECORD_BYTES+i*8,ring[slot+i]);}
                    batch.putLong(128,n);
                    if(BridgeNative.displayPublish(batch,HEADER_BYTES+n*RECORD_BYTES))read.lazySet(r+n);
                    Thread.sleep(200);
                }
            }catch(Throwable ignored){nativeReady=false;failed();}
        },"PSX display diagnostics");worker.setDaemon(true);worker.start();
    }
    static void resetForTest(){generation=0;nativeReady=false;prepare();failure=0;recordGeneration=0;thread=0;osThread=0;painting.set(0);sequence=0;write.set(0);read.set(0);for(int i=0;i<128;i++)counters.set(i,0);}
    static long countForTest(int id){return counters.get(id);}
    static int failureForTest(){return failure;}
}
