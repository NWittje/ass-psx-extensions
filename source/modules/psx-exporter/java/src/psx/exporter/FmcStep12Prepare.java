package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;

/** Complete source-side reconstruction/admission composition. This is a
 * preparation contract for steps1/2; command9, journal, runtime capability and
 * loaded acceptance remain the later service-integration steps. Capture's
 * qualified native coherence witnesses are preserved, rather than silently
 * replacing them with restore access witnesses. */
final class FmcStep12Prepare {
    record Result(Map<String,byte[]> changed,Set<Prepare.MethodId> roots,Set<FmcQualification.Member> writes,
                  Set<FmcQualification.Member> currentInputs,FmcBackgroundAdmissionProfile.Plan admission,
                  FmcBackgroundAdmissionFaults.Roster faults,List<String> unresolvedFrames,
                  FmcBackgroundAdmissionOwnership.Profile ownership,FmcBackgroundAdmissionInputRoots.Profile currentRoots,
                  FmcReconstructionExecRoots.Profile execRoots,FmcReconstructionExecAudit.Effects ordinaryEffects,
                  FmcBackgroundAdmissionTransport.Profile transport,FmcBackgroundAdmissionLifecycle.Profile lifecycleEffects,
                  FmcReconstructionNormalExec.Profile normalExec,FmcReconstructionFreshCache.Profile freshCache,
                  FmcReconstructionExecScope.Profile execScope,FmcBackgroundAdmissionObservers.Profile observers){}
    private FmcStep12Prepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC steps1/2 "+why);}
    static Result qualify(Prepare data,Map<String,byte[]> lower)throws Exception{
        var source=FmcSourceLayer.qualify(data,lower);require(!source.overlay().isEmpty(),"independent capture source: "+source.evidence());
        var recipe=FmcReconstructionProfile.qualify(data,lower);var raw=new HashMap<>(FmcReconstructionHeaderPrepare.transform(data,recipe.changed()));
        raw=new HashMap<>(FmcReconstructionMenuPrepare.transform(data,raw));raw=new HashMap<>(FmcReconstructionNavPrepare.transform(data,raw));var control=FmcReconstructionControlPrepare.transform(data,raw);raw=new HashMap<>(FmcReconstructionControlAliases.transform(data,control.changed()));
        var roots=new HashSet<>(recipe.roots());roots.addAll(FmcReconstructionHeaderPrepare.roots());roots.addAll(FmcReconstructionMenuPrepare.roots());roots.addAll(FmcReconstructionNavPrepare.roots());roots.addAll(FmcReconstructionControlPrepare.roots());
        var writes=new HashSet<>(recipe.writes());writes.addAll(FmcReconstructionHeaderPrepare.writes());writes.addAll(FmcReconstructionMenuPrepare.writes());writes.addAll(FmcReconstructionNavPrepare.writes());writes.addAll(control.writes());
        var inputs=new HashSet<>(recipe.currentInputs());inputs.addAll(FmcReconstructionNavPrepare.inputs());inputs.addAll(FmcReconstructionMenuPrepare.inputs());inputs.addAll(control.currentInputs());inputs.addAll(FmcBackgroundAdmissionInputRoots.FIELDS);for(String name:List.of("ngFmc","defaultSpdTransIas","defaultSpdTransAlt"))inputs.add(new FmcQualification.Member("Option",name,name.equals("ngFmc")?"Z":"I"));
        raw.putAll(FmcReconstructionActivationPrepare.transform(data,raw));roots.addAll(FmcReconstructionActivationPrepare.roots());var currentRoots=FmcBackgroundAdmissionInputRoots.transform(data,raw);raw.putAll(currentRoots.changed());
        raw.putAll(FmcReconstructionPausePrepare.transform(data,raw));inputs.addAll(FmcReconstructionPausePrepare.FIELDS);
        raw.putAll(FmcReconstructionCruisePrepare.transform(data,raw));roots.addAll(FmcReconstructionCruisePrepare.roots());writes.addAll(FmcReconstructionCruisePrepare.writes());inputs.addAll(FmcReconstructionCruisePrepare.inputs());
        raw.putAll(FmcReconstructionOffsetPrepare.transform(data,raw));roots.addAll(FmcReconstructionOffsetPrepare.roots());writes.addAll(FmcReconstructionOffsetPrepare.writes());inputs.addAll(FmcReconstructionOffsetPrepare.inputs());
        raw.putAll(FmcRenderingPrepare.bindings(data,raw));inputs.addAll(FmcRenderingPrepare.inputs());
        raw.putAll(FmcBackgroundAdmissionAfdsPrepare.bindings(data,raw));inputs.addAll(FmcBackgroundAdmissionAfdsPrepare.inputs());
        raw.putAll(FmcBackgroundAdmissionSavePrepare.bindings(data,raw));inputs.addAll(FmcBackgroundAdmissionSavePrepare.INPUTS);
        raw.putAll(FmcBackgroundAdmissionOptionPrepare.bindings(data,raw));inputs.addAll(FmcBackgroundAdmissionOptionPrepare.configurationInputs(data));
        require(Collections.disjoint(writes,FmcReconstructionPreservationPrepare.fields()),"recipe cannot publish current manual fuel/flight number");raw.putAll(FmcReconstructionPreservationPrepare.transform(data,raw));inputs.addAll(FmcReconstructionPreservationPrepare.fields());
        var protectedBanks=new HashSet<FmcReconstructionExecRoots.Bank>();for(String name:List.of("qsFmcPerfInit","qsFmcRteViAcMo","qsNavRadSet","qsIrsMonRef","qsFmcPredA","qsFmcPredM","qsRteOffsetNg"))protectedBanks.add(new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob","qs","[LQs;"),new FmcQualification.Member("Glob",name,"I")));for(String name:List.of("qiStepSizeNg","qiMinFuelTemp"))protectedBanks.add(new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob","qi","[LQi;"),new FmcQualification.Member("Glob",name,"I")));
        var execScope=FmcReconstructionExecScope.inspect(data);raw.putAll(FmcReconstructionNavigationPrepare.transform(data,raw,execScope));inputs.addAll(FmcReconstructionNavigationPrepare.inputs());inputs.addAll(FmcReconstructionTransitionEffects.inputs());protectedBanks.addAll(FmcReconstructionTransitionEffects.banks());raw.putAll(FmcReconstructionTransitionEffects.transform(data,raw,Set.copyOf(writes),Set.copyOf(inputs)));var normalExec=FmcReconstructionNormalExec.inspect(data,Set.copyOf(writes),Set.copyOf(inputs),Set.copyOf(protectedBanks));
        normalExec=FmcReconstructionAcarsEffects.apply(data,normalExec,Set.copyOf(writes),Set.copyOf(inputs),Set.copyOf(protectedBanks));
        normalExec=FmcReconstructionPageEffects.apply(data,normalExec,Set.copyOf(writes),Set.copyOf(inputs));normalExec=FmcReconstructionLandingEffects.apply(data,normalExec,Set.copyOf(writes),Set.copyOf(inputs));
        normalExec=FmcReconstructionIceEffects.apply(data,normalExec,Set.copyOf(writes),Set.copyOf(inputs));
        normalExec=FmcReconstructionBusIsolationEffects.apply(data,normalExec,Set.copyOf(writes),Set.copyOf(inputs));
        var approach=FmcReconstructionApproachEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs));var approachFields=new HashSet<>(normalExec.fields());approachFields.addAll(approach.fields());var approachReferences=new HashSet<>(normalExec.references());approachReferences.addAll(approach.references());normalExec=new FmcReconstructionNormalExec.Profile(Set.copyOf(approachFields),normalExec.banks(),normalExec.retainedInputs(),normalExec.bankReaders(),Set.copyOf(approachReferences),normalExec.dynamicScratchpads(),normalExec.atomicCurrentOutputs());
        var transitionFields=new HashSet<>(normalExec.fields());transitionFields.addAll(FmcReconstructionTransitionEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs)));transitionFields.addAll(FmcReconstructionDatalinkEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs)));transitionFields.addAll(FmcReconstructionGuidanceStateEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs),execScope));transitionFields.addAll(FmcReconstructionDisplayStateEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs),execScope));transitionFields.addAll(FmcBackgroundAdmissionSchedulingEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs),execScope));
        var atomicOutputs=new HashSet<>(normalExec.atomicCurrentOutputs());atomicOutputs.addAll(FmcReconstructionTransitionEffects.atomicCurrentOutputs(data,Set.copyOf(writes),Set.copyOf(inputs)));var memory=FmcBackgroundAdmissionMemoryEffects.inspect(data,Set.copyOf(writes),Set.copyOf(inputs),Set.copyOf(protectedBanks));var outputReferences=new HashSet<>(normalExec.references());outputReferences.addAll(memory.references());var outputBanks=new HashSet<>(normalExec.banks());outputBanks.addAll(memory.banks());normalExec=new FmcReconstructionNormalExec.Profile(Set.copyOf(transitionFields),Set.copyOf(outputBanks),normalExec.retainedInputs(),normalExec.bankReaders(),Set.copyOf(outputReferences),normalExec.dynamicScratchpads(),Set.copyOf(atomicOutputs));
        var execRoots=FmcReconstructionExecRoots.transform(data,raw,normalExec,execScope);raw.putAll(execRoots.changed());
        require(execRoots.unclosedReceivers().isEmpty()&&execRoots.unclosedReferences().isEmpty(),"unresolved published EXEC receiver/reference frontier");
        require(execRoots.banks().stream().noneMatch(bank->bank.index().name().equals("qsFmcAirlPol")||bank.index().name().startsWith("qiCfg")||bank.index().name().startsWith("qsCfg")),"quiet configuration poll banks must retain distinct unowned native scope");
        raw.putAll(FmcReconstructionDisplayPrepare.transform(data,raw));inputs.addAll(FmcReconstructionDisplayPrepare.fields());inputs.addAll(FmcReconstructionDisplayPrepare.PAGE_INPUTS);
        var ordinaryEffects=FmcReconstructionExecAudit.inspect(data);require(ordinaryEffects.writes().containsAll(execScope.writes()),"executable effects exceed full potential authority");var transport=FmcBackgroundAdmissionTransport.protect(FmcBackgroundAdmissionTransport.inspect(data),Set.copyOf(writes),Set.copyOf(inputs));require(inputs.containsAll(FmcBackgroundAdmissionTransport.CURRENT_INPUTS),"genuine native network geometry predicates retain changed-value current input witnesses");var lifecycleEffects=FmcBackgroundAdmissionLifecycle.inspect(data,ordinaryEffects);var freshCache=FmcReconstructionFreshCache.inspect(data);
        writes.addAll(execScope.writes());writes.removeAll(transport.fields());writes.removeAll(lifecycleEffects.fields());writes.removeAll(normalExec.fields());writes.removeAll(freshCache.recordFields());inputs.addAll(execRoots.references());inputs.addAll(freshCache.references());inputs.removeAll(transport.references());inputs.removeAll(lifecycleEffects.fields());inputs.removeAll(normalExec.references());
        // Native phase is a current geometry input and an accepted atomic
        // ordinary EXEC guidance output. Keep its changed-value writer hook;
        // an unrelated scalar reader need not be frozen during route rebuild.
        require(inputs.containsAll(normalExec.atomicCurrentOutputs()),"atomic native guidance must retain current input witnesses");writes.removeAll(normalExec.atomicCurrentOutputs());
        writes.removeAll(FmcReconstructionPreservationPrepare.fields());
        writes.removeAll(FmcReconstructionNavigationPrepare.inputs());
        var capture=new HashMap<>(lower);capture.putAll(source.overlay());var merged=mergeAdditions(data,lower,capture,raw);
        var currentOnly=new HashSet<>(inputs);currentOnly.removeAll(writes);var plan=FmcBackgroundAdmissionProfile.inspect(data,Set.copyOf(writes),Set.copyOf(currentOnly),execRoots.references(),execRoots);require(plan.unresolvedStarts().isEmpty(),"unresolved native start receivers "+plan.unresolvedStarts());
        var allFields=new HashSet<>(writes);allFields.addAll(inputs);allFields.addAll(lifecycleEffects.privateFields());var tagged=new HashMap<>(merged);tagged.putAll(FmcBackgroundAdmissionOwnership.transform(data,merged,Set.copyOf(allFields)).changed());
        tagged.putAll(FmcReconstructionExecRoots.propagate(data,tagged,execRoots));
        var readers=FmcBackgroundAdmissionProfile.transformReaders(data,tagged,plan);
        var transported=new HashMap<>(readers);transported.putAll(FmcBackgroundAdmissionTransportPrepare.transform(data,readers));readers=Map.copyOf(transported);
        var withInputs=new HashMap<>(readers);withInputs.putAll(FmcBackgroundAdmissionInputPrepare.transform(data,readers,Set.copyOf(currentOnly)).changed());readers=Map.copyOf(withInputs);
        var exec=FmcExecPrepare.transform(data,readers);
        var mutation=FmcExecMutationPrepare.transform(data,exec,Set.copyOf(roots),transport.continuationMethods());
        var starts=new ArrayList<>(plan.launches());starts.add(new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte",FmcExecPrepare.LEFT,"(J)Ljava/lang/Thread;"),1));starts.add(new FmcJobPrepare.Start(new Prepare.MethodId("FmcRte",FmcExecPrepare.RIGHT,"(J)Ljava/lang/Thread;"),1));
        var output=new HashMap<>(mutation);output.putAll(FmcJobPrepare.quietWithScope(data,mutation,starts,plan.jobs()));
        var finalModels=FmcQualification.finalModels(data,output);for(var root:roots)require(finalModels.get(root.owner()).methods().stream().anyMatch(m->m.methodName().equalsString(root.name())&&m.methodType().equalsString(root.descriptor())),"final trusted mutator root "+root);
        for(var entry:output.entrySet())require(data.cf.verify(entry.getValue()).isEmpty(),"final verifier "+entry.getKey());var legalBefore=new HashMap<>(data.inputs);legalBefore.putAll(merged);TerrainAccess.verifyAdded(legalBefore,output,data.inputs);
        var observers=FmcBackgroundAdmissionObservers.inspect(data);var unresolved=new TreeSet<String>();for(String path:FmcBackgroundAdmissionClosure.framePaths(data,plan))unresolved.add("FRAME "+path);for(String path:FmcBackgroundAdmissionClosure.observerPaths(data,plan))unresolved.add("OBSERVER "+path);var frames=List.copyOf(unresolved);var faults=FmcBackgroundAdmissionFaults.roster(data,output);
        var ownership=FmcBackgroundAdmissionOwnership.transform(data,output,Set.copyOf(allFields));require(ownership.changed().isEmpty(),"final metadata provenance");
        FmcReconstructionCapacity.checkedBudget(execRoots);FmcReconstructionCapacity.checkedTargetBudget(data,execRoots,ownership.owners());
        var pollOwners=new HashSet<String>();for(var receiver:plan.receivers().values()){String type=receiver.descriptor();if(type.startsWith("L")&&type.endsWith(";"))pollOwners.add(type.substring(1,type.length()-1));}
        FmcReconstructionCapacity.workerOwnerUpperBound(data,execRoots,ownership.owners(),pollOwners);
        return new Result(Map.copyOf(output),Set.copyOf(roots),Set.copyOf(writes),Set.copyOf(inputs),plan,faults,frames,ownership,currentRoots,execRoots,ordinaryEffects,transport,lifecycleEffects,normalExec,freshCache,execScope,observers);
    }
    /** Both independent proofs start from identical original input. Only the
     * recipe's newly declared, exact generated methods enter capture's tree;
     * any changed original method/field is a failed composition, not a prefix
     * exception permitting arbitrary modified native code. */
    static Map<String,byte[]> mergeAdditions(Prepare data,Map<String,byte[]> lower,Map<String,byte[]> capture,Map<String,byte[]> recipe){
        var output=new HashMap<>(capture);var before=new HashMap<>(capture);
        for(var entry:recipe.entrySet()){
            String name=entry.getKey();byte[] base=lower.getOrDefault(name,data.inputs.get(name));require(base!=null,"original recipe owner");var original=data.cf.parse(base);var added=data.cf.parse(entry.getValue());
            var originalFields=new TreeSet<String>();for(var f:original.fields())originalFields.add(f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask());var fields=new TreeSet<String>();for(var f:added.fields())fields.add(f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask());require(fields.equals(originalFields),"recipe changes native field shape "+name);
            var methods=new HashMap<String,MethodModel>();for(var m:original.methods())methods.put(m.methodName()+m.methodType().stringValue(),m);var additions=new ArrayList<MethodModel>();
            for(var m:added.methods()){String key=m.methodName()+m.methodType().stringValue();var old=methods.remove(key);if(old!=null){require(Arrays.equals(body(data,name,old),body(data,name,m)),"recipe changes original native method "+name+"."+key);}else{require(m.methodName().stringValue().startsWith("$psxExporter$fmc")&&(m.flags().flagsMask()&ClassFile.ACC_SYNTHETIC)!=0&&m.code().isPresent(),"unqualified recipe member "+name+"."+key);additions.add(m);}}
            require(methods.isEmpty(),"recipe removed native method");if(additions.isEmpty())continue;
            byte[] current=capture.getOrDefault(name,base);var model=data.cf.parse(current);for(var m:additions)require(model.methods().stream().noneMatch(x->x.methodName().equals(m.methodName())&&x.methodType().equals(m.methodType())),"capture/recipe generated collision");before.put(name,current);
            output.put(name,data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){for(var m:additions)b.withMethod(m.methodName().stringValue(),m.methodTypeSymbol(),m.flags().flagsMask(),builder->builder.transformCode(m.code().orElseThrow(),(c,e)->c.with(e)));}}));
        }
        TerrainAccess.verifyAdded(before,output,data.inputs);return Map.copyOf(output);
    }
    private static byte[] body(Prepare data,String owner,MethodModel method){return data.cf.build(ClassDesc.of(owner),b->{if(method.code().isPresent())b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e)));else b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->{});});}
}
