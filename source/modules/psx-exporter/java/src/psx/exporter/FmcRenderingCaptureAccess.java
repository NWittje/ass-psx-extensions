package psx.exporter;
import java.awt.*;import java.lang.invoke.*;import java.lang.reflect.Modifier;import java.util.Objects;import java.util.function.BooleanSupplier;
/** Worker-prepared original drawing call. Buffer ownership stays with this
 * binding; the complete-paint collector supplies the original admitted native
 * callback and prevents recursion or disposal until that callback retires. */
final class FmcRenderingCaptureAccess {
 static FmcRenderingCache.Drawing bind(FmcInitialization.Context context,BooleanSupplier qualified,Object[] boundRoots)throws Throwable{
  Objects.requireNonNull(context);Objects.requireNonNull(qualified);require(context.current()&&qualified.getAsBoolean()&&!FmcExclusion.admissionClosed()&&!FmcExclusion.currentAdmitted(),"open worker initialization binding");
  var nd=context.owner("NavigationDisplay");context.owner("Visuals");context.owner("Tcas");context.owner("Core");Object[] roots=boundRoots.clone();require(roots.length==4,"four actual native roots");for(int i=0;i<4;i++){require(nd.isInstance(roots[i]),"native root type");for(int j=0;j<i;j++)require(roots[j]!=roots[i],"distinct native root identities");}
  var m=nd.getDeclaredMethod(FmcRenderingCapturePrepare.NAME,Object.class,Graphics2D.class,Point.class,Object[].class,long[].class,Rectangle.class);require(m.isSynthetic()&&m.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|0x1000)&&m.getReturnType()==boolean.class,"exact canonical capture bridge");var call=MethodHandles.privateLookupIn(nd,MethodHandles.lookup()).findStatic(nd,FmcRenderingCapturePrepare.NAME,MethodType.methodType(boolean.class,Object.class,Graphics2D.class,Point.class,Object[].class,long[].class,Rectangle.class));
  Object[][] refs=new Object[4][FmcRenderingCapturePrepare.REFERENCES];long[][] raw=new long[4][FmcRenderingCapturePrepare.RAW];Rectangle[] clips=new Rectangle[4];for(int i=0;i<4;i++)clips[i]=new Rectangle(0,0,FmcRenderingCapturePrepare.WIDTH,FmcRenderingCapturePrepare.HEIGHT);
  require(context.current()&&qualified.getAsBoolean(),"lifecycle changed during worker preparation");
  return (owner,graphics,point)->{if(!context.current()||!qualified.getAsBoolean()||!FmcExclusion.currentAdmitted())return false;int index=-1;for(int i=0;i<4;i++)if(roots[i]==owner){index=i;break;}if(index<0)return false;Object[] references=refs[index];long[] primitives=raw[index];Rectangle clip=clips[index];boolean complete=(boolean)call.invokeExact(owner,graphics,point,references,primitives,clip);return complete&&context.current()&&qualified.getAsBoolean();};
 }
 private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC canonical drawing binding "+why);}
 private FmcRenderingCaptureAccess(){}
}