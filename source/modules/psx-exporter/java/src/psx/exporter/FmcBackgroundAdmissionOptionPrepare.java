package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.attribute.CodeAttribute;import java.lang.classfile.instruction.*;import java.lang.constant.*;import java.util.*;import static java.lang.constant.ConstantDescs.*;
/** Native configuration polling keeps its physical/UI work and input consumption.
 * Old whole calls drain before publication. Only a true configuration-change
 * branch in a new closed call invalidates recall; quiet no-change polls remain
 * live on the original renderer/network threads. No native value is fabricated. */
final class FmcBackgroundAdmissionOptionPrepare {
 static final Prepare.MethodId METHOD=new Prepare.MethodId("Option","monitor","()V");
 static final String ENTRY="$psxExporter$fmcOptionEnter",LEAVE="$psxExporter$fmcOptionLeave",VIOLATION="$psxExporter$fmcOptionChange";
 static final String FAULT=FmcBackgroundAdmissionPrepare.FAULT;
 private static final ClassDesc GATE=ClassDesc.of("psx.exporter.FmcExclusion"),REPORT=ClassDesc.of("psx.exporter.FmcBackgroundAdmission");
 record Region(int begin,int end){}
 private static final Map<Prepare,List<Region>> CACHE=Collections.synchronizedMap(new WeakHashMap<>());
 static List<Region> qualify(Prepare data){return CACHE.computeIfAbsent(data,FmcBackgroundAdmissionOptionPrepare::inspect);}
 private static List<Region> inspect(Prepare data){
  try{require(Identity.hash(data.inputs.get("Option")).equalsIgnoreCase("D8059DDF3AF5A03AD659ADC5B9FA35584F3AA24888D41E7C7FD4135030BEFAF5"),"whole original Option source");}catch(Exception e){throw new IllegalArgumentException("FMC config source digest",e);}
  var method=data.methods.get(METHOD);require(method!=null&&method.flags().flagsMask()==ClassFile.ACC_STATIC&&method.code().isPresent(),"original finite static monitor");
  var ops=Prepare.instructions(method);var code=(CodeAttribute)method.code().orElseThrow();var regions=new ArrayList<Region>();int bci=0;
  for(int i=0;i<ops.size();i++){
   var op=ops.get(i);
   if(op instanceof BranchInstruction branch&&op.opcode()==Opcode.IFEQ&&i>0){
    var prev=ops.get(i-1);boolean updated=prev instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKEVIRTUAL&&Set.of("Qi","Qs").contains(c.owner().asInternalName())&&c.name().equalsString("isUpdated")&&c.type().equalsString("()Z");
    boolean changed=prev instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("Option")&&Set.of("tempUpdateFlag","updFuelLimit","newDragFfLoaded").contains(f.name().stringValue())&&f.type().equalsString("Z");
    if(updated||changed)regions.add(new Region(bci+op.sizeInBytes(),code.labelToBci(branch.target())));
   }
   bci+=op.sizeInBytes();
  }
  var starts=new TreeSet<Integer>();for(var r:regions){require(r.begin()<r.end(),"forward config-effect branch");starts.add(r.begin());}
  require(starts.equals(new TreeSet<>(List.of(13,1064,2342,2637,3637,3688,3942,4050,4128,4394,4445,4589,5305,5546,5595,5706,5755,5840,5973,6091,6209,6271,6408,6487,6540,6597,6689,6742,6812,6864,6925,6980,7035,7123,7172,7232,7302,7358,7414,7522,7572,7642,7694,7711))),"exact config-change branch roster: "+starts);
  // All explicit entries to an effect block pass its guard, including the
  // alternative isUpdated(true) edge into drag/FF at4128. Native catch handlers
  // stay in their original region and preserve original Throwable identity.
  int at=0;for(var op:ops){if(op instanceof BranchInstruction branch){int target=code.labelToBci(branch.target());for(var r:regions)require(at>=r.begin()&&at<r.end()||target<=r.begin()||target>=r.end(),"foreign branch enters after change guard");}if(op instanceof LookupSwitchInstruction||op instanceof TableSwitchInstruction)throw new IllegalArgumentException("FMC config unexpected switch");at+=op.sizeInBytes();}
  for(var h:code.exceptionHandlers()){int handler=code.labelToBci(h.handler());for(var r:regions)if(handler>r.begin()&&handler<r.end())require(code.labelToBci(h.tryStart())>=r.begin()&&code.labelToBci(h.tryEnd())<=r.end(),"foreign exception entry into config block");}
  return List.copyOf(regions);
 }
 /** Exact original instruction indices whose content effects require a true
  * guarded config change. Never use this pruning for old jobs/lifetimes. */
 static Set<Integer> excludedSites(Prepare data){var regions=qualify(data);var result=new HashSet<Integer>();int bci=0,index=0;for(var op:Prepare.instructions(data.methods.get(METHOD))){final int position=bci;if(regions.stream().anyMatch(r->position>=r.begin()&&position<r.end()))result.add(index);bci+=op.sizeInBytes();index++;}return Set.copyOf(result);}
 static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
  var regions=qualify(data);return weave(data,lower,METHOD,regions.stream().map(Region::begin).collect(java.util.stream.Collectors.toUnmodifiableSet()));
 }
 /** Shared only with authored fixtures; production source seal and region
  * dominance checks are mandatory in transform. */
 static Map<String,byte[]> weave(Prepare data,Map<String,byte[]> lower,Prepare.MethodId id,Set<Integer> entries){
  var owner=ClassDesc.of(id.owner());var input=lower.getOrDefault(id.owner(),data.inputs.get(id.owner()));var model=data.cf.parse(input);require(data.methods.get(id).flags().flagsMask()==ClassFile.ACC_STATIC&&id.descriptor().equals("()V"),"finite static void original");
  for(String name:List.of(ENTRY,LEAVE,VIOLATION))require(model.methods().stream().noneMatch(m->m.methodName().equalsString(name)),"generated helper collision");
  require(data.models.get(id.owner()).fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT)),"original failure field collision");
  var faults=model.fields().stream().filter(f->f.fieldName().equalsString(FAULT)).toList();int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC;
  require(faults.isEmpty()||faults.size()==1&&faults.getFirst().fieldType().equalsString("Z")&&faults.getFirst().flags().flagsMask()==flags,"exact existing failure field");
  // Locate guards by original instruction ordinal. Earlier transforms may add
  // calls, so this lane must precede content-field/input/operation weaving.
  var original=Prepare.instructions(data.methods.get(id));var guardIndices=new HashSet<Integer>();int bci=0;for(int i=0;i<original.size();i++){if(entries.contains(bci))guardIndices.add(i);bci+=original.get(i).sizeInBytes();}require(guardIndices.size()==entries.size(),"aligned native effect guards");
  var candidate=model.methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).findFirst().orElseThrow();boolean captured=false;
  if(!Arrays.equals(body(data,id.owner(),candidate),body(data,id.owner(),data.methods.get(id)))){
   require(id.equals(METHOD),"complete config original method preimage before other weaving");var canonical=compatibility(data,Map.of());var expected=data.cf.parse(canonical.get("Option")).methods().stream().filter(m->m.methodName().equalsString(METHOD.name())&&m.methodType().equalsString(METHOD.descriptor())).findFirst().orElseThrow();require(Arrays.equals(body(data,"Option",candidate),body(data,"Option",expected)),"exact existing compatibility polling template preimage");captured=true;
   byte[] restored=data.cf.transformClass(model,(b,e)->{if(e instanceof MethodModel m&&m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor()))b.with(data.methods.get(id));else b.with(e);});model=data.cf.parse(restored);
  }
  byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
   public void accept(ClassBuilder out,ClassElement element){if(!(element instanceof MethodModel method)||!method.methodName().equalsString(id.name())||!method.methodType().equalsString(id.descriptor())){out.with(element);return;}
    out.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){int ticket,thrown,index;Label begin,end,handler;List<ExceptionCatch> originalHandlers;
     public void atStart(CodeBuilder c){ticket=c.allocateLocal(TypeKind.INT);thrown=c.allocateLocal(TypeKind.REFERENCE);originalHandlers=new ArrayList<>();c.invokestatic(owner,ENTRY,MethodTypeDesc.of(CD_int)).istore(ticket);begin=c.newLabel();end=c.newLabel();handler=c.newLabel();c.labelBinding(begin);}
     public void accept(CodeBuilder c,CodeElement element){if(element instanceof ExceptionCatch h){originalHandlers.add(h);return;}if(element instanceof Instruction){if(guardIndices.contains(index))c.iload(ticket).invokestatic(owner,VIOLATION,MethodTypeDesc.of(CD_void,CD_int));index++;}if(element instanceof ReturnInstruction)c.iload(ticket).invokestatic(owner,LEAVE,MethodTypeDesc.of(CD_void,CD_int));c.with(element);}
     public void atEnd(CodeBuilder c){require(index==original.size(),"complete monitor instruction weave");for(var h:originalHandlers)c.with(h);c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(thrown).iload(ticket).invokestatic(owner,LEAVE,MethodTypeDesc.of(CD_void,CD_int)).aload(thrown).athrow();}
    }));
   }
   public void atEnd(ClassBuilder out){if(faults.isEmpty())out.withField(FAULT,CD_boolean,flags);helpers(out,owner);}
  });
  require(data.cf.verify(bytes).isEmpty(),"final config monitor verifier");var output=new HashMap<>(lower);output.put(id.owner(),bytes);if(captured)output.putAll(compatibility(data,output));var before=new HashMap<>(data.inputs);before.putAll(lower);TerrainAccess.verifyAdded(before,output,data.inputs);return Map.copyOf(output);
 }
 /** Exactly the existing capture poll witness, never a new compatibility mode.
  * The canonical preimage is built from original native bytes. Reapplication
  * surrounds only the already composed monitor and preserves final-model
  * changed-value accounting, optional helper failure and original exceptions. */
 private static Map<String,byte[]> compatibility(Prepare data,Map<String,byte[]> lower){return FmcWriterPrepare.transform(data,lower,Set.of(METHOD),ClassDesc.of("psx.exporter.FmcCompatibilityWitness"),Set.of(),Map.of(METHOD,Set.of(new FmcQualification.Member("Option","ngFmc","Z"),new FmcQualification.Member("Option","engineModel","I"))));}
 private static void helpers(ClassBuilder b,ClassDesc owner){int flags=ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
  b.withMethodBody(ENTRY,MethodTypeDesc.of(CD_int),flags,c->{int result=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(result);guard(c,owner,()->c.invokestatic(GATE,"enter",MethodTypeDesc.of(CD_int)).istore(result));c.iload(result).ireturn();});
  b.withMethodBody(LEAVE,MethodTypeDesc.of(CD_void,CD_int),flags,c->{var done=c.newLabel();c.iload(0).ifle(done);guard(c,owner,()->c.iload(0).invokestatic(GATE,"leave",MethodTypeDesc.of(CD_void,CD_int)));c.labelBinding(done).return_();});
  b.withMethodBody(VIOLATION,MethodTypeDesc.of(CD_void,CD_int),flags,c->{var done=c.newLabel();c.iload(0).ifne(done);c.iconst_1().putstatic(owner,FAULT,CD_boolean);guard(c,owner,()->c.invokestatic(REPORT,"failed",MethodTypeDesc.of(CD_void)));c.labelBinding(done).return_();});
 }
 static boolean generated(Prepare data,String owner,MethodModel method){
  if(!owner.equals("Option")||!Set.of(ENTRY,LEAVE,VIOLATION).contains(method.methodName().stringValue())||data.methods.containsKey(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue())))return false;
  var expected=data.cf.parse(data.cf.build(ClassDesc.of(owner),b->helpers(b,ClassDesc.of(owner)))).methods().stream().filter(m->m.methodName().equals(method.methodName())).findFirst().orElseThrow();
  return method.flags().flagsMask()==expected.flags().flagsMask()&&method.methodType().equals(expected.methodType())&&Arrays.equals(body(data,owner,method),body(data,owner,expected));
 }
 private static byte[] body(Prepare data,String owner,MethodModel m){return data.cf.build(ClassDesc.of(owner),b->b.withMethod(m.methodName().stringValue(),m.methodTypeSymbol(),m.flags().flagsMask(),x->x.transformCode(m.code().orElseThrow(),(c,e)->c.with(e))));}
 private static void guard(CodeBuilder c,ClassDesc owner,Runnable work){var start=c.newLabel();var end=c.newLabel();var failed=c.newLabel();var done=c.newLabel();c.labelBinding(start);work.run();c.labelBinding(end).goto_(done).exceptionCatchAll(start,end,failed).labelBinding(failed).pop().iconst_1().putstatic(owner,FAULT,CD_boolean);var report=c.newLabel();var finish=c.newLabel();var absent=c.newLabel();c.labelBinding(report).invokestatic(REPORT,"failed",MethodTypeDesc.of(CD_void)).labelBinding(finish).goto_(done).exceptionCatchAll(report,finish,absent).labelBinding(absent).pop().labelBinding(done);}
 private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native config monitor "+why);}
 static final String CONFIG="$psxExporter$fmcOptionConfigurationBanks",CONFIG_COUNT="$psxExporter$fmcOptionConfigurationBankCount";
 /** Native config indices read by the sealed monitor, including airline policy.
  * These receivers keep their ordinary freshness; none may become recall-owned. */
 static List<FmcReconstructionExecRoots.Bank> configurationBanks(Prepare data){
  qualify(data);var rows=new TreeMap<String,FmcReconstructionExecRoots.Bank>();
  for(var op:Prepare.instructions(data.methods.get(METHOD)))if(op instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("Glob")&&f.type().equalsString("I")){
   String name=f.name().stringValue();if(!name.startsWith("qiCfg")&&!name.startsWith("qsCfg")&&!name.equals("qsFmcAirlPol"))continue;String array=name.startsWith("qi")?"qi":"qs",descriptor=array.equals("qi")?"[LQi;":"[LQs;";
   var index=FmcQualification.resolveField(data.models,"Glob",name,"I",new HashSet<>());var root=FmcQualification.resolveField(data.models,"Glob",array,descriptor,new HashSet<>());require(index!=null&&root!=null,"resolved configuration bank role");rows.put(name,new FmcReconstructionExecRoots.Bank(root,index));
  }
  require(!rows.isEmpty()&&rows.containsKey("qsFmcAirlPol"),"complete actual configuration/airline bank roster");return List.copyOf(rows.values());
 }
 static Set<FmcQualification.Member> configurationInputs(Prepare data){var fields=new HashSet<FmcQualification.Member>();for(var bank:configurationBanks(data)){fields.add(bank.array());fields.add(bank.index());}return Set.copyOf(fields);}
 /** Worker-only readonly identity capture. Object slots0/1 are actual array
  * roots; subsequent slots are bank receivers. Primitive indices use their own
  * preallocated buffer, so repeated preflight neither allocates nor consumes. */
 static Map<String,byte[]> bindings(Prepare data,Map<String,byte[]> lower){
  var banks=configurationBanks(data);byte[] input=lower.getOrDefault("Option",data.inputs.get("Option"));var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(CONFIG,CONFIG_COUNT).contains(m.methodName().stringValue())),"configuration getter collision");byte[] bytes=addConfigurationBridge(data,input,banks);var output=new HashMap<>(lower);output.put("Option",bytes);var before=new HashMap<>(data.inputs);before.putAll(lower);var after=new HashMap<>(before);after.putAll(output);TerrainAccess.verifyAdded(before,after,data.inputs);return Map.copyOf(output);
 }
 static byte[] addConfigurationBridge(Prepare data,byte[] input,List<FmcReconstructionExecRoots.Bank> banks){
  byte[] out=data.cf.transformClass(data.cf.parse(input),ClassTransform.endHandler(b->{b.withMethodBody(CONFIG_COUNT,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.loadConstant(banks.size()).ireturn());b.withMethodBody(CONFIG,MethodTypeDesc.of(CD_void,CD_Object.arrayType(),CD_int.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
   var glob=ClassDesc.of("Glob");c.aload(0).iconst_0().getstatic(glob,"qi",ClassDesc.ofDescriptor("[LQi;")).aastore();c.aload(0).iconst_1().getstatic(glob,"qs",ClassDesc.ofDescriptor("[LQs;")).aastore();
   for(int i=0;i<banks.size();i++){var bank=banks.get(i);c.aload(1).loadConstant(i).getstatic(glob,bank.index().name(),CD_int).iastore();c.aload(0).loadConstant(i+2).getstatic(glob,bank.array().name(),ClassDesc.ofDescriptor(bank.array().descriptor())).getstatic(glob,bank.index().name(),CD_int).aaload().aastore();}c.return_();
  });}));require(data.cf.verify(out).isEmpty(),"readonly configuration getter verifier");return out;
 }
 private FmcBackgroundAdmissionOptionPrepare(){}
}



