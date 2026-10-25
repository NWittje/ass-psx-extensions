package psx.exporter;

import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Native FMC guidance memory is one atomic bit word, not imported route/PERF
 * cells. Partition only its proved current mem1 receiver and bank identity;
 * shared MemBits/Qi fields keep their receiver guards on every other object.
 * The write branch publishes native bit changes; the ordinary FMC cadence
 * still defers its multi-field read/apply branch during reconstruction. */
final class FmcBackgroundAdmissionMemoryEffects {
    static final FmcQualification.Member REFERENCE=new FmcQualification.Member("FmcGlobal","mem1","LFmcGlobal$MemBits;");
    static final FmcReconstructionExecRoots.Bank BANK=new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob","qi","[LQi;"),new FmcQualification.Member("Glob","qiFmcMem1","I"));
    record Profile(Set<FmcQualification.Member> references,Set<FmcReconstructionExecRoots.Bank> banks,FmcReconstructionArrayProof.ElementResult elementAccesses){}
    private FmcBackgroundAdmissionMemoryEffects(){}
    static Profile inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> inputs,Set<FmcReconstructionExecRoots.Bank> protectedBanks){
        require(!construction.contains(REFERENCE)&&!inputs.contains(REFERENCE)&&!protectedBanks.contains(BANK),"protected current memory receiver/bank");require(!construction.contains(new FmcQualification.Member("FmcGlobal$MemBits","mask","I"))&&!inputs.contains(new FmcQualification.Member("FmcGlobal$MemBits","mask","I")),"protected guidance word");
        FmcBackgroundAdmissionAfdsPrepare.qualifyMemory(data);var writers=new HashSet<Prepare.MethodId>();int stores=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            var ops=Prepare.instructions(entry.getValue());for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction field&&(field.opcode()==Opcode.PUTSTATIC||field.opcode()==Opcode.PUTFIELD)&&REFERENCE.equals(FmcQualification.resolveField(data.models,field.owner().asInternalName(),field.name().stringValue(),field.type().stringValue(),new HashSet<>()))){stores++;writers.add(entry.getKey());require(field.opcode()==Opcode.PUTSTATIC&&at>=3&&ops.get(at-3) instanceof NewObjectInstruction made&&made.className().asInternalName().equals("FmcGlobal$MemBits")&&ops.get(at-2).opcode()==Opcode.DUP&&Prepare.call(ops.get(at-1),"FmcGlobal$MemBits","<init>","()V"),"sole native fresh memory receiver store");}
        }
        require(stores==1&&writers.equals(Set.of(new Prepare.MethodId("FmcGlobal","<clinit>","()V"))),"immutable current guidance memory root");
        var outside=new HashSet<>(data.methods.keySet());outside.removeAll(writers);var access=FmcReconstructionArrayProof.elementAccesses(data,Set.of(REFERENCE),outside,"FmcGlobal$MemBits");require(access.proof().mutations().isEmpty(),"memory receiver cannot be published through another alias");
        require(REFERENCE.equals(FmcQualification.resolveField(data.models,REFERENCE.owner(),REFERENCE.name(),REFERENCE.descriptor(),new HashSet<>()))&&BANK.index().equals(FmcQualification.resolveField(data.models,BANK.index().owner(),BANK.index().name(),BANK.index().descriptor(),new HashSet<>())),"exact current memory bank/root members");
        return new Profile(Set.of(REFERENCE),Set.of(BANK),access);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native memory effect "+why);}
}
