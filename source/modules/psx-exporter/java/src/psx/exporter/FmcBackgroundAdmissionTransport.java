package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Exact original outgoing-bank transport frontier. Its error/shutdown work
 * belongs to the native transport, not to entered FMC storage. All potential
 * effects remain in the operation audit; these proofs permit only the bounded
 * foreign effects to leave recall's ownership/admission/journal roster. */
final class FmcBackgroundAdmissionTransport {
    static final Prepare.MethodId FRONTIER=new Prepare.MethodId("Network","toPrint","(ICILjava/lang/String;I)V");
    static final Set<FmcQualification.Member> FIELDS=Set.of(
        new FmcQualification.Member("Network","servSockAmount","I"),new FmcQualification.Member("Network","shouldBe","I"),
        new FmcQualification.Member("Network","netModeWdoTitle","Ljava/lang/String;"),new FmcQualification.Member("Visuals","layoutChange","I"),
        new FmcQualification.Member("Demand$Reg","isOn","Z"),new FmcQualification.Member("Demand$Reg","level","I"),
        new FmcQualification.Member("InstructorSaveSitu","selectedFileName","Ljava/lang/String;"));
    static final Set<FmcQualification.Member> CURRENT_INPUTS=Set.of(new FmcQualification.Member("Network","servSockAmount","I"),new FmcQualification.Member("Network","shouldBe","I"));
    static final Set<Prepare.MethodId> ARRAYS=Set.of(new Prepare.MethodId("Network","setExitedFlag","(I)V"),new Prepare.MethodId("Demand$Reg","reset","()V"),new Prepare.MethodId("Demand$Reg","del","(I)V"));
    static final List<String> DEMAND_ROOTS=List.of("apServo","elevFeel","stbyHdg","surfSpoil","surfAilEleRud","flameN1","engParam","miscFltData","rmiL","rmiR","gndSpd","captAlt","stbyIas","stbyAlt","ilsL");
    static final Set<FmcQualification.Member> REFERENCES=references();
    record Profile(Set<FmcQualification.Member> fields,Set<Prepare.MethodId> arrayMethods,Set<FmcQualification.Member> references,
                   Set<Prepare.MethodId> continuationMethods,Set<Prepare.MethodId> sharedMethods){}
    private FmcBackgroundAdmissionTransport(){}
    private static Set<FmcQualification.Member> references(){var result=new HashSet<FmcQualification.Member>();result.add(new FmcQualification.Member("Network","sbPr","Ljava/lang/StringBuilder;"));result.add(new FmcQualification.Member("Network","serverThread","[LNetServerThread;"));result.add(new FmcQualification.Member("Demand$Reg","reg","[I"));for(String name:DEMAND_ROOTS)result.add(new FmcQualification.Member("Demand",name,"LDemand$Reg;"));return Set.copyOf(result);}
    static Profile protect(Profile profile,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        require(Collections.disjoint(profile.fields(),construction),"transport output overlaps reconstruction content");
        var overlap=new HashSet<>(profile.fields());overlap.retainAll(currentInputs);
        // These exact transport cells also control current native geometry.
        // They remain current-only changed-store witnesses; shutdown/error
        // continuation completes natively and revokes recall before new work.
        require(CURRENT_INPUTS.containsAll(overlap),"transport output overlaps preserved current input "+overlap);
        return profile;
    }
    static Profile inspect(Prepare data){
        var potential=FmcReconstructionExecAudit.inspect(data);var nativeMethods=FmcExecMutationPrepare.profile(data).methods();
        return inspect(data,potential,nativeMethods);
    }
    static Profile inspect(Prepare data,FmcReconstructionExecAudit.Effects potential,Set<Prepare.MethodId> nativeMethods){
        require(data.methods.containsKey(FRONTIER)&&nativeMethods.contains(FRONTIER),"exact original bank transport frontier");
        qualifyCurrentInputs(data);
        require(potential.writes().containsAll(FIELDS)&&potential.arrayStores().containsAll(ARRAYS),"native foreign effect vector");
        for(var f:REFERENCES)require(f.equals(FmcQualification.resolveField(data.models,f.owner(),f.name(),f.descriptor(),new HashSet<>())),"original foreign reference vector");
        var callees=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        FmcRecallBoundary.inspect(data,Set.of()).callers().forEach((child,parents)->{for(var parent:parents)callees.computeIfAbsent(parent,k->new HashSet<>()).add(child);});
        for(var parent:nativeMethods)for(var op:Prepare.instructions(data.methods.get(parent)))if(op instanceof NewObjectInstruction allocation){
            var run=run(data,allocation.className().asInternalName());
            if(run!=null&&nativeMethods.contains(run))callees.computeIfAbsent(parent,k->new HashSet<>()).add(run);
        }
        var roots=Set.of(new Prepare.MethodId("FmcRte$ExecL","run","()V"),new Prepare.MethodId("FmcRte$ExecR","run","()V"));
        var without=reach(roots,callees,nativeMethods,true);var continuation=reach(Set.of(FRONTIER),callees,nativeMethods,false);
        for(var f:FIELDS)for(var writer:potential.writers().get(f))require(!without.contains(writer)&&continuation.contains(writer),"foreign store has an independent EXEC path "+f+" "+writer);
        for(var method:ARRAYS)require(!without.contains(method)&&continuation.contains(method),"foreign array has an independent EXEC path "+method);
        var shared=new HashSet<>(continuation);shared.retainAll(without);
        // Never turn an ordinary FMC content mutator into a transport helper.
        // A shared pure formatter can remain checkpointed until uncertainty is
        // reported after the complete original toPrint continuation returns.
        continuation.removeAll(shared);
        return new Profile(FIELDS,ARRAYS,REFERENCES,Set.copyOf(continuation),Set.copyOf(shared));
    }
    private static void qualifyCurrentInputs(Prepare data){
        var method=data.methods.get(new Prepare.MethodId("Network","notClient","()Z"));require(method!=null&&method.code().isPresent(),"native current transport predicate");
        var ops=Prepare.instructions(method);var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();
        require(ops.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.GETSTATIC,Opcode.ICONST_2,Opcode.IF_ICMPNE,Opcode.GETSTATIC,Opcode.IFNE,Opcode.ICONST_1,Opcode.GOTO,Opcode.ICONST_0,Opcode.IRETURN))&&code.exceptionHandlers().isEmpty(),"exact pure transport predicate");
        require(Prepare.field(ops.get(0),Opcode.GETSTATIC,"Network","shouldBe")&&Prepare.field(ops.get(3),Opcode.GETSTATIC,"Network","servSockAmount")&&ops.stream().filter(x->x instanceof BranchInstruction).map(x->code.labelToBci(((BranchInstruction)x).target())).toList().equals(List.of(13,17,18)),"exact current geometry transport inputs/joins");
    }
    private static HashSet<Prepare.MethodId> reach(Set<Prepare.MethodId> roots,Map<Prepare.MethodId,Set<Prepare.MethodId>> edges,Set<Prepare.MethodId> methods,boolean stop){
        var seen=new HashSet<Prepare.MethodId>();var pending=new ArrayDeque<>(roots);
        while(!pending.isEmpty()){var id=pending.removeFirst();if(stop&&id.equals(FRONTIER)||!methods.contains(id)||!seen.add(id))continue;pending.addAll(edges.getOrDefault(id,Set.of()));}
        return seen;
    }
    private static Prepare.MethodId run(Prepare data,String owner){
        var seen=new HashSet<String>();while(seen.add(owner)){
            var id=new Prepare.MethodId(owner,"run","()V");if(data.methods.containsKey(id))return id;var model=data.models.get(owner);
            if(model==null||model.superclass().isEmpty())return null;owner=model.superclass().orElseThrow().asInternalName();
        }return null;
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native transport partition "+why);}
}
