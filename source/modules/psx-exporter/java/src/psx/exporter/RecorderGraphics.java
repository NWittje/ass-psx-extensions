package psx.exporter;

import java.awt.*;
import java.awt.font.*;
import java.awt.geom.*;
import java.awt.image.*;
import java.awt.image.renderable.RenderableImage;
import java.text.AttributedCharacterIterator;
import java.util.Map;

/** Source drawing is primary. Recorder failures never re-execute a source operation. */
final class RecorderGraphics extends Graphics2D {
    PfdRecorder.State state;
    InstrumentRecorder instrument;
    Graphics2D source,canonical; PfdFrame frame; int context; boolean owned,disposed;
    final AffineTransform mapping=new AffineTransform(),mapped=new AffineTransform();
    final AffineTransform sourceBaseTransform=new AffineTransform(),sourceCurrentTransform=new AffineTransform();Shape sourceNativeClip;
    Shape sourceDeviceClip; final Rectangle fullClip=new Rectangle();
    interface Capture {void run();}
    void capture(Capture action){if(frame.valid)try{action.run();}catch(Throwable failure){frame.fail();}}
    void reference(Capture action){try{action.run();}catch(Throwable failure){frame.fail();}}
    void bind(Graphics2D s,Graphics2D c,PfdFrame f,int id,AffineTransform map,Shape deviceClip,Rectangle full,boolean child){source=s;canonical=c;frame=f;context=id;mapping.setTransform(map);sourceDeviceClip=deviceClip;fullClip.setBounds(full);owned=child;disposed=false;}
    void initialize(Graphics2D s,Point point,PfdFrame f){AffineTransform nativeTransform=s.getTransform();mapping.setTransform(nativeTransform);mapping.translate(point.x,point.y);
        Shape clip=s.getClip();sourceDeviceClip=clip==null?null:nativeTransform.createTransformedShape(clip);if(clip instanceof Rectangle2D){Rectangle2D device=sourceDeviceClip.getBounds2D();int x=(int)Math.ceil(device.getMinX()-.5),y=(int)Math.ceil(device.getMinY()-.5),right=(int)Math.ceil(device.getMaxX()-.5),bottom=(int)Math.ceil(device.getMaxY()-.5);sourceDeviceClip=new Rectangle(x,y,Math.max(0,right-x),Math.max(0,bottom-y));}fullClip.setBounds(point.x,point.y,f.width,f.height);
        bind(s,f.graphics,f,0,mapping,sourceDeviceClip,fullClip,false);
        sourceBaseTransform.setTransform(nativeTransform);sourceNativeClip=clip;
        canonical.setTransform(AffineTransform.getTranslateInstance(-point.x,-point.y));canonical.setClip(fullClip);canonical.setRenderingHints(s.getRenderingHints());canonical.setColor(s.getColor());canonical.setStroke(s.getStroke());canonical.setComposite(s.getComposite());canonical.setBackground(s.getBackground());
        syncClip();
        capture(()->{frame.transform(context,canonical.getTransform());frame.operand(PfdFrame.CLIP,context,frame.path(fullClip));frame.operand(PfdFrame.COLOR,context,canonical.getColor().getRGB());frame.operand(PfdFrame.USE_STROKE,context,frame.stroke(canonical.getStroke()));frame.composite(context,canonical.getComposite());for(int i=0;i<PfdFrame.KEYS.length;i++)frame.hint(context,PfdFrame.KEYS[i],canonical.getRenderingHint(PfdFrame.KEYS[i]));});
    }
    void syncTransform(){mapped.setTransform(mapping);mapped.concatenate(canonical.getTransform());source.setTransform(mapped);capture(()->frame.transform(context,canonical.getTransform()));}
    void restoreSource(){source.setTransform(sourceBaseTransform);source.setClip(sourceNativeClip);}
    static Shape mapClip(Shape clip,AffineTransform transform){if(clip instanceof Rectangle2D r&&transform.getShearX()==0&&transform.getShearY()==0&&transform.getScaleX()>0&&transform.getScaleY()>0)return new Rectangle2D.Double(r.getX()*transform.getScaleX()+transform.getTranslateX(),r.getY()*transform.getScaleY()+transform.getTranslateY(),r.getWidth()*transform.getScaleX(),r.getHeight()*transform.getScaleY());return transform.createTransformedShape(clip);}
    void syncClip(){try{Shape clip=canonical.getClip();sourceCurrentTransform.setTransform(source.getTransform());AffineTransform lcdTransform=sourceCurrentTransform.createInverse();lcdTransform.concatenate(mapping);lcdTransform.concatenate(canonical.getTransform());Shape lcd=mapClip(clip==null?fullClip:clip,lcdTransform);source.setTransform(new AffineTransform());source.setClip(sourceDeviceClip);source.setTransform(sourceCurrentTransform);source.clip(lcd);}catch(NoninvertibleTransformException failure){frame.fail();source.setClip(new Rectangle());}}
    @Override public void draw(Shape s){source.draw(s);reference(()->canonical.draw(s));capture(()->frame.operand(PfdFrame.DRAW,context,frame.path(s)));}
    @Override public void fill(Shape s){source.fill(s);reference(()->canonical.fill(s));capture(()->frame.operand(PfdFrame.FILL,context,frame.path(s)));}
    @Override public void setColor(Color c){source.setColor(c);reference(()->canonical.setColor(c));capture(()->frame.operand(PfdFrame.COLOR,context,canonical.getColor().getRGB()));}
    @Override public Color getColor(){return canonical.getColor();}
    @Override public void setPaint(Paint p){source.setPaint(p);reference(()->canonical.setPaint(p));if(p instanceof Color c)capture(()->frame.operand(PfdFrame.COLOR,context,c.getRGB()));else frame.fail();}
    @Override public Paint getPaint(){return canonical.getPaint();}
    @Override public void setStroke(Stroke s){source.setStroke(s);reference(()->canonical.setStroke(s));capture(()->frame.operand(PfdFrame.USE_STROKE,context,frame.stroke(s)));}
    @Override public Stroke getStroke(){return canonical.getStroke();}
    @Override public void setComposite(Composite c){source.setComposite(c);reference(()->canonical.setComposite(c));capture(()->frame.composite(context,c));}
    @Override public Composite getComposite(){return canonical.getComposite();}
    @Override public void setRenderingHint(RenderingHints.Key k,Object v){source.setRenderingHint(k,v);reference(()->canonical.setRenderingHint(k,v));capture(()->frame.hint(context,k,v));}
    @Override public Object getRenderingHint(RenderingHints.Key k){return canonical.getRenderingHint(k);}
    @Override public void setRenderingHints(Map<?,?> h){source.setRenderingHints(h);reference(()->canonical.setRenderingHints(h));frame.fail();}
    @Override public void addRenderingHints(Map<?,?> h){source.addRenderingHints(h);reference(()->canonical.addRenderingHints(h));capture(()->{for(var e:h.entrySet())frame.hint(context,(RenderingHints.Key)e.getKey(),e.getValue());});}
    @Override public RenderingHints getRenderingHints(){return canonical.getRenderingHints();}
    @Override public void setTransform(AffineTransform t){canonical.setTransform(t);syncTransform();}
    @Override public AffineTransform getTransform(){return canonical.getTransform();}
    @Override public void transform(AffineTransform t){canonical.transform(t);syncTransform();}
    @Override public void translate(int x,int y){translate((double)x,(double)y);}
    @Override public void translate(double x,double y){canonical.translate(x,y);syncTransform();}
    @Override public void rotate(double a){canonical.rotate(a);syncTransform();}
    @Override public void rotate(double a,double x,double y){canonical.rotate(a,x,y);syncTransform();}
    @Override public void scale(double x,double y){canonical.scale(x,y);syncTransform();}
    @Override public void shear(double x,double y){canonical.shear(x,y);syncTransform();}
    @Override public void setClip(Shape s){canonical.setClip(s);syncClip();capture(()->frame.operand(PfdFrame.CLIP,context,frame.path(s)));}
    @Override public void setClip(int x,int y,int w,int h){setClip(new Rectangle(x,y,w,h));}
    @Override public Shape getClip(){return canonical.getClip();}
    @Override public Rectangle getClipBounds(){return canonical.getClipBounds();}
    @Override public void clip(Shape s){canonical.clip(s);syncClip();capture(()->frame.operand(PfdFrame.INTERSECT,context,frame.path(s)));}
    @Override public void clipRect(int x,int y,int w,int h){clip(new Rectangle(x,y,w,h));}
    @Override public boolean hitClip(int x,int y,int w,int h){return canonical.hitClip(x,y,w,h);}
    @Override public boolean hit(Rectangle r,Shape s,boolean stroke){return canonical.hit(r,s,stroke);}
    @Override public GraphicsConfiguration getDeviceConfiguration(){return canonical.getDeviceConfiguration();}
    @Override public FontRenderContext getFontRenderContext(){return canonical.getFontRenderContext();}
    @Override public void setBackground(Color c){source.setBackground(c);canonical.setBackground(c);frame.fail();}
    @Override public Color getBackground(){return canonical.getBackground();}
    @Override public Font getFont(){return canonical.getFont();}
    @Override public void setFont(Font f){source.setFont(f);canonical.setFont(f);frame.fail();}
    @Override public FontMetrics getFontMetrics(Font f){return canonical.getFontMetrics(f);}
    @Override public void setPaintMode(){source.setPaintMode();canonical.setPaintMode();capture(()->frame.composite(context,canonical.getComposite()));}
    @Override public void setXORMode(Color c){source.setXORMode(c);canonical.setXORMode(c);frame.fail();}
    @Override public Graphics create(){Graphics2D s=(Graphics2D)source.create();Graphics2D c=(Graphics2D)canonical.create();RecorderGraphics child=instrument!=null?instrument.child():state.child();if(child==null){c.dispose();frame.fail();return s;}int id=child.context;child.bind(s,c,frame,id,mapping,sourceDeviceClip,fullClip,true);child.sourceBaseTransform.setTransform(sourceBaseTransform);child.sourceNativeClip=sourceNativeClip;capture(()->frame.operand(PfdFrame.CREATE,context,id));return child;}
    @Override public void dispose(){if(disposed)return;disposed=true;if(owned){source.dispose();canonical.dispose();capture(()->{frame.op(PfdFrame.DISPOSE,context,0);});}}
    @Override public void drawLine(int x,int y,int a,int b){source.drawLine(x,y,a,b);reference(()->canonical.drawLine(x,y,a,b));capture(()->frame.primitive(context,1,x,y,a,b));}
    @Override public void fillRect(int x,int y,int w,int h){source.fillRect(x,y,w,h);reference(()->canonical.fillRect(x,y,w,h));capture(()->frame.primitive(context,2,x,y,w,h));}
    @Override public void clearRect(int x,int y,int w,int h){source.clearRect(x,y,w,h);reference(()->canonical.clearRect(x,y,w,h));frame.fail();}
    @Override public void drawRect(int x,int y,int w,int h){source.drawRect(x,y,w,h);reference(()->canonical.drawRect(x,y,w,h));capture(()->frame.primitive(context,4,x,y,w,h));}
    @Override public void drawOval(int x,int y,int w,int h){source.drawOval(x,y,w,h);reference(()->canonical.drawOval(x,y,w,h));capture(()->frame.primitive(context,5,x,y,w,h));}
    @Override public void fillOval(int x,int y,int w,int h){source.fillOval(x,y,w,h);reference(()->canonical.fillOval(x,y,w,h));capture(()->frame.primitive(context,6,x,y,w,h));}
    @Override public void drawRoundRect(int x,int y,int w,int h,int a,int b){source.drawRoundRect(x,y,w,h,a,b);reference(()->canonical.drawRoundRect(x,y,w,h,a,b));capture(()->frame.primitive(context,7,x,y,w,h,a,b));}
    @Override public void fillRoundRect(int x,int y,int w,int h,int a,int b){source.fillRoundRect(x,y,w,h,a,b);reference(()->canonical.fillRoundRect(x,y,w,h,a,b));capture(()->frame.primitive(context,8,x,y,w,h,a,b));}
    @Override public void drawArc(int x,int y,int w,int h,int a,int b){source.drawArc(x,y,w,h,a,b);reference(()->canonical.drawArc(x,y,w,h,a,b));capture(()->frame.primitive(context,9,x,y,w,h,a,b));}
    @Override public void fillArc(int x,int y,int w,int h,int a,int b){source.fillArc(x,y,w,h,a,b);reference(()->canonical.fillArc(x,y,w,h,a,b));capture(()->frame.primitive(context,10,x,y,w,h,a,b));}
    @Override public void drawPolyline(int[] x,int[] y,int n){source.drawPolyline(x,y,n);reference(()->canonical.drawPolyline(x,y,n));capture(()->frame.polygon(context,11,x,y,n));}
    @Override public void drawPolygon(int[] x,int[] y,int n){source.drawPolygon(x,y,n);reference(()->canonical.drawPolygon(x,y,n));capture(()->frame.polygon(context,12,x,y,n));}
    @Override public void fillPolygon(int[] x,int[] y,int n){source.fillPolygon(x,y,n);reference(()->canonical.fillPolygon(x,y,n));capture(()->frame.polygon(context,13,x,y,n));}
    @Override public void copyArea(int x,int y,int w,int h,int a,int b){source.copyArea(x,y,w,h,a,b);reference(()->canonical.copyArea(x,y,w,h,a,b));frame.fail();}
    @Override public void drawString(String s,int x,int y){source.drawString(s,x,y);reference(()->canonical.drawString(s,x,y));frame.fail();}
    @Override public void drawString(String s,float x,float y){source.drawString(s,x,y);reference(()->canonical.drawString(s,x,y));frame.fail();}
    @Override public void drawString(AttributedCharacterIterator s,int x,int y){source.drawString(s,x,y);reference(()->canonical.drawString(s,x,y));frame.fail();}
    @Override public void drawString(AttributedCharacterIterator s,float x,float y){source.drawString(s,x,y);reference(()->canonical.drawString(s,x,y));frame.fail();}
    @Override public void drawGlyphVector(GlyphVector v,float x,float y){source.drawGlyphVector(v,x,y);reference(()->canonical.drawGlyphVector(v,x,y));frame.fail();}
    void bitmap(Image image,AffineTransform t){capture(()->{int id=frame.image(image);t.getMatrix(frame.matrix);frame.op(PfdFrame.BITMAP,context,52);frame.bytes.putInt(id);for(double v:frame.matrix)frame.bytes.putDouble(v);});}
    @Override public boolean drawImage(Image image,AffineTransform t,ImageObserver observer){boolean result=source.drawImage(image,t,observer);reference(()->canonical.drawImage(image,t,observer));if(observer!=null)frame.fail();else bitmap(image,t);return result;}
    @Override public boolean drawImage(Image image,int x,int y,ImageObserver observer){boolean result=source.drawImage(image,x,y,observer);reference(()->canonical.drawImage(image,x,y,observer));if(observer!=null)frame.fail();else bitmap(image,AffineTransform.getTranslateInstance(x,y));return result;}
    @Override public boolean drawImage(Image image,int x,int y,int w,int h,ImageObserver observer){boolean r=source.drawImage(image,x,y,w,h,observer);reference(()->canonical.drawImage(image,x,y,w,h,observer));frame.fail();return r;}
    @Override public boolean drawImage(Image image,int x,int y,Color bg,ImageObserver observer){boolean r=source.drawImage(image,x,y,bg,observer);reference(()->canonical.drawImage(image,x,y,bg,observer));frame.fail();return r;}
    @Override public boolean drawImage(Image image,int x,int y,int w,int h,Color bg,ImageObserver observer){boolean r=source.drawImage(image,x,y,w,h,bg,observer);reference(()->canonical.drawImage(image,x,y,w,h,bg,observer));frame.fail();return r;}
    @Override public boolean drawImage(Image image,int x1,int y1,int x2,int y2,int a,int b,int c,int d,ImageObserver observer){boolean r=source.drawImage(image,x1,y1,x2,y2,a,b,c,d,observer);reference(()->canonical.drawImage(image,x1,y1,x2,y2,a,b,c,d,observer));frame.fail();return r;}
    @Override public boolean drawImage(Image image,int x1,int y1,int x2,int y2,int a,int b,int c,int d,Color bg,ImageObserver observer){boolean r=source.drawImage(image,x1,y1,x2,y2,a,b,c,d,bg,observer);reference(()->canonical.drawImage(image,x1,y1,x2,y2,a,b,c,d,bg,observer));frame.fail();return r;}
    @Override public void drawImage(BufferedImage image,BufferedImageOp operation,int x,int y){source.drawImage(image,operation,x,y);reference(()->canonical.drawImage(image,operation,x,y));frame.fail();}
    @Override public void drawRenderedImage(RenderedImage image,AffineTransform t){source.drawRenderedImage(image,t);reference(()->canonical.drawRenderedImage(image,t));frame.fail();}
    @Override public void drawRenderableImage(RenderableImage image,AffineTransform t){source.drawRenderableImage(image,t);reference(()->canonical.drawRenderableImage(image,t));frame.fail();}
}
