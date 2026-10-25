package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** The user's ordinary EXEC/guidance and no-scratchpad contracts. This
 * partitions exact potential native outputs, never entered construction or
 * current physical/manual input cells. Original execution and all potential
 * mutation/lifecycle checks remain in force. */
final class FmcReconstructionNormalExec {
    private static final Set<String> OUTPUT_OWNERS=Set.of("AfdsHub","AfdsFcc","AfdsFcc$Fcc","AfdsFcc$Fcc$10","FmcMessages","FmcMessages$HiMsg","FmcMessages$CduBox","AcarsMessages","AcarsMessages$CduBox","EicasMessages","EicasMessages$Message");
    private static final Set<FmcQualification.Member> RELATED=Set.of(new FmcQualification.Member("FmcRte","lnavLastHdgWingsLevelRequested","Z"),new FmcQualification.Member("Autothrottle","tlaMan","F"),new FmcQualification.Member("Autothrottle","tlaCom","F"));
    private static final Set<String> BANK_NAMES=Set.of("qsAfds","qsTogaTrk","qiMcpWdoVs","qiMcpWdoHdg","qiMcpWdoSpd","qiAltAlert","qsFmcThrSet","qsLcduScrPad","qsRcduScrPad","qsCcduScrPad","qsFmcScrPads","qsAcarsScrPads","qsCduColSpL","qsCduColSpR","qsCduColSpC");
    record Profile(Set<FmcQualification.Member> fields,Set<FmcReconstructionExecRoots.Bank> banks,Set<FmcQualification.Member> retainedInputs,Map<FmcReconstructionExecRoots.Bank,Set<Prepare.MethodId>> bankReaders,Set<FmcQualification.Member> references,Set<FmcReconstructionExecRoots.Bank> dynamicScratchpads,Set<FmcQualification.Member> atomicCurrentOutputs){}
    static Profile inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs,Set<FmcReconstructionExecRoots.Bank> protectedBanks){
        var potential=FmcReconstructionExecAudit.inspect(data);var forbidden=new HashSet<>(construction);forbidden.addAll(currentInputs);var fields=new HashSet<FmcQualification.Member>();var retained=new HashSet<FmcQualification.Member>();
        for(String name:List.of("tlaMan","tlaCom"))if(!potential.writers().getOrDefault(new FmcQualification.Member("Autothrottle",name,"F"),Set.of()).equals(Set.of(new Prepare.MethodId("Autothrottle","checkTlaRate","(I)V"))))throw new IllegalArgumentException("native throttle guidance cache writer drift");
        if(!potential.writers().getOrDefault(new FmcQualification.Member("EicasMessages$Message","timer","F"),Set.of()).equals(Set.of(new Prepare.MethodId("EicasMessages$Message","startAddTimer","()V"),new Prepare.MethodId("EicasMessages$Message","startDelTimer","()V"))))throw new IllegalArgumentException("native guidance warning timer writer drift");
        for(var field:potential.writes())if(OUTPUT_OWNERS.contains(field.owner())||RELATED.contains(field)){if(forbidden.contains(field))retained.add(field);else fields.add(field);}
        var uses=new HashMap<FmcReconstructionExecRoots.Bank,Set<Prepare.MethodId>>();
        for(var id:FmcExecMutationPrepare.profile(data).methods())if(OUTPUT_OWNERS.contains(id.owner())){var ops=Prepare.instructions(data.methods.get(id));for(int i=0;i+2<ops.size();i++)if(ops.get(i) instanceof FieldInstruction array&&array.opcode()==Opcode.GETSTATIC&&array.owner().asInternalName().equals("Glob")&&Set.of("qi","qs","qh").contains(array.name().stringValue())&&ops.get(i+1) instanceof FieldInstruction index&&index.opcode()==Opcode.GETSTATIC&&index.owner().asInternalName().equals("Glob")&&index.type().equalsString("I")&&ops.get(i+2).opcode()==Opcode.AALOAD){var bank=new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob",array.name().stringValue(),array.type().stringValue()),new FmcQualification.Member("Glob",index.name().stringValue(),"I"));uses.computeIfAbsent(bank,k->new HashSet<>()).add(id);}}
        // Explicit native AFDS/message/scratchpad output bank identities. An
        // arbitrary bank read in an AFDS method is not an output classification.
        var banks=new HashSet<FmcReconstructionExecRoots.Bank>();for(var bank:uses.keySet())if(BANK_NAMES.contains(bank.index().name())&&!protectedBanks.contains(bank))banks.add(bank);
        var dynamic=scratchpadBanks(data);for(var bank:dynamic)if(!protectedBanks.contains(bank))banks.add(bank);
        // These are exact published output buffers/objects, not arbitrary
        // reference fields of an AFDS or message class. Protection of any
        // construction/current input reference wins over this partition.
        var references=new HashSet<FmcQualification.Member>();for(var id:FmcExecMutationPrepare.profile(data).methods())if(OUTPUT_OWNERS.contains(id.owner()))for(var op:Prepare.instructions(data.methods.get(id)))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)){
            var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field==null||forbidden.contains(field))continue;
            String type=field.descriptor();while(type.startsWith("["))type=type.substring(1);
            if(type.equals("Ljava/lang/StringBuilder;")||type.equals("Ljava/lang/StringBuffer;")||type.startsWith("L")&&OUTPUT_OWNERS.contains(type.substring(1,type.length()-1))||field.descriptor().startsWith("[")&&OUTPUT_OWNERS.contains(field.owner()))references.add(field);
        }
        for(var field:fields)if(field.descriptor().startsWith("[")||field.descriptor().startsWith("L")&&!field.descriptor().equals("Ljava/lang/String;"))references.add(field);
        if(!Collections.disjoint(fields,forbidden))throw new IllegalArgumentException("normal EXEC partition overlaps construction/current inputs");
        var frozen=new HashMap<FmcReconstructionExecRoots.Bank,Set<Prepare.MethodId>>();uses.forEach((k,v)->frozen.put(k,Set.copyOf(v)));return new Profile(Set.copyOf(fields),Set.copyOf(banks),Set.copyOf(retained),Map.copyOf(frozen),Set.copyOf(references),Set.copyOf(dynamic),atomicCurrentOutputs(data,construction));
    }
    /** Phase is a current geometry input during load and a single-cell native
     * guidance publication during ordinary EXEC. Its changed-value input
     * witness remains installed. Only reconstruction reader/journal protection
     * is removed; this exception grants no writer or lifecycle authority. */
    private static Set<FmcQualification.Member> atomicCurrentOutputs(Prepare data,Set<FmcQualification.Member> construction){
        var phase=new FmcQualification.Member("FmcVnav","phase","I");
        if(construction.contains(phase))throw new IllegalArgumentException("reconstruction cannot publish native phase");
        if(java.util.stream.Stream.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS).flatMap(Collection::stream).anyMatch(f->f.owner().equals(phase.owner())&&f.name().equals(phase.name())))throw new IllegalArgumentException("phase cannot be artifact content");
        var exec=Set.of(new Prepare.MethodId("FmcRte","setModAct","(I)Z"),new Prepare.MethodId("FmcVnav","activateCrzPhase","()V"),new Prepare.MethodId("FmcVnav","activateEarlyDescent","()V"));
        var expected=new HashSet<>(exec);expected.addAll(Set.of(new Prepare.MethodId("FmcVnav","activateClbPhase","(Z)V"),new Prepare.MethodId("FmcVnav","activateNormalDescent","(I)V"),new Prepare.MethodId("FmcVnav","readGlobVnavX","()V"),new Prepare.MethodId("FmcVnav","setDesPhaseByInstructor","()V")));
        var writers=new HashSet<Prepare.MethodId>();int stores=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent())for(var op:Prepare.instructions(entry.getValue()))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)&&phase.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()))){
            if(f.opcode()!=Opcode.PUTSTATIC)throw new IllegalArgumentException("phase publication must be one static int cell");writers.add(entry.getKey());stores++;
        }
        if(stores!=7||!writers.equals(expected)||!FmcReconstructionExecAudit.inspect(data).writers().getOrDefault(phase,Set.of()).equals(exec))throw new IllegalArgumentException("native phase atomic publication writer drift "+writers+" sites="+stores);
        return Set.of(phase);
    }
    private static Set<FmcReconstructionExecRoots.Bank> scratchpadBanks(Prepare data){
        var indices=new HashSet<String>();int stores=0;
        for(var id:data.methods.keySet()){if(data.methods.get(id).code().isEmpty())continue;var ops=Prepare.instructions(data.methods.get(id));for(int i=0;i<ops.size();i++)if(ops.get(i) instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals("ControlDisplayUnit")&&f.name().equalsString("globScrPadIdx")&&f.type().equalsString("I")){
            stores++;if(!id.owner().equals("ControlDisplayUnit")||!id.name().equals("<init>")||i==0||!(ops.get(i-1) instanceof FieldInstruction index)||index.opcode()!=Opcode.GETSTATIC||!index.owner().asInternalName().equals("Glob")||!Set.of("qsLcduScrPad","qsRcduScrPad","qsCcduScrPad").contains(index.name().stringValue())||!index.type().equalsString("I"))throw new IllegalArgumentException("unqualified native scratchpad index writer "+id);indices.add(index.name().stringValue());
        }}
        if(stores!=3||indices.size()!=3)throw new IllegalArgumentException("native scratchpad index constructor coverage");
        var result=new HashSet<FmcReconstructionExecRoots.Bank>();for(String name:indices)result.add(new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob","qs","[LQs;"),new FmcQualification.Member("Glob",name,"I")));return Set.copyOf(result);
    }
    public static void main(String[] args)throws Exception{var data=new Prepare(Prepare.readJar(java.nio.file.Path.of(args[0])));var p=inspect(data,Set.of(),Set.of(),Set.of());System.out.println("Normal EXEC outputs fields="+p.fields.size()+" banks="+p.banks.size()+" references="+p.references.size());for(var f:new TreeSet<>(p.fields))System.out.println("FIELD "+f);for(var b:p.banks.stream().sorted(Comparator.comparing(b->b.index().name())).toList())System.out.println("BANK "+b+" <- "+p.bankReaders.getOrDefault(b,Set.of()));for(var f:new TreeSet<>(p.references))System.out.println("REF "+f);}
}
