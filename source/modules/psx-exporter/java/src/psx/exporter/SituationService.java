package psx.exporter;
import java.awt.EventQueue;
import java.lang.reflect.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Fixed native commands. File parsing/IPC stays off native frames; ordinary
 * native save/load executes on its existing EDT, including its native file IO. */
final class SituationService {
 private static volatile boolean disabled;
 private static boolean started;
 private static volatile Pending pending;
 private static final class Pending {
  final long token;final int command,length;final Path path;final double[] readback=new double[12];
  String importedHash;boolean owned;long observation,epoch;int proof;volatile int done;
  Pending(long token,int command,int length,Path path){this.token=token;this.command=command;this.length=length;this.path=path;epoch=Observer.terrainEpoch();}
 }
 private SituationService(){}
 static void disable(){disabled=true;}
 static synchronized void start(ClassLoader loader,Path input){if(started)return;started=true;Thread worker=new Thread(()->run(loader,input),"PSX-exporter-situation");worker.setDaemon(true);worker.start();}
 static Method[] bind(ClassLoader loader,Path input)throws Exception{Class<?> owner=Class.forName("SituationFiler",false,loader);if(!Agent.sameInputSource(owner,input))throw new IllegalArgumentException("situation source");Method[] m={owner.getDeclaredMethod(SituationPrepare.PREFIX+"Save",String.class,String.class),owner.getDeclaredMethod(SituationPrepare.PREFIX+"Load",String.class),owner.getDeclaredMethod(SituationPrepare.PREFIX+"Finish"),owner.getDeclaredMethod(SituationPrepare.PREFIX+"Read",double[].class)};for(Method method:m)if(!Modifier.isStatic(method.getModifiers())||!Modifier.isPublic(method.getModifiers())||!method.trySetAccessible())throw new IllegalAccessException("situation bridge");return m;}
 private static void execute(Method[] bridges,Pending operation){
  if(!EventQueue.isDispatchThread())throw new IllegalStateException("situation command not EDT");
  try{if(disabled||!SituationNative.begin(operation.token,Observer.terrainEpoch())){operation.done=9;return;}}catch(Throwable linkage){disabled=true;operation.done=7;return;}
  String name=operation.path.getFileName().toString();boolean recall=operation.command==3;double[] readback=operation.readback;
  int evidence=0,result=0;
  try{
   if(recall){boolean loaded=(Boolean)bridges[1].invoke(null,name);evidence=1;if(!loaded)result=10;else{evidence|=2;bridges[2].invoke(null);bridges[3].invoke(null,(Object)readback);if(readback[9]==1&&(int)readback[10]%2==1&&readback[11]==0)evidence|=4;if(readback[8]==0)evidence|=8;if((evidence&12)!=12)result=7;}}
   else{bridges[0].invoke(null,name,"ASS PSX Addons native capture");evidence=1;}
  }catch(Throwable nativeFailure){result=8;/* Never retry, call a recovery initializer or replace the native failure. */}
  operation.proof=evidence;
  try{operation.epoch=Observer.terrainEpoch();operation.observation=BridgeNative.qpc();}catch(Throwable diagnosticFailure){if(result==0)result=7;disabled=true;}
  operation.done=result==0?-1:result;
 }
 static boolean dispatchPending(){return pending!=null;}
 private static void retire(Pending operation,ByteBuffer response){
  int reason=operation.done<0?0:operation.done;
  try{
   if(operation.command==3&&reason==0&&(Files.size(operation.path)!=operation.length||!operation.importedHash.equals(Identity.hash(operation.path))))throw new IllegalArgumentException("situation import changed during load");
   response.clear();int size=0;
   if(reason==0&&operation.command==1){long n=Files.size(operation.path);if(n<=0||n>response.capacity())throw new IllegalArgumentException("native save size");byte[] bytes=Files.readAllBytes(operation.path);if(bytes.length!=n)throw new IllegalArgumentException("native save changed");response.put(bytes);size=bytes.length;}
   SituationNative.complete(operation.token,reason,operation.proof,operation.epoch,operation.observation,response,size,operation.readback);
  }catch(Throwable fileFailure){try{SituationNative.complete(operation.token,10,operation.proof,operation.epoch,operation.observation,null,0,operation.readback);}catch(Throwable linkage){disabled=true;}}
  finally{
   if(operation.owned)try{if(Files.isSymbolicLink(operation.path))throw new IllegalStateException("situation staging replaced by link");Files.deleteIfExists(operation.path);}catch(Exception cleanupFailure){disabled=true;}
   pending=null;
  }
 }
 private static void run(ClassLoader loader,Path input){
  try{
   if(SituationNative.abiVersion()!=1||!SituationNative.initialize(true))return;
   ByteBuffer hash=ByteBuffer.allocateDirect(32);hash.put(HexFormat.of().parseHex(Identity.hash(input))).flip();SituationNative.sourceHash(hash);
   ByteBuffer request=ByteBuffer.allocateDirect(8*1024*1024+64).order(ByteOrder.LITTLE_ENDIAN),response=ByteBuffer.allocateDirect(8*1024*1024);
   Path folder=input.getParent().resolve("Situations").toRealPath();Path actual=Path.of("Situations").toRealPath();if(!Files.isSameFile(folder,actual)||Files.isSymbolicLink(folder))throw new IllegalStateException("native situation directory context");
   Method[] methods=null;
   FmcSourceBackend fmc=null;FmcNativeRecordsService nativeFmc=null;long nextCompatibility=0;
   while(!disabled||pending!=null){
    Pending outstanding=pending;
    if(outstanding!=null){
     // A native deadline can reject a queued callback, but cannot retire an
     // entered save/load. Keep its stage and sole Java handoff until the
     // actual EDT callback returns; native status/receipts remain independent.
     if(outstanding.done==0){Thread.sleep(5);continue;}
     retire(outstanding,response);continue;
    }
    if(methods==null&&Observer.firstFrame){methods=bind(loader,input);SituationNative.capabilities(15);}
    if(methods!=null&&System.nanoTime()>=nextCompatibility){
     try{if(FmcSourceBackend.prepared){if(fmc==null)fmc=new FmcSourceBackend(loader,input);fmc.refresh();}}
     catch(Throwable optionalFailure){FmcSourceDiagnostics.record("binding_failure",optionalFailure);FmcSourceBackend.disable();}
     try{if(FmcNativeRecordsService.prepared){if(nativeFmc==null)nativeFmc=new FmcNativeRecordsService(loader,input);nativeFmc.refresh();}}
     catch(Throwable optionalFailure){FmcSourceDiagnostics.record("native_record_binding_failure",optionalFailure);FmcNativeRecordsService.disable();}
     nextCompatibility=System.nanoTime()+1_000_000_000;
    }
    if(nativeFmc!=null)nativeFmc.pump();
    if(methods!=null&&!FmcSourceBackend.prepared)FmcSourceDiagnostics.record("prepared_unavailable",null);
    if(methods!=null&&SituationNative.poll(request,Observer.terrainEpoch())){
     long token=request.getLong(0);int command=request.getInt(8),length=request.getInt(12);long operation=request.getLong(16);
     if(length<0||length>response.capacity())throw new IllegalArgumentException("situation payload bound");
     if(request.getInt(32)==3&&(command==8||command==9)){
      try{if(nativeFmc==null)throw new IllegalStateException("native FMC binding unavailable");byte[] bytes=null;if(command==9){bytes=new byte[length];request.position(64);request.get(bytes);}nativeFmc.begin(token,command,request.getLong(40),request.getLong(24),bytes);}
      catch(Throwable before){FmcSourceDiagnostics.record("native_record_admission_failure",before);FmcNativeRecordsNative.complete(token,5,13,request.getLong(40),request.getLong(24),0,null,0,false,false);}
      continue;
     }
     if(command==8){
      try{
       if(!SituationNative.begin(token,Observer.terrainEpoch()))continue;
       if(fmc==null)throw new IllegalStateException("FMC source binding unavailable");
       var capture=fmc.capture();response.clear();response.put(capture.bytes());
       FmcNative.complete(token,0,capture.lifecycle(),capture.generation(),capture.observation(),response,capture.bytes().length);
      }catch(Throwable unavailable){FmcSourceDiagnostics.record("capture_failure",unavailable);FmcNative.complete(token,14,Observer.terrainEpoch(),0,0,null,0);}
      continue;
     }
     String basename="ASS-Situation-"+ProcessHandle.current().pid()+"-"+token+"-"+operation+".situ";Path path=folder.resolve(basename);
     Pending selected=new Pending(token,command,length,path);
     try{
      if(command!=1&&command!=3)throw new IllegalArgumentException("unsupported situation command");
      // A colliding file is never owned or removed by this service.
      Files.createFile(path);selected.owned=true;
      if(command==3){byte[] bytes=new byte[length];request.position(64);request.get(bytes);selected.importedHash=Identity.hash(bytes);Files.write(path,bytes,StandardOpenOption.WRITE);if(Files.size(path)!=length||!selected.importedHash.equals(Identity.hash(path)))throw new IllegalArgumentException("situation import identity");}
      final Method[] bridges=methods;pending=selected;EventQueue.invokeLater(()->execute(bridges,selected));
     }catch(Throwable fileFailure){selected.done=10;retire(selected,response);}
    }
    Thread.sleep(5);
   }
  }catch(Throwable failure){disabled=true;}
  finally{try{FmcNativeRecordsNative.configure(null,0,0);}catch(Throwable ignored){}try{SituationNative.capabilities(0);}catch(Throwable ignored){}}
 }
}
