package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Ordinary EXEC may cancel an incomplete datalink logon. The native <8
 * predicate preserves completed/logging states at or above8, including11.
 * No message, transport, radio or physical cadence is suppressed. */
final class FmcReconstructionDatalinkEffects {
    static final FmcQualification.Member TIMER=new FmcQualification.Member("Fans","logonTimer","F");
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        require(!construction.contains(TIMER)&&!currentInputs.contains(TIMER),"construction/current logon timer collision");var reset=new Prepare.MethodId("FmcDataLink","resetAllTimers","(I)V");var expected=Set.of(reset,new Prepare.MethodId("CduPgAtcLogon$ThreadL1","run","()V"),new Prepare.MethodId("CduPgAtcLogonNg1$ThreadL1","run","()V"),new Prepare.MethodId("Fans","cduLogon","()V"),new Prepare.MethodId("Fans","cduLogoff","()V"),new Prepare.MethodId("Fans","runLento","()V"),new Prepare.MethodId("Fans","readGlobBasics","()V"));require(FmcReconstructionPageEffects.writers(data,Set.of(TIMER)).getOrDefault(TIMER,Set.of()).equals(expected)&&FmcReconstructionExecAudit.inspect(data).writers().getOrDefault(TIMER,Set.of()).equals(Set.of(reset)),"exact native logon timer writers");
        var method=data.methods.get(reset);var pos=FmcReconstructionTransitionEffects.positions(method);var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();require(Prepare.field(pos.get(52),Opcode.GETSTATIC,"Fans","logonTimer")&&pos.get(55) instanceof ConstantInstruction limit&&limit.constantValue().equals(8f)&&pos.get(58).opcode()==Opcode.FCMPG&&pos.get(59) instanceof BranchInstruction done&&done.opcode()==Opcode.IFGE&&code.labelToBci(done.target())==66&&pos.get(62) instanceof ConstantInstruction zero&&zero.constantValue().equals(0f)&&Prepare.field(pos.get(63),Opcode.PUTSTATIC,"Fans","logonTimer")&&pos.get(66).opcode()==Opcode.RETURN,"native incomplete-only logon reset branch");
        for(var group:List.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS))for(var input:group)require(!TIMER.equals(new FmcQualification.Member(input.owner(),input.name(),input.descriptor())),"logon timer cannot be artifact content");return Set.of(TIMER);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native datalink output "+why);}
}
