package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.attribute.CodeAttribute;import java.lang.classfile.instruction.*;import java.util.*;
/** Profile-jump availability is a finite UI query. During reconstruction the
 * last button state may remain; its original Routes cadence retries afterward.
 * Actual time/position jump, bank consumption and mixed instructor frames are
 * outside this boundary. Old complete synchronized queries drain normally. */
final class FmcBackgroundAdmissionJumpPrepare {
 static final Prepare.MethodId METHOD=new Prepare.MethodId("RouteTools","checkProfileJumpButton","()V");
 private record Site(Prepare.MethodId method,int bci){}
 static void qualify(Prepare data){
  try{require(Identity.hash(data.inputs.get("RouteTools")).equalsIgnoreCase("A7A5A01950606E9A072D86DAA25F406061C4D06B7DA2EFA89492E27AD4562D2F"),"whole original UI source");}catch(Exception e){throw new IllegalArgumentException("FMC profile UI source digest",e);}
  var method=data.methods.get(METHOD);require(method!=null&&method.flags().flagsMask()==(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED)&&method.code().isPresent(),"finite original synchronized static UI query");require(((CodeAttribute)method.code().orElseThrow()).exceptionHandlers().isEmpty(),"native UI query has no altered exception scope");
  var stores=new ArrayList<String>();var calls=new ArrayList<String>();for(var op:Prepare.instructions(method)){
   if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC))stores.add(f.owner().asInternalName()+"."+f.name().stringValue()+":"+f.type().stringValue());
   if(op instanceof InvokeInstruction i)calls.add(i.owner().asInternalName()+"."+i.name().stringValue()+i.type().stringValue());
   require(!(op instanceof NewObjectInstruction)&&!(op instanceof NewReferenceArrayInstruction)&&!(op instanceof NewPrimitiveArrayInstruction)&&!(op instanceof InvokeDynamicInstruction)&&!Set.of(Opcode.IASTORE,Opcode.LASTORE,Opcode.FASTORE,Opcode.DASTORE,Opcode.AASTORE,Opcode.BASTORE,Opcode.CASTORE,Opcode.SASTORE).contains(op.opcode()),"no input/job/array publication in UI availability leaf");
  }
  require(stores.equals(List.of("RouteTools.profileEnable:Z","RouteTools.profileMode:I")),"only UI availability outputs");require(calls.equals(List.of("PButton.setEnabled(Z)V","PCheckBox.transferFocus()V","PButton.setText(Ljava/lang/String;)V")),"only exact button/focus UI continuations");
  var callers=new HashSet<Site>();for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(op instanceof InvokeInstruction i&&i.owner().asInternalName().equals(METHOD.owner())&&i.name().equalsString(METHOD.name())&&i.type().equalsString(METHOD.descriptor())){require(i.opcode()==Opcode.INVOKESTATIC,"native UI static invocation");callers.add(new Site(entry.getKey(),bci));}bci+=op.sizeInBytes();}}
  require(callers.equals(Set.of(new Site(new Prepare.MethodId("RouteTools","checkTimeJumpButton","()V"),255),new Site(new Prepare.MethodId("InstructorSituTime","runModerato","()V"),67))),"only exact ordinary availability callers");
 }
 static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){qualify(data);return FmcExclusionPrepare.transform(data,lower,Set.of(METHOD));}
 private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC finite profile UI "+why);}
 private FmcBackgroundAdmissionJumpPrepare(){}
}