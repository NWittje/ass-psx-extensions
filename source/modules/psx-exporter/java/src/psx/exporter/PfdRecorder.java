package psx.exporter;

import java.awt.*;
import java.awt.geom.AffineTransform;
import java.nio.*;
import java.util.concurrent.atomic.AtomicLong;
import java.lang.management.ManagementFactory;
import javax.imageio.ImageIO;
import java.io.ByteArrayOutputStream;

/** One paint-thread producer; fixed owned frames; independent lease/worker. */
public final class PfdRecorder {
    private static State[] states;
    private static volatile long paints;
    private static State active;
    private static final AtomicLong paintOwner=new AtomicLong();
    private PfdRecorder(){}
    private static volatile boolean timingEnabled;
    private static void timingFailed(){timingEnabled=false;try{DuTimingNative.disable();}catch(Throwable ignored){}}
    private static void timingPrepare(){timingEnabled=false;try{DuTiming.prepare();timingEnabled=true;}catch(Throwable ignored){timingFailed();}}
    private static void timingInitialize(String source){if(timingEnabled)try{DuTiming.initialize(source);}catch(Throwable ignored){timingFailed();}}
    private static void timingFrame(int producer,int stage,int unit,PfdFrame frame,long value){if(timingEnabled)try{DuTiming.frame(producer,stage,unit,frame,value);}catch(Throwable ignored){timingFailed();}}
    private static void timingEvent(int producer,int stage,int unit,long generation,long frame,long paint,long value){if(timingEnabled)try{DuTiming.event(producer,stage,unit,generation,frame,paint,value);}catch(Throwable ignored){timingFailed();}}
    private static void timingChunk(int producer,int stage,int unit,long generation,long sequence,ByteBuffer payload,int kind,long value){if(timingEnabled)try{DuTiming.chunk(producer,stage,unit,generation,sequence,payload,kind,value);}catch(Throwable ignored){timingFailed();}}
    private static long timingClock(){if(timingEnabled)try{return DuTiming.clock();}catch(Throwable ignored){timingFailed();}return 0;}
    private static long recallCaptureToken(int assignment){try{return FmcRenderingCache.captureToken(assignment);}catch(Throwable ignored){recallFailed();return 0;}}
    private static boolean recallCaptureCurrent(long token){try{return FmcRenderingCache.captureCurrent(token);}catch(Throwable ignored){recallFailed();return false;}}
    private static void recallFailed(){try{FmcRenderingCache.failed();}catch(Throwable ignored){}}
    private static void recallAccept(PfdFrame frame){try{FmcRenderingCache.accept(frame);}catch(Throwable ignored){recallFailed();}}
    static void prepare(){try{FmcRenderingCache.initialize();}catch(Throwable ignored){}timingPrepare();states=null;active=null;paints=0;paintOwner.set(0);try{if(BridgeNative.duRecorderAbiVersion()!=1)throw new IllegalStateException("multi-DU JNI ABI");State[] prepared=new State[6];for(int i=0;i<6;i++){prepared[i]=new State(i);prepared[i].prepare();}states=prepared;}catch(Throwable original){states=null;active=null;paintOwner.set(0);throw original;}}
    public static void failed(){if(states!=null)for(State s:states)s.failed();}
    public static void event(int code,int value){long thread=Thread.currentThread().threadId();if(code==DisplayProbe.PAINT_IN){if(!paintOwner.compareAndSet(0,thread)){failed();return;}}else if(paintOwner.get()!=thread)return;if(code==DisplayProbe.PAINT_IN&&states!=null&&states[0].enabled)paints++;if(states!=null)for(State s:states)s.event(code,value);if(code==DisplayProbe.UNIT_OUT||code==DisplayProbe.PAINT_OUT)active=null;if(code==DisplayProbe.PAINT_OUT){paintOwner.set(0);try{FmcRenderingCache.paintOut(value==0);}catch(Throwable original){recallFailed();}}}
    public static void unit(int id,int assigned,boolean refresh){if(states==null||paintOwner.get()!=Thread.currentThread().threadId())return;for(State s:states)s.unit(id,assigned,refresh);active=id>=0&&id<6?states[id]:null;}
    public static void gate(int id,int value){if(active!=null)active.gate(id,value);}
    public static Graphics2D begin(Graphics2D graphics,Point point,boolean refresh){return active==null?graphics:active.begin(graphics,point,refresh);}
    public static boolean refresh(Graphics2D graphics,boolean original){return graphics instanceof RecorderGraphics||original;}
    public static Rectangle clip(Rectangle nativeClip,Graphics2D graphics){return graphics instanceof RecorderGraphics r?r.fullClip:nativeClip;}
    public static void drawCount(){if(active!=null)active.drawCount();}
    public static void end(Graphics2D graphics){if(graphics instanceof RecorderGraphics r)r.state.end(graphics);}
    public static Graphics2D nativeGraphics(Graphics2D value){return value instanceof RecorderGraphics r?r.source:value;}
    private static RecorderGraphics current(Graphics2D value){State s=active;if(s==null||!s.recordingInvocation||s.owner.get()!=Thread.currentThread().threadId())return null;RecorderGraphics r=s.adapters[0];return value==r.source||value==r?r:null;}
    public static Graphics2D replayGraphics(Graphics2D value){RecorderGraphics r=current(value);return r==null?value:r;}
    public static AffineTransform nativeTransform(AffineTransform value,Graphics2D graphics){RecorderGraphics r=current(graphics);if(r==null||value==null)return value;AffineTransform mapped=new AffineTransform(r.mapping);mapped.concatenate(value);return mapped;}
    public static AffineTransform replayTransform(AffineTransform value,Graphics2D graphics)throws java.awt.geom.NoninvertibleTransformException{RecorderGraphics r=current(graphics);if(r==null||value==null)return value;AffineTransform mapped=r.mapping.createInverse();mapped.concatenate(value);return mapped;}
    static RecorderGraphics child(){return active==null?null:active.child();}
    static void testLease(long generation,long until){states[0].testLease(generation,until);}
    static void testLease(int unit,long generation,long until){states[unit].testLease(generation,until);}
    static PfdFrame testFrame(){return states[0].testFrame();}
    static PfdFrame testCompleted(){return states[0].testCompleted();}
    static PfdFrame testCompleted(int unit){return states[unit].testCompleted();}
    static long testLosses(){return states[0].testLosses();}
    static void startWorker(String metadata){timingInitialize(metadata);if(!metadata.endsWith("}"))throw new IllegalArgumentException("source metadata");String capability=metadata.substring(0,metadata.length()-1)+",\"du_recorder\":\"1\"}";for(State state:states)state.startWorker(capability);}
    static final class State {
    private PfdFrame[] slots;private RecorderGraphics[] adapters;
    private final AtomicLong head=new AtomicLong(),tail=new AtomicLong(),owner=new AtomicLong();
    private volatile boolean enabled;private volatile long lease,deadline,frequency;
    private final ByteBuffer paintControl=ByteBuffer.allocateDirect(16).order(ByteOrder.LITTLE_ENDIAN);
    private boolean testControl,fmcPaint;private long paintDeadline,abandoned;
    private volatile long accepted,dropped,invalid,retries,failures,transportLost,paints;private long fmcSequence,fmcAccepted,fmcDropped,fmcInvalid;
    private volatile long sourceThread;
    private volatile long ordinaryDraws,recordedDraws;private boolean recordingInvocation;private long drawStart;
    private long paintSequence,nextDue,epoch,sequence,paintGeneration,rootStart,allocatedStart;
    private int unit=-1,assignment=-1,lastAssignment=-1,attempt,childIndex;private long gates,yes;
    private boolean selected,eligible,nativeFailure;private PfdFrame candidate;
    private com.sun.management.ThreadMXBean allocation;
    private ByteBuffer workerControl,workerChunk;private BoundedPng workerPng;private javax.imageio.ImageWriter pngWriter;private java.util.List<java.lang.management.GarbageCollectorMXBean> gcCollectors;
    final int physical;
    State(int physical){this.physical=physical;}
    void prepare(){slots=new PfdFrame[4];for(int i=0;i<4;i++)slots[i]=new PfdFrame();adapters=new RecorderGraphics[17];for(int i=0;i<17;i++){adapters[i]=new RecorderGraphics();adapters[i].context=i;adapters[i].state=this;}
        frequency=BridgeNative.displayFrequency();BridgeNative.duRecorderPoll(physical,ByteBuffer.allocateDirect(16));BridgeNative.duRecorderPublish(physical,ByteBuffer.allocateDirect(64),0);BridgeNative.qpc();BridgeNative.displayThreadId();
        workerControl=ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN);workerChunk=ByteBuffer.allocateDirect(65520).order(ByteOrder.LITTLE_ENDIAN);workerPng=new BoundedPng();gcCollectors=ManagementFactory.getGarbageCollectorMXBeans();pngWriter=ImageIO.getImageWritersByFormatName("png").next();for(PfdFrame frame:slots){frame.reset();frame.digest();}try{encodePng(slots[0],workerPng);}catch(java.io.IOException failure){throw new IllegalStateException(failure);}workerPng.reset();
        if(ManagementFactory.getThreadMXBean() instanceof com.sun.management.ThreadMXBean b&&b.isThreadAllocatedMemorySupported()){b.setThreadAllocatedMemoryEnabled(true);allocation=b;b.getCurrentThreadAllocatedBytes();}
        BridgeNative.duRecorderPoll(physical,paintControl);paintControl.getLong(0);paintControl.getLong(8);head.set(0);tail.set(0);owner.set(0);sourceThread=accepted=dropped=invalid=retries=failures=transportLost=paints=abandoned=ordinaryDraws=recordedDraws=0;fmcSequence=fmcAccepted=fmcDropped=fmcInvalid=0;recordingInvocation=false;testControl=false;enabled=true;}
    public void failed(){enabled=false;lease=0;failures++;if(candidate!=null)candidate.fail();}
    int content(){return assignment<=2?1:assignment<=6?4:assignment<=9?5:6;}
    private boolean live(){return enabled&&paintGeneration!=0&&BridgeNative.qpc()<paintDeadline;}
    public void event(int code,int value){if(!enabled)return;long thread=Thread.currentThread().threadId();
        if(code==DisplayProbe.PAINT_IN){if(!owner.compareAndSet(0,thread)){failed();return;}if(sourceThread!=0&&sourceThread!=thread){failed();owner.set(0);return;}sourceThread=thread;paintSequence++;paints++;candidate=null;selected=false;eligible=false;nativeFailure=false;attempt=0;unit=-1;paintGeneration=0;paintDeadline=0;fmcPaint=false;if(testControl){paintGeneration=lease;paintDeadline=deadline;}else if(lease!=0){BridgeNative.duRecorderPoll(physical,paintControl);paintGeneration=paintControl.getLong(0);paintDeadline=paintControl.getLong(8);}
            long capture=recallCaptureToken(lastAssignment);if(capture>0&&!live())try{paintGeneration=capture;paintDeadline=BridgeNative.qpc()+frequency;fmcPaint=true;}catch(Throwable ignored){paintGeneration=0;paintDeadline=0;recallFailed();}
            if(live()){long now=BridgeNative.qpc();if(now>=nextDue){eligible=true;long period=Math.max(1,frequency/30);nextDue=nextDue==0?now+period:nextDue+(1+(now-nextDue)/period)*period;}}return;}
        if(owner.get()!=thread)return;
        if(code==DisplayProbe.BUFFER_PASS){if(++attempt>1){retries++;if(candidate!=null)candidate.fail();}}
        if(code==DisplayProbe.NATIVE_FAILURE){nativeFailure=true;if(candidate!=null)candidate.fail();}
        if(code==DisplayProbe.UNIT_OUT){if(unit==physical&&!selected&&eligible&&live()){
            if((gates&1)!=0&&(yes&1)!=0){PfdFrame f=reserve(2);if(f!=null){selected=true;candidate=f;}}
            else if(assignment<1||assignment>12||(gates&1)!=0&&(yes&1)!=0||((gates&8)!=0&&(yes&8)==0)){PfdFrame f=reserve(3);if(f!=null){selected=true;candidate=f;f.bytes.putInt(144,4);}}else {PfdFrame f=reserve(3);if(f!=null){selected=true;candidate=f;f.bytes.putInt(144,16);}}}unit=-1;}
        if(code==DisplayProbe.PAINT_OUT){if(candidate!=null){long current=lease,until=deadline;if(!candidate.fmcOnly&&!testControl){BridgeNative.duRecorderPoll(physical,paintControl);current=paintControl.getLong(0);until=paintControl.getLong(8);}boolean complete=value==0&&!nativeFailure&&attempt<=1&&candidate.valid;boolean authorized=candidate.fmcOnly?recallCaptureCurrent(candidate.fmcCaptureToken)&&BridgeNative.qpc()<paintDeadline:paintGeneration==current&&BridgeNative.qpc()<until;if(complete&&enabled&&authorized){candidate.bytes.putLong(48,BridgeNative.qpc());candidate.bytes.putLong(128,dropped);candidate.freeze();timingFrame(physical,25,physical,candidate,0);head.lazySet(head.get()+1);if(candidate.fmcOnly)fmcAccepted++;else accepted++;timingFrame(physical,57,physical,candidate,0);}else {timingFrame(physical,58,physical,candidate,complete?1:2);if(candidate.fmcOnly)fmcInvalid++;else if(complete)abandoned++;else invalid++;candidate.fail();}}candidate=null;unit=-1;owner.set(0);}
    }
    public void unit(int id,int assigned,boolean refresh){if(!enabled||owner.get()!=Thread.currentThread().threadId())return;unit=id;assignment=assigned;if(id==physical)gates=yes=0;
        if(id==physical&&assigned!=lastAssignment){epoch++;lastAssignment=assigned;if(candidate!=null)candidate.fail();}}
    public void gate(int id,int value){if(!enabled||owner.get()!=Thread.currentThread().threadId()||unit!=physical||id<0||id>11)return;long bit=1L<<id;gates|=bit;if(id==0?value>0:value!=0)yes|=bit;else yes&=~bit;}
    private PfdFrame reserve(int kind){long w=head.get();if(w-tail.get()>=4){timingEvent(physical,58,physical,paintGeneration,0,paintSequence,3);if(fmcPaint)fmcDropped++;else dropped++;return null;}PfdFrame f=slots[(int)(w&3)];f.reset();f.generation=paintGeneration;f.fmcOnly=fmcPaint;f.fmcCaptureToken=recallCaptureToken(assignment);f.bytes.putInt(4,physical==0&&kind<=3?1:2).putInt(12,kind).putLong(16,fmcPaint?++fmcSequence:++sequence).putLong(24,paintSequence).putLong(32,paintGeneration).putLong(40,BridgeNative.qpc()).putLong(56,frequency).putInt(72,physical).putInt(76,assignment).putLong(80,epoch).putLong(88,Thread.currentThread().threadId()).putLong(96,BridgeNative.displayThreadId()).putLong(120,gates).putLong(136,yes);return f;}
    public Graphics2D begin(Graphics2D source,Point position,boolean refresh){try{
        recordingInvocation=false;
        if(!enabled||unit!=physical||selected||!eligible||!live()||owner.get()!=Thread.currentThread().threadId())return source;
        selected=true;AffineTransform t=source.getTransform();Shape sourceClip=source.getClip();boolean profile=Double.isFinite(t.getScaleX())&&t.getScaleX()>0.4&&t.getScaleY()==t.getScaleX()&&t.getShearX()==0&&t.getShearY()==0&&(sourceClip==null||sourceClip instanceof java.awt.geom.Rectangle2D);
        PfdFrame f=reserve(profile?content():3);if(f==null)return source;candidate=f;f.bytes.putDouble(192,t.getScaleX()).putDouble(200,t.getScaleY()).putDouble(208,t.getShearX()).putDouble(216,t.getShearY()).putInt(224,position.x).putInt(228,position.y).putInt(232,refresh?1:0);
        if(!profile){f.bytes.putInt(144,2);return source;}
        childIndex=1;rootStart=BridgeNative.qpc();timingFrame(physical,23,physical,f,rootStart);allocatedStart=allocation==null?0:allocation.getCurrentThreadAllocatedBytes();drawStart=recordedDraws;adapters[0].initialize(source,position,f);f.bytes.putInt(144,1);recordingInvocation=true;return adapters[0];
    }catch(Throwable failure){if(fmcPaint){recallFailed();if(candidate!=null)candidate.fail();recordingInvocation=false;}else failed();if(adapters!=null&&adapters[0].source==source)try{adapters[0].restoreSource();}catch(Throwable ignored){}return source;}}
    public boolean refresh(Graphics2D graphics,boolean original){return graphics instanceof RecorderGraphics||original;}
    public Rectangle clip(Rectangle nativeClip,Graphics2D graphics){return graphics instanceof RecorderGraphics r?r.fullClip:nativeClip;}
    public void drawCount(){if(!enabled||unit!=physical||owner.get()!=Thread.currentThread().threadId())return;if(recordingInvocation)recordedDraws++;else ordinaryDraws++;}
    public void end(Graphics2D graphics){recordingInvocation=false;if(!(graphics instanceof RecorderGraphics r))return;try{for(int i=1;i<childIndex;i++)if(!adapters[i].disposed)adapters[i].dispose();if(r.frame.valid&&(gates&32)!=0&&(yes&32)==0&&(yes&128)==0)r.frame.unsupported(4);if(r.frame.valid&&content()==6&&(gates&64)!=0&&(yes&64)==0)r.frame.black();r.frame.bytes.putInt(156,(int)Math.min(Integer.MAX_VALUE,recordedDraws-drawStart)).putLong(120,gates).putLong(136,yes).putLong(104,BridgeNative.qpc()-rootStart).putLong(112,allocation==null?0:allocation.getCurrentThreadAllocatedBytes()-allocatedStart);}
        catch(Throwable failure){r.frame.fail();}finally{try{r.restoreSource();r.frame.bytes.putLong(104,BridgeNative.qpc()-rootStart).putLong(112,allocation==null?0:allocation.getCurrentThreadAllocatedBytes()-allocatedStart);}catch(Throwable failure){r.frame.fail();}timingFrame(physical,24,physical,r.frame,0);for(int i=0;i<childIndex;i++){adapters[i].source=null;adapters[i].canonical=null;}}}
    RecorderGraphics child(){if(childIndex>=17)return null;return adapters[childIndex++];}
    void testLease(long generation,long until){testControl=true;lease=generation;deadline=until;nextDue=0;}
    PfdFrame testFrame(){return candidate;}
    PfdFrame testCompleted(){long r=tail.get();if(r>=head.get())return null;PfdFrame f=slots[(int)(r&3)];tail.lazySet(r+1);return f;}
    long testLosses(){return dropped;}
    void startWorker(String metadata){Thread thread=new Thread(()->{try{
        if(!BridgeNative.duRecorderInitialize(physical,metadata)){failed();return;}ByteBuffer control=workerControl,chunk=workerChunk;long lastImage=0,lastEpoch=-1;int lastKind=-1;BoundedPng png=workerPng;var collectors=gcCollectors;
        for(;;){BridgeNative.duRecorderPoll(physical,control);long generation=control.getLong(0);if(generation==-1){failed();return;}deadline=control.getLong(8);lease=generation;
            long read=tail.get();if(read<head.get()){PfdFrame f=slots[(int)(read&3)];timingFrame(6+physical,26,physical,f,0);long sending=generation!=0?generation:control.getLong(32);try{recallAccept(f);if(!f.fmcOnly){if(f.generation!=sending||sending==0||generation==0&&BridgeNative.qpc()>=control.getLong(40)){timingFrame(6+physical,58,physical,f,4);transportLost++;}else{
                timingFrame(6+physical,27,physical,f,0);byte[] digest=f.digest();timingFrame(6+physical,28,physical,f,0);for(int i=0;i<32;i++)f.bytes.put(160+i,digest[i]);long now=f.bytes.getLong(40);boolean sample=f.bytes.getInt(12)!=3&&(lastImage==0||now-lastImage>=frequency||lastEpoch!=f.bytes.getLong(80)||lastKind!=f.bytes.getInt(12));
                if(!send(chunk,f.bytes,f.used,1,f.bytes.getLong(16),sending))transportLost++;
                else if(sample){timingFrame(6+physical,29,physical,f,0);encodePng(f,png);timingFrame(6+physical,30,physical,f,0);ByteBuffer image=ByteBuffer.wrap(png.buffer(),0,png.size());if(!send(chunk,image,png.size(),2,f.bytes.getLong(16),sending))transportLost++;lastImage=now;lastEpoch=f.bytes.getLong(80);}lastKind=f.bytes.getInt(12);
            }}}finally{long timingGeneration=0,timingSequence=0,timingPaint=0;boolean timingRelease=timingEnabled;if(timingRelease)try{timingGeneration=f.generation;timingSequence=f.bytes.getLong(16);timingPaint=f.bytes.getLong(24);}catch(Throwable ignored){timingFailed();timingRelease=false;}tail.lazySet(read+1);if(timingRelease)timingEvent(6+physical,31,physical,timingGeneration,timingSequence,timingPaint,0);}}
            else {long gcCount=0,gcMillis=0;for(var gc:collectors){gcCount+=Math.max(0,gc.getCollectionCount());gcMillis+=Math.max(0,gc.getCollectionTime());}long sourceBytes=allocation==null||sourceThread==0?0:Math.max(0,allocation.getThreadAllocatedBytes(sourceThread)),workerBytes=allocation==null?0:allocation.getCurrentThreadAllocatedBytes();timingEvent(6+physical,59,physical,generation,0,0,gcCount);timingEvent(6+physical,60,physical,generation,0,0,gcMillis);timingEvent(6+physical,61,physical,generation,0,0,sourceBytes);timingEvent(6+physical,62,physical,generation,0,0,workerBytes);chunk.clear();chunk.putInt(3).putInt(152).putLong(generation).putLong(accepted).putLong(dropped).putLong(invalid).putLong(retries).putLong(failures).putLong(transportLost).putLong(paints).putLong(head.get()-tail.get()).putLong(control.getLong(16)).putLong(control.getLong(24)).putLong(gcCount).putLong(gcMillis).putLong(sourceBytes).putLong(workerBytes).putLong(abandoned).putLong(ordinaryDraws).putLong(recordedDraws);BridgeNative.duRecorderPublish(physical,chunk,152);Thread.sleep(20);}
        }
    }catch(Throwable failure){failed();}},"PSX DU "+physical+" recorder diagnostics");thread.setDaemon(true);thread.start();}
    private void encodePng(PfdFrame frame,BoundedPng png)throws java.io.IOException{png.reset();try(var output=new javax.imageio.stream.MemoryCacheImageOutputStream(png)){pngWriter.setOutput(output);pngWriter.write(frame.reference);output.flush();}finally{pngWriter.setOutput(null);}}
    private boolean send(ByteBuffer chunk,ByteBuffer payload,int size,int kind,long sequence,long generation)throws Exception{int offset=0;while(offset<size){int n=Math.min(65480,size-offset);chunk.clear();chunk.putInt(kind).putInt(size).putLong(sequence).putLong(generation).putInt(offset).putInt(n);ByteBuffer part=payload.duplicate();part.position(offset).limit(offset+n);chunk.put(part);long until=System.nanoTime()+250_000_000L;long retryStart=0;while(!BridgeNative.duRecorderPublish(physical,chunk,32+n)){if(retryStart==0)retryStart=timingClock();timingChunk(6+physical,34,physical,generation,sequence,payload,kind,offset);if(System.nanoTime()>=until){timingChunk(6+physical,35,physical,generation,sequence,payload,kind,timingClock()-retryStart);return false;}Thread.sleep(1);}if(retryStart!=0)timingChunk(6+physical,35,physical,generation,sequence,payload,kind,timingClock()-retryStart);offset+=n;}return true;}
    private static final class BoundedPng extends ByteArrayOutputStream {
        BoundedPng(){super(4*1024*1024);} byte[] buffer(){return buf;}
        @Override public synchronized void write(int value){if(count==buf.length)throw new IllegalStateException("PNG budget");super.write(value);}
        @Override public synchronized void write(byte[] data,int offset,int length){if(length>buf.length-count)throw new IllegalStateException("PNG budget");super.write(data,offset,length);}
    }
    }
}
