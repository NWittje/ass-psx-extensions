package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;
import java.nio.file.*;

/** One data-only coverage model for recall readers, writers, native jobs and
 * command authority. A discovered writer is never automatically a deferrable
 * callback. Profiles must seal actual input-retention/error/order proofs at
 * frontiers before returning a complete optional transformation.
 * Runtime capture remains independent of a rejected recall proof. */
public final class FmcRecallBoundary {
    enum Kind { RETRYABLE_POLL, RETAINED_JOB, COMMAND_SEAM }
    record Frontier(Prepare.MethodId method,Kind kind,String nativeClassHash,
        boolean beforeInputConsumption,boolean preservesInputAndOrdering,
        boolean originalExceptions,boolean readerPublicationExcluded){}
    record Evidence(Set<Prepare.MethodId> readers,Set<Prepare.MethodId> writers,
        Set<Prepare.MethodId> nonvoidWriters,Map<Prepare.MethodId,Set<Prepare.MethodId>> callers,
        List<FmcAsyncAudit.Run> jobs,List<FmcAsyncAudit.Start> starts,List<Prepare.MethodId> scheduling){}
    record Coverage(Set<Prepare.MethodId> unresolved,Set<Prepare.MethodId> frontiers){
        // This is FIELD-path coverage only. Bank freshness/receiver aliases,
        // array/buffer stores, scheduler lifetime and reflection need their
        // separately sealed installed proofs before recall can qualify.
        boolean fieldPathsCovered(){return unresolved.isEmpty()&&!frontiers.isEmpty();}
    }
    record InputRead(Prepare.MethodId method,int instruction,String receiverOrigin){}
    private FmcRecallBoundary(){}
    /** A retained Thread does not freeze Qs contents. This narrow discovery
     * identifies actual native read-mode calls without invoking transfer,
     * consuming freshness or assuming that a nearby bank index is its receiver.
     * Transitive/receiver retention is an additional requirement, not implied
     * by absence from this direct-call list. */
    static List<InputRead> inputReads(Prepare data,Collection<Prepare.MethodId> scope){
        var found=new ArrayList<InputRead>();
        for(var id:scope){
            var method=data.methods.get(id);require(method!=null,"input body missing "+id);
            if(method.code().isEmpty())continue;
            var ops=Prepare.instructions(method);
            for(int at=3;at<ops.size();at++)if(Prepare.call(ops.get(at),"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")
                &&ops.get(at-2) instanceof ConstantInstruction mode&&Integer.valueOf('r').equals(mode.constantValue())
                &&ops.get(at-1) instanceof ConstantInstruction origin&&Integer.valueOf(0).equals(origin.constantValue())){
                String receiver="unresolved native Qs receiver";
                // Exact LEGS expression uses the job's cdu field and its
                // scratchpad index. Do not attribute other calls by proximity.
                if(at>=9&&ops.get(at-3).opcode()==Opcode.ACONST_NULL&&ops.get(at-4).opcode()==Opcode.AALOAD
                    &&ops.get(at-5) instanceof FieldInstruction index&&index.opcode()==Opcode.GETFIELD
                    &&index.owner().asInternalName().equals("ControlDisplayUnit")&&index.name().equalsString("globScrPadIdx")&&index.type().equalsString("I")
                    &&ops.get(at-6) instanceof FieldInstruction cdu&&cdu.opcode()==Opcode.GETFIELD&&cdu.type().equalsString("LControlDisplayUnit;")
                    &&ops.get(at-7) instanceof LoadInstruction object&&object.typeKind()==TypeKind.REFERENCE&&object.slot()==0
                    &&ops.get(at-8) instanceof FieldInstruction bank&&bank.opcode()==Opcode.GETSTATIC
                    &&bank.owner().asInternalName().equals("Glob")&&bank.name().equalsString("qs")&&bank.type().equalsString("[LQs;"))
                    receiver="live Glob.qs[this.cdu.globScrPadIdx]; read consumes Q freshness";
                found.add(new InputRead(id,at,receiver));
            }
        }
        found.sort(Comparator.comparing(r->r.method.toString()+":"+r.instruction));return List.copyOf(found);
    }
    /** Follow installed dispatch edges, including native virtual overrides.
     * An input in a callee is as live as an input in run(). Absence here proves
     * only this class-data call graph; asynchronous scheduling, aliases and
     * platform/reflection entry points still need their own sealed profile. */
    static List<InputRead> reachableInputs(Prepare data,Collection<Prepare.MethodId> roots){
        var evidence=inspect(data,Set.of());
        var callees=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        evidence.callers.forEach((target,parents)->{
            for(var parent:parents)callees.computeIfAbsent(parent,k->new HashSet<>()).add(target);
        });
        var reached=new HashSet<Prepare.MethodId>();var pending=new ArrayDeque<>(roots);
        while(!pending.isEmpty()){
            var method=pending.removeFirst();require(data.methods.containsKey(method),"native input root missing "+method);
            if(reached.add(method))pending.addAll(callees.getOrDefault(method,Set.of()));
        }
        var inputs=new ArrayList<>(inputReads(data,reached));
        for(var method:reached){
            if(data.methods.get(method).code().isEmpty())continue;
            var ops=Prepare.instructions(data.methods.get(method));
            for(int at=0;at<ops.size();at++){
                var op=ops.get(at);String reason=null;
                if(op instanceof InvokeInstruction call){
                    String owner=call.owner().asInternalName(),name=call.name().stringValue();
                    if(Set.of("Q","Qi","Qs","Qh").contains(owner)&&Set.of("transfer","isUpdated").contains(name))
                        reason="native bank call requires receiver/mode/freshness retention proof";
                }else if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)
                    &&Set.of("Q","Qi","Qs","Qh").contains(f.owner().asInternalName()))
                    reason="direct native bank field read requires receiver/coherence proof";
                final int ordinal=at;
                if(reason!=null&&inputs.stream().noneMatch(r->r.method.equals(method)&&r.instruction==ordinal))
                    inputs.add(new InputRead(method,at,reason));
            }
        }
        inputs.sort(Comparator.comparing(r->r.method.toString()+":"+r.instruction));return List.copyOf(inputs);
    }
    static Evidence inspect(Prepare data,Set<FmcQualification.Member> selected){
        var fields=new TreeMap<FmcQualification.Member,List<FmcQualification.Access>>();
        for(var f:selected)fields.put(f,new ArrayList<>());
        var reads=new HashSet<Prepare.MethodId>();var writes=new HashSet<Prepare.MethodId>();
        var callers=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        var ancestry=new HashMap<String,Set<String>>();
        var overrides=new HashMap<String,Map<String,Set<Prepare.MethodId>>>();
        for(var entry:data.methods.entrySet()){
            var id=entry.getKey();if(id.name().startsWith("<")||(entry.getValue().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE))!=0)continue;
            for(String parent:ancestry(data,id.owner(),ancestry,new HashSet<>()))
                overrides.computeIfAbsent(parent,k->new HashMap<>()).computeIfAbsent(id.name()+id.descriptor(),k->new HashSet<>()).add(id);
        }
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent())
            for(var ins:Prepare.instructions(entry.getValue())){
                if(ins instanceof FieldInstruction f){
                    var resolved=FmcQualification.resolve(data,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                    if(resolved!=null){fields.get(resolved).add(new FmcQualification.Access(entry.getKey(),f.opcode()));
                        if(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)writes.add(entry.getKey());else reads.add(entry.getKey());}
                }
                if(ins instanceof InvokeInstruction call){
                    var target=resolve(data,call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue(),new HashSet<>());
                    if(target!=null)callers.computeIfAbsent(target,k->new HashSet<>()).add(entry.getKey());
                    // An installed subtype can be selected through a symbolic
                    // superclass/interface. Keep every matching override until
                    // receiver-flow proves a narrower dispatch set.
                    if(call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE)
                        for(var candidate:overrides.getOrDefault(call.owner().asInternalName(),Map.of()).getOrDefault(call.name().stringValue()+call.type().stringValue(),Set.of()))
                            callers.computeIfAbsent(candidate,k->new HashSet<>()).add(entry.getKey());
                }
            }
        var inventory=new FmcQualification.Inventory(Map.of(),fields,List.copyOf(selected),Map.of(),Map.of(),Map.of(),Set.of(),Set.of(),List.of());
        var async=FmcAsyncAudit.inspect(data,inventory);var nonvoid=new HashSet<Prepare.MethodId>();
        for(var writer:writes)if(!writer.name().startsWith("<")&&!writer.descriptor().endsWith(")V"))nonvoid.add(writer);
        var frozen=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();callers.forEach((k,v)->frozen.put(k,Set.copyOf(v)));
        return new Evidence(Set.copyOf(reads),Set.copyOf(writes),Set.copyOf(nonvoid),Map.copyOf(frozen),async.runs(),async.starts(),async.otherScheduling());
    }
    /** Every path back from an owned reader/writer must cross a sealed frontier.
     * Static initialization is separately discharged by VM initialized-roster
     * evidence. Constructors require an explicit private-factory proof; they
     * are not exempted by name. Cycles are retained unless they can reach a cut.
     * Reflection/platform dispatch remain additional explicit obligations. */
    static Coverage cover(Prepare data,Evidence evidence,List<Frontier> sealed,Set<Prepare.MethodId> privateInitializers){
        var cuts=new HashSet<Prepare.MethodId>();
        for(var frontier:sealed){
            var method=data.methods.get(frontier.method);
            require(method!=null&&method.code().isPresent(),"missing frontier "+frontier.method);
            require(frontier.beforeInputConsumption&&frontier.preservesInputAndOrdering&&frontier.originalExceptions&&frontier.readerPublicationExcluded,"incomplete frontier proof "+frontier.method);
            try{require(Identity.hash(data.inputs.get(frontier.method.owner())).equalsIgnoreCase(frontier.nativeClassHash),"frontier preimage drift");}
            catch(Exception failure){throw new IllegalArgumentException("FMC frontier identity",failure);}
            if(frontier.kind==Kind.RETRYABLE_POLL)require(frontier.method.descriptor().endsWith(")V")&&!frontier.method.name().startsWith("<"),"nonvoid/native initializer cannot be skipped");
            require(cuts.add(frontier.method),"duplicate frontier");
        }
        var reached=new HashSet<Prepare.MethodId>();var pending=new ArrayDeque<Prepare.MethodId>();
        pending.addAll(evidence.readers);pending.addAll(evidence.writers);
        var unresolved=new HashSet<Prepare.MethodId>();var traversed=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        while(!pending.isEmpty()){
            var method=pending.removeFirst();if(!reached.add(method)||cuts.contains(method)||privateInitializers.contains(method))continue;
            if(method.name().equals("<clinit>"))continue;
            var parents=evidence.callers.getOrDefault(method,Set.of());traversed.put(method,parents);
            if(parents.isEmpty())unresolved.add(method);else pending.addAll(parents);
            var flags=data.methods.get(method).flags().flagsMask();
            // Public platform callbacks and externally callable methods need
            // their own qualified admission, even if native direct callers exist.
            if((flags&(ClassFile.ACC_PUBLIC|ClassFile.ACC_PROTECTED))!=0||method.name().equals("run")||method.name().equals("paint"))unresolved.add(method);
        }
        // Every external root found above must be sealed. A private recursive
        // component reached solely from sealed entries is covered; requiring
        // every parent to be discharged first would reject all such cycles.
        // An isolated caller cycle still has no qualified entry and stays open.
        var discharged=new HashSet<>(cuts);discharged.addAll(privateInitializers);
        for(var method:reached)if(method.name().equals("<clinit>"))discharged.add(method);
        boolean changed;
        do{changed=false;for(var entry:traversed.entrySet())if(entry.getValue().stream().anyMatch(discharged::contains)&&discharged.add(entry.getKey()))changed=true;}while(changed);
        for(var method:reached)if(!discharged.contains(method))unresolved.add(method);
        return new Coverage(Set.copyOf(unresolved),Set.copyOf(cuts));
    }
    private static Prepare.MethodId resolve(Prepare data,String owner,String name,String descriptor,Set<String> visited){
        if(!visited.add(owner))return null;var id=new Prepare.MethodId(owner,name,descriptor);if(data.methods.containsKey(id))return id;
        var model=data.models.get(owner);if(model==null||name.equals("<init>"))return null;
        if(model.superclass().isPresent()){var found=resolve(data,model.superclass().get().asInternalName(),name,descriptor,visited);if(found!=null)return found;}
        for(var parent:model.interfaces()){var found=resolve(data,parent.asInternalName(),name,descriptor,visited);if(found!=null)return found;}return null;
    }
    private static Set<String> ancestry(Prepare data,String owner,Map<String,Set<String>> cache,Set<String> active){
        var known=cache.get(owner);if(known!=null)return known;
        require(active.add(owner),"hierarchy cycle");var result=new HashSet<String>();result.add(owner);var model=data.models.get(owner);
        if(model!=null){
            if(model.superclass().isPresent())result.addAll(ancestry(data,model.superclass().get().asInternalName(),cache,active));
            for(var parent:model.interfaces())result.addAll(ancestry(data,parent.asInternalName(),cache,active));
        }
        active.remove(owner);var complete=Set.copyOf(result);cache.put(owner,complete);return complete;
    }
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException("FMC recall boundary "+why);}
    /** Explicit installed-data diagnostic for the same selected profile used
     * by tests. No empty/Boolean-only cut is converted into a runtime permit;
     * a report is neither a preparation overlay nor capability advertisement. */
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report required");Path jar=Path.of(arguments[0]);
        String hash=Identity.hash(jar);require(hash.equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));FmcRecallProfile.qualifyRoles(data);
        // Qualify the real installed launchers and guarded operation-owned job
        // bridges as data too; authored execution does not prove their shape.
        var execBridges=FmcExecPrepare.transform(data,Map.of());
        var nativeExec=new HashMap<>(execBridges);
        nativeExec.putAll(FmcJobPrepare.quiet(data,execBridges,List.of(
            new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte",FmcExecPrepare.LEFT,"(J)Ljava/lang/Thread;"),1),
            new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte",FmcExecPrepare.RIGHT,"(J)Ljava/lang/Thread;"),1),
            new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte","execL","()V"),1),
            new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte","execR","()V"),1)),
            Set.of(new Prepare.MethodId("FmcRte$ExecL","run","()V"),new Prepare.MethodId("FmcRte$ExecR","run","()V"))));
        var selected=new HashSet<FmcQualification.Member>();
        for(var field:FmcRecallProfile.fields())selected.add(new FmcQualification.Member(field.owner(),field.name(),field.descriptor()));
        var evidence=inspect(data,selected);
        var inputs=inputReads(data,List.of(new Prepare.MethodId("CduPgLegsL$ThreadL","run","()V"),new Prepare.MethodId("CduPgLegsR$ThreadL","run","()V")));
        var reachable=reachableInputs(data,List.of(new Prepare.MethodId("CduPgLegsL$ThreadL","run","()V"),new Prepare.MethodId("CduPgLegsR$ThreadL","run","()V")));
        require(inputs.size()==2&&inputs.stream().allMatch(r->r.receiverOrigin.startsWith("live Glob.qs")),"installed LEGS input receiver qualification");
        var out=new StringBuilder("{\"schema\":1,\"state\":\"installed_recall_frontier_evidence\",\"recallQualified\":false,\"vendorClassesDefined\":false,\"jarSha256\":")
            .append(FmcQualification.quote(hash)).append(",\"inputContract\":").append(FmcQualification.quote(FmcRecallProfile.INPUT_CONTRACT))
            .append(",\"roleFields\":").append(FmcRecallProfile.fields().size())
            .append(",\"uniqueFields\":").append(selected.size()).append(",\"readers\":").append(evidence.readers.size())
            .append(",\"writers\":").append(evidence.writers.size()).append(",\"nonvoidWriters\":[");boolean comma=false;
        for(var id:evidence.nonvoidWriters.stream().sorted(Comparator.comparing(Prepare.MethodId::toString)).toList()){
            if(comma)out.append(',');comma=true;out.append(FmcQualification.quote(id.toString()));
        }
        out.append("],\"liveInputReads\":[");comma=false;
        for(var read:inputs){if(comma)out.append(',');comma=true;out.append("{\"method\":").append(FmcQualification.quote(read.method.toString()))
            .append(",\"instruction\":").append(read.instruction).append(",\"receiver\":").append(FmcQualification.quote(read.receiverOrigin))
            .append(",\"classSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(read.method.owner())))).append('}');}
        out.append("],\"reachableNativeInputSites\":").append(reachable.size()).append(",\"nativeExecClasses\":[");comma=false;
        for(String owner:List.of("FmcRte","FmcRte$ExecL","FmcRte$ExecR")){
            if(comma)out.append(',');comma=true;out.append("{\"owner\":").append(FmcQualification.quote(owner))
                .append(",\"originalSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(owner))))
                .append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(nativeExec.get(owner)))).append('}');
        }
        out.append("],\"limitation\":\"Selected contract excludes scratchpad save/restore/preservation and requires no external edits during restore. Composed quiet-input native EXEC job hooks qualify as class data. Complete background reader/writer/alias admission and reconstruction/binding recipe remain required. No native mutation or capability advertisement.\"}\n");
        Files.writeString(Path.of(arguments[1]),out,StandardOpenOption.CREATE_NEW);
        System.out.println("Installed recall frontier evidence: "+selected.size()+" fields; "+evidence.nonvoidWriters.size()+" nonvoid writers; "+inputs.size()+" qualified live scratchpad reads; recall unavailable");
    }
}
