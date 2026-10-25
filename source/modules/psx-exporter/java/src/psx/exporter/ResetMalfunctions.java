package psx.exporter;
import java.awt.EventQueue;
import java.awt.event.ActionEvent;
import javax.swing.AbstractButton;
import javax.swing.Action;
import javax.swing.AbstractAction;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.reflect.Field;
import java.nio.*;
import java.nio.file.Path;
import java.util.*;
import java.util.jar.JarFile;
import java.util.concurrent.atomic.AtomicBoolean;
/** Repair, then erase native warning latches, on the ordinary EDT. */
final class ResetMalfunctions {
 private static volatile Action action;
 private static volatile boolean bindingPosted;
 private static boolean started;
 private static final AtomicBoolean dispatchPosted=new AtomicBoolean();
 private ResetMalfunctions() {}
 static synchronized void start(ClassLoader loader,Path input,Path nativePath) {
  if(started)return;started=true;
  Thread thread=new Thread(()->run(loader,input,nativePath),"PSX-exporter-reset");
  thread.setDaemon(true);thread.start();
 }
 static Map<String,byte[]> inspect(Path input)throws Exception {
  Map<String,byte[]> bytes=new HashMap<>();
  try(JarFile jar=new JarFile(input.toFile())) {
   for(String name:List.of("Core","Instructor","InstructorSituation","InstructorMalf","InstructorMalfGeneral","InstructorMalfGeneral$5","InstructorSituServ","InstructorSituServ$15","PButton")) {
    var entry=jar.getJarEntry(name+".class");if(entry==null||entry.getSize()>5*1024*1024)throw new IllegalArgumentException("reset class missing");
    try(var stream=jar.getInputStream(entry)){byte[] b=stream.readNBytes(5*1024*1024+1);if(b.length>5*1024*1024)throw new IllegalArgumentException("reset class bound");bytes.put(name,b);}
   }
  }qualify(bytes);return bytes;
 }
 static void qualify(Map<String,byte[]> bytes) {
  field(bytes,"Core","instructor","LInstructor;",true);
  field(bytes,"Instructor","iSituation","LInstructorSituation;",false);
  field(bytes,"InstructorSituation","iMalfunctions","LInstructorMalf;",false);
  field(bytes,"InstructorMalf","iMalfUnprep","LInstructorMalfGeneral;",false);
  field(bytes,"InstructorMalfGeneral","buttResetMalf","LPButton;",false);
  field(bytes,"InstructorSituation","iService","LInstructorSituServ;",false);
  field(bytes,"InstructorSituServ","jButtUnlatchStat","LPButton;",false);
  ClassModel button=model(bytes,"PButton");if(!button.superclass().orElseThrow().asInternalName().equals("javax/swing/JButton"))throw new IllegalArgumentException("reset button superclass");
  ClassModel owner=model(bytes,"InstructorMalfGeneral$5");if(!owner.superclass().orElseThrow().asInternalName().equals("javax/swing/AbstractAction"))throw new IllegalArgumentException("reset action superclass");
  MethodModel method=owner.methods().stream().filter(m->m.methodName().equalsString("actionPerformed")&&m.methodType().equalsString("(Ljava/awt/event/ActionEvent;)V")).findFirst().orElseThrow();
  if((method.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC))!=ClassFile.ACC_PUBLIC)throw new IllegalArgumentException("reset action visibility");
  List<Instruction> ops=new ArrayList<>();for(var e:method.code().orElseThrow())if(e instanceof Instruction i)ops.add(i);
  if(ops.size()!=11||!call(ops.get(0),Opcode.INVOKESTATIC,"MalfunctionsClear","clearAll","()V")||!call(ops.get(1),Opcode.INVOKESTATIC,"Malfunction","extDisarmLongAll","()V")||!member(ops.get(2),"Glob","qs","[LQs;")||!member(ops.get(3),"Glob","qsRadStatFail","I")||ops.get(4).opcode()!=Opcode.AALOAD||!(ops.get(5) instanceof ConstantInstruction.LoadConstantInstruction c)||!c.constantValue().equals("omi;;;;;;")||!(ops.get(6) instanceof ConstantInstruction x)||!x.constantValue().equals(119)||!(ops.get(7) instanceof ConstantInstruction y)||!y.constantValue().equals(1)||!call(ops.get(8),Opcode.INVOKEVIRTUAL,"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")||ops.get(9).opcode()!=Opcode.POP||ops.get(10).opcode()!=Opcode.RETURN)throw new IllegalArgumentException("reset complete action changed");
  if(!method.code().orElseThrow().exceptionHandlers().isEmpty())throw new IllegalArgumentException("reset action handlers changed");
  ClassModel erase=model(bytes,"InstructorSituServ$15");if(!erase.superclass().orElseThrow().asInternalName().equals("javax/swing/AbstractAction"))throw new IllegalArgumentException("erase action superclass");
  MethodModel eraseMethod=erase.methods().stream().filter(m->m.methodName().equalsString("actionPerformed")&&m.methodType().equalsString("(Ljava/awt/event/ActionEvent;)V")).findFirst().orElseThrow();
  if((eraseMethod.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC))!=ClassFile.ACC_PUBLIC)throw new IllegalArgumentException("erase action visibility");
  var code=(java.lang.classfile.attribute.CodeAttribute)eraseMethod.code().orElseThrow();
  List<Instruction> e=new ArrayList<>();List<Integer> offsets=new ArrayList<>();int offset=0;
  for(var element:code)if(element instanceof Instruction i){e.add(i);offsets.add(offset);offset+=i.sizeInBytes();}
  // Preserve PSX's conditional A/P acknowledgement: healthy operation must not
  // receive an unconditional disconnect command while clearing old latches.
  if(!code.exceptionHandlers().isEmpty()||e.size()!=18
    ||!call(e.get(0),Opcode.INVOKESTATIC,"EiuSystem","unlatchAll","()V")
    ||!member(e.get(1),"Mawea","sysBit","I")||!constant(e.get(2),1)||e.get(3).opcode()!=Opcode.IAND
    ||!branch(code,e.get(4),Opcode.IFNE,offsets.get(9))
    ||!member(e.get(5),"Mawea","sysBit","I")||!constant(e.get(6),8192)||e.get(7).opcode()!=Opcode.IAND
    ||!branch(code,e.get(8),Opcode.IFEQ,offsets.get(17))
    ||!member(e.get(9),"Glob","qh","[LQh;")||!member(e.get(10),"Glob","qApDisc","I")||e.get(11).opcode()!=Opcode.AALOAD
    ||!constant(e.get(12),1)||!constant(e.get(13),119)||!constant(e.get(14),1)
    ||!call(e.get(15),Opcode.INVOKEVIRTUAL,"Qh","transfer","(ICI)I")||e.get(16).opcode()!=Opcode.POP||e.get(17).opcode()!=Opcode.RETURN)
   throw new IllegalArgumentException("erase complete action changed");
 }
 private static boolean constant(Instruction i,int value){return i instanceof ConstantInstruction c&&c.constantValue().equals(value);}
 private static boolean branch(java.lang.classfile.attribute.CodeAttribute code,Instruction i,Opcode op,int target){return i instanceof BranchInstruction b&&b.opcode()==op&&code.labelToBci(b.target())==target;}
 private static ClassModel model(Map<String,byte[]> bytes,String name){ClassModel m=ClassFile.of().parse(Objects.requireNonNull(bytes.get(name)));if(!m.thisClass().asInternalName().equals(name))throw new IllegalArgumentException("reset owner");return m;}
 private static void field(Map<String,byte[]> bytes,String owner,String name,String type,boolean isStatic){var f=model(bytes,owner).fields().stream().filter(v->v.fieldName().equalsString(name)&&v.fieldType().equalsString(type)).findFirst().orElseThrow();if(((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)!=isStatic)throw new IllegalArgumentException("reset field staticness");}
 private static boolean call(Instruction i,Opcode op,String owner,String name,String type){return i instanceof InvokeInstruction v&&v.opcode()==op&&v.owner().asInternalName().equals(owner)&&v.name().equalsString(name)&&v.type().equalsString(type);}
 private static boolean member(Instruction i,String owner,String name,String type){return i instanceof FieldInstruction v&&v.opcode()==Opcode.GETSTATIC&&v.owner().asInternalName().equals(owner)&&v.name().equalsString(name)&&v.type().equalsString(type);}
 static Action bind(ClassLoader loader,Path input)throws Exception {
  if(!EventQueue.isDispatchThread())throw new IllegalStateException("reset not EDT");
  Class<?> core=Class.forName("Core",false,loader);if(!Agent.sameInputSource(core,input))throw new IllegalArgumentException("reset source");
  Action repair=button(core,input,List.of("instructor","iSituation","iMalfunctions","iMalfUnprep","buttResetMalf"),"InstructorMalfGeneral$5");
  Action erase=button(core,input,List.of("instructor","iSituation","iService","jButtUnlatchStat"),"InstructorSituServ$15");
  return new AbstractAction(){public void actionPerformed(ActionEvent event){
   repair.actionPerformed(new ActionEvent(repair,ActionEvent.ACTION_PERFORMED,"Malfunctions"));
   erase.actionPerformed(new ActionEvent(erase,ActionEvent.ACTION_PERFORMED,"Erase latched messages"));
  }};
 }
 private static Action button(Class<?> core,Path input,List<String> path,String actionClass)throws Exception {
  Object value=null;Class<?> type=core;
  for(String name:path) {
   Field f=type.getDeclaredField(name);if(!f.trySetAccessible())throw new IllegalAccessException("reset field");value=f.get(value);if(value==null)throw new IllegalStateException("reset not initialized");type=value.getClass();if(!Agent.sameInputSource(type,input))throw new IllegalArgumentException("reset instance source");
  }
  Action result=((AbstractButton)value).getAction();if(result==null||!result.getClass().getName().equals(actionClass)||!Agent.sameInputSource(result.getClass(),input))throw new IllegalStateException("reset action unavailable");return result;
 }
 interface Gate {boolean begin();void complete(boolean returned);}
 static boolean dispatch(Action selected,Gate gate){
  if(!dispatchPosted.compareAndSet(false,true))return false;
  try{EventQueue.invokeLater(()->{
   try{
    try{if(!gate.begin())return;}catch(Throwable rejected){return;}
    boolean returned=false;try{selected.actionPerformed(new ActionEvent(selected,ActionEvent.ACTION_PERFORMED,"Malfunctions"));returned=true;}catch(Throwable nativeFailure){/* Never retry a partially executed original action. */}
    finally{try{gate.complete(returned);}catch(Throwable ignored){}}
   }finally{dispatchPosted.set(false);}
  });return true;}catch(Throwable postingFailure){dispatchPosted.set(false);throw postingFailure;}
 }
 static boolean dispatchPending(){return dispatchPosted.get();}
 private static void run(ClassLoader loader,Path input,Path nativePath) {
  try {
   System.load(nativePath.toString());if(ResetNative.abiVersion()!=1||!ResetNative.initialize(true))return;
   inspect(input);ByteBuffer buffer=ByteBuffer.allocateDirect(40).order(ByteOrder.LITTLE_ENDIAN);long lastBinding=0;
   for(;;) {
    long now=System.nanoTime();
    if(action==null&&Observer.firstFrame&&!bindingPosted&&now-lastBinding>=1_000_000_000L){lastBinding=now;bindingPosted=true;EventQueue.invokeLater(()->{try{action=bind(loader,input);ResetNative.capabilities(true);}catch(Throwable unavailable){try{ResetNative.capabilities(false);}catch(Throwable ignored){}}finally{bindingPosted=false;}});}
    // A disconnected native slot can be reused before its old EDT callback
    // retires. Keep the Java handoff bounded independently of that slot.
    if(!dispatchPosted.get()&&ResetNative.poll(buffer,Observer.terrainEpoch())) {
     final long token=buffer.getLong(0),lifecycle=buffer.getLong(32);final Action selected=action;
     if(selected!=null){if(!dispatch(selected,new Gate(){public boolean begin(){return ResetNative.begin(token,Observer.terrainEpoch());}public void complete(boolean returned){ResetNative.complete(token,returned);}}))ResetNative.complete(token,false);}
     else ResetNative.complete(token,false);
    }
    Thread.sleep(5);
   }
  }catch(Throwable unavailable){try{ResetNative.capabilities(false);}catch(Throwable ignored){}}
 }
}
