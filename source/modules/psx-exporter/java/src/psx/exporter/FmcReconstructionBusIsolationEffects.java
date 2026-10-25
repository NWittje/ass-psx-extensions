package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Native AFDS mode feedback to electrical isolation is one boolean output.
 * The original request, synchronized isolation check and electrical cadence
 * continue; recall neither imports nor forces an isolation state. */
final class FmcReconstructionBusIsolationEffects {
    static final FmcQualification.Member OUTPUT=new FmcQualification.Member("ElecControls","busIsolRequested","Z");
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        if(construction.contains(OUTPUT)||currentInputs.contains(OUTPUT))throw new IllegalArgumentException("native isolation output overlaps reconstruction/current input");
        for(var group:List.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS))for(var input:group)if(input.owner().equals(OUTPUT.owner())&&input.name().equals(OUTPUT.name()))throw new IllegalArgumentException("isolation cannot be artifact content");
        var writer=new Prepare.MethodId("ElecControls","requestBusIsolation","(I)V");var caller=new Prepare.MethodId("AfdsHub","readGlob","()V");int stores=0,calls=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(Prepare.field(op,Opcode.PUTSTATIC,OUTPUT.owner(),OUTPUT.name())){if(!entry.getKey().equals(writer)||bci!=26)throw new IllegalArgumentException("native isolation sole store drift "+entry.getKey());stores++;}if(Prepare.call(op,writer.owner(),writer.name(),writer.descriptor())){if(!entry.getKey().equals(caller)||bci!=2010)throw new IllegalArgumentException("native isolation AFDS caller drift "+entry.getKey());calls++;}bci+=op.sizeInBytes();}}
        var ops=Prepare.instructions(data.requireMethod(writer.owner(),writer.name(),writer.descriptor()));var code=(java.lang.classfile.attribute.CodeAttribute)data.methods.get(writer).code().orElseThrow();var constants=ops.stream().filter(ConstantInstruction.class::isInstance).map(ConstantInstruction.class::cast).map(ConstantInstruction::constantValue).toList();var branches=ops.stream().filter(BranchInstruction.class::isInstance).map(BranchInstruction.class::cast).map(b->code.labelToBci(b.target())).toList();
        if(stores!=1||calls!=1||ops.size()!=17||!Prepare.field(ops.getFirst(),Opcode.GETSTATIC,OUTPUT.owner(),OUTPUT.name())||!constants.equals(List.of(1,1,0,1,1,0))||!branches.equals(List.of(12,13,32,25,26))||!Prepare.call(ops.get(15),"ElecControls","checkBusIsolation","()V")||ops.getLast().opcode()!=Opcode.RETURN)throw new IllegalArgumentException("native isolation changed-value boolean/check dataflow drift");
        var check=data.requireMethod("ElecControls","checkBusIsolation","()V");if((check.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)==0||Prepare.instructions(check).stream().noneMatch(i->Prepare.field(i,Opcode.GETSTATIC,OUTPUT.owner(),OUTPUT.name())))throw new IllegalArgumentException("native synchronized electrical consequence drift");
        if(!FmcReconstructionExecAudit.inspect(data).writers().getOrDefault(OUTPUT,Set.of()).equals(Set.of(writer)))throw new IllegalArgumentException("potential native isolation authority drift");return Set.of(OUTPUT);
    }
    static FmcReconstructionNormalExec.Profile apply(Prepare data,FmcReconstructionNormalExec.Profile normal,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){var fields=new HashSet<>(normal.fields());fields.addAll(inspect(data,construction,currentInputs));return new FmcReconstructionNormalExec.Profile(Set.copyOf(fields),normal.banks(),normal.retainedInputs(),normal.bankReaders(),normal.references(),normal.dynamicScratchpads(),normal.atomicCurrentOutputs());}
}
