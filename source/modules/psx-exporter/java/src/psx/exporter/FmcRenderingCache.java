package psx.exporter;

import java.awt.*;
import java.awt.image.*;
import java.util.concurrent.atomic.*;

/** A worker-owned copy of a completed native ND image. Arming precedes
 * admission closure: old native drawings drain, subsequent drawings retain
 * complete pixels, and the other instrument displays continue normally.
 * Native renderer objects and image contents never originate in FMC2. */
public final class FmcRenderingCache {
    private static final AtomicLong sequence=new AtomicLong();
    private static volatile Request request;
    private static final class Raster {
        final BufferedImage image;final BufferedImage[] strips;
        Raster(BufferedImage image){this.image=image;int split=image.getHeight()/2;strips=new BufferedImage[]{image.getSubimage(0,0,image.getWidth(),split),image.getSubimage(0,split,image.getWidth(),image.getHeight()-split)};}
    }
    /** Prepared on the command worker; invoked only by the original paint-out
     * callback after all ordinary instrument drawing has finished. */
    interface Drawing {boolean draw(Object owner,Graphics2D graphics,Point point)throws Throwable;}
    private static final class Capture {
        final Raster raster;final Graphics2D graphics;final Point point=new Point();
        Capture(){var image=new BufferedImage(PfdFrame.WIDTH,PfdFrame.HEIGHT,BufferedImage.TYPE_INT_ARGB_PRE);raster=new Raster(image);graphics=image.createGraphics();graphics.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);graphics.setRenderingHint(RenderingHints.KEY_INTERPOLATION,RenderingHints.VALUE_INTERPOLATION_BILINEAR);graphics.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL,RenderingHints.VALUE_STROKE_PURE);graphics.setClip(0,0,PfdFrame.WIDTH,PfdFrame.HEIGHT);graphics.setColor(Color.BLACK);graphics.fillRect(0,0,PfdFrame.WIDTH,PfdFrame.HEIGHT);}
    }
    static final class Request {
        final long identity,lifecycle;final Object[] owners;
        final AtomicReferenceArray<Raster> images=new AtomicReferenceArray<>(4);
        final Drawing drawing;final Capture[] captures;final AtomicInteger captureState;
        volatile boolean armed,failed,closedObserved;volatile Throwable captureFailure;
        Request(long identity,long lifecycle,Object[] owners,Drawing drawing){this.identity=identity;this.lifecycle=lifecycle;this.owners=owners.clone();this.drawing=drawing;captureState=new AtomicInteger(drawing==null?2:0);captures=drawing==null?null:new Capture[4];if(captures!=null)for(int i=0;i<4;i++)captures[i]=new Capture();}
    }
    private FmcRenderingCache(){}
    static void initialize(){}
    static long request(long lifecycle,Object[] owners){return request(lifecycle,owners,null);}
    static synchronized long request(long lifecycle,Object[] owners,Drawing drawing){
        require(request==null&&!FmcExclusion.admissionClosed()&&lifecycle==Observer.terrainEpoch()&&owners.length==4,"open renderer binding");
        for(int i=0;i<4;i++){require(owners[i]!=null,"native ND root");for(int j=0;j<i;j++)require(owners[i]!=owners[j],"distinct native ND roots");}
        long id=sequence.incrementAndGet();require(id>0,"request identity exhausted");request=new Request(id,lifecycle,owners,drawing);return id;
    }
    /** Paint producer primitive probe. It never requests a DU transport lease
     * or changes an external collector generation. */
    static long captureToken(int assignment){var r=request;return r!=null&&!r.failed&&!r.armed&&r.lifecycle==Observer.terrainEpoch()&&assignment>=3&&assignment<=6&&r.images.get(assignment-3)==null?r.identity:0;}
    static boolean captureCurrent(long identity){var r=request;return identity>0&&r!=null&&r.identity==identity&&!r.failed&&!r.armed&&r.lifecycle==Observer.terrainEpoch();}
    /** The existing recorder worker owns the completed slot until its tail
     * publication. Copy pixels here, never on the native drawing callback. */
    static void accept(PfdFrame frame){
        var r=request;int assignment=frame.bytes.getInt(76);long token=frame.fmcCaptureToken;
        if(r==null||token!=r.identity||!captureCurrent(token)||assignment<3||assignment>6||frame.bytes.getInt(12)!=4||!frame.valid)return;
        int index=assignment-3;if(r.images.get(index)!=null)return;
        var image=new BufferedImage(PfdFrame.WIDTH,PfdFrame.HEIGHT,BufferedImage.TYPE_INT_ARGB_PRE);
        image.getRaster().setRect(frame.reference.getRaster());
        if(captureCurrent(token))r.images.compareAndSet(index,null,new Raster(image));
    }
    /** Existing complete-paint callback only. No waits, I/O, jobs or request
     * allocation occur here. Each missing root gets its own original draw;
     * hidden second NDs never borrow pixels from the visible first NDs. */
    static void paintOut(boolean complete){
        var r=request;if(!complete||r==null||r.drawing==null||r.failed||r.armed||r.lifecycle!=Observer.terrainEpoch()||FmcExclusion.admissionClosed()||!r.captureState.compareAndSet(0,1))return;
        int ticket=0;
        try{
            ticket=FmcExclusion.enter();if(ticket<=0){r.failed=true;return;}
            for(int i=0;i<4;i++){
                if(r.failed||r.lifecycle!=Observer.terrainEpoch()){r.failed=true;return;}
                if(r.images.get(i)!=null)continue;
                var capture=r.captures[i];
                if(r.drawing.draw(r.owners[i],capture.graphics,capture.point)&&r.lifecycle==Observer.terrainEpoch()&&!r.failed)r.images.compareAndSet(i,null,capture.raster);
            }
        }catch(Throwable original){r.captureFailure=original;r.failed=true;}
        finally{try{if(ticket>0)FmcExclusion.leave(ticket);}catch(Throwable failure){r.failed=true;if(r.captureFailure==null)r.captureFailure=failure;}finally{r.captureState.set(2);}}
    }
static void arm(long identity,Object[] visible){
        var r=request;if(r!=null&&r.identity==identity&&r.captureFailure!=null)throw new IllegalStateException("FMC original hidden rendering failed before admission closure",r.captureFailure);require(r!=null&&r.identity==identity&&!r.failed&&!r.armed&&r.lifecycle==Observer.terrainEpoch()&&!FmcExclusion.admissionClosed(),"current unarmed renderer request");require(r.captureState.get()!=1,"original hidden renderer capture must retire before arm");
        for(Object owner:visible)if(owner!=null){int index=index(r,owner);require(index>=0&&r.images.get(index)!=null,"complete visible ND image pending");}
        int state=r.captureState.get();require(state!=1&&(state!=0||r.captureState.compareAndSet(0,3)),"pending capture must seal atomically before admission closure");r.armed=true;
    }
    static boolean armed(long identity){var r=request;if(r!=null&&r.identity==identity&&r.armed&&FmcExclusion.admissionClosed())r.closedObserved=true;return r!=null&&r.identity==identity&&r.armed&&!r.failed&&r.lifecycle==Observer.terrainEpoch();}
    static boolean complete(long identity,Object owner){var r=request;if(r==null||r.identity!=identity||r.failed||r.lifecycle!=Observer.terrainEpoch())return false;int index=index(r,owner);return index>=0&&r.images.get(index)!=null;}
    /** Worker-only publication of the immutable two-strip raster. Native
     * fallback owns a separate array, while the completed pixels stay shared. */
    static BufferedImage[] nativeImage(long identity,Object owner){var r=request;require(!FmcExclusion.currentAdmitted()&&r!=null&&r.identity==identity&&!r.failed&&r.lifecycle==Observer.terrainEpoch(),"current worker raster publication");int i=index(r,owner);require(i>=0,"native renderer identity");var raster=r.images.get(i);return raster==null?null:raster.strips.clone();}
    /** Called after the original native clip and power/blank predicates. The
     * untouched drawing body is used when no recall image is armed. */
    public static boolean draw(Object owner,Graphics2D graphics,Point point){
        var r=request;if(r==null||!r.armed)return false;
        if(r.closedObserved&&!FmcExclusion.admissionClosed())return false;
        if(r.lifecycle!=Observer.terrainEpoch()){failed();return false;}
        if(r.failed)failed();
        int index=index(r,owner);if(index<0){failed();return false;}
        var image=r.images.get(index);if(image==null){failed();return false;}
        // Each strip fits the existing recorder's one-MiB bitmap resource
        // scratch space. The pixels share the immutable worker-owned raster.
        graphics.drawImage(image.strips[0],point.x,point.y,null);
        graphics.drawImage(image.strips[1],point.x,point.y+image.strips[0].getHeight(),null);return true;
    }
    static void clear(long identity){var r=request;require(!FmcExclusion.admissionClosed()&&r!=null&&r.identity==identity,"terminal renderer release");int before=r.captureState.get();require(before!=1&&(before==3||r.captureState.compareAndSet(before,3)),"original hidden rendering must retire before release");request=null;if(r.captures!=null)for(var capture:r.captures)capture.graphics.dispose();}
    public static void failed(){var r=request;if(r!=null)r.failed=true;if(FmcExclusion.admissionClosed())FmcExclusion.failed();}
    private static int index(Request r,Object owner){for(int i=0;i<4;i++)if(r.owners[i]==owner)return i;return -1;}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC rendering "+why);}
}
