package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** One preparation model for the quiet-input recall window. The destination
 * recipe supplies its complete native write fields, not the serialized raw
 * capture table. Original Pause-return methods and narrow subsystem regions
 * have executable qualifications; jobs and additional poll cuts must retain
 * their original finite lifetime/receiver contracts. Unresolved paths reject
 * optional admission rather than being converted into guessed returns. */
final class FmcBackgroundAdmissionProfile {
    record Plan(Set<FmcQualification.Member> fields,FmcBackgroundAdmissionPrepare.Profile access,Set<Prepare.MethodId> paused,
        Set<Prepare.MethodId> polls,Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers,
        Set<Prepare.MethodId> jobs,List<FmcJobPrepare.Start> launches,Set<Prepare.MethodId> persistent,
        Set<Prepare.MethodId> unresolvedStarts,Set<FmcQualification.Member> publishedReferences,
        Set<FmcQualification.Member> ownedStatics,Set<FmcQualification.Member> ownedChildren,Set<Prepare.MethodId> globalPageContent,Set<Prepare.MethodId> inputLifetimes){
        Plan(Set<FmcQualification.Member> fields,FmcBackgroundAdmissionPrepare.Profile access,Set<Prepare.MethodId> paused,
             Set<Prepare.MethodId> polls,Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers,Set<Prepare.MethodId> jobs,
             List<FmcJobPrepare.Start> launches,Set<Prepare.MethodId> persistent,Set<Prepare.MethodId> unresolvedStarts,
             Set<FmcQualification.Member> publishedReferences,Set<FmcQualification.Member> ownedStatics,Set<FmcQualification.Member> ownedChildren,
             Set<Prepare.MethodId> globalPageContent){this(fields,access,paused,polls,receivers,jobs,launches,persistent,unresolvedStarts,publishedReferences,ownedStatics,ownedChildren,globalPageContent,Set.of());}
    }
    private static final Set<String> PERSISTENT=Set.of("Core$PrestoThread","Core$VoiceThread","Core$AllegroThread","Core$LargoThread","Core$LentoThread","Core$ModeratoThread",
        "NetClientThread","NetServerThread","TerrainReader$TerrainReadThread");
    private static final Map<String,Set<String>> POLLS=Map.ofEntries(
        Map.entry("FmcPerf",Set.of("runAllegro","runModerato","runLargo","runLento","updatePerfInitText","makeDisplaysRest","makeDisplays1B2B")),
        Map.entry("FmcRte",Set.of("runAllegro","runLento","runModerato","runLargo")),
        Map.entry("Routes",Set.of("runDelta","runLento","runLargo")),
        Map.entry("FmcVnav",Set.of("runModerato","runLento","runLargo")),
        Map.entry("FmcVnavCalc",Set.of("runLento")),
        Map.entry("FmcHold",Set.of("runLento","runLargo")),
        Map.entry("FmcAppRef",Set.of("runModerato","runLargo")),
        Map.entry("FmcTkofRef",Set.of("runAllegro","runModerato","runLento","runLargo")),
        Map.entry("FmcThrustLim",Set.of("runAllegro","runModerato","runLento","runLargo")),
        Map.entry("FmcFixInfo",Set.of("runAllegro","runLargo","runLento","runModerato")),
        Map.entry("FmcDataLink",Set.of("runLento","runLargo")),
        Map.entry("RoutesStby$Rte",Set.of("runModerato","runLento")),
        Map.entry("FmcNg",Set.of("runLargo","runLento","runModerato")),
        Map.entry("FmcProgress1",Set.of("runLargo","runLento","runModerato")),
        Map.entry("FmcProgress2",Set.of("runLargo","runLento","runModerato")),
        Map.entry("RadioAtcBasis",Set.of("setOrigData","setDestData","runLargo")),
        Map.entry("Fuel",Set.of("setAutoFuelToRemain")),
        Map.entry("NavRadios",Set.of("runModerato")));
    private FmcBackgroundAdmissionProfile(){}
    static final Set<FmcQualification.Member> ALIASES=Set.of(
        new FmcQualification.Member("FmcRte$Rte","sbOrig","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbDest","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbCoRte","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbRwy","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbFltNo","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbRteTit","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbLegsTit","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sb6B","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("Routes$RteBody","leg","[LRouteWpt;"));
    static final Set<FmcQualification.Member> BANK_CONTENT=Set.of(
        new FmcQualification.Member("Q","fresh","Z"),new FmcQualification.Member("Q","updated","Z"),
        new FmcQualification.Member("Qi","iVar","I"),new FmcQualification.Member("Qs","sVar","Ljava/lang/String;"),
        new FmcQualification.Member("Qs","lastComeFrom","I"));
    static final Set<FmcQualification.Member> UI_BUFFERS=Set.of(
        new FmcQualification.Member("FmcRte","sbRteTitleL","Ljava/lang/StringBuilder;"),new FmcQualification.Member("FmcRte","sbRteTitleR","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte","sbLegsTitleL","Ljava/lang/StringBuilder;"),new FmcQualification.Member("FmcRte","sbLegsTitleR","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte","sb6bL","Ljava/lang/StringBuilder;"),new FmcQualification.Member("FmcRte","sb6bR","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte","sbRteTitleCduL","Ljava/lang/StringBuilder;"),new FmcQualification.Member("FmcRte","sbRteTitleCduR","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte","sbLegsTitleCduL","Ljava/lang/StringBuilder;"),new FmcQualification.Member("FmcRte","sbLegsTitleCduR","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte","sbFltNoTitleBuild","Ljava/lang/StringBuilder;"));
    static final Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> BUFFER_PARAMETERS=Map.of(
        new Prepare.MethodId("CduHub","insert_","(Ljava/lang/StringBuilder;II)V"),new FmcExclusionPrepare.Receiver(0,"Ljava/lang/StringBuilder;"),
        new Prepare.MethodId("CduHub","insertChar","(Ljava/lang/StringBuilder;IIC)V"),new FmcExclusionPrepare.Receiver(0,"Ljava/lang/StringBuilder;"));
    static final Set<Prepare.MethodId> NETWORK_AGGREGATES=Set.of(
        new Prepare.MethodId("NetServerThread","dumpStartVars","()V"),new Prepare.MethodId("NetServerThread","dumpAll","()V"),new Prepare.MethodId("NetServerThread","fakeSituationFileLoad","()V"));
    static Plan inspect(Prepare data,Set<FmcQualification.Member> completeWrites){
        return inspect(data,completeWrites,Set.of());
    }
    static Plan inspect(Prepare data,Set<FmcQualification.Member> completeWrites,Set<FmcQualification.Member> currentInputFields){
        return inspect(data,completeWrites,currentInputFields,Set.of());
    }
    static Plan inspect(Prepare data,Set<FmcQualification.Member> completeWrites,Set<FmcQualification.Member> currentInputFields,Set<FmcQualification.Member> publishedReferences){
        return inspect(data,completeWrites,currentInputFields,publishedReferences,null);
    }
    static Plan inspect(Prepare data,Set<FmcQualification.Member> completeWrites,Set<FmcQualification.Member> currentInputFields,Set<FmcQualification.Member> publishedReferences,FmcReconstructionExecRoots.Profile bound){
        if(completeWrites.isEmpty())throw new IllegalArgumentException("FMC admission needs complete recipe fields");
        var content=new HashSet<>(completeWrites);content.addAll(ALIASES);content.addAll(BANK_CONTENT);content.addAll(UI_BUFFERS);
        var access=FmcBackgroundAdmissionPrepare.inspect(data,content);
        FmcBackgroundAdmissionRenderingProducer.qualify(data);FmcBackgroundAdmissionRenderingProducer.qualifyRadio(data);FmcBackgroundAdmissionSavePrepare.qualify(data);FmcBackgroundAdmissionCoreRequests.qualify(data);
        var buffers=content.stream().filter(f->f.descriptor().equals("Ljava/lang/StringBuilder;")).collect(java.util.stream.Collectors.toUnmodifiableSet());
        FmcBufferFlowAudit.audit(data,Map.of(),buffers);
        // Common bank base classes also serve every unrelated simulator
        // subsystem. Their actual field sites use exact ownership tags; they
        // cannot make arbitrary bank consumers FMC poll/job boundaries.
        var substantive=new HashSet<>(content);substantive.removeAll(BANK_CONTENT);
        var evidence=FmcRecallBoundary.inspect(data,substantive);
        var reachable=new HashSet<Prepare.MethodId>(evidence.readers());reachable.addAll(evidence.writers());var pending=new ArrayDeque<>(reachable);
        while(!pending.isEmpty())for(var parent:evidence.callers().getOrDefault(pending.removeFirst(),Set.of()))if(reachable.add(parent))pending.addLast(parent);
        var paused=new HashSet<Prepare.MethodId>();var polls=new HashSet<Prepare.MethodId>();
        polls.addAll(FmcBackgroundAdmissionCadencePrepare.inspect(data).methods());
        polls.addAll(FmcBackgroundAdmissionCduPrepare.qualify(data));FmcBackgroundAdmissionJumpPrepare.qualify(data);polls.add(FmcBackgroundAdmissionJumpPrepare.METHOD);
        var monitorConsumer=new Prepare.MethodId("FmcMaintenance","readGlobIrsMonRef","()V");
        if(!data.methods.containsKey(monitorConsumer))throw new IllegalArgumentException("FMC maintenance consumer drift");
        polls.add(monitorConsumer);
        var receivers=new HashMap<Prepare.MethodId,FmcExclusionPrepare.Receiver>();polls.add(FmcBackgroundAdmissionSavePrepare.METHOD);receivers.put(FmcBackgroundAdmissionSavePrepare.METHOD,FmcBackgroundAdmissionSavePrepare.receiver());
        polls.add(FmcBackgroundAdmissionRenderingProducer.METHOD);receivers.put(FmcBackgroundAdmissionRenderingProducer.METHOD,FmcBackgroundAdmissionRenderingProducer.receiver());
        polls.add(FmcBackgroundAdmissionRenderingProducer.RADIO);
        var glyphConsumer=new Prepare.MethodId("ControlDisplayUnit$ScreenLine","setUpdate","()V");
        if(!data.methods.containsKey(glyphConsumer))throw new IllegalArgumentException("FMC current glyph consumer drift");
        polls.add(glyphConsumer);receivers.put(glyphConsumer,new FmcExclusionPrepare.Receiver(0,"LControlDisplayUnit$ScreenLine;"));
        for(var id:reachable){var method=data.methods.get(id);if(method==null||method.code().isEmpty()||!id.descriptor().endsWith(")V")||id.name().startsWith("<"))continue;
            if(FmcExclusionAudit.entry(method).earlyPauseReturn()){paused.add(id);continue;}
            if(POLLS.getOrDefault(id.owner(),Set.of()).contains(id.name())||id.owner().startsWith("CduPg")&&(id.name().equals("processAll")||id.name().equals("processTitle")||id.name().startsWith("processText")))polls.add(id);
            if(polls.contains(id)&&id.owner().startsWith("CduPg")&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0&&(id.name().equals("processAll")||id.name().equals("processTitle")||id.name().startsWith("processText")))receivers.put(id,new FmcExclusionPrepare.Receiver(0,"L"+id.owner()+";","CduHub$Page"));
            // Body/header cadence entry is receiver-qualified. A private
            // staging/inactive alias of the same native type keeps its path.
            if(Set.of("Routes$RteBody","FmcRte$Rte","FmcDepArr$DepTransPack","FmcDepArr$ArrTransPack").contains(id.owner())&&Set.of("runDelta","runLento","runLargo","runModerato","doOneSecondTask").contains(id.name())
                &&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0){polls.add(id);receivers.put(id,new FmcExclusionPrepare.Receiver(0,"L"+id.owner()+";"));}
        }
        var boundStatics=new HashSet<FmcQualification.Member>();var boundChildren=new HashSet<FmcQualification.Member>();
        if(bound!=null){
            bound.statics().values().forEach(boundStatics::addAll);bound.children().values().forEach(boundChildren::addAll);
            if(boundStatics.stream().anyMatch(f->f.owner().equals("Glob")&&Set.of("qi","qs","qh").contains(f.name())))throw new IllegalArgumentException("FMC bank ownership needs selected cells, never entire Glob arrays");
            // ExecRoots.read deliberately retains these old packs only as
            // transaction preimages. Their rows are not tagged or traversed.
            boundChildren.removeIf(f->f.owner().equals("FmcRte$Rte")&&Set.of("sidPack","arrPkOr","appPkOr","arrPkDe","appPkDe").contains(f.name()));
            for(String name:FmcReconstructionDisplayPrepare.UNITS_FIELDS)boundStatics.add(new FmcQualification.Member("Core",name,"LControlDisplayUnit;"));
            for(String name:FmcReconstructionDisplayPrepare.LINE_FIELDS)boundChildren.add(new FmcQualification.Member("ControlDisplayUnit",name,"LControlDisplayUnit$ScreenLine;"));
            boundChildren.add(new FmcQualification.Member("ControlDisplayUnit","page","LCduHub$Page;"));
            // The separately prebound SaveAccess bridge proves these exact
            // three current singleton objects, verifies every pointer before
            // mutation, and registers all three ownership targets. These are
            // actual binding roles rather than same-type GUI assumptions.
            boundStatics.add(new FmcQualification.Member("Core","instructor","LInstructor;"));
            boundChildren.add(new FmcQualification.Member("Instructor","iSituation","LInstructorSituation;"));
            boundChildren.add(new FmcQualification.Member("InstructorSituation","iSave","LInstructorSaveSitu;"));
            for(String name:FmcRenderingPrepare.NAMES)boundStatics.add(new FmcQualification.Member("IntegratedDisplaySystem",name,"LNavigationDisplay;"));
        }
        var pageCandidates=new HashSet<Prepare.MethodId>();for(var entry:receivers.entrySet())if("CduHub$Page".equals(entry.getValue().tagOwner()))pageCandidates.add(entry.getKey());
        var globalPages=FmcBackgroundAdmissionPageContent.global(data,Set.copyOf(content),Set.copyOf(boundStatics),Set.copyOf(boundChildren),Set.copyOf(pageCandidates),Set.copyOf(publishedReferences));
        globalPages.forEach(receivers::remove);
        var jobs=new HashSet<Prepare.MethodId>();var persistent=new HashSet<Prepare.MethodId>();
        for(var job:evidence.jobs()){if(PERSISTENT.contains(job.method().owner()))persistent.add(job.method());else jobs.add(job.method());}
        if(bound!=null){var lifetimePlan=new Plan(Set.copyOf(content),access,Set.copyOf(paused),Set.copyOf(polls),Map.copyOf(receivers),Set.of(),List.of(),Set.copyOf(persistent),Set.of(),Set.copyOf(publishedReferences),Set.copyOf(boundStatics),Set.copyOf(boundChildren),globalPages);jobs.retainAll(FmcBackgroundAdmissionClosure.relevantJobs(data,lifetimePlan,Set.copyOf(jobs)));}
        var inputEvidence=FmcRecallBoundary.inspect(data,currentInputFields);var inputWriters=new HashSet<>(inputEvidence.writers());var inputsPending=new ArrayDeque<>(inputWriters);
        while(!inputsPending.isEmpty())for(var parent:inputEvidence.callers().getOrDefault(inputsPending.removeFirst(),Set.of()))if(inputWriters.add(parent))inputsPending.addLast(parent);
        var inputJobs=new HashSet<Prepare.MethodId>();for(var job:inputEvidence.jobs())if(inputWriters.contains(job.method())){if(PERSISTENT.contains(job.method().owner()))persistent.add(job.method());else inputJobs.add(job.method());}
        if(bound!=null&&!inputJobs.isEmpty()){var inputLifetime=new Plan(Set.copyOf(currentInputFields),access,Set.of(),Set.of(),Map.of(),Set.of(),List.of(),Set.copyOf(persistent),Set.of(),Set.copyOf(publishedReferences),Set.copyOf(boundStatics),Set.copyOf(boundChildren),Set.of());inputJobs.retainAll(FmcBackgroundAdmissionClosure.relevantWriters(data,inputLifetime,Set.copyOf(inputJobs)));}jobs.addAll(inputJobs);
        var launches=new ArrayList<FmcJobPrepare.Start>();var unresolved=new HashSet<Prepare.MethodId>();
        var startFlow=FmcBackgroundAdmissionStartFlow.inspect(data,evidence.starts().stream().map(FmcAsyncAudit.Start::caller).toList());
        var ordinals=new HashMap<Prepare.MethodId,Set<Integer>>();var totals=new HashMap<Prepare.MethodId,Integer>();
        for(var site:startFlow.sites())if(jobs.contains(site.run())){
            ordinals.computeIfAbsent(site.caller(),k->new TreeSet<>()).add(site.startOrdinal());totals.put(site.caller(),site.totalStarts());
        }
        for(var entry:ordinals.entrySet())launches.add(new FmcJobPrepare.Start(entry.getKey(),totals.get(entry.getKey()),entry.getValue()));
        launches.sort(Comparator.comparing(s->s.caller().toString()));
        for(var caller:startFlow.unresolved().keySet())if(reachable.contains(caller)||inputWriters.contains(caller))unresolved.add(caller);
        var plan=new Plan(Set.copyOf(content),access,Set.copyOf(paused),Set.copyOf(polls),Map.copyOf(receivers),Set.copyOf(jobs),List.copyOf(launches),Set.copyOf(persistent),Set.copyOf(unresolved),Set.copyOf(publishedReferences),Set.copyOf(boundStatics),Set.copyOf(boundChildren),globalPages);
        var inputLifetimes=FmcBackgroundAdmissionInputLifetimes.qualify(data,plan,currentInputFields);
        return new Plan(plan.fields(),plan.access(),plan.paused(),plan.polls(),plan.receivers(),plan.jobs(),plan.launches(),plan.persistent(),plan.unresolvedStarts(),plan.publishedReferences(),plan.ownedStatics(),plan.ownedChildren(),plan.globalPageContent(),inputLifetimes);
    }
    /** Exact composition order: narrow regions are inside their original host;
     * field witnesses begin at native accesses, and deferred poll gates sit
     * outside those accesses so closed polls cannot acquire a false witness. */
    static Map<String,byte[]> transformReaders(Prepare data,Map<String,byte[]> lower,Plan plan)throws Exception{
        if(plan.fields.contains(new FmcQualification.Member("NavigationDisplay","eiuIsPowered","Z")))throw new IllegalArgumentException("physical ND power must remain a current-only input, outside rendering read witnesses");
        var output=new HashMap<>(lower);output.putAll(FmcBackgroundAdmissionOwnership.transform(data,output,plan.fields).changed());
        output.putAll(FmcTerrainRoutePrepare.transform(data,output).changed());
        output.putAll(FmcBackgroundAdmissionRegionPrepare.transform(data,output));
        output.putAll(FmcRenderingPrepare.transform(data,output));
        output.putAll(FmcBackgroundAdmissionAfdsPrepare.transform(data,output));
        output.putAll(FmcBackgroundAdmissionCoreRequests.transform(data,output));
        output.putAll(FmcBackgroundAdmissionOptionPrepare.transform(data,output));
        output.putAll(FmcBackgroundAdmissionPacketPrepare.transform(data,output));
        output.putAll(FmcBackgroundAdmissionRenderingProducer.transform(data,output));
        var witnessed=new HashSet<>(plan.access.accesses());witnessed.addAll(plan.paused);
        for(var id:plan.access.initializers())if(!id.name().equals("<clinit>"))witnessed.add(id);
        // Region guards skip only the qualified calculation. Field witnesses
        // begin at substantive sites, so accesses elsewhere in the host still
        // hold their native method lifetime rather than escaping admission.
        witnessed.remove(FmcTerrainRoutePrepare.ORIGINAL);
        // The producer already owns an entry-to-retirement ticket. Its native
        // prefix runs on closed admission; a field witness there would revoke
        // the valid image interval before the qualified tail deferral.
        witnessed.remove(FmcBackgroundAdmissionRenderingProducer.METHOD);
        var dispatchOnly=new HashSet<>(FmcBackgroundAdmissionDispatchProjection.qualify(data,output,plan));if(!plan.ownedStatics().isEmpty())dispatchOnly.addAll(FmcBackgroundAdmissionRenderingProducer.argumentSites(data));
        output.putAll(FmcBackgroundAdmissionFieldPrepare.transform(data,output,plan.fields,witnessed,plan.paused,dispatchOnly));
        var aggregates=new HashSet<>(NETWORK_AGGREGATES);aggregates.addAll(BUFFER_PARAMETERS.keySet());aggregates.addAll(plan.inputLifetimes());aggregates.add(FmcBackgroundAdmissionCoreRequests.LOAD);
        output.putAll(FmcBackgroundAdmissionPrepare.transform(data,output,aggregates,BUFFER_PARAMETERS));
        // Native array membership is constructor-only; independent receiver
        // flow rejects escapes or newly introduced element replacements.
        FmcRouteArrayOwnership.qualify(data,Map.of());
        output.putAll(FmcBackgroundAdmissionBankPrepare.transform(data,output));
        var polls=new HashSet<>(plan.polls);polls.remove(FmcBackgroundAdmissionRenderingProducer.METHOD);var receivers=new HashMap<>(plan.receivers);receivers.remove(FmcBackgroundAdmissionRenderingProducer.METHOD);
        output.putAll(FmcExclusionPrepare.transform(data,output,polls,receivers));
        return Map.copyOf(output);
    }
}
