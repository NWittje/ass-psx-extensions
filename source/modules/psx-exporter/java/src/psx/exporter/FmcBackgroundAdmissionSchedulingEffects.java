package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.attribute.CodeAttribute;import java.lang.classfile.instruction.*;import java.util.*;
/** Native requests schedule the already-qualified VNAV cadence. They are never
 * imported route/PERF content. Original one-second countdown and native -1/1
 * direction semantics remain; the unused integer is a caller diagnostic ID. */
final class FmcBackgroundAdmissionSchedulingEffects {
 static final FmcQualification.Member TIMER=new FmcQualification.Member("FmcVnav","wptDataCalcTimer","I");
 static final Set<FmcQualification.Member> FIELDS=Set.of(TIMER);
 static final Prepare.MethodId REQUEST=new Prepare.MethodId("FmcVnav","startWptDataCalcTimer","(ZI)V"),CADENCE=new Prepare.MethodId("FmcVnav","runLargo","()V");
 private record Site(Prepare.MethodId method,int bci){}
 private static final Set<Site> WRITERS=Set.of(new Site(REQUEST,9),new Site(CADENCE,857),new Site(CADENCE,884));
 static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs,FmcReconstructionExecScope.Profile actual){
  require(Collections.disjoint(FIELDS,construction)&&Collections.disjoint(FIELDS,currentInputs),"entered/current input collision");
  var writes=new HashSet<Site>();for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(TIMER.equals(member)){require(f.opcode()==Opcode.PUTSTATIC,"single static scheduling publication");writes.add(new Site(entry.getKey(),bci));}}bci+=op.sizeInBytes();}}
  require(writes.equals(WRITERS),"whole original request/countdown writer roster");
  try{require(Identity.hash(data.inputs.get("FmcVnav")).equalsIgnoreCase("A764CF1D893A9C9FD281B8612C7D97E0E4BF3E82B71842DC64D6EE90680F4709"),"complete original request/countdown source");}catch(Exception failure){throw new IllegalArgumentException("FMC scheduling source digest",failure);}
  var method=data.methods.get(REQUEST);var ops=Prepare.instructions(method);var code=(CodeAttribute)method.code().orElseThrow();
  require(ops.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.ILOAD_0,Opcode.IFEQ,Opcode.ICONST_M1,Opcode.GOTO,Opcode.ICONST_1,Opcode.PUTSTATIC,Opcode.RETURN))&&code.exceptionHandlers().isEmpty(),"exact boolean request, no timer input/read/call/job");
  require(ops.stream().filter(x->x instanceof BranchInstruction).map(x->code.labelToBci(((BranchInstruction)x).target())).toList().equals(List.of(8,9)),"exact -1/1 publication joins");
  for(var f:FmcNarrowContent.HEADER_INPUTS)require(!TIMER.equals(new FmcQualification.Member(f.owner(),f.name(),f.descriptor())),"captured header collision");
  for(var f:FmcNarrowContent.PERF_INPUTS)require(!TIMER.equals(new FmcQualification.Member(f.owner(),f.name(),f.descriptor())),"captured PERF collision");
  for(var f:FmcNarrowContent.WAYPOINT_INPUTS)require(!TIMER.equals(new FmcQualification.Member(f.owner(),f.name(),f.descriptor())),"captured waypoint collision");
  require(actual.writes().contains(TIMER)&&actual.methods().contains(REQUEST),"actual ordinary EXEC request producer");
  require(FmcBackgroundAdmissionCadencePrepare.inspect(data).methods().contains(CADENCE),"original countdown cadence is admitted/drained/retried");
  return FIELDS;
 }
 private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native scheduling output "+why);}
 private FmcBackgroundAdmissionSchedulingEffects(){}
}