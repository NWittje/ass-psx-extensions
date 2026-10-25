package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Data-only asynchronous coverage discovery across the installed class graph.
 * Direct field writers are only seeds. Virtual native overrides, external CDU
 * jobs and their callers are included before a start/run profile is selected.
 * No candidate is automatically instrumented or called a finite native job. */
public final class FmcAsyncAudit {
    record Run(Prepare.MethodId method,boolean writer,boolean reader,boolean thread,boolean runnable,int sleeps,int backwardBranches){}
    record Start(Prepare.MethodId caller,int count,List<String> constructedNativeJobs){}
    record Result(List<Run> runs,List<Start> starts,List<Prepare.MethodId> otherScheduling,int writerMethods,int readerMethods){}
    private FmcAsyncAudit(){}
    private static Set<String> ancestry(Prepare data,String owner,Map<String,Set<String>> cache,Set<String> active){
        Set<String> known=cache.get(owner);if(known!=null)return known;if(!active.add(owner))throw new IllegalArgumentException("FMC native hierarchy cycle");
        var result=new HashSet<String>();result.add(owner);var model=data.models.get(owner);
        if(model!=null){for(var iface:model.interfaces())result.addAll(ancestry(data,iface.asInternalName(),cache,active));model.superclass().ifPresent(parent->result.addAll(ancestry(data,parent.asInternalName(),cache,active)));}
        active.remove(owner);var complete=Set.copyOf(result);cache.put(owner,complete);return complete;
    }
    private static Prepare.MethodId resolve(Prepare data,String owner,String name,String descriptor,Set<String> visited){
        if(!visited.add(owner))return null;var id=new Prepare.MethodId(owner,name,descriptor);if(data.methods.containsKey(id))return id;
        var model=data.models.get(owner);if(model==null||name.equals("<init>"))return null;
        if(model.superclass().isPresent()){var found=resolve(data,model.superclass().get().asInternalName(),name,descriptor,visited);if(found!=null)return found;}
        for(var iface:model.interfaces()){var found=resolve(data,iface.asInternalName(),name,descriptor,visited);if(found!=null)return found;}return null;
    }
    static Result inspect(Prepare data,FmcQualification.Inventory inventory){
        var hierarchy=new HashMap<String,Set<String>>();for(String owner:data.models.keySet())ancestry(data,owner,hierarchy,new HashSet<>());
        var overrides=new HashMap<String,Map<String,List<Prepare.MethodId>>>();
        for(var entry:data.methods.entrySet())if(!entry.getKey().name().startsWith("<")&&(entry.getValue().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE))==0){
            var id=entry.getKey();String signature=id.name()+id.descriptor();
            for(String parent:hierarchy.get(id.owner()))overrides.computeIfAbsent(parent,k->new HashMap<>()).computeIfAbsent(signature,k->new ArrayList<>()).add(id);
        }
        var callers=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent())for(var instruction:Prepare.instructions(entry.getValue()))if(instruction instanceof InvokeInstruction call){
            String owner=call.owner().asInternalName(),name=call.name().stringValue(),descriptor=call.type().stringValue();
            var target=resolve(data,owner,name,descriptor,new HashSet<>());if(target!=null)callers.computeIfAbsent(target,k->new HashSet<>()).add(entry.getKey());
            if(call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE)
                for(var override:overrides.getOrDefault(owner,Map.of()).getOrDefault(name+descriptor,List.of()))callers.computeIfAbsent(override,k->new HashSet<>()).add(entry.getKey());
        }
        var writeSeeds=new HashSet<Prepare.MethodId>();var readSeeds=new HashSet<Prepare.MethodId>();
        for(var accesses:inventory.accesses().values())for(var access:accesses){
            if(access.opcode()==Opcode.PUTFIELD||access.opcode()==Opcode.PUTSTATIC)writeSeeds.add(access.method());else readSeeds.add(access.method());
        }
        Set<Prepare.MethodId> writers=closure(writeSeeds,callers),readers=closure(readSeeds,callers);
        var runs=new ArrayList<Run>();var jobs=new HashSet<String>();var scheduling=new TreeSet<Prepare.MethodId>(Comparator.comparing(Prepare.MethodId::toString));
        for(var entry:data.methods.entrySet()){
            var id=entry.getKey();Set<String> parents=hierarchy.get(id.owner());boolean thread=parents.contains("java/lang/Thread"),runnable=parents.contains("java/lang/Runnable");
            if(!id.name().equals("run")||!id.descriptor().equals("()V")||(!thread&&!runnable)||entry.getValue().code().isEmpty()||(!writers.contains(id)&&!readers.contains(id)))continue;
            jobs.add(id.owner());int sleeps=0,back=0;var labels=new HashSet<Label>();
            for(var element:entry.getValue().code().orElseThrow()){
                if(element instanceof LabelTarget target)labels.add(target.label());
                if(!(element instanceof Instruction instruction))continue;
                if(instruction instanceof InvokeInstruction call&&call.owner().asInternalName().equals("java/lang/Thread")&&call.name().equalsString("sleep"))sleeps++;
                if(instruction instanceof BranchInstruction branch&&labels.contains(branch.target()))back++;
            }
            runs.add(new Run(id,writers.contains(id),readers.contains(id),thread,runnable,sleeps,back));
        }
        var runBodies=new HashSet<Prepare.MethodId>();for(var run:runs)runBodies.add(run.method);
        for(var owner:data.models.keySet()){
            Set<String> parents=hierarchy.get(owner);if(!parents.contains("java/lang/Thread")&&!parents.contains("java/lang/Runnable"))continue;
            var inherited=resolve(data,owner,"run","()V",new HashSet<>());if(inherited!=null&&runBodies.contains(inherited))jobs.add(owner);
        }
        var starts=new ArrayList<Start>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            int count=0;var constructed=new TreeSet<String>();boolean other=false;
            for(var instruction:Prepare.instructions(entry.getValue())){
                if(instruction instanceof NewObjectInstruction allocation&&jobs.contains(allocation.className().asInternalName()))constructed.add(allocation.className().asInternalName());
                if(instruction instanceof InvokeInstruction call){
                    String owner=call.owner().asInternalName(),name=call.name().stringValue();
                    if(name.equals("start")&&call.type().equalsString("()V")&&hierarchy.getOrDefault(owner,Set.of(owner)).contains("java/lang/Thread"))count++;
                    if((owner.startsWith("java/util/concurrent/")&&Set.of("execute","submit","schedule","scheduleAtFixedRate","scheduleWithFixedDelay").contains(name))||
                        (owner.equals("java/util/Timer")&&name.startsWith("schedule"))||
                        (owner.equals("java/awt/EventQueue")&&Set.of("invokeLater","invokeAndWait").contains(name)))other=true;
                }
            }
            // Keep all native start sites as discovery, including unknown or
            // factory-supplied Runnable shapes. The completed profile must
            // account for aliases/dispatch; allocation proximity is no proof.
            if(count>0)starts.add(new Start(entry.getKey(),count,List.copyOf(constructed)));
            // Passing a Runnable is not a synchronous call edge. Keep every
            // alternate scheduling site until its task/alias ownership is
            // classified; a caller with no direct field access may still queue
            // a later FMC writer.
            if(other)scheduling.add(entry.getKey());
        }
        runs.sort(Comparator.comparing(run->run.method.toString()));starts.sort(Comparator.comparing(start->start.caller.toString()));
        return new Result(List.copyOf(runs),List.copyOf(starts),List.copyOf(scheduling),writers.size(),readers.size());
    }
    private static Set<Prepare.MethodId> closure(Set<Prepare.MethodId> seeds,Map<Prepare.MethodId,Set<Prepare.MethodId>> callers){
        var reached=new HashSet<>(seeds);var pending=new ArrayDeque<>(seeds);
        while(!pending.isEmpty())for(var caller:callers.getOrDefault(pending.removeFirst(),Set.of()))if(reached.add(caller))pending.addLast(caller);return Set.copyOf(reached);
    }
    static String report(Result result){
        var b=new StringBuilder("{\"schema\":1,\"state\":\"discovery_only\",\"captureQualified\":false,\"recallQualified\":false,\"writerReachableMethods\":").append(result.writerMethods).append(",\"readerReachableMethods\":").append(result.readerMethods).append(",\"runs\":[");boolean comma=false;
        for(var run:result.runs){if(comma)b.append(',');comma=true;b.append("{\"method\":").append(FmcQualification.quote(run.method.toString())).append(",\"writer\":").append(run.writer).append(",\"reader\":").append(run.reader).append(",\"thread\":").append(run.thread).append(",\"runnable\":").append(run.runnable).append(",\"sleeps\":").append(run.sleeps).append(",\"backwardBranches\":").append(run.backwardBranches).append('}');}
        b.append("],\"starts\":[");comma=false;
        for(var start:result.starts){if(comma)b.append(',');comma=true;b.append("{\"caller\":").append(FmcQualification.quote(start.caller.toString())).append(",\"count\":").append(start.count).append(",\"constructedCandidates\":[");boolean second=false;for(String name:start.constructedNativeJobs){if(second)b.append(',');second=true;b.append(FmcQualification.quote(name));}b.append("]}");}
        b.append("],\"otherScheduling\":[");comma=false;for(var id:result.otherScheduling){if(comma)b.append(',');comma=true;b.append(FmcQualification.quote(id.toString()));}return b.append("]}").toString();
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=2)throw new IllegalArgumentException("installed JAR and owned output required");Path jar=Path.of(args[0]);String hash=Identity.hash(Files.readAllBytes(jar));
        if(!hash.equalsIgnoreCase(FmcQualification.INSTALLED_JAR))throw new IllegalArgumentException("installed FMC JAR identity differs");
        var data=new Prepare(Prepare.readJar(jar));var result=inspect(data,FmcQualification.inventory(data));Files.writeString(Path.of(args[1]),report(result),StandardOpenOption.CREATE_NEW);
        System.out.println("FMC asynchronous discovery only: "+result.runs.size()+" run bodies, "+result.starts.size()+" start callers; no native capture/recall capability");
    }
}
