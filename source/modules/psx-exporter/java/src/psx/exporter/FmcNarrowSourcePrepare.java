package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;

/** Transactional read-only source preparation. Discovery is checked against
 * installed entered-field semantics, pure constructors and fixed-array proof.
 * Buffer receiver-flow is proved before getters expose references to workers.
 * Native execution never waits; writer witnesses only reject concurrent copies.
 * Runtime initialization, lifecycle/source binding and service admission are
 * separate prerequisites; this component cannot advertise a JNI capability. */
public final class FmcNarrowSourcePrepare {
    record Qualified(Map<String,byte[]> changed,Set<Prepare.MethodId> writers,
        Set<Prepare.MethodId> initializedConstructors,int bufferSites){}
    private FmcNarrowSourcePrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC entered source preparation "+why);}
    private static Prepare.MethodId id(String owner,MethodModel method){return new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());}
    private static byte[] body(Prepare data,String owner,MethodModel method){return data.cf.build(ClassDesc.of(owner.replace('/','.')),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));}
    private static boolean matches(Prepare data,Prepare.MethodId method,byte[] actual,Map<String,byte[]> qualified){
        byte[] bytes=qualified.get(method.owner());if(bytes==null)return false;
        var expected=data.cf.parse(bytes).methods().stream().filter(m->id(method.owner(),m).equals(method)).findFirst().orElseThrow();
        return Arrays.equals(body(data,method.owner(),expected),actual);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower)throws Exception {
        var members=FmcNarrowMembersPrepare.qualify(data,lower,true);
        var bufferProof=FmcBufferFlowAudit.audit(data,lower);
        var fresh=FmcFreshSelectionPrepare.transform(data,lower);
        // Constructors with entered writes operate only on fresh, private
        // storage. Static initializers precede the strong VM-initialized roster.
        // None is wrapped using an uninitialized-this receiver.
        // Reuse the exact reachable constructor proof. Owner membership alone
        // must never admit an overload that the fresh factory did not qualify.
        var pure=fresh.initializedConstructors();
        require(fresh.constructors()==20&&pure.size()==20,"complete pure constructor family");
        var staticInitializers=Set.of(new Prepare.MethodId("FmcRte","<clinit>","()V"),new Prepare.MethodId("Routes","<clinit>","()V"),new Prepare.MethodId("FmcPerf","<clinit>","()V"));
        var writers=new TreeSet<Prepare.MethodId>(Comparator.comparing(Prepare.MethodId::toString));var initialized=new HashSet<Prepare.MethodId>();
        var models=FmcQualification.finalModels(data,lower);
        // Earlier pose/environment layers witness lifecycle/configuration edits
        // in some native FMC writers. Admit only exact regenerated bodies; an
        // arbitrary changed writer remains a qualification failure. Reusing a
        // name prefix or trusting every supplied lower method would hide drift.
        // Most source writers are unchanged. Reconstruct a qualified lower
        // body only for an actual mismatch, parse only that owner, and reuse
        // the completed pose qualification on the ordinary preparation path.
        Prepare pose=data;EnvironmentPrepare environment=null;
        Map<String,byte[]> poseBytes=null,environmentBytes=null,combinedBytes=null;
        for(var method:members.writers().values().stream().flatMap(Set::stream).distinct().toList()){
            var original=data.methods.get(method);var current=models.get(method.owner()).methods().stream().filter(m->id(method.owner(),m).equals(method)).findFirst().orElseThrow();
            require(original!=null&&original.code().isPresent()&&current.code().isPresent(),"missing native writer body "+method);
            byte[] actual=body(data,method.owner(),current);
            boolean exact=Arrays.equals(body(data,method.owner(),original),actual);
            if(!exact){
                if(pose.guards.isEmpty()){pose=new Prepare(data.inputs);pose.qualify();}
                if(pose.guards.contains(method)){
                    if(poseBytes==null)poseBytes=pose.transform();
                    exact=matches(data,method,actual,poseBytes);
                }
                if(!exact){
                    if(environment==null){environment=new EnvironmentPrepare(data);environment.qualify();}
                    if(environment.guards.contains(method)){
                        if(environmentBytes==null)environmentBytes=environment.transform(Map.of());
                        exact=matches(data,method,actual,environmentBytes);
                        if(!exact&&pose.guards.contains(method)){
                            if(poseBytes==null)poseBytes=pose.transform();
                            if(combinedBytes==null)combinedBytes=environment.transform(poseBytes);
                            exact=matches(data,method,actual,combinedBytes);
                        }
                    }
                }
            }
            require(exact,"native writer body drift "+method);
            if(method.name().startsWith("<")){require(pure.contains(method)||staticInitializers.contains(method),"unqualified initializer publication");initialized.add(method);}else writers.add(method);
        }
        require(!writers.isEmpty(),"ordinary writer coverage");
        var memberClasses=new HashMap<>(lower);memberClasses.putAll(members.changed());
        var wrapped=FmcBufferMutatorPrepare.transform(data,lower,bufferProof);var methods=new HashSet<Prepare.MethodId>();
        for(var site:bufferProof.mutations())if(!FmcBufferFlowAudit.paddingHelper(site.method()))methods.add(site.method());
        // Compose already-qualified wrapper methods with the member switches.
        // Original methods must still be exact; no general prefix exemption or
        // re-analysis of a getter's deliberate worker-only escape is used.
        for(String owner:methods.stream().map(Prepare.MethodId::owner).collect(java.util.stream.Collectors.toSet())){
            var source=data.cf.parse(memberClasses.get(owner));var bufferModel=data.cf.parse(wrapped.get(owner));var replacements=new HashMap<Prepare.MethodId,MethodModel>();var additions=new ArrayList<MethodModel>();
            for(var method:bufferModel.methods()){
                if(methods.contains(id(owner,method)))replacements.put(id(owner,method),method);
                if(method.methodName().stringValue().startsWith("$psxExporter$fmcBufferMutation"))additions.add(method);
            }
            require(replacements.size()==methods.stream().filter(m->m.owner().equals(owner)).count()&&!additions.isEmpty(),"complete buffer wrapper composition");
            for(var method:source.methods())if(replacements.containsKey(id(owner,method)))require(Arrays.equals(body(data,owner,data.methods.get(id(owner,method))),body(data,owner,method)),"buffer source body drift");
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){if(element instanceof MethodModel method&&replacements.containsKey(id(owner,method)))builder.with(replacements.get(id(owner,method)));else builder.with(element);}
                public void atEnd(ClassBuilder builder){for(var method:additions)builder.with(method);}
            });require(data.cf.verify(output).isEmpty(),"composed buffer bytecode");memberClasses.put(owner,output);
        }
        var result=new HashMap<>(memberClasses);result.putAll(FmcWriterPrepare.transform(data,memberClasses,writers));
        TerrainAccess.verifyAdded(lower,result,data.inputs);
        for(var entry:result.entrySet())require(data.cf.verify(entry.getValue()).isEmpty(),"final source bytecode");
        return new Qualified(Map.copyOf(result),Set.copyOf(writers),Set.copyOf(initialized),bufferProof.mutations().size());
    }
    public static void main(String[] args)throws Exception {
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");var result=qualify(new Prepare(Prepare.readJar(jar)),Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"read_only_source_preparation_component\",\"captureQualified\":false,\"recallQualified\":false,\"members\":66,\"ordinaryWriters\":"+result.writers().size()+",\"initializedWriters\":"+result.initializedConstructors().size()+",\"bufferSites\":"+result.bufferSites()+",\"owners\":"+result.changed().size()+",\"limit\":\"Read-only switches plus complete entered-field/buffer witnesses only. Runtime initialized-lifecycle binding, source JNI/service and full recall remain unqualified. No installed class definition/initialization/invocation.\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("Read-only entered source component prepared; native capture/recall unavailable");
    }
}
