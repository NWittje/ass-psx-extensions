package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;
/** Accepted ordinary native delayed route-elevation publication. The native
 * phase/cruise predicate selects origin or destination, with its same-airport
 * special case and2000-ft fallback. Reconstruction forces no altitude; the
 * original3/29-second timer and native selection remain unchanged. */
final class FmcReconstructionLandingEffects {
    static final Set<FmcQualification.Member> OUTPUTS=Set.of(new FmcQualification.Member("FmcRte","ldgAlt","I"),new FmcQualification.Member("FmcRte","ldgAltPending","I"),new FmcQualification.Member("FmcRte","ldgAltSetTimer","F"));
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        if(!Collections.disjoint(OUTPUTS,construction)||!Collections.disjoint(OUTPUTS,currentInputs))throw new IllegalArgumentException("native landing output overlaps reconstruction/current input");
        var writer=new Prepare.MethodId("FmcRte","checkLdgAlt","()V");var all=FmcReconstructionPageEffects.writers(data,OUTPUTS);var potential=FmcReconstructionExecAudit.inspect(data);
        for(var field:OUTPUTS)if(!all.getOrDefault(field,Set.of()).equals(Set.of(writer))||!potential.writers().getOrDefault(field,Set.of()).equals(Set.of(writer)))throw new IllegalArgumentException("native landing publication writer drift "+field);
        var ops=Prepare.instructions(data.methods.get(writer));boolean airport=false,runway=false,elapsed=false,shortDelay=false,longDelay=false,subtraction=false,publication=false;
        for(int i=0;i<ops.size();i++){
            var op=ops.get(i);airport|=Prepare.field(op,Opcode.GETFIELD,"FmcRte$Rte","destElev");runway|=Prepare.field(op,Opcode.GETFIELD,"FmcRte$Rwy","elev");elapsed|=Prepare.field(op,Opcode.GETSTATIC,"Core","deltaModeratoAcc");subtraction|=op.opcode()==Opcode.FSUB;
            if(op instanceof ConstantInstruction value){shortDelay|=Objects.equals(value.constantValue(),3000f);longDelay|=Objects.equals(value.constantValue(),29000f);}
            if(Prepare.field(op,Opcode.PUTSTATIC,"FmcRte","ldgAlt")){if(i==0||!Prepare.field(ops.get(i-1),Opcode.GETSTATIC,"FmcRte","ldgAltPending"))throw new IllegalArgumentException("landing publication must use delayed pending elevation");publication=true;}
        }
        if(!airport||!runway||!elapsed||!shortDelay||!longDelay||!subtraction||!publication)throw new IllegalArgumentException("native destination landing timer/dataflow drift");
        for(var group:List.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS))for(var input:group)if(OUTPUTS.contains(new FmcQualification.Member(input.owner(),input.name(),input.descriptor())))throw new IllegalArgumentException("landing altitude cannot be artifact input");
        return OUTPUTS;
    }
    static FmcReconstructionNormalExec.Profile apply(Prepare data,FmcReconstructionNormalExec.Profile normal,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){var fields=new HashSet<>(normal.fields());fields.addAll(inspect(data,construction,currentInputs));return new FmcReconstructionNormalExec.Profile(Set.copyOf(fields),normal.banks(),normal.retainedInputs(),normal.bankReaders(),normal.references(),normal.dynamicScratchpads(),normal.atomicCurrentOutputs());}
}
