package psx.exporter;

import java.util.*;
import java.util.function.BooleanSupplier;

/** Worker binding for the complete prepared recipe. Service/journal scheduling
 * is deliberately separate. The original native EXEC seam invokes load; a
 * later qualified ordinary dispatcher invokes finish after all original jobs
 * retire. Neither method grants its own mutation or rollback permission. */
final class FmcStep12Bindings {
    private final FmcInitialization.Context context;private final FmcInitialization.Source lifecycleSource;
    private final FmcStep12Prepare.Result prepared;private final FmcReconstructionAccess.Recipe recipe;
    private final FmcReconstructionNavAccess navigation;private final FmcReconstructionPlan plan;
    private final Object[] inputRoots,radioRoots,pageRoots;private final BooleanSupplier qualified;
    private final FmcReconstructionPauseAccess pause;private int pauseMask;
    private final FmcReconstructionPreservationAccess preservation;
    private final FmcRenderingAccess rendering;private final FmcReconstructionNavigationAccess navigationDirectory;private final FmcBackgroundAdmissionAfdsAccess afds;
    private final FmcBackgroundAdmissionOptionAccess option;private final FmcBackgroundAdmissionSaveAccess save;private final FmcReconstructionTransitionAccess transitions;private boolean captured,loaded,propagated,finished;
    private FmcStep12Bindings(FmcInitialization.Context context,FmcInitialization.Source lifecycleSource,FmcStep12Prepare.Result prepared,
                             FmcReconstructionAccess.Recipe recipe,FmcReconstructionNavAccess navigation,FmcReconstructionPlan plan,
                             Object[] inputRoots,Object[] radioRoots,Object[] pageRoots,BooleanSupplier qualified,FmcReconstructionPauseAccess pause,FmcReconstructionPreservationAccess preservation,FmcRenderingAccess rendering,FmcReconstructionNavigationAccess navigationDirectory,FmcBackgroundAdmissionAfdsAccess afds,FmcReconstructionTransitionAccess transitions,FmcBackgroundAdmissionSaveAccess save,FmcBackgroundAdmissionOptionAccess option){this.context=context;this.lifecycleSource=lifecycleSource;this.prepared=prepared;this.recipe=recipe;this.navigation=navigation;this.plan=plan;this.inputRoots=inputRoots;this.radioRoots=radioRoots;this.pageRoots=pageRoots;this.qualified=qualified;this.pause=pause;this.preservation=preservation;this.rendering=rendering;this.navigationDirectory=navigationDirectory;this.afds=afds;this.transitions=transitions;this.save=save;this.option=option;}
    static FmcStep12Bindings bind(FmcInitialization.Context context,FmcInitialization.Source lifecycleSource,FmcNarrowSource.Native source,
                                 BooleanSupplier qualified,FmcPublication.Authority authority,FmcStep12Prepare.Result prepared,
                                 FmcReconstructionPlan plan,FmcNavigationCatalog catalog)throws Throwable{
        Objects.requireNonNull(prepared);Objects.requireNonNull(lifecycleSource);require(context.current()&&qualified.getAsBoolean()&&!FmcExclusion.admissionClosed(),"open worker binding");
        var navigationDirectory=FmcReconstructionNavigationAccess.bind(context,qualified,catalog);var cells=FmcReconstructionAccess.bind(context,source,qualified,authority);var headers=FmcReconstructionHeaderAccess.bind(context,qualified,authority,catalog);var controls=FmcReconstructionControlAccess.bind(context,qualified,authority);
        var recipe=cells.prepareRecipe(plan,headers,controls);var navigation=FmcReconstructionNavAccess.bind(context,qualified,authority);var pause=FmcReconstructionPauseAccess.bind(context);var preservation=FmcReconstructionPreservationAccess.bind(context,qualified);var transitions=FmcReconstructionTransitionAccess.bind(context,qualified);var save=FmcBackgroundAdmissionSaveAccess.bind(context);var afds=FmcBackgroundAdmissionAfdsAccess.bind(context,qualified);var rendering=FmcRenderingAccess.prepare(context,qualified);
        try{
        Object[] inputs=FmcBackgroundAdmissionInputRoots.read(context,prepared.currentRoots()),radios=navigation.receivers(),pages=FmcReconstructionDisplayAccess.readPages(context);
        var objects=new ArrayList<Object>();objects.addAll(Arrays.asList(recipe.ownedAliases()));objects.addAll(Arrays.asList(recipe.ownedBanks()));objects.addAll(Arrays.asList(inputs));objects.addAll(Arrays.asList(save.roots()));objects.add(transitions.receiver());objects.addAll(Arrays.asList(radios));objects.addAll(Arrays.asList(preservation.receivers()));objects.addAll(Arrays.asList(pages));objects.addAll(Arrays.asList(rendering.receivers()));
        // Static control title buffers are returned by the exact generated
        // worker bridge, rather than discovered with reflection/type scans.
        objects.addAll(Arrays.asList(FmcReconstructionControlAliases.read(context)));
        objects.addAll(Arrays.asList(FmcReconstructionExecRoots.read(context,prepared.execRoots(),List.of(
            source.reference(null,60),source.reference(null,plan.execution().replacement().target()==1?62:63)))));
        objects.addAll(Arrays.asList(FmcReconstructionDisplayAccess.read(context)));
        var unique=new IdentityHashMap<Object,Boolean>();var actual=new ArrayList<Object>();for(Object o:objects)if(o!=null&&unique.put(o,Boolean.TRUE)==null)actual.add(o);
        for(Object object:actual){requireReconstructionAlias(object);FmcReconstructionEiuSignalEffects.requireUnownedReceiver(object);}
        var option=FmcBackgroundAdmissionOptionAccess.bind(context,qualified,actual);
        var targets=new ArrayList<FmcBackgroundAdmissionOwnership.Target>();var aliases=new ArrayList<Object>();
        var initializedOwners=new HashMap<String,Class<?>>();
        for(Object object:actual)for(Class<?> type=object.getClass();type!=null;type=type.getSuperclass()){
            // An unused potential write owner (notably an operation-created
            // Thread) must not cause eager native initialization on the worker.
            // The actual object's declaring ancestry selects the exact owner.
            String owner=type.getName().replace('.','/');if(!prepared.ownership().owners().contains(owner))continue;
            Class<?> initialized=initializedOwners.get(owner);if(initialized==null){initialized=context.owner(type.getName());initializedOwners.put(owner,initialized);}
            require(initialized==type,"initialized actual metadata owner");targets.add(new FmcBackgroundAdmissionOwnership.Target(owner,object));
        }
        var pollOwners=new HashSet<String>();for(var receiver:prepared.admission().receivers().values()){String descriptor=receiver.descriptor();if(descriptor.startsWith("L")&&descriptor.endsWith(";"))pollOwners.add(descriptor.substring(1,descriptor.length()-1));}
        // Resolve only actual declaring ancestry, preserving initialized-class
        // evidence without loading unused native page/poll classes. Future
        // operation pages use their exact inherited metadata scope tags.
        for(Object object:actual){boolean selected=object instanceof StringBuilder;for(Class<?> type=object.getClass();type!=null;type=type.getSuperclass())if(pollOwners.contains(type.getName().replace('.','/'))){if(!type.getName().startsWith("java."))require(context.owner(type.getName())==type,"initialized actual poll owner");selected=true;}if(selected)aliases.add(object);}
        require(context.current()&&qualified.getAsBoolean(),"binding lifecycle changed");FmcBackgroundAdmissionOwnership.bind(context,prepared.ownership(),targets,aliases);
        FmcNavigationOutputs.bind(lifecycleSource.qualifiedLifecycle(),radios[0],radios[6]);return new FmcStep12Bindings(context,lifecycleSource,prepared,recipe,navigation,plan,inputs,radios,pages,qualified,pause,preservation,rendering,navigationDirectory,afds,transitions,save,option);
        }catch(Throwable original){try{rendering.release();}catch(Throwable release){original.addSuppressed(release);}throw original;}
    }
    /** The later service worker retries only image readiness before closing
     * admission. No native callback waits for capture or image publication. */
    void armRendering()throws Throwable{require(!captured&&context.current()&&qualified.getAsBoolean()&&!FmcExclusion.admissionClosed(),"renderer arming before closure");save.verify();option.verify();rendering.arm();try{afds.arm();}catch(Throwable failure){try{rendering.release();}catch(Throwable cleanup){failure.addSuppressed(cleanup);}throw failure;}}
    /** The service worker calls this only once admission is closed and original
     * finite jobs/consumer methods are quiescent. This is read-only preimage
     * capture; it cannot arm a load with a replaced current receiver. */
    void capturePreservation(long token)throws Throwable{require(!captured&&context.current()&&qualified.getAsBoolean()&&FmcExclusion.quiescent(token),"closed quiescent preimage");save.verify();option.verify();rendering.verify();navigationDirectory.verifyCatalog();afds.verifyPreimage();transitions.verifyPreimage();pauseMask=pause.ordinaryMask();same(FmcBackgroundAdmissionInputRoots.read(context,prepared.currentRoots()),inputRoots);same(navigation.receivers(),radioRoots);same(FmcReconstructionDisplayAccess.readPages(context),pageRoots);recipe.verifyPreimage();navigation.capture();navigation.installReleasePrerequisite(token);preservation.capture();pause.requireMask(pauseMask);require(FmcExclusion.quiescent(token),"closed Pause/input proof changed");captured=true;}
    void load(long token,int logicalSide)throws Throwable{require(captured&&!loaded&&context.current()&&qualified.getAsBoolean()&&FmcExclusion.commandActive(token)&&logicalSide==plan.execution().logicalSide(),"original EXEC load authority/side");save.verify();option.verify();rendering.verify();navigationDirectory.verify();pause.requireMask(pauseMask);preservation.verify();same(FmcReconstructionDisplayAccess.readPages(context),pageRoots);same(FmcBackgroundAdmissionInputRoots.read(context,prepared.currentRoots()),inputRoots);afds.verifyPreimage();transitions.verifyPreimage();FmcExecSeam.requireFinishing(token);recipe.load();loaded=true;}
    /** Native EXEC and its children retire before a separate EDT propagation.
     * The complete native control reader may create further original jobs;
     * these must retire before final reconciliation and acknowledgement. */
    void propagate(long token)throws Throwable{try{propagatePrepared(token);}catch(Throwable original){FmcExecSeam.failed(token,original);throw original;}}
    private void propagatePrepared(long token)throws Throwable{
        require(loaded&&!propagated&&!finished&&context.current()&&qualified.getAsBoolean()&&FmcExclusion.commandQuiescent(token),"post-EXEC propagation authority");
        var request=FmcExecSeam.enterFinisher(token,FmcExecSeam.FinishStage.PROPAGATE);Throwable failure=null;
        try{save.verify();option.verify();rendering.verify();navigationDirectory.verify();pause.requireMask(pauseMask);preservation.verify();same(FmcBackgroundAdmissionInputRoots.read(context,prepared.currentRoots()),inputRoots);recipe.propagate();require(context.current()&&qualified.getAsBoolean(),"propagation lifecycle changed");propagated=true;}
        catch(Throwable original){failure=original;throw original;}
        finally{FmcExecSeam.leaveFinisher(request,FmcExecSeam.FinishStage.PROPAGATE,failure);}
    }
    /** A second ordinary EDT turn follows retirement of all propagation jobs.
     * Current navigation is restored only after their native effects finish. */
    void finish(long token)throws Throwable{try{finishPrepared(token);}catch(Throwable original){FmcExecSeam.failed(token,original);throw original;}}
    private void finishPrepared(long token)throws Throwable{
        require(loaded&&propagated&&!finished&&context.current()&&qualified.getAsBoolean()&&FmcExclusion.commandQuiescent(token),"post-propagation reconciliation authority");
        var request=FmcExecSeam.enterFinisher(token,FmcExecSeam.FinishStage.RECONCILE);Throwable failure=null;
        try{save.verify();option.verify();rendering.verify();navigationDirectory.verify();pause.requireMask(pauseMask);preservation.verify();same(FmcBackgroundAdmissionInputRoots.read(context,prepared.currentRoots()),inputRoots);recipe.finish();navigation.restore();navigation.verify();preservation.verify();rendering.verify();navigationDirectory.verify();pause.requireMask(pauseMask);require(context.current()&&qualified.getAsBoolean(),"post-EXEC lifecycle changed");afds.complete();finished=true;}
        catch(Throwable original){failure=original;throw original;}
        finally{FmcExecSeam.leaveFinisher(request,FmcExecSeam.FinishStage.RECONCILE,failure);}
    }
    /** Worker prerequisite before successful reopen; even final reconciliation
     * cannot let an original operation child outlive the closed interval. */
    boolean readyToReopen(long token){require(finished&&context.current()&&qualified.getAsBoolean(),"finished same-lifecycle state");return FmcExclusion.quiescent(token);}
    /** Mandatory terminal prerequisite on the later service worker, after admission reopens. */
    boolean reconciledAfterReopen(){require(finished&&!FmcExclusion.admissionClosed(),"complete NEW state and reopened admission");return afds.reconciled();}
    FmcBackgroundAdmissionFaults.Result failures(ClassLoader loader,long token)throws Throwable{return FmcBackgroundAdmissionFaults.scan(loader,prepared.faults(),lifecycleSource,()->FmcExclusion.closedAndRetired(token));}
    void release()throws Throwable{require(!FmcExclusion.admissionClosed(),"terminal admission must reopen first");if(finished&&FmcExclusion.available())require(afds.reconciled(),"native AFDS reconciliation must retire before successful release");Throwable failure=null;try{afds.release();}catch(Throwable cleanup){failure=cleanup;}try{FmcNavigationOutputs.clear();}catch(Throwable cleanup){if(failure==null)failure=cleanup;else failure.addSuppressed(cleanup);}try{rendering.release();}catch(Throwable cleanup){if(failure==null)failure=cleanup;else failure.addSuppressed(cleanup);}if(failure!=null)throw failure;}
    /** Ordinary EXEC pack-selector/caution banks stay native outputs. Actual
     * retained Qh objects, including typed arrays and subclasses, cannot enter
     * the reconstruction ownership set under that negative receiver premise. */
    static void requireReconstructionAlias(Object object){Class<?> type=Objects.requireNonNull(object).getClass();while(type.isArray())type=type.getComponentType();for(;type!=null;type=type.getSuperclass())require(!type.getName().equals("Qh"),"native Qh output cannot be a reconstruction-owned alias");}
    private static void same(Object[] actual,Object[] expected){require(actual.length==expected.length,"current root roster");for(int i=0;i<actual.length;i++)require(actual[i]==expected[i],"current root identity replaced");}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC steps1/2 binding "+why);}
}
