package psx.exporter;

import java.awt.*;
import java.awt.geom.*;
import java.awt.image.*;
import java.nio.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.Arrays;

/** DPF1 contains numeric geometry and owned pixels, never Java serialization. */
final class PfdFrame {
    static final int MAGIC=0x31465044, HEADER=256, LIMIT=8*1024*1024, WIDTH=662, HEIGHT=685;
    static final int PATH=1, STROKE=2, IMAGE=3, DRAW=4, FILL=5, CLIP=6, CREATE=7, DISPOSE=8,
        TRANSFORM=9, COLOR=10, USE_STROKE=11, COMPOSITE=12, HINT=13, INTERSECT=14, PRIMITIVE=15, BITMAP=16;
    static final RenderingHints.Key[] KEYS={RenderingHints.KEY_ANTIALIASING,RenderingHints.KEY_STROKE_CONTROL,RenderingHints.KEY_INTERPOLATION};
    static final Object[][] VALUES={{RenderingHints.VALUE_ANTIALIAS_DEFAULT,RenderingHints.VALUE_ANTIALIAS_OFF,RenderingHints.VALUE_ANTIALIAS_ON},
        {RenderingHints.VALUE_STROKE_DEFAULT,RenderingHints.VALUE_STROKE_NORMALIZE,RenderingHints.VALUE_STROKE_PURE},
        {RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR,RenderingHints.VALUE_INTERPOLATION_BILINEAR,RenderingHints.VALUE_INTERPOLATION_BICUBIC}};
    final int width,height,limit,magic,format;
    final ByteBuffer bytes,scratch;
    final BufferedImage reference;
    final Graphics2D graphics;
    final int[] pixels,offsets,hashes,lengths,ids;
    final byte[] pixelBytes;
    final MessageDigest referenceDigest;
    final double[] coordinates=new double[6], matrix=new double[6];
    final int[] versions=new int[17];
    int operations,resources,used,bitmapBytes; boolean valid,fmcOnly; long generation,fmcCaptureToken;
    PfdFrame(){this(LIMIT,WIDTH,HEIGHT,MAGIC,0,65536);}
    PfdFrame(int limit,int width,int height,int magic,int format,int dictionary){
        if(limit<HEADER||limit>LIMIT||width<1||height<1||width>4096||height>4096||dictionary<1||Integer.bitCount(dictionary)!=1||dictionary>65536)throw new IllegalArgumentException("drawing frame profile");
        this.limit=limit;this.width=width;this.height=height;this.magic=magic;this.format=format;
        bytes=ByteBuffer.allocateDirect(limit).order(ByteOrder.LITTLE_ENDIAN);scratch=ByteBuffer.allocate(Math.min(1024*1024,limit-HEADER)).order(ByteOrder.LITTLE_ENDIAN);
        reference=new BufferedImage(width,height,BufferedImage.TYPE_INT_ARGB_PRE);graphics=reference.createGraphics();pixels=new int[width*height];pixelBytes=new byte[width*height*4];
        offsets=new int[dictionary];hashes=new int[dictionary];lengths=new int[dictionary];ids=new int[dictionary];
        try{referenceDigest=MessageDigest.getInstance("SHA-256");}catch(java.security.NoSuchAlgorithmException failure){throw new IllegalStateException(failure);}
    }
    void reset(){bytes.clear();for(int i=0;i<HEADER;i++)bytes.put(i,(byte)0);bytes.position(HEADER);Arrays.fill(offsets,0);Arrays.fill(versions,0);operations=resources=bitmapBytes=0;valid=true;generation=0;fmcOnly=false;fmcCaptureToken=0;
        graphics.setTransform(new AffineTransform());graphics.setClip(null);graphics.setComposite(AlphaComposite.Src);graphics.setColor(Color.BLACK);graphics.fillRect(0,0,width,height);graphics.setComposite(AlphaComposite.SrcOver);}
    void fail(){valid=false;}
    void black(){operations=resources=bitmapBytes=0;bytes.position(HEADER);bytes.putInt(12,2).putInt(144,0);graphics.setTransform(new AffineTransform());graphics.setClip(null);graphics.setComposite(AlphaComposite.Src);graphics.setColor(Color.BLACK);graphics.fillRect(0,0,width,height);graphics.setComposite(AlphaComposite.SrcOver);}
    void unsupported(int reason){operations=resources=bitmapBytes=0;bytes.position(HEADER);bytes.putInt(12,3).putInt(144,reason);}
    void reserve(int count){if(count<0||bytes.remaining()<count){valid=false;throw new IllegalStateException("DPF1 arena limit");}}
    void op(int code,int context,int length){reserve(16+length);bytes.putInt(code).putInt(length).putInt(context).putInt(++versions[context]);operations++;}
    int resource(int code){int length=scratch.position();if(length>limit-HEADER-16){fail();return 0;}int hash=0x811c9dc5;
        for(int i=0;i<length;i++)hash=(hash^(scratch.get(i)&255))*0x01000193;hash^=code;int mask=offsets.length-1,slot=hash&mask;
        for(int probes=0;probes<offsets.length;probes++,slot=(slot+1)&mask){if(offsets[slot]==0){op(code,0,length+4);int id=++resources;bytes.putInt(id);int at=bytes.position();scratch.flip();bytes.put(scratch);offsets[slot]=at;lengths[slot]=length;hashes[slot]=hash;ids[slot]=id;return id;}
            if(hashes[slot]==hash&&lengths[slot]==length){boolean same=true;for(int i=0;i<length;i++)if(bytes.get(offsets[slot]+i)!=scratch.get(i)){same=false;break;}if(same)return ids[slot];}}
        fail();return 0;
    }
    int path(Shape shape){if(shape==null)return 0;scratch.clear();int precision=shape instanceof Path2D.Float?4:8;
        PathIterator iterator=shape.getPathIterator(null);scratch.putInt(precision).putInt(iterator.getWindingRule()).putInt(0);int count=0;
        while(!iterator.isDone()){int kind=iterator.currentSegment(coordinates),n=switch(kind){case 0,1->2;case 2->4;case 3->6;case 4->0;default->throw new IllegalArgumentException("path segment");};
            scratch.putInt(kind);for(int i=0;i<n;i++)if(precision==4)scratch.putFloat((float)coordinates[i]);else scratch.putDouble(coordinates[i]);count++;iterator.next();}
        scratch.putInt(8,count);return resource(PATH);
    }
    int stroke(Stroke stroke){if(!(stroke instanceof BasicStroke s)){fail();return 0;}scratch.clear();scratch.putFloat(s.getLineWidth()).putInt(s.getEndCap()).putInt(s.getLineJoin()).putFloat(s.getMiterLimit()).putFloat(s.getDashPhase());float[] dash=s.getDashArray();scratch.putInt(dash==null?0:dash.length);if(dash!=null)for(float v:dash)scratch.putFloat(v);return resource(STROKE);}
    int image(Image image){if(!(image instanceof BufferedImage b)||b.getWidth()<1||b.getHeight()<1||(long)b.getWidth()*b.getHeight()>(scratch.capacity()-16)/4){fail();return 0;}
        scratch.clear();scratch.putInt(b.getWidth()).putInt(b.getHeight());for(int y=0;y<b.getHeight();y++)for(int x=0;x<b.getWidth();x++)scratch.putInt(b.getRGB(x,y));bitmapBytes+=b.getWidth()*b.getHeight()*4;return resource(IMAGE);}
    void transform(int context,AffineTransform t){t.getMatrix(matrix);op(TRANSFORM,context,48);for(double v:matrix)bytes.putDouble(v);}
    void operand(int code,int context,int value){op(code,context,4);bytes.putInt(value);}
    void composite(int context,Composite composite){if(!(composite instanceof AlphaComposite a)){fail();return;}op(COMPOSITE,context,8);bytes.putInt(a.getRule()).putFloat(a.getAlpha());}
    void hint(int context,RenderingHints.Key key,Object value){for(int k=0;k<KEYS.length;k++)if(key==KEYS[k]){if(value==null&&k==2)value=RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR;for(int v=0;v<VALUES[k].length;v++)if(value==VALUES[k][v]){op(HINT,context,8);bytes.putInt(k).putInt(v);return;}fail();return;}fail();}
    void primitive(int context,int kind,int... arguments){op(PRIMITIVE,context,8+arguments.length*4);bytes.putInt(kind).putInt(arguments.length);for(int a:arguments)bytes.putInt(a);}
    void polygon(int context,int kind,int[] x,int[] y,int n){op(PRIMITIVE,context,8+n*8);bytes.putInt(kind).putInt(n*2);for(int i=0;i<n;i++)bytes.putInt(x[i]).putInt(y[i]);}
    void freeze(){used=bytes.position();bytes.putInt(0,magic).putInt(4,format!=0?format:bytes.getInt(72)==0&&bytes.getInt(12)<=3?1:2).putInt(8,used).putInt(64,width).putInt(68,height).putInt(148,operations).putInt(152,resources).putInt(240,bitmapBytes);}
    byte[] digest() {reference.getRGB(0,0,width,height,pixels,0,width);int at=0;for(int p:pixels){pixelBytes[at++]=(byte)(p>>>24);pixelBytes[at++]=(byte)(p>>>16);pixelBytes[at++]=(byte)(p>>>8);pixelBytes[at++]=(byte)p;}return referenceDigest.digest(pixelBytes);}
    static ByteBuffer load(Path path)throws Exception {long size=Files.size(path);if(size<HEADER||size>LIMIT)throw new IllegalArgumentException("DPF1 file budget");return ByteBuffer.wrap(Files.readAllBytes(path)).order(ByteOrder.LITTLE_ENDIAN);}
    static BufferedImage replay(ByteBuffer input,int width,int height){return replay(input,width,height,MAGIC,WIDTH,HEIGHT,LIMIT);}
    static BufferedImage replayInstrument(ByteBuffer input,int width,int height){
        ByteBuffer b=input.duplicate().order(ByteOrder.LITTLE_ENDIAN);if(b.limit()<HEADER)throw new IllegalArgumentException("DPI1 header");
        int w=b.getInt(64),h=b.getInt(68);if(w<1||h<1||w>2048||h>2048||(long)w*h>262144||b.getInt(4)!=1||b.getInt(72)!=1||b.getInt(76)!=0)throw new IllegalArgumentException("DPI1 profile");
        return replay(input,width,height,InstrumentRecorder.MAGIC,w,h,InstrumentRecorder.LIMIT);
    }
    private static BufferedImage replay(ByteBuffer input,int width,int height,int magic,int logicalWidth,int logicalHeight,int limit){ByteBuffer b=input.duplicate().order(ByteOrder.LITTLE_ENDIAN);if(b.limit()<HEADER||b.limit()>limit||b.getInt(0)!=magic||(b.getInt(4)!=1&&(magic!=MAGIC||b.getInt(4)!=2))||b.getInt(8)!=b.limit()||b.getInt(64)!=logicalWidth||b.getInt(68)!=logicalHeight)throw new IllegalArgumentException("drawing header");
        BufferedImage image=new BufferedImage(width,height,BufferedImage.TYPE_INT_ARGB_PRE);Graphics2D root=image.createGraphics();root.setColor(Color.BLACK);root.fillRect(0,0,width,height);
        double scale=Math.min((double)width/logicalWidth,(double)height/logicalHeight);AffineTransform viewport=new AffineTransform();viewport.translate((width-logicalWidth*scale)/2,(height-logicalHeight*scale)/2);viewport.scale(scale,scale);
        Graphics2D[] contexts=new Graphics2D[17];int[] versions=new int[17];contexts[0]=root;Object[] resources=new Object[65537];int definitions=0,operations=0;b.position(HEADER);
        try{while(b.hasRemaining()){int code=b.getInt(),length=b.getInt(),context=b.getInt(),version=b.getInt();if(length<0||length>b.remaining()||context<0||context>=17||version!=++versions[context])throw new IllegalArgumentException("DPF1 operation length/context/version");int end=b.position()+length;Graphics2D g=contexts[context];if(g==null)throw new IllegalArgumentException("DPF1 disposed context");
            if(code>=PATH&&code<=IMAGE){int id=b.getInt();if(id!=++definitions||id>65536)throw new IllegalArgumentException("DPF1 resource order");
                if(code==PATH){int precision=b.getInt(),winding=b.getInt(),count=b.getInt();if((precision!=4&&precision!=8)||count<0||count>length/4)throw new IllegalArgumentException("DPF1 path");Path2D p=precision==4?new Path2D.Float(winding):new Path2D.Double(winding);double[] c=new double[6];for(int n=0;n<count;n++){int k=b.getInt(),v=switch(k){case 0,1->2;case 2->4;case 3->6;case 4->0;default->throw new IllegalArgumentException("DPF1 segment");};for(int i=0;i<v;i++)c[i]=precision==4?b.getFloat():b.getDouble();switch(k){case 0->p.moveTo(c[0],c[1]);case 1->p.lineTo(c[0],c[1]);case 2->p.quadTo(c[0],c[1],c[2],c[3]);case 3->p.curveTo(c[0],c[1],c[2],c[3],c[4],c[5]);case 4->p.closePath();}}resources[id]=p;
                }else if(code==STROKE){float w=b.getFloat();int cap=b.getInt(),join=b.getInt();float m=b.getFloat(),phase=b.getFloat();int n=b.getInt();if(n<0||n>(end-b.position())/4)throw new IllegalArgumentException("DPF1 dash");float[] dash=n==0?null:new float[n];for(int i=0;i<n;i++)dash[i]=b.getFloat();resources[id]=new BasicStroke(w,cap,join,m,dash,phase);
                }else {int w=b.getInt(),h=b.getInt();if(w<1||h<1||(long)w*h*4!=end-b.position())throw new IllegalArgumentException("DPF1 bitmap");BufferedImage r=new BufferedImage(w,h,BufferedImage.TYPE_INT_ARGB);for(int y=0;y<h;y++)for(int x=0;x<w;x++)r.setRGB(x,y,b.getInt());resources[id]=r;}
            }else switch(code){
                case DRAW,FILL,CLIP,INTERSECT,USE_STROKE->{int id=b.getInt();if(id<0||id>definitions)throw new IllegalArgumentException("DPF1 missing resource");Object r=resources[id];switch(code){case DRAW->g.draw((Shape)r);case FILL->g.fill((Shape)r);case CLIP->g.setClip((Shape)r);case INTERSECT->g.clip((Shape)r);case USE_STROKE->g.setStroke((Stroke)r);}}
                case CREATE->{int child=b.getInt();if(child<1||child>=17||contexts[child]!=null)throw new IllegalArgumentException("DPF1 child lifetime");contexts[child]=(Graphics2D)g.create();}
                case DISPOSE->{if(context==0)throw new IllegalArgumentException("DPF1 root disposal");g.dispose();contexts[context]=null;}
                case TRANSFORM->{double[] m=new double[6];for(int i=0;i<6;i++)m[i]=b.getDouble();AffineTransform t=new AffineTransform(viewport);t.concatenate(new AffineTransform(m));g.setTransform(t);}
                case COLOR->g.setColor(new Color(b.getInt(),true));
                case COMPOSITE->g.setComposite(AlphaComposite.getInstance(b.getInt(),b.getFloat()));
                case HINT->{int k=b.getInt(),v=b.getInt();if(k<0||k>=KEYS.length||v<0||v>=VALUES[k].length)throw new IllegalArgumentException("DPF1 hint");g.setRenderingHint(KEYS[k],VALUES[k][v]);}
                case BITMAP->{int id=b.getInt();if(id<1||id>definitions||!(resources[id] instanceof BufferedImage))throw new IllegalArgumentException("DPF1 image reference");double[] m=new double[6];for(int i=0;i<6;i++)m[i]=b.getDouble();g.drawImage((BufferedImage)resources[id],new AffineTransform(m),null);}
                case PRIMITIVE->{int kind=b.getInt(),n=b.getInt();if(n<0||n>100000||(long)n*4!=end-b.position())throw new IllegalArgumentException("DPF1 primitive");int[] a=new int[n];for(int i=0;i<n;i++)a[i]=b.getInt();primitive(g,kind,a);}
                default->throw new IllegalArgumentException("DPF1 opcode");
            }
            if(b.position()!=end)throw new IllegalArgumentException("DPF1 operand consumption");operations++;}
            if(operations!=b.getInt(148)||definitions!=b.getInt(152))throw new IllegalArgumentException("DPF1 counts");return image;
        }finally{for(Graphics2D g:contexts)if(g!=null)g.dispose();}
    }
    static void primitive(Graphics2D g,int kind,int[] a){switch(kind){case 1->g.drawLine(a[0],a[1],a[2],a[3]);case 2->g.fillRect(a[0],a[1],a[2],a[3]);case 3->g.clearRect(a[0],a[1],a[2],a[3]);case 4->g.drawRect(a[0],a[1],a[2],a[3]);case 5->g.drawOval(a[0],a[1],a[2],a[3]);case 6->g.fillOval(a[0],a[1],a[2],a[3]);case 7->g.drawRoundRect(a[0],a[1],a[2],a[3],a[4],a[5]);case 8->g.fillRoundRect(a[0],a[1],a[2],a[3],a[4],a[5]);case 9->g.drawArc(a[0],a[1],a[2],a[3],a[4],a[5]);case 10->g.fillArc(a[0],a[1],a[2],a[3],a[4],a[5]);case 11,12,13->{int n=a.length/2;int[] x=new int[n],y=new int[n];for(int i=0;i<n;i++){x[i]=a[i*2];y[i]=a[i*2+1];}if(kind==11)g.drawPolyline(x,y,n);else if(kind==12)g.drawPolygon(x,y,n);else g.fillPolygon(x,y,n);}default->throw new IllegalArgumentException("DPF1 primitive kind");}}
}
