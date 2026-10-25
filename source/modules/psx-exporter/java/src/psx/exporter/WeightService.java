package psx.exporter;
import java.awt.EventQueue;
import java.lang.classfile.*;
import java.lang.reflect.*;
import java.nio.*;
import java.nio.file.Path;
import java.util.*;
import java.util.jar.JarFile;
import java.util.concurrent.atomic.AtomicBoolean;
/** Ordinary EDT actions, independently optional from readouts. IPC remains on workers. */
final class WeightService {
 static volatile int preparedActions;
 private static volatile boolean disabled,posted;
 private static boolean started;
 private static volatile Reader reader;
 private static volatile Pending pending;
 private static final AtomicBoolean dispatchPosted=new AtomicBoolean();
 private record Pending(long token,long lifecycle,int command,long started){}
 private WeightService(){}
 static void disable(){disabled=true;WeightHooks.failed(0);}
 static synchronized void start(ClassLoader loader,Path input,Path nativePath){
  if(started)return;started=true;Thread worker=new Thread(()->run(loader,input,nativePath),"PSX-exporter-weight-service");worker.setDaemon(true);worker.start();
 }
 static final class Reader {
  final ClassLoader loader;final Path input;final Map<String,byte[]> bytes=new HashMap<>();
  Field loading,cg,capacity,total,units,situLoaded,irsBusy;
  Method distribute,resetLag,fuelEvent,alignStandby,align,cool,quickstart;
  int actions;
  Reader(ClassLoader loader,Path input)throws Exception{
   this.loader=loader;this.input=input;
   if(!InstructorMetadata.INSTALLED_JAR.equalsIgnoreCase(Identity.hash(input)))throw new IllegalArgumentException("Weight source identity");
   try(JarFile jar=new JarFile(input.toFile())){for(String owner:List.of("Core","WgtBal","Fuel","Option","InertialReference","FmcProgress2","Gear","EngineBasics")){
    var entry=jar.getJarEntry(owner+".class");if(entry==null)continue;
    try(var stream=jar.getInputStream(entry)){byte[] data=stream.readNBytes(5*1024*1024+1);if(data.length>5*1024*1024)throw new IllegalArgumentException("Weight source bound");bytes.put(owner,data);}
   }}
  }
  Class<?> type(String owner)throws Exception{Class<?> value=Class.forName(owner,false,loader);if(!Agent.sameInputSource(value,input))throw new IllegalArgumentException("Weight source owner");return value;}
  Field scalar(String owner,String name,String descriptor,Class<?> kind)throws Exception{
   ClassModel model=ClassFile.of().parse(Objects.requireNonNull(bytes.get(owner)));
   if(!model.thisClass().asInternalName().equals(owner))throw new IllegalArgumentException("Weight scalar owner");
   var declared=model.fields().stream().filter(f->f.fieldName().equalsString(name)&&f.fieldType().equalsString(descriptor)).findFirst().orElseThrow();
   if((declared.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))!=ClassFile.ACC_STATIC)throw new IllegalArgumentException("Weight scalar mutability");
   Field field=type(owner).getDeclaredField(name);if(field.getType()!=kind||!Modifier.isStatic(field.getModifiers())||!field.trySetAccessible())throw new IllegalAccessException("Weight scalar inaccessible");return field;
  }
  Method bridge(String owner,String name,Class<?>...arguments)throws Exception{
   Method method=type(owner).getDeclaredMethod(WeightPrepare.PREFIX+"call$"+name,arguments);int flags=method.getModifiers();
   if(!Modifier.isPublic(flags)||!Modifier.isStatic(flags)||!method.isSynthetic()||method.getReturnType()!=void.class||!method.trySetAccessible())throw new IllegalAccessException("Weight bridge unavailable");return method;
  }
  void bind()throws Exception{
   if(!EventQueue.isDispatchThread())throw new IllegalStateException("Weight bind not EDT");
   loading=scalar("Core","loaderPaused","Z",boolean.class);
   try{cg=scalar("WgtBal","nonFuelCg","I",int.class);}catch(Throwable unavailable){}
   try{capacity=scalar("Fuel","totalFuelLimit","I",int.class);}catch(Throwable unavailable){}
   if(preparedActions!=15||disabled)return;
   try{total=scalar("Fuel","totalQty","F",float.class);units=scalar("Option","lbsFac","F",float.class);distribute=bridge("Fuel","distribute",float.class);resetLag=bridge("Fuel","resetAllLag");fuelEvent=bridge("FmcProgress2","instructorEvent");if(capacity!=null)actions|=1;}catch(Throwable unavailable){}
   try{situLoaded=scalar("Core","situLoadedTime","I",int.class);irsBusy=scalar("InertialReference","isThreadAlignBusy","Z",boolean.class);alignStandby=bridge("Core","alignStandby");align=bridge("InertialReference","instructorAlignment");actions|=2;}catch(Throwable unavailable){}
   try{cool=bridge("Gear","coolAllBrakes");actions|=4;}catch(Throwable unavailable){}
   try{quickstart=bridge("EngineBasics","quickEngStart");actions|=8;}catch(Throwable unavailable){}
  }
  boolean loading()throws Exception{return loading==null||loading.getBoolean(null);}
  double capacity()throws Exception{int value=capacity.getInt(null);if(value<=0||value>400000)throw new IllegalArgumentException("Native fuel capacity");return value;}
  int actionBits()throws Exception{
   if(disabled||preparedActions!=15||loading()||Observer.metadataMutationActive()||WeightHooks.busy())return 0;
   int value=actions;if((value&2)!=0&&(situLoaded.getInt(null)<2||irsBusy.getBoolean(null)))value&=~2;return value;
  }
  void sample(){
   int capabilities=0;double value=0,limit=0;boolean busy=true;long epoch=Observer.terrainEpoch(),version=Observer.metadataMutationVersion();
   try{busy=loading()||Observer.metadataMutationActive();if(!busy){
    if(cg!=null)try{value=cg.getInt(null)*.1;if(value>=0&&value<=100)capabilities|=16;else value=0;}catch(Throwable unavailable){}
    if(capacity!=null)try{limit=capacity();capabilities|=32;}catch(Throwable unavailable){}
    capabilities|=actionBits();
   }if(epoch!=Observer.terrainEpoch()||version!=Observer.metadataMutationVersion()||Observer.metadataMutationActive()){busy=true;capabilities=0;}}
   catch(Throwable unavailable){capabilities=0;busy=true;}
   WeightNative.sample(capabilities,epoch,value,limit,352739,busy);
  }
  boolean apply(int command,double pounds)throws Throwable{
   if(!EventQueue.isDispatchThread()||loading()||disabled)throw new IllegalStateException("Native service unavailable");
   if(command==1){
    if(!Double.isFinite(pounds)||pounds<0||pounds>capacity()||Math.rint(pounds)!=pounds)throw new IllegalArgumentException("Native fuel target");
    float factor=units.getFloat(null),current=total.getFloat(null);
    if(!Float.isFinite(factor)||factor<=0||!Float.isFinite(current)||current<0)throw new IllegalArgumentException("Native fuel units");
    if(Math.round(current*.1f/factor)==Math.round((float)pounds/factor))return true;
    invoke(distribute,(float)(pounds*10/factor));invoke(resetLag);invoke(fuelEvent);
   }else if(command==2){if(situLoaded.getInt(null)<2||irsBusy.getBoolean(null))throw new IllegalStateException("Native alignment busy");invoke(alignStandby);invoke(align);}
   else if(command==3)invoke(cool);else if(command==4)invoke(quickstart);else throw new IllegalArgumentException("Native service command");
   return false;
  }
 }
 private static void invoke(Method method,Object...arguments)throws Throwable{if(method==null)throw new IllegalStateException("Unqualified service bridge");try{method.invoke(null,arguments);}catch(InvocationTargetException failure){throw failure.getCause();}}
 @SuppressWarnings("unchecked") private static <E extends Throwable>void rethrow(Throwable failure)throws E{throw(E)failure;}
 private static void dispatch(Reader selected,long token,long epoch,int command,double pounds){
  if(!dispatchPosted.compareAndSet(false,true)){WeightNative.reject(token);return;}
  try{EventQueue.invokeLater(()->{
   try{
    try{if(selected.loading()||epoch!=Observer.terrainEpoch()||disabled||WeightHooks.busy()||(selected.actionBits()&(1<<(command-1)))==0){WeightNative.reject(token);return;}}
    catch(Throwable unavailable){WeightNative.reject(token);return;}
    if(!WeightNative.begin(token,Observer.terrainEpoch()))return;
    if(!WeightHooks.arm(token,epoch,command)){WeightNative.complete(token,false);return;}
    pending=new Pending(token,epoch,command,System.nanoTime());
    try{WeightHooks.nativeReturned(selected.apply(command,pounds));}
    catch(Throwable nativeFailure){WeightHooks.actionFailed();WeightService.<RuntimeException>rethrow(nativeFailure);}
   }finally{dispatchPosted.set(false);}
  });}catch(Throwable postingFailure){dispatchPosted.set(false);throw postingFailure;}
 }
 static boolean dispatchPending(){return dispatchPosted.get();}
 private static void run(ClassLoader loader,Path input,Path nativePath){
  try{
   System.load(nativePath.toString());if(WeightNative.abiVersion()!=1||!WeightNative.initialize(true))return;
   WeightHooks.prepare();Reader candidate=new Reader(loader,input);ByteBuffer request=ByteBuffer.allocateDirect(32).order(ByteOrder.LITTLE_ENDIAN);long sampled=0;
   for(;;){
    long now=System.nanoTime();Pending operation=pending;
    if(operation!=null){
     if(WeightHooks.workFinished()){boolean returned=WeightHooks.success(operation.token,Observer.terrainEpoch())&&operation.lifecycle==Observer.terrainEpoch();WeightNative.complete(operation.token,returned);WeightHooks.clear(operation.token);pending=null;}
     else if(now-operation.started>=5_000_000_000L||operation.lifecycle!=Observer.terrainEpoch()||disabled)WeightNative.uncertain(operation.token);
    }
    if(Observer.firstFrame&&!posted&&now-sampled>=1_000_000_000L){sampled=now;posted=true;EventQueue.invokeLater(()->{
     try{if(reader==null){candidate.bind();reader=candidate;}reader.sample();}catch(Throwable unavailable){WeightNative.sample(0,Observer.terrainEpoch(),0,0,352739,true);}finally{posted=false;}
    });}
    if(preparedActions==15&&!disabled&&pending==null&&!dispatchPosted.get()&&WeightNative.poll(request,Observer.terrainEpoch())){long token=request.getLong(0),epoch=request.getLong(8);int command=(int)request.getLong(16);Reader selected=reader;if(selected==null)WeightNative.reject(token);else dispatch(selected,token,epoch,command,request.getDouble(24));}
    Thread.sleep(10);
   }
  }catch(Throwable unavailable){try{WeightNative.sample(0,Observer.terrainEpoch(),0,0,352739,true);}catch(Throwable ignored){}}
 }
}
