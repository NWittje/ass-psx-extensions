package psx.exporter;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional command bridges only: no added native-frame work or vendor initialization. */
final class SituationPrepare {
 static final String PREFIX="$psxExporter$situation";
 static final String[] RATES={"ptVel","bkVel","targBkVel","ywVel","targYwVel","pitchRate","hdgVelByPitch","targHdgVelPerSec","actHdgVelPerSec"};
 static final String[] POSITION={"realLat","realLon","trueHeading","totalPitch","bank","altitudeReal","tasReal","vtVel"};
 final Prepare base;final List<String> evidence=new ArrayList<>();
 SituationPrepare(Prepare base){this.base=base;}
 void auditInstalled()throws Exception{
  Map<String,String> pins=Map.of("SituationFiler","31ea8387e4af44ee693a17df0e108a58e341d293013bb5de85a8f4e4daecd0c9","Core","d522384326e98914fb2d2028588a6c2c8d1aa8dc7ffdfb7582aee5be49c7e037","Aerodynamics","e68d4f2d2b6850c10b4162a12e55c97451a6263da7e2c424b497d94336cf2e53","Qi","c3a7b8ae1a67ee4ba0e9c08256b2c5c48f8a9d9024e0f939dd0e862820a666dc","Glob","d8c552ccaaabf80f92358af7edb4abdd4ba9e77ad6d432fe38889ce655b3dec1","InstructorSaveSitu$2","5ecd51495b129973a0de2b1657fa348a8443c8b0d8930314c3144ed05aa7c82a","InstructorLoadSitu$2","6e734e96656a68ec44aa0c7690635307913b4f14058afde4d4be458dd16ace6c","FmcGlobal","417aabb68a22a03d5af3e1188c83229ddcb1d26587ac4796496e071eeee75026","GearLever","6d1554dd886fd0368e687eae7afae60b3d51ed94ae78e5a1914dce44fe397ad3","OvhdJettison$EngStartSw","f52037e7282b911dfc856e8276ad9e53d4c7933aed2450a4fbb5567b16218552");
  for(var pin:pins.entrySet()){if(!pin.getValue().equals(Identity.hash(base.inputs.get(pin.getKey()))))throw new IllegalArgumentException("situation audited native class changed: "+pin.getKey());evidence.add("native class "+pin.getKey()+" SHA256 "+pin.getValue());}
  for(String name:List.of("InstructorSaveSitu$2","InstructorLoadSitu$2"))if(!base.models.get(name).superclass().orElseThrow().asInternalName().equals("javax/swing/AbstractAction"))throw new IllegalArgumentException("ordinary situation action thread changed");
  evidence.add("ordinary native save/load actions are qualified Swing AbstractAction actionPerformed callbacks on EDT; save uses false recorder flag");
  evidence.add("bkVelAccel is the native deltaAcc*0.003 capped interpolation factor, not a rotation rate/history; it and inertia divisors remain unchanged");
  Set<String> angular=new HashSet<>(Arrays.asList(RATES));angular.add("hdgVelPerFrame");
  Set<String> seen=new HashSet<>();int callers=0;
  for(var item:base.methods.entrySet())if(item.getValue().code().isPresent())for(var op:Prepare.instructions(item.getValue())){
   if(op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("Aerodynamics")&&angular.contains(f.name().stringValue())){seen.add(f.name().stringValue());evidence.add("angular writer "+item.getKey()+" -> "+f.name().stringValue());}
   if(op instanceof InvokeInstruction v&&v.owner().asInternalName().equals("SituationFiler")&&(v.name().equalsString("save")||v.name().equalsString("load"))){evidence.add("ordinary native situation caller "+item.getKey()+" -> "+v.name().stringValue());callers++;}
  }
  if(!seen.equals(angular)||callers==0)throw new IllegalArgumentException("situation angular writer/caller audit incomplete");
  // Inspect the actual reachable initializer graph; unresolved dispatch/Q
  // transfers are an isolation failure, not permission for a partial load.
  Set<Prepare.MethodId> visited=new HashSet<>();Deque<Prepare.MethodId> todo=new ArrayDeque<>();todo.add(new Prepare.MethodId("FmcGlobal","situLoadInit","()V"));
  while(!todo.isEmpty()&&visited.size()<4096){var id=todo.removeFirst();if(!visited.add(id))continue;var m=base.methods.get(id);if(m==null||m.code().isEmpty()){evidence.add("FMC isolation unresolved method "+id);continue;}for(var op:Prepare.instructions(m)){
   if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)&&!f.owner().asInternalName().startsWith("Fmc"))evidence.add("FMC initializer closure external writer "+id+" -> "+f.owner().asInternalName()+"."+f.name().stringValue());
   if(op instanceof InvokeInstruction v){String owner=v.owner().asInternalName();if(owner.startsWith("Fmc"))todo.add(new Prepare.MethodId(owner,v.name().stringValue(),v.type().stringValue()));else if(!owner.startsWith("java/"))evidence.add("FMC initializer closure external call/dispatch "+id+" -> "+owner+"."+v.name().stringValue()+v.type().stringValue());}
  }}
  evidence.add("FMC isolated recall not advertised; external writer/dispatch dependencies above are not qualified for an FMC-only mutation");
 }
 private MethodModel method(String owner,String name,String type){base.requireMethod(owner,name,type);var m=base.methods.get(new Prepare.MethodId(owner,name,type));if((m.flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("situation static method "+owner+"."+name);return m;}
 private void field(String owner,String name,String descriptor){var f=base.models.get(owner).fields().stream().filter(v->v.fieldName().equalsString(name)&&v.fieldType().equalsString(descriptor)).findFirst().orElseThrow();if((f.flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("situation static field "+name);}
 void qualify(){
  var save=method("SituationFiler","save","(Ljava/lang/String;Ljava/lang/String;Z)V");
  if((save.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)==0)throw new IllegalArgumentException("native save serialization changed");
  var load=method("SituationFiler","load","(Ljava/lang/String;)Z");
  if(Prepare.instructions(save).stream().filter(i->Prepare.call(i,"Core","updateAllSituStartVars","()V")).count()!=1)throw new IllegalArgumentException("native saved-state producer changed");
  for(String call:List.of("setLoaderPaused","unpauseLoader"))if(Prepare.instructions(load).stream().filter(i->Prepare.call(i,"Core",call,"()V")).count()!=1)throw new IllegalArgumentException("native loader phase "+call);
  method("Core","updateAllSituStartVars","()V");method("Core","setGlobPauseMode","(II)V");method("Core","setPaused","(I)V");
  if(Prepare.instructions(method("Core","unpauseLoader","()V")).stream().filter(i->Prepare.call(i,"Aerodynamics","runDelta","()V")).count()!=1)throw new IllegalArgumentException("native loader dynamics initialization changed");
  base.requireMethod("Qi","transfer","(ICI)I");if((base.methods.get(new Prepare.MethodId("Qi","transfer","(ICI)I")).flags().flagsMask()&ClassFile.ACC_STATIC)!=0)throw new IllegalArgumentException("native pause transfer became static");field("Glob","qi","[LQi;");field("Glob","qiSimPaused","I");field("Core","paused","Z");field("Core","loaderPaused","Z");
  for(String n:RATES)field("Aerodynamics",n,"F");field("Aerodynamics","hdgVelPerFrame","D");
  for(int i=0;i<POSITION.length;i++)field("Aerodynamics",POSITION[i],i<3?"D":"F");
  // The inherited terrain/pose lifecycle boundary precedes destination writes.
  var loader=base.methods.get(new Prepare.MethodId("Core","setLoaderPaused","()V"));
  if(loader==null)throw new IllegalArgumentException("native load entry absent");
  for(String cls:List.of("EngModCf6","EngModPw4000","EngModRb211","Flaps","Gear","CabinPress","FlightControls","FmcGlobal"))base.requireMethod(cls,"situLoadInit","()V");
  evidence.add("native save: ordinary false event-recorder flag, updateAllSituStartVars then native file serializer; running-save coherence is vendor fidelity");
  evidence.add("native load: existing setLoaderPaused -> native transfers -> unpauseLoader, complete ordinary systems initialization");
  for(String n:RATES)evidence.add("one-time angular motion reset Aerodynamics."+n+"; NOT inertia divisors, forces, controls or aircraft attitude");
  evidence.add("one-time angular motion reset Aerodynamics.hdgVelPerFrame:D");
  evidence.add("FMC standalone recall unavailable: full reachable initializer/transfer closure is not independently qualified; no partial whole-aircraft load fallback");
 }
 Map<String,byte[]> transform(Map<String,byte[]> lower){
  byte[] original=lower.getOrDefault("SituationFiler",base.inputs.get("SituationFiler"));ClassDesc owner=ClassDesc.of("SituationFiler"),core=ClassDesc.of("Core"),aero=ClassDesc.of("Aerodynamics"),glob=ClassDesc.of("Glob"),qi=ClassDesc.of("Qi");
  int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
  byte[] changed=base.cf.transformClass(base.cf.parse(original),new ClassTransform(){
   public void accept(ClassBuilder b,ClassElement e){b.with(e);}
   public void atEnd(ClassBuilder b){
    b.withMethodBody(PREFIX+"Save",MethodTypeDesc.of(CD_void,CD_String,CD_String),flags,c->c.aload(0).aload(1).iconst_0().invokestatic(owner,"save",MethodTypeDesc.of(CD_void,CD_String,CD_String,CD_boolean)).return_());
    b.withMethodBody(PREFIX+"Load",MethodTypeDesc.of(CD_boolean,CD_String),flags,c->c.aload(0).invokestatic(owner,"load",MethodTypeDesc.of(CD_boolean,CD_String)).ireturn());
    b.withMethodBody(PREFIX+"Finish",MethodTypeDesc.of(CD_void),flags,c->{
     c.iconst_0().iconst_3().invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int));
     c.iconst_1().invokestatic(core,"setPaused",MethodTypeDesc.of(CD_void,CD_int));
     for(String n:RATES)c.fconst_0().putstatic(aero,n,CD_float);
     c.dconst_0().putstatic(aero,"hdgVelPerFrame",CD_double).return_();
    });
    b.withMethodBody(PREFIX+"Read",MethodTypeDesc.of(CD_void,CD_double.arrayType()),flags,c->{
     for(int i=0;i<POSITION.length;i++){c.aload(0).loadConstant(i).getstatic(aero,POSITION[i],i<3?CD_double:CD_float);if(i>=3)c.f2d();c.dastore();}
     c.aload(0).loadConstant(8).dconst_0();for(String n:RATES)c.getstatic(aero,n,CD_float).f2d().invokestatic(ClassDesc.of("java.lang.Math"),"abs",MethodTypeDesc.of(CD_double,CD_double)).invokestatic(ClassDesc.of("java.lang.Math"),"max",MethodTypeDesc.of(CD_double,CD_double,CD_double));c.getstatic(aero,"hdgVelPerFrame",CD_double).invokestatic(ClassDesc.of("java.lang.Math"),"abs",MethodTypeDesc.of(CD_double,CD_double)).invokestatic(ClassDesc.of("java.lang.Math"),"max",MethodTypeDesc.of(CD_double,CD_double,CD_double)).dastore();
     c.aload(0).loadConstant(9).getstatic(core,"paused",CD_boolean).i2d().dastore();
     c.aload(0).loadConstant(10).getstatic(glob,"qi",qi.arrayType()).getstatic(glob,"qiSimPaused",CD_int).aaload().iconst_0().loadConstant(114).iconst_0().invokevirtual(qi,"transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int)).i2d().dastore();
     c.aload(0).loadConstant(11).getstatic(core,"loaderPaused",CD_boolean).i2d().dastore().return_();
    });
   }
  });
  var errors=base.cf.verify(changed);if(!errors.isEmpty())throw new IllegalArgumentException("situation bridge verification "+errors.getFirst());
  Map<String,byte[]> before=new HashMap<>(lower);before.put("SituationFiler",original);Map<String,byte[]> result=Map.of("SituationFiler",changed);
  TerrainAccess.verifyAdded(before,result,base.inputs);
  evidence.add("all added members verified against final vendor and exact packaged helper data; legal caller, descriptor, visibility and staticness");
  return result;
 }
}
