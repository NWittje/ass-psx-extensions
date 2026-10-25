package psx.exporter;

import java.awt.*;
import java.awt.geom.AffineTransform;
import java.awt.image.BufferedImage;
import java.nio.*;
import java.util.concurrent.atomic.AtomicLong;

/** One ordinary paint producer and one worker; no IPC or wait on the paint thread. */
public final class InstrumentRecorder {
    static final int MAGIC=0x31495044,LIMIT=1024*1024;
    // Native captain artwork coordinates. This face includes scale, lettering,
    // the complete strip travel and the OFF-flag clipping rectangle.
    static final Point CROP=new Point(-790,-270);
    static final int WIDTH=80,HEIGHT=240;
    private static final AffineTransform IDENTITY=new AffineTransform();
    private static InstrumentRecorder instance;
    private final PfdFrame[] slots=new PfdFrame[4];
    private final RecorderGraphics[] adapters=new RecorderGraphics[17];
    private final BufferedImage surface=new BufferedImage(WIDTH,HEIGHT,BufferedImage.TYPE_INT_ARGB_PRE);
    private final Graphics2D graphics=surface.createGraphics();
    private final ByteBuffer control=ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN);
    private final AtomicLong head=new AtomicLong(),tail=new AtomicLong(),owner=new AtomicLong();
    private volatile boolean enabled;
    private volatile boolean lowerAvailable;
    private boolean testControl,paintOkay,nativeFailure;
    private long lease,deadline,paintLease,paintDeadline,paintSequence,sequence,nextDue,sourceThread,frequency;
    private volatile long accepted,dropped,invalid,retries,failures,transportLost,paints;
    private int attempt,childIndex;
    private PfdFrame candidate;
    private com.sun.management.ThreadMXBean allocation;
    private long allocationStart;
    private InstrumentRecorder(){
        for(int n=0;n<4;n++)slots[n]=new PfdFrame(LIMIT,WIDTH,HEIGHT,MAGIC,1,8192);
        for(int n=0;n<17;n++){adapters[n]=new RecorderGraphics();adapters[n].instrument=this;adapters[n].context=n;}
        frequency=BridgeNative.displayFrequency();if(frequency<=0)throw new IllegalStateException("instrument QPC frequency");
        BridgeNative.instrumentRecorderPoll(control);BridgeNative.instrumentRecorderPublish(control,0);BridgeNative.qpc();BridgeNative.displayThreadId();
        head.get();tail.get();owner.compareAndSet(0,0);head.lazySet(0);tail.lazySet(0);
        java.lang.invoke.VarHandle.fullFence();
        for(PfdFrame frame:slots){frame.reset();frame.digest();}enabled=true;
        if(java.lang.management.ManagementFactory.getThreadMXBean() instanceof com.sun.management.ThreadMXBean counter&&counter.isThreadAllocatedMemorySupported()){
            counter.setThreadAllocatedMemoryEnabled(true);counter.getCurrentThreadAllocatedBytes();allocation=counter;
        }
    }
    static void prepare(){prepare(false);}
    static void prepare(boolean lower){instance=null;if(BridgeNative.instrumentRecorderAbiVersion()!=1)throw new IllegalStateException("instrument JNI ABI");InstrumentRecorder prepared=new InstrumentRecorder();prepared.lowerAvailable=lower;instance=prepared;}
    public static void lowerFailed(){InstrumentRecorder s=instance;if(s!=null)s.lowerAvailable=false;}
    public static void lower(long token){InstrumentRecorder s=instance;if(s==null||s.candidate==null||!s.lowerAvailable||s.owner.get()!=Thread.currentThread().threadId())return;
        int assignment=(int)(token&255),revision=(int)((token>>>24)&255);long seen=(token>>>8)&255,yes=(token>>>16)&255;
        if(revision!=1||assignment<10||assignment>12||seen!=233||(yes&~seen)!=0)return;
        s.candidate.bytes.putLong(120,seen).putLong(136,yes).putInt(192,assignment).putInt(196,revision);
    }
    public static void failed(){InstrumentRecorder s=instance;if(s!=null){s.enabled=false;s.failures++;s.paintOkay=false;if(s.candidate!=null)s.candidate.fail();s.candidate=null;s.owner.set(0);}}
    private static boolean owns(Graphics2D graphics){InstrumentRecorder s=instance;return s!=null&&s.enabled&&graphics instanceof RecorderGraphics r&&r.instrument==s&&r.frame==s.candidate&&s.owner.get()==Thread.currentThread().threadId();}
    public static int origin(int original,Graphics2D graphics){return owns(graphics)?0:original;}
    public static AffineTransform transform(AffineTransform original,Graphics2D graphics){return owns(graphics)?IDENTITY:original;}
    public static Rectangle clip(Rectangle original,Graphics2D graphics){return owns(graphics)?((RecorderGraphics)graphics).fullClip:original;}
    public static void event(int code,int value){InstrumentRecorder s=instance;if(s!=null)s.onEvent(code,value);}
    private void poll(){if(!testControl){BridgeNative.instrumentRecorderPoll(control);lease=control.getLong(0);deadline=control.getLong(8);if(lease==-1)throw new IllegalStateException("instrument transport initialization");}}
    private void onEvent(int code,int value){
        if(!enabled)return;long thread=Thread.currentThread().threadId();
        if(code==DisplayProbe.PAINT_IN){
            if(!owner.compareAndSet(0,thread)){failed();return;}if(sourceThread!=0&&sourceThread!=thread){failed();return;}sourceThread=thread;
            poll();paintLease=lease;paintDeadline=deadline;paintSequence++;paints++;attempt=0;nativeFailure=false;paintOkay=false;candidate=null;return;
        }
        if(owner.get()!=thread)return;
        if(code==DisplayProbe.BUFFER_PASS){if(++attempt>1)retries++;}
        if(code==DisplayProbe.NATIVE_FAILURE)nativeFailure=true;
        if(code==DisplayProbe.PAINT_OUT){paintOkay=value==0&&!nativeFailure&&attempt<=1;if(!paintOkay){invalid++;owner.set(0);}}
    }
    public static Graphics2D begin(){InstrumentRecorder s=instance;return s==null?null:s.beginFrame();}
    private Graphics2D beginFrame(){
        if(!enabled||owner.get()!=Thread.currentThread().threadId()||!paintOkay)return null;
        // The existing ordinary-frame initialization witness is independent of
        // pose subscriptions. Do not initialize native UI owners by observing
        // them during an earlier startup paint.
        if(!Observer.firstFrame){owner.set(0);return null;}
        paintOkay=false;poll();long now=BridgeNative.qpc();
        if(lease==0||lease!=paintLease||now>=deadline||now>=paintDeadline||now<nextDue){owner.set(0);return null;}
        nextDue=now+Math.max(1,(frequency+29)/30);
        long write=head.get();if(write-tail.get()>=4){dropped++;owner.set(0);return null;}
        allocationStart=allocation==null?0:allocation.getCurrentThreadAllocatedBytes();
        PfdFrame frame=slots[(int)(write&3)];frame.reset();frame.generation=lease;candidate=frame;childIndex=1;
        frame.bytes.putInt(12,1).putLong(16,++sequence).putLong(24,paintSequence).putLong(32,lease).putLong(40,now).putLong(56,frequency)
            .putInt(72,1).putInt(76,0).putLong(80,1).putLong(88,sourceThread).putLong(96,BridgeNative.displayThreadId());
        graphics.setTransform(IDENTITY);graphics.setClip(0,0,WIDTH,HEIGHT);graphics.setComposite(AlphaComposite.Src);graphics.setColor(Color.BLACK);graphics.fillRect(0,0,WIDTH,HEIGHT);
        graphics.setComposite(AlphaComposite.SrcOver);graphics.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
        graphics.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL,RenderingHints.VALUE_STROKE_PURE);
        graphics.translate(-CROP.x,-CROP.y);adapters[0].initialize(graphics,CROP,frame);return adapters[0];
    }
    public static void complete(){InstrumentRecorder s=instance;if(s!=null)s.completeFrame();}
    private void completeFrame(){
        try{
            if(candidate==null||owner.get()!=Thread.currentThread().threadId())return;
            for(int n=1;n<childIndex;n++)if(!adapters[n].disposed)adapters[n].dispose();
            poll();long end=BridgeNative.qpc();
            if(enabled&&candidate.valid&&lease==paintLease&&end<deadline&&end<paintDeadline){
                candidate.bytes.putLong(48,end).putLong(104,end-candidate.bytes.getLong(40)).putLong(112,allocation==null?0:Math.max(0,allocation.getCurrentThreadAllocatedBytes()-allocationStart)).putLong(128,dropped);
                candidate.freeze();head.lazySet(head.get()+1);accepted++;
            }else{candidate.fail();invalid++;}
        }finally{candidate=null;owner.set(0);for(int n=0;n<childIndex;n++){adapters[n].source=null;adapters[n].canonical=null;}}
    }
    RecorderGraphics child(){return childIndex<adapters.length?adapters[childIndex++]:null;}
    static void startWorker(String metadata){
        InstrumentRecorder s=instance;if(s==null)throw new IllegalStateException("instrument preparation missing");
        if(!metadata.endsWith("}"))throw new IllegalArgumentException("instrument source metadata");
        String capability=metadata.substring(0,metadata.length()-1)+",\"instrument_recorder\":\"1\",\"instrument_id\":\"1\",\"lower_eligibility\":\""+(s.lowerAvailable?"1":"0")+"\",\"allocation_counter\":\""+(s.allocation==null?"unavailable":"available")+"\"}";
        Thread worker=new Thread(()->s.run(capability),"PSX instrument recorder");worker.setDaemon(true);worker.start();
    }
    private void run(String metadata){try{
        if(!BridgeNative.instrumentRecorderInitialize(metadata))throw new IllegalStateException("instrument transport unavailable");
        ByteBuffer workerControl=ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN),chunk=ByteBuffer.allocateDirect(65520).order(ByteOrder.LITTLE_ENDIAN);
        for(;;){
            BridgeNative.instrumentRecorderPoll(workerControl);long generation=workerControl.getLong(0);if(generation==-1)throw new IllegalStateException("instrument transport failed");
            long read=tail.get(),write=head.get();
            if(read<write){
                // Drop only complete unclaimed frames; the producer cannot reuse
                // a slot until this worker publishes its release.
                if(write-read>1){transportLost+=write-read-1;read=write-1;tail.lazySet(read);}
                PfdFrame frame=slots[(int)(read&3)];long sending=generation!=0?generation:workerControl.getLong(32);
                try{
                    if(sending==0||frame.generation!=sending||(generation==0&&BridgeNative.qpc()>=workerControl.getLong(40)))transportLost++;
                    else{byte[] digest=frame.digest();for(int n=0;n<32;n++)frame.bytes.put(160+n,digest[n]);if(!send(chunk,frame,sending))transportLost++;}
                }finally{tail.lazySet(read+1);}
            }else{
                chunk.clear();chunk.putInt(3).putInt(152).putLong(generation).putLong(accepted).putLong(dropped).putLong(invalid).putLong(retries).putLong(failures).putLong(transportLost).putLong(paints);
                while(chunk.position()<152)chunk.putLong(0);BridgeNative.instrumentRecorderPublish(chunk,152);Thread.sleep(20);
            }
        }
    }catch(Throwable failure){failed();}}
    private boolean send(ByteBuffer chunk,PfdFrame frame,long generation)throws InterruptedException{
        int offset=0;while(offset<frame.used){int n=Math.min(65480,frame.used-offset);chunk.clear();chunk.putInt(1).putInt(frame.used).putLong(frame.bytes.getLong(16)).putLong(generation).putInt(offset).putInt(n);
            ByteBuffer part=frame.bytes.duplicate();part.position(offset).limit(offset+n);chunk.put(part);long until=System.nanoTime()+250_000_000L;
            while(!BridgeNative.instrumentRecorderPublish(chunk,32+n)){if(!enabled||System.nanoTime()>=until)return false;Thread.sleep(1);}offset+=n;
        }return true;
    }
    static void testLease(long generation,long deadline){InstrumentRecorder s=instance;s.testControl=true;s.lease=generation;s.deadline=deadline;s.nextDue=0;}
    static PfdFrame testCompleted(){InstrumentRecorder s=instance;long read=s.tail.get();if(read>=s.head.get())return null;PfdFrame result=s.slots[(int)(read&3)];s.tail.lazySet(read+1);return result;}
    static long testAccepted(){return instance.accepted;}
}
