package psx.exporter;

import java.util.*;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;

/** Data-only normal recurrent path check. Cut semantics are qualified by the
 * corresponding production transforms; this audit reports residual native
 * frame paths rather than mistaking generic field witnesses for deferral. */
final class FmcBackgroundAdmissionClosure {
    private FmcBackgroundAdmissionClosure(){}
    private static final Map<Prepare,Map<String,String>> PAGES=Collections.synchronizedMap(new WeakHashMap<>());
    private static final Map<Prepare,Boolean> PURE_PAUSE=Collections.synchronizedMap(new WeakHashMap<>());
    private record Path(List<Prepare.MethodId> methods,Set<Integer> sites,boolean receiverRequired){}
    private record State(Prepare.MethodId method,Set<Integer> sites,boolean receiverRequired){}
    private record PrivateAccess(Prepare.MethodId method,FmcQualification.Member field){}
    private record CallEdge(Prepare.MethodId parent,Prepare.MethodId target){}
    private record Signature(String name,String descriptor){}
    private record RoleEdge(Prepare.MethodId parent,int ordinal,Prepare.MethodId target,Set<Integer> substantive){}
    static List<String> framePaths(Prepare data,FmcBackgroundAdmissionProfile.Plan plan){
        var roots=new HashSet<Prepare.MethodId>();for(var id:plan.persistent())if(id.owner().startsWith("Core$"))roots.add(id);
        return paths(data,plan,roots);
    }
    static List<String> observerPaths(Prepare data,FmcBackgroundAdmissionProfile.Plan plan){
        var profile=FmcBackgroundAdmissionObservers.inspect(data);var roots=new HashSet<>(profile.roots().keySet());roots.removeAll(profile.quietInputs());return paths(data,plan,roots);
    }
    private static List<String> paths(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<Prepare.MethodId> roots){
        return paths(data,plan,roots,false,false);
    }
    /** Old finite jobs must retain the whole original lifetime, including a
     * call already past a native Pause predicate. Only exact independent
     * receiver/value projections may remove an unrelated job; new-call poll
     * cuts and paused CFG folding provide no lifetime evidence here. */
    static Set<Prepare.MethodId> relevantJobs(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<Prepare.MethodId> candidates){
        return relevantJobs(data,plan,candidates,false);
    }
    static Set<Prepare.MethodId> relevantWriters(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<Prepare.MethodId> candidates){
        return relevantJobs(data,plan,candidates,true);
    }
    private static Set<Prepare.MethodId> relevantJobs(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<Prepare.MethodId> candidates,boolean writers){
        var indexed=new HashMap<String,Prepare.MethodId>();for(var id:candidates)indexed.put(id.toString(),id);var result=new HashSet<Prepare.MethodId>();
        for(String path:paths(data,plan,candidates,true,writers)){int start=path.lastIndexOf(", ");String root=path.substring(start<0?1:start+2,path.length()-1);var id=indexed.get(root);if(id==null)throw new IllegalArgumentException("FMC job native root attribution");result.add(id);}return Set.copyOf(result);
    }
    private static List<String> paths(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<Prepare.MethodId> roots,boolean jobs,boolean writers){
        var selected=new HashSet<>(plan.fields());selected.removeAll(FmcBackgroundAdmissionProfile.BANK_CONTENT);
        var evidence=FmcRecallBoundary.inspect(data,selected);var cuts=new HashSet<Prepare.MethodId>();for(var poll:plan.polls())if(!plan.receivers().containsKey(poll))cuts.add(poll);cuts.addAll(plan.paused());cuts.addAll(plan.jobs());
        cuts.addAll(FmcBackgroundAdmissionProfile.NETWORK_AGGREGATES);cuts.add(FmcTerrainRoutePrepare.ORIGINAL);
        // Worker binding arms an immutable complete image for every powered
        // native ND root before closing admission. Full draw entry drains old
        // bodies; the original physical prefix and same-owner raster fallback
        // serve new draws even if either optional helper is unavailable.
        if(!jobs&&!plan.ownedStatics().isEmpty()){
            FmcRenderingPrepare.qualify(data);FmcRenderingCapturePrepare.qualify(data);
            for(String name:FmcRenderingPrepare.NAMES)if(!plan.ownedStatics().contains(new FmcQualification.Member("IntegratedDisplaySystem",name,"LNavigationDisplay;")))throw new IllegalArgumentException("FMC complete rendering roots omitted");
            cuts.add(FmcRenderingPrepare.ROOT);
        }
        cuts.addAll(FmcBackgroundAdmissionPrivate.formatters(data));
        if(jobs)cuts.clear();
        var found=new TreeSet<String>();var seeds=new HashSet<Prepare.MethodId>();if(!writers)seeds.addAll(evidence.readers());seeds.addAll(evidence.writers());var calls=new HashMap<Prepare.MethodId,Set<Integer>>();var pages=PAGES.computeIfAbsent(data,k->FmcReconstructionExecScope.pages(k));var dispatch=new FmcBackgroundAdmissionDispatch(data,pages);var privateReceivers=new HashMap<List<Prepare.MethodId>,Boolean>();var projections=projections(data,plan);
        var callArguments=new HashMap<Prepare.MethodId,Map<Integer,List<Integer>>>();var argumentContexts=new HashMap<Prepare.MethodId,Map<Integer,Set<Integer>>>();
        // Dispatch depends only on this immutable preparation/active-method
        // view. Every seed still applies its own substantive-site, argument,
        // negative-role and actual receiver decisions after this exact index.
        var dispatched=new HashMap<CallEdge,Set<Integer>>();
        var invokeIndex=new HashMap<Prepare.MethodId,Map<Signature,Set<Integer>>>();
        var negativeRoles=new HashMap<RoleEdge,Boolean>();
        var privateAccess=new HashMap<PrivateAccess,Boolean>();
        var receiverFlow=new FmcBackgroundAdmissionReceiverFlow(data,plan.ownedStatics(),plan.ownedChildren());var roles=new FmcBackgroundAdmissionRoles(data,plan);
        for(var seed:seeds){
            var original=data.methods.get(seed);var active=calls.computeIfAbsent(seed,k->admittedInstructions(data,k,projections,jobs));var instructions=Prepare.instructions(original);boolean substantive=false;
            for(int at:active)if(instructions.get(at) instanceof FieldInstruction f&&(!writers||f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){var field=FmcQualification.resolve(data,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());if(field!=null&&!privateAccess.computeIfAbsent(new PrivateAccess(seed,field),k->FmcBackgroundAdmissionPrivate.unrelatedAccess(data,k.method(),k.field(),plan.publishedReferences()))){substantive=true;break;}}
            if(!substantive)continue;
            var selectedSites=new HashSet<Integer>();for(int at:active)if(instructions.get(at) instanceof FieldInstruction f&&(!writers||f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){var field=FmcQualification.resolve(data,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());if(field!=null&&!privateAccess.computeIfAbsent(new PrivateAccess(seed,field),k->FmcBackgroundAdmissionPrivate.unrelatedAccess(data,k.method(),k.field(),plan.publishedReferences())))selectedSites.add(at);}
            var visited=new HashSet<State>();var pending=new ArrayDeque<Path>();pending.add(new Path(List.of(seed),Set.copyOf(selectedSites),false));
            while(!pending.isEmpty()){
                var node=pending.removeFirst();var path=node.methods();var id=path.getLast();if(!visited.add(new State(id,node.sites(),node.receiverRequired()))||cuts.contains(id))continue;
                boolean receiverRequired=node.receiverRequired()||!jobs&&plan.polls().contains(id)&&plan.receivers().containsKey(id);
                if(roots.contains(id)){found.add(path.toString());continue;}
                for(var parent:evidence.callers().getOrDefault(id,Set.of()))if(!privateReceivers.computeIfAbsent(List.of(parent,id),key->FmcBackgroundAdmissionPrivate.unrelatedReceiver(data,parent,id,plan.publishedReferences()))){
                    var parentActive=calls.computeIfAbsent(parent,k->admittedInstructions(data,k,projections,jobs));var invoked=new HashSet<Integer>();var receiverInvoked=new HashSet<Integer>();Map<Integer,List<Integer>> arguments=Map.of();
                    int argumentSlot=knownArgumentSlot(id);if(argumentSlot>=0)arguments=callArguments.computeIfAbsent(parent,k->{try{return FmcReconstructionArrayProof.arguments(data,k);}catch(IllegalArgumentException unknown){return Map.of();}});
                    var invocationSites=dispatched.computeIfAbsent(new CallEdge(parent,id),edge->{
                        var index=invokeIndex.computeIfAbsent(parent,method->{var bySignature=new HashMap<Signature,Set<Integer>>();var invokeOps=Prepare.instructions(data.methods.get(method));for(int at=0;at<invokeOps.size();at++)if(invokeOps.get(at) instanceof InvokeInstruction call)bySignature.computeIfAbsent(new Signature(call.name().stringValue(),call.type().stringValue()),k->new HashSet<>()).add(at);return bySignature;});
                        var sites=new HashSet<Integer>();for(int at:index.getOrDefault(new Signature(id.name(),id.descriptor()),Set.of()))if(parentActive.contains(at)&&dispatch.mayCall(parent,id,Set.of(at)))sites.add(at);return Set.copyOf(sites);
                    });
                    for(int at:invocationSites)if(!negativeRoles.computeIfAbsent(new RoleEdge(parent,at,id,node.sites()),edge->roles.unboundCall(edge.parent(),edge.ordinal(),edge.target(),edge.substantive()))){
                        var vector=arguments.get(at);boolean reachable=true;if(argumentSlot>=0&&vector!=null&&!vector.isEmpty()&&vector.getFirst()!=FmcBackgroundAdmissionArgumentFlow.UNKNOWN){var context=argumentContexts.computeIfAbsent(id,k->new HashMap<>()).computeIfAbsent(vector.getFirst(),value->FmcBackgroundAdmissionArgumentFlow.reachable(data.methods.get(id),argumentSlot,value));reachable=node.sites().stream().anyMatch(context::contains);}
                        if(reachable){
                            var origin=receiverRequired?receiverFlow.receiver(parent,at):FmcBackgroundAdmissionReceiverFlow.Origin.UNKNOWN;
                            if(origin==FmcBackgroundAdmissionReceiverFlow.Origin.OWNED)continue;
                            if(origin==FmcBackgroundAdmissionReceiverFlow.Origin.THIS)receiverInvoked.add(at);else invoked.add(at);
                        }
                    }
                    if(!invoked.isEmpty()||!receiverInvoked.isEmpty()){var next=new ArrayList<>(path);next.add(parent);if(!invoked.isEmpty())pending.add(new Path(List.copyOf(next),Set.copyOf(invoked),false));if(!receiverInvoked.isEmpty())pending.add(new Path(List.copyOf(next),Set.copyOf(receiverInvoked),true));}
                }
            }
        }
        return List.copyOf(found);
    }
    private static int knownArgumentSlot(Prepare.MethodId id){
        if(id.equals(new Prepare.MethodId("AfdsHub","setPitchModes","(IZ)V"))||id.equals(new Prepare.MethodId("AfdsHub","trans","(ZI)V")))return 0;
        if(id.equals(new Prepare.MethodId("FmcGlobal$MemBits","trans","(ZZI)V")))return 1;
        return -1;
    }
    /** Exact residual access sites after native Pause, narrow region and pure
     * identity projections. Diagnostic attribution uses this same substantive
     * set rather than all fields in an original host method. */
    static Map<Prepare.MethodId,Set<FmcQualification.Member>> substantiveFields(Prepare data,FmcBackgroundAdmissionProfile.Plan plan){
        var selected=new HashSet<>(plan.fields());selected.removeAll(FmcBackgroundAdmissionProfile.BANK_CONTENT);var projections=projections(data,plan);var result=new HashMap<Prepare.MethodId,Set<FmcQualification.Member>>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            var fields=new HashSet<FmcQualification.Member>();var ops=Prepare.instructions(entry.getValue());for(int at:admittedInstructions(data,entry.getKey(),projections))if(ops.get(at) instanceof FieldInstruction f){var member=FmcQualification.resolve(data,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());if(member!=null&&!FmcBackgroundAdmissionPrivate.unrelatedAccess(data,entry.getKey(),member,plan.publishedReferences()))fields.add(member);}
            if(!fields.isEmpty())result.put(entry.getKey(),Set.copyOf(fields));
        }
        return Map.copyOf(result);
    }
    private static Set<FmcBackgroundAdmissionProjection.Site> projections(Prepare data,FmcBackgroundAdmissionProfile.Plan plan){var result=new HashSet<>(FmcBackgroundAdmissionProjection.qualify(data,Map.of()));result.addAll(FmcBackgroundAdmissionDispatchProjection.qualify(data,Map.of(),plan));
        // Complete binding arms and validates the immutable old view before
        // closure, then owes final original-cadence reconciliation. Unbound
        // partial profiles cannot claim these three native read projections.
        if(!plan.ownedStatics().isEmpty()){result.addAll(FmcBackgroundAdmissionAfdsPrepare.sites(data));result.addAll(FmcBackgroundAdmissionRenderingProducer.argumentSites(data));}result.addAll(new FmcBackgroundAdmissionRoles(data,plan).directFields());return Set.copyOf(result);
    }
    private static Set<Integer> admittedInstructions(Prepare data,Prepare.MethodId id,Set<FmcBackgroundAdmissionProjection.Site> projections){
        return admittedInstructions(data,id,projections,false);
    }
    private static Set<Integer> admittedInstructions(Prepare data,Prepare.MethodId id,Set<FmcBackgroundAdmissionProjection.Site> projections,boolean jobs){
        var method=data.methods.get(id);var ops=Prepare.instructions(method);var active=new HashSet<Integer>();if(jobs){for(int at=0;at<ops.size();at++)active.add(at);}else active.addAll(pausedInstructions(data,method));int bci=0;
        for(int at=0;at<ops.size();at++){
            if(projections.contains(new FmcBackgroundAdmissionProjection.Site(id,at)))active.remove(at);
            if(!jobs)for(var region:FmcBackgroundAdmissionRegionPrepare.INSTALLED)if(region.method().equals(id)&&bci>=region.beginBci()&&bci<region.endBci())active.remove(at);
            bci+=ops.get(at).sizeInBytes();
        }
        if(!jobs){
            if(id.equals(FmcBackgroundAdmissionCoreRequests.LENTO)||id.equals(FmcBackgroundAdmissionCoreRequests.MODERATO))active.removeAll(FmcBackgroundAdmissionCoreRequests.sites(data,id));
            if(id.equals(FmcBackgroundAdmissionOptionPrepare.METHOD))active.removeAll(FmcBackgroundAdmissionOptionPrepare.excludedSites(data));active.removeAll(FmcBackgroundAdmissionPacketPrepare.excludedSites(data,id));
        }
        return Set.copyOf(active);
    }
    /** Only the exact adjacent native Core.paused boolean branch is folded.
     * Every other condition and native exception successor stays conservative.
     * This qualifies particular existing branches, never a whole-frame skip. */
    static Set<Integer> pausedInstructions(MethodModel method){return pausedInstructions(null,method);}
    static Set<Integer> pausedInstructions(Prepare data,MethodModel method){
        boolean purePause=data!=null&&PURE_PAUSE.computeIfAbsent(data,FmcBackgroundAdmissionClosure::purePause);
        var labels=new HashMap<Label,Integer>();var ops=new ArrayList<Instruction>();var code=method.code().orElseThrow();
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ops.size());if(e instanceof Instruction i)ops.add(i);}
        var reached=new HashSet<Integer>();var pending=new ArrayDeque<Integer>();pending.add(0);
        while(!pending.isEmpty()){
            int at=pending.removeFirst();if(at>=ops.size()||!reached.add(at))continue;var op=ops.get(at);
            for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd()))pending.add(labels.get(h.handler()));
            if(op instanceof ReturnInstruction||op.opcode()==Opcode.ATHROW)continue;
            if(op instanceof BranchInstruction branch){
                boolean pause=at>0&&ops.get(at-1) instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("Core")
                    &&(f.name().equalsString("paused")&&f.type().equalsString("Z")||f.name().equalsString("globSimPaused")&&f.type().equalsString("I"));
                if(purePause&&at>0&&ops.get(at-1).opcode()==Opcode.INVOKESTATIC&&Prepare.call(ops.get(at-1),"Core","isPaused","()Z"))pause=true;
                if(pause&&op.opcode()==Opcode.IFEQ){pending.add(at+1);continue;}
                if(pause&&op.opcode()==Opcode.IFNE){pending.add(labels.get(branch.target()));continue;}
                pending.add(labels.get(branch.target()));if(op.opcode()==Opcode.GOTO||op.opcode()==Opcode.GOTO_W)continue;
            }else if(op instanceof LookupSwitchInstruction s){pending.add(labels.get(s.defaultTarget()));for(var c:s.cases())pending.add(labels.get(c.target()));continue;}
            else if(op instanceof TableSwitchInstruction s){pending.add(labels.get(s.defaultTarget()));for(var c:s.cases())pending.add(labels.get(c.target()));continue;}
            pending.add(at+1);
        }
        return Set.copyOf(reached);
    }
    /** Core.isPaused returns paused || loaderPaused. Required ordinary Pause
     * proves true without changing any frame or inspecting loader state. A
     * drifted/opaque predicate retains both successors instead of assuming. */
    static boolean purePause(Prepare data){
        var method=data.methods.get(new Prepare.MethodId("Core","isPaused","()Z"));if(method==null||method.code().isEmpty()||(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)return false;var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();var ops=Prepare.instructions(method);
        return code.exceptionHandlers().isEmpty()&&ops.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.GETSTATIC,Opcode.IFNE,Opcode.GETSTATIC,Opcode.IFEQ,Opcode.ICONST_1,Opcode.GOTO,Opcode.ICONST_0,Opcode.IRETURN))
            &&Prepare.field(ops.get(0),Opcode.GETSTATIC,"Core","paused")&&((FieldInstruction)ops.get(0)).type().equalsString("Z")&&Prepare.field(ops.get(2),Opcode.GETSTATIC,"Core","loaderPaused")&&((FieldInstruction)ops.get(2)).type().equalsString("Z")
            &&code.labelToBci(((BranchInstruction)ops.get(1)).target())==12&&code.labelToBci(((BranchInstruction)ops.get(3)).target())==16&&code.labelToBci(((BranchInstruction)ops.get(5)).target())==17;
    }
}
