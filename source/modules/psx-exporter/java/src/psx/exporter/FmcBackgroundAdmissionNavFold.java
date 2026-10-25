package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.attribute.CodeAttribute;import java.lang.classfile.instruction.*;import java.util.*;
/** Exact argument projection on an unchanged original call edge. Native
 * navFold(null) keeps its synchronized current-directory observation but cannot
 * enter the non-null directory/cache replacement branch. This proves neither
 * cache immutability nor an unowned receiver; direct/current reads remain live. */
final class FmcBackgroundAdmissionNavFold {
 static final Prepare.MethodId METHOD=new Prepare.MethodId("Core","navFold","(Ljava/lang/String;)Ljava/lang/String;");
 private static final String CORE="D522384326E98914FB2D2028588A6C2C8D1AA8DC7FFDFB7582AEE5BE49C7E037";
 private static final Set<Integer> NON_NULL=Set.of(2,3,4,5);
 private final Prepare data;private final Map<Prepare.MethodId,Set<Integer>> literalCalls=new HashMap<>();
 FmcBackgroundAdmissionNavFold(Prepare data){this.data=Objects.requireNonNull(data);qualify(data);}
 static Set<Integer> qualify(Prepare data){
  byte[] source=data.inputs.get("Core");require(source!=null,"original Core source absent");try{require(Identity.hash(source).equalsIgnoreCase(CORE),"whole original Core source identity");}catch(Exception e){throw new IllegalArgumentException("FMC native navFold source seal",e);}
  var method=data.methods.get(METHOD);require(method!=null&&method.flags().flagsMask()==(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED)&&method.code().isPresent(),"original synchronized native getter/setter");var ops=Prepare.instructions(method);require(ops.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.ALOAD_0,Opcode.IFNULL,Opcode.ALOAD_0,Opcode.PUTSTATIC,Opcode.GETSTATIC,Opcode.INVOKESTATIC,Opcode.GETSTATIC,Opcode.ARETURN)),"complete original native null branch");
  var code=(CodeAttribute)method.code().orElseThrow();require(code.exceptionHandlers().isEmpty()&&code.labelToBci(((BranchInstruction)ops.get(1)).target())==14,"exact null return branch without hidden handler");
  require(Prepare.field(ops.get(3),Opcode.PUTSTATIC,"Core","navDir")&&Prepare.field(ops.get(4),Opcode.GETSTATIC,"Core","navDir")&&Prepare.call(ops.get(5),"ScanComm","makeCache","(Ljava/lang/String;)V")&&Prepare.field(ops.get(6),Opcode.GETSTATIC,"Core","navDir"),"conditional directory publication/cache rebuild and unconditional current read");return NON_NULL;
 }
 /** Substantive original callee instruction ordinals must all belong to the
  * unreachable non-null branch. Never prune the method or its current reader. */
 boolean unboundCall(Prepare.MethodId parent,int callOrdinal,Prepare.MethodId callee,Set<Integer> substantive){
  if(!METHOD.equals(callee)||substantive==null||substantive.isEmpty()||!NON_NULL.containsAll(substantive))return false;
  return literalCalls.computeIfAbsent(parent,this::literalCalls).contains(callOrdinal);
 }
 private Set<Integer> literalCalls(Prepare.MethodId parent){
  var method=data.methods.get(parent);if(method==null||method.code().isEmpty())return Set.of();var code=method.code().orElseThrow();var ops=Prepare.instructions(method);var labels=new HashMap<Label,Integer>();int index=0;for(var element:code){if(element instanceof LabelTarget target)labels.put(target.label(),index);if(element instanceof Instruction)index++;}
  var entries=new HashSet<Integer>();for(var op:ops){if(op instanceof BranchInstruction b)entries.add(labels.get(b.target()));else if(op instanceof LookupSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}else if(op instanceof TableSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}}for(var h:code.exceptionHandlers())entries.add(labels.get(h.handler()));
  var calls=new HashSet<Integer>();for(int at=1;at<ops.size();at++)if(!entries.contains(at)&&ops.get(at-1).opcode()==Opcode.ACONST_NULL&&ops.get(at) instanceof InvokeInstruction invoke&&invoke.opcode()==Opcode.INVOKESTATIC&&invoke.owner().asInternalName().equals(METHOD.owner())&&invoke.name().equalsString(METHOD.name())&&invoke.type().equalsString(METHOD.descriptor()))calls.add(at);return Set.copyOf(calls);
 }
 private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native literal-null projection "+why);}
}
