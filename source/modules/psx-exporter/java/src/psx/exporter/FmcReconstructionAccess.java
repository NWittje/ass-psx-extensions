package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Worker resolution and ordinary-EXEC route-cell publication. Native fields
 * are read only through qualified generated members. No field reflection,
 * native initializer, bank-consuming transfer or vendor lookup is used here.
 * The operation supplies its full initialization/lifecycle/admission authority;
 * resolving a kernel alone never grants a recall capability. */
final class FmcReconstructionAccess {
    record Route(Object header,Object body,Object[] storageRoot,Object[] storage,int count){
        Route(Object header,Object body,Object[] storage,int count){this(header,body,storage,storage,count);}
        Route{Objects.requireNonNull(storageRoot);storage=storage.clone();}public Object[] storage(){return storage.clone();}
    }
    private final FmcInitialization.Context context;
    private final FmcNarrowSource.Native source;
    private final BooleanSupplier qualified;
    private final FmcPublication.Authority authority;
    private final MethodHandle reset,entered,count,currentPosition;
    private final Object[] freshDefaultCells;
    private FmcReconstructionAccess(FmcInitialization.Context context,FmcNarrowSource.Native source,BooleanSupplier qualified,
                                    FmcPublication.Authority authority,MethodHandle reset,MethodHandle entered,MethodHandle count,MethodHandle currentPosition,Object[] freshDefaultCells){
        this.context=context;this.source=source;this.qualified=qualified;this.authority=authority;this.reset=reset;this.entered=entered;this.count=count;this.currentPosition=currentPosition;this.freshDefaultCells=freshDefaultCells;
    }
    private static void require(boolean yes,String reason){if(!yes)throw new IllegalStateException("FMC reconstruction access "+reason);}
    private static MethodHandle method(Class<?> owner,String name,MethodType type)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,type.parameterArray());require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==type.returnType(),"kernel flags/descriptor");
        return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,type);
    }
    private static MethodHandle instance(Class<?> owner,String name,MethodType type)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,type.parameterArray());require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&!Modifier.isStatic(m.getModifiers())&&m.getReturnType()==type.returnType(),"instance kernel flags/descriptor");
        var parameters=new Class<?>[type.parameterCount()+1];parameters[0]=Object.class;for(int i=0;i<type.parameterCount();i++)parameters[i+1]=type.parameterType(i);
        return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findVirtual(owner,name,type).asType(MethodType.methodType(type.returnType(),parameters));
    }
    /** Trusted worker-bound header/control publishers. Implementations resolve
     * generated members from the initialization context and detached catalog;
     * neither receivers nor executable callbacks come from artifact bytes. */
    interface Headers {
        void validate(FmcReconstructionPlan plan,Route target,Route mod)throws Throwable;
        void verifyPreimage(Route route)throws Throwable;
        Object[] banks(Route route)throws Throwable;
        void load(FmcReconstructionPlan plan,Route route)throws Throwable;
        void copyCurrentForecast(Route mod,Route target)throws Throwable;
        default void reconcile(Route route)throws Throwable{}
        default Object[] ownedAliases()throws Throwable{return new Object[0];}
    }
    interface Controls {
        Object[] banks()throws Throwable;
        void validate(FmcNarrowControl.Execution execution)throws Throwable;
        void verifyPreimage()throws Throwable;
        void load(FmcNarrowControl.State state)throws Throwable;void propagate(int nativeCount)throws Throwable;
    }
    static final class Recipe {
        private final FmcReconstructionAccess cells;
        private final Headers headers;private final Controls controls;private final FmcReconstructionCruiseAccess cruise;private final FmcReconstructionOffsetAccess offset;
        private final MethodHandle bodyBank,serialize,reconstruct,ndRefresh,copyForecast,courseEntered,courseExec,dynamicDirect,dynamicCourse,geometry,groundAnchor;
        private final MethodHandle bodyReset;private final Object bodyDefaults;
        private final MethodHandle bodyAliases,safety,waypointForecast,windAliases;
        private final MethodHandle perf,perfBanks,ballast,pending,ack,qiValue,qsValue,qiAccepts,qsAccepts,qiFlush,qsFlush,takeoffText,navTuningDrain;
        private final FmcReconstructionPlan plan;private final Route destination,mod,inactive;private final Object[] banks,inactiveBanks;private final String[] inactiveText;private final long[] perfInputs;
        private boolean loaded,propagated,finished;
        Recipe(FmcReconstructionAccess cells,Headers headers,Controls controls,FmcReconstructionPlan plan)throws Throwable{
            this.cells=cells;this.headers=Objects.requireNonNull(headers);this.controls=Objects.requireNonNull(controls);this.plan=Objects.requireNonNull(plan);
            var context=cells.context;Class<?> body=context.owner("Routes$RteBody"),performance=context.owner("FmcPerf"),q=context.owner("Q"),qi=context.owner("Qi"),qs=context.owner("Qs");
            var parserOptions=method(body,FmcReconstructionBodyPrepare.OPTIONS,MethodType.methodType(int[].class));FmcReconstructionDomain.currentParser(plan,(int[])parserOptions.invokeExact());
            bodyBank=method(body,FmcReconstructionBodyPrepare.BANK,MethodType.methodType(Object.class,Object.class));
            bodyAliases=method(body,FmcReconstructionBodyPrepare.ALIASES,MethodType.methodType(Object[].class,Object.class));safety=method(body,FmcReconstructionBodyPrepare.SAFETY,MethodType.methodType(Object.class));
            waypointForecast=method(context.owner("RouteWpt"),FmcReconstructionBodyPrepare.WPT_FORECAST,MethodType.methodType(Object.class,Object.class));windAliases=method(context.owner("FmcWindOat"),FmcReconstructionBodyPrepare.WIND_ALIASES,MethodType.methodType(Object[].class,Object.class));
            ndRefresh=method(body,FmcReconstructionBodyPrepare.ND,MethodType.methodType(void.class,Object.class));serialize=instance(body,FmcReconstructionBodyPrepare.SEND,MethodType.methodType(void.class,int.class));reconstruct=instance(body,FmcReconstructionBodyPrepare.READ,MethodType.methodType(void.class,String.class));
            copyForecast=method(body,FmcReconstructionBodyPrepare.FORECAST,MethodType.methodType(void.class,Object.class,Object.class));
            courseEntered=instance(body,"$psxExporter$fmcReconstructEnteredCourse",MethodType.methodType(void.class,int.class));courseExec=instance(body,"$psxExporter$fmcReconstructCourseFrom",MethodType.methodType(void.class));
            dynamicDirect=instance(body,"$psxExporter$fmcReconstructDynamicDirect",MethodType.methodType(void.class));dynamicCourse=instance(body,"$psxExporter$fmcReconstructDynamicCourse",MethodType.methodType(void.class));
            geometry=instance(body,"$psxExporter$fmcCalculateRouteGeometry",MethodType.methodType(void.class,int.class));
            groundAnchor=instance(body,"$psxExporter$fmcReconstructGroundAnchor",MethodType.methodType(void.class,int.class));
            bodyReset=method(body,FmcReconstructionBodyDefaults.RESET,MethodType.methodType(void.class,Object.class,Object.class));
            for(String owner:FmcFreshRoutePrepare.OWNERS)context.owner(owner);
            var freshBody=method(body,FmcReconstructionBodyDefaults.FACTORY,MethodType.methodType(Object.class));bodyDefaults=(Object)freshBody.invokeExact();require(body.isInstance(bodyDefaults),"private pure native body defaults");
            perf=method(performance,FmcReconstructionPerfPrepare.NAME,MethodType.methodType(void.class,long[].class));perfBanks=method(performance,FmcReconstructionPerfPrepare.BANKS,MethodType.methodType(Object[].class));ballast=method(performance,FmcReconstructionPerfPrepare.BALLAST,MethodType.methodType(int.class));
            takeoffText=method(context.owner(FmcReconstructionTakeoffDisplayPrepare.OWNER),FmcReconstructionTakeoffDisplayPrepare.REFRESH,MethodType.methodType(void.class));
            navTuningDrain=method(context.owner("NavRadios"),FmcBackgroundAdmissionNavTuningPrepare.DRAIN,MethodType.methodType(void.class));
            pending=method(q,FmcReconstructionBanks.PENDING,MethodType.methodType(boolean.class,Object.class));ack=method(q,FmcReconstructionBanks.ACK,MethodType.methodType(void.class,Object.class));
            qiValue=method(qi,FmcReconstructionBanks.VALUE,MethodType.methodType(int.class,Object.class));qsValue=method(qs,FmcReconstructionBanks.VALUE,MethodType.methodType(String.class,Object.class));
            qiAccepts=method(qi,FmcReconstructionBanks.ACCEPTS,MethodType.methodType(boolean.class,Object.class,int.class));qsAccepts=method(qs,FmcReconstructionBanks.ACCEPTS,MethodType.methodType(boolean.class,Object.class,String.class));
            qiFlush=method(qi,FmcReconstructionBanks.FLUSH,MethodType.methodType(void.class,Object.class));qsFlush=method(qs,FmcReconstructionBanks.FLUSH,MethodType.methodType(void.class,Object.class));
            int target=plan.execution().replacement().target();destination=cells.read(target);mod=cells.read(0);inactive=cells.read(3-target);
            headers.validate(plan,destination,mod);controls.validate(plan.execution());
            var selected=new ArrayList<Object>();for(Route route:List.of(destination,mod)){Object[] headerBanks=headers.banks(route);Object routeBank=bodyBank.invokeExact(route.body);
                require(headerBanks.length==5&&headerBanks[0]==routeBank,"native shared header/body bank role");selected.addAll(Arrays.asList(headerBanks));}
            cruise=FmcReconstructionCruiseAccess.bind(context,cells.qualified,cells.authority,(int)plan.content().perfBits(4),mod.body);
            offset=FmcReconstructionOffsetAccess.bind(context,cells.qualified,cells.authority,destination.body,mod.body,inactive.body);
            Object[] performanceBanks=(Object[])perfBanks.invokeExact();require(performanceBanks.length==3,"exact native PERF bank count");selected.addAll(Arrays.asList(performanceBanks));selected.addAll(Arrays.asList(controls.banks()));selected.addAll(Arrays.asList(cruise.banks()));
            var unique=new IdentityHashMap<Object,Boolean>();for(Object bank:selected)require(bank!=null&&(qi.isInstance(bank)||qs.isInstance(bank))&&unique.put(bank,Boolean.TRUE)==null,"exact distinct selected native banks");banks=selected.toArray();inactiveBanks=headers.banks(inactive);require(inactiveBanks.length==5,"complete inactive header/body/selection input banks");inactiveText=new String[5];for(int i=0;i<5;i++){Object bank=inactiveBanks[i];require(qs.isInstance(bank)&&unique.put(bank,Boolean.TRUE)==null&&!(boolean)pending.invokeExact(bank),"distinct settled inactive input bank");inactiveText[i]=(String)qsValue.invokeExact(bank);require(inactiveText[i]!=null,"inactive bank text");}
            perfInputs=new long[11];for(int i=0;i<11;i++)perfInputs[i]=plan.content().perfBits(i+1);
            String performanceText=FmcReconstructionPerfPrepare.bank(perfInputs,(int)ballast.invokeExact());
            require((boolean)qsAccepts.invokeExact(performanceBanks[0],performanceText)&&(boolean)qiAccepts.invokeExact(performanceBanks[1],(int)perfInputs[6])&&(boolean)qiAccepts.invokeExact(performanceBanks[2],(int)perfInputs[7]),"PERF bank limits before mutation");
            for(Route route:List.of(destination,mod)){
                Object bank=bodyBank.invokeExact(route.body);String value=(String)qsValue.invokeExact(bank);require(value!=null&&value.indexOf('#')>0,"current native route prefix before mutation");
            }
            for(Object bank:banks)require(!(boolean)pending.invokeExact(bank),"pending native bank input before mutation");cells.check();
        }
        Object[] ownedBanks(){return banks.clone();}Object[] inputBanks(){return inactiveBanks.clone();}
        Object[] ownedAliases()throws Throwable{
            require(cells.context.current()&&cells.qualified.getAsBoolean(),"alias binding lifecycle");var objects=new ArrayList<Object>();
            objects.addAll(Arrays.asList(inactiveBanks));objects.add(inactive.header);objects.addAll(Arrays.asList(headers.ownedAliases()));objects.addAll(Arrays.asList(cruise.aliases()));objects.addAll(Arrays.asList(offset.aliases()));objects.add(destination.header);objects.add(mod.header);
            Object safetyBody=(Object)safety.invokeExact();require(safetyBody!=null&&cells.context.owner("Routes$RteBody").isInstance(safetyBody),"actual native safety cache root");
            for(Object body:List.of(destination.body,mod.body,safetyBody)){objects.add(body);for(Object value:(Object[])bodyAliases.invokeExact(body))if(value!=null){objects.add(value);if(value instanceof Object[] rows)objects.addAll(Arrays.asList(rows));}}
            var snapshot=new ArrayList<>(objects);for(Object object:snapshot)if(object!=null&&cells.context.owner("RouteWpt").isInstance(object))objects.add(waypointForecast.invokeExact(object));
            snapshot=new ArrayList<>(objects);for(Object object:snapshot)if(object!=null&&cells.context.owner("FmcWindOat").isInstance(object)){
                Object[] aliases=(Object[])windAliases.invokeExact(object);require(aliases.length==5&&aliases[0] instanceof Object[]&&((Object[])aliases[0]).length==4,"current native forecast cells");objects.addAll(Arrays.asList(aliases));
            }
            var unique=new IdentityHashMap<Object,Boolean>();var result=new ArrayList<Object>();for(Object object:objects)if(object!=null&&unique.put(object,Boolean.TRUE)==null)result.add(object);
            require(cells.context.current()&&cells.qualified.getAsBoolean(),"alias binding lifecycle changed");return result.toArray();
        }
        void verifyPreimage()throws Throwable{
            require(!loaded&&!finished&&cells.context.current()&&cells.source.qualified()&&cells.qualified.getAsBoolean(),"preimage lifecycle/order");
            int target=plan.execution().replacement().target();cells.same(destination,target);cells.same(mod,0);cells.same(inactive,3-target);
            verifyInactiveInputs();headers.verifyPreimage(destination);headers.verifyPreimage(mod);controls.verifyPreimage();cruise.verifyPreimage();offset.verifyPreimage();
            var current=new ArrayList<Object>();for(Route route:List.of(destination,mod))current.addAll(Arrays.asList(headers.banks(route)));
            current.addAll(Arrays.asList((Object[])perfBanks.invokeExact()));current.addAll(Arrays.asList(controls.banks()));current.addAll(Arrays.asList(cruise.banks()));
            require(current.size()==banks.length,"bound publication bank roster");for(int i=0;i<banks.length;i++)require(current.get(i)==banks[i]&&!(boolean)pending.invokeExact(banks[i]),"publication bank replaced or became pending");
            require(cells.context.current()&&cells.source.qualified()&&cells.qualified.getAsBoolean(),"preimage lifecycle changed");
        }
        private void verifyInactiveInputs()throws Throwable{Object[] now=headers.banks(inactive);require(now.length==5,"inactive input bank roster");for(int i=0;i<5;i++)require(now[i]==inactiveBanks[i]&&!(boolean)pending.invokeExact(now[i])&&inactiveText[i].equals((String)qsValue.invokeExact(now[i])),"inactive entered input bank changed or pending");}
        void load()throws Throwable{
            require(!loaded&&!finished,"recipe cannot replay");cells.check();verifyPreimage();
            cells.publishCells(plan,destination,mod,plan.execution().replacement().target());
            for(Route route:List.of(destination,mod)){cells.check();bodyReset.invokeExact(route.body,bodyDefaults);}
            for(Route route:List.of(destination,mod)){cells.check();headers.load(plan,route);}
            cells.check();headers.copyCurrentForecast(mod,destination);cells.check();copyForecast.invokeExact(mod.body,destination.body);
            cells.check();perf.invokeExact(perfInputs);
            for(Route route:List.of(destination,mod)){
                if(plan.navigation().departure()!=null){int runway=plan.navigation().departureRunways().indexOf(plan.navigation().departure());require(runway>=0&&runway<30,"trusted current departure runway index");cells.check();groundAnchor.invokeExact(route.body,runway);}
                int index=plan.courseIndex();if(index>=0){int course=(int)plan.row(index).bits()[15];cells.check();courseEntered.invokeExact(route.body,course);}
                cells.check();dynamicDirect.invokeExact(route.body);cells.check();dynamicCourse.invokeExact(route.body);
                reconcile(route);
            }
            cells.check();offset.cancel();cells.check();cruise.load(mod.body);for(Route route:List.of(destination,mod)){cells.check();ndRefresh.invokeExact(route.body);}cells.check();controls.load(plan.execution().beforeExec());cells.check();cells.same(inactive,plan.execution().replacement().inactive());loaded=true;
        }
        private void reconcile(Route route)throws Throwable{
            cells.check();serialize.invokeExact(route.body,747400);Object bank=bodyBank.invokeExact(route.body);String text=(String)qsValue.invokeExact(bank);
            cells.check();reconstruct.invokeExact(route.body,text);
            cells.check();geometry.invokeExact(route.body,-1);
            cells.check();headers.reconcile(route);
            // The parser derives turns/holds/current labels; serialize those
            // exact cells for future native consumers, without importing an old
            // bank or consuming the bank's freshness.
            cells.check();serialize.invokeExact(route.body,747400);
        }
        /** A separately authorized ordinary native dispatcher action after
         * EXEC and its operation-owned delayed work have retired. The closed
         * same-lifecycle gate remains held; the original job is not kept alive
         * merely to wait for its children or perform this final publication. */
        /** Complete native activation propagation in its own authorized action.
         * Its original UI children retire before the final reconciliation. */
        void propagate()throws Throwable{require(loaded&&!propagated&&!finished,"post-EXEC propagation order");cells.check();verifyInactiveInputs();controls.propagate(Math.max(3,plan.waypoints()+2));cells.check();propagated=true;}
        void finish()throws Throwable{
            require(loaded&&propagated&&!finished,"native propagation children must retire before reconciliation");cells.check();verifyInactiveInputs();for(Route route:List.of(destination,mod))reconcile(route);
            cells.check();cells.same(inactive,plan.execution().replacement().inactive());
            offset.verifyFinished();for(Route route:List.of(destination,mod)){cells.check();ndRefresh.invokeExact(route.body);}
            // Retire any latest-state native procedure-tuning request before
            // publication. Manual tuning is restored by the outer finisher
            // after this native effect; a failed request cannot reach ACK.
            cells.check();navTuningDrain.invokeExact();cells.check();
            // A physical lift-off may have deferred its one-shot weight/CG
            // formatting while entered PERF data was incomplete. Recompute
            // that native cached text only after coherent route reconciliation.
            cells.check();takeoffText.invokeExact();cells.check();
            for(Object bank:banks){cells.check();if(cells.context.owner("Qi").isInstance(bank))qiFlush.invokeExact(bank);else qsFlush.invokeExact(bank);}
            for(Object bank:banks){cells.check();ack.invokeExact(bank);}cells.check();finished=true;
        }
    }
    Recipe prepareRecipe(FmcReconstructionPlan plan,Headers headers,Controls controls)throws Throwable{return new Recipe(this,headers,controls,plan);}
    static FmcReconstructionAccess bind(FmcInitialization.Context context,FmcNarrowSource.Native source,BooleanSupplier qualified,
                                      FmcPublication.Authority authority)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(source);Objects.requireNonNull(qualified);Objects.requireNonNull(authority);
        require(context.current()&&source.qualified()&&qualified.getAsBoolean(),"worker qualification/lifecycle");
        Class<?> waypoint=context.owner("RouteWpt"),body=context.owner("Routes$RteBody");
        var reset=method(waypoint,FmcReconstructionPrepare.RESET,MethodType.methodType(void.class,Object.class,Object.class));
        var entered=method(waypoint,FmcReconstructionPrepare.ENTERED,MethodType.methodType(void.class,Object.class,long[].class,String[].class));
        var count=method(body,FmcReconstructionPrepare.COUNT,MethodType.methodType(void.class,Object.class,int.class));
        var position=method(body,FmcReconstructionPrepare.CURRENT_POSITION,MethodType.methodType(void.class,Object.class,int.class));
        // Every reachable native constructor must already be initialized. This
        // factory is worker-only and its inputs cannot originate in FMC2.
        context.owner("FmcWindOat");context.owner("FmcWindOat$WindOatAlt");
        var factory=method(waypoint,FmcReconstructionPrepare.DEFAULTS,MethodType.methodType(Object[].class));
        Object[] defaults=(Object[])factory.invokeExact();
        require(context.current()&&source.qualified()&&qualified.getAsBoolean(),"changed during worker resolution");
        return new FmcReconstructionAccess(context,source,qualified,authority,reset,entered,count,position,defaults);
    }
    private void check()throws Throwable{require(context.current()&&source.qualified()&&qualified.getAsBoolean(),"qualification/lifecycle replaced");authority.check();}
    Route read(int bank)throws Throwable{
        require(bank>=0&&bank<=2,"trusted bank identifier");require(context.current()&&source.qualified()&&qualified.getAsBoolean(),"source lifecycle");
        Object header=source.reference(null,bank==0?60:bank==1?62:63),body=source.reference(null,bank==0?61:bank==1?64:65);
        require(header!=null&&body!=null&&context.owner("FmcRte$Rte").isInstance(header)&&context.owner("Routes$RteBody").isInstance(body),"exact live root types");
        require(source.reference(header,54)==body&&source.reference(body,58)==header&&source.bits(header,59)==bank,"header/body/id aliases");
        Object storage=source.reference(body,56);require(storage instanceof Object[]&&((Object[])storage).length==253,"native waypoint storage");
        int count=(int)source.bits(body,57);require(count>=3&&count<=253,"native count");
        Object[] cells=((Object[])storage).clone();var unique=new IdentityHashMap<Object,Boolean>();
        for(Object cell:cells)require(cell!=null&&context.owner("RouteWpt").isInstance(cell)&&unique.put(cell,Boolean.TRUE)==null,"exact distinct native waypoint cells");
        require(context.current()&&source.qualified()&&source.reference(body,56)==storage,"source changed while binding");return new Route(header,body,(Object[])storage,cells,count);
    }
    private void same(Route expected,int bank)throws Throwable{
        var actual=read(bank);require(actual.header()==expected.header()&&actual.body()==expected.body()&&actual.storageRoot()==expected.storageRoot()&&actual.count()==expected.count(),"live root/storage/count preimage changed");
        for(int i=0;i<253;i++)require(actual.storage[i]==expected.storage[i],"live waypoint alias changed");
    }
    /** Defaults originate only in the private worker-bound qualified factory;
     * no operation/artifact can provide or replace them during publication. */
    void publishCells(FmcReconstructionPlan plan,Route destination,Route mod,int target)throws Throwable{
        Objects.requireNonNull(plan);Objects.requireNonNull(destination);Objects.requireNonNull(mod);
        require(target==plan.execution().replacement().target()&&target>=1&&target<=2&&freshDefaultCells.length==253,"trusted target/fresh capacity");
        check();same(destination,target);same(mod,0);var inactive=read(3-target);
        var liveCells=new IdentityHashMap<Object,Boolean>();
        for(Route route:List.of(destination,mod,inactive))for(Object cell:route.storage)require(liveCells.put(cell,Boolean.TRUE)==null,"cross-bank waypoint alias");
        var freshUnique=new IdentityHashMap<Object,Boolean>();
        for(Object cell:freshDefaultCells)require(cell!=null&&context.owner("RouteWpt").isInstance(cell)&&!liveCells.containsKey(cell)&&freshUnique.put(cell,Boolean.TRUE)==null,"private distinct fresh constructor cells");
        // Resolve all clearing and loaded vectors before the first live write.
        long[][] raw=new long[253][27];String[][] text=new String[253][27];
        for(int index=0;index<253;index++)for(int field=0;field<27;field++){
            int member=25+field;boolean reference=FmcNarrowContent.WAYPOINT_INPUTS.get(field).descriptor().equals("Ljava/lang/String;");
            if(reference){Object value=source.reference(freshDefaultCells[index],member);require(value instanceof String,"fresh native text");text[index][field]=(String)value;}
            else raw[index][field]=source.bits(freshDefaultCells[index],member);
        }
        for(int index=0;index<plan.waypoints();index++){var row=plan.row(index);raw[index+2]=row.bits();text[index+2]=row.text();}
        for(Route route:List.of(destination,mod)){
            for(int index=0;index<253;index++){
                check();reset.invokeExact(route.storage[index],freshDefaultCells[index]);
                check();entered.invokeExact(route.storage[index],raw[index],text[index]);
            }
            check();count.invokeExact(route.body,Math.max(3,plan.waypoints()+2));
            for(int index=0;index<2;index++){check();currentPosition.invokeExact(route.body,index);}
            for(int index=0;index<plan.waypoints();index++)if(plan.row(index).coordinate()==FmcReconstructionPlan.Coordinate.CURRENT_PPOS){
                check();currentPosition.invokeExact(route.body,index+2);
            }
        }
        check();same(inactive,3-target);
    }
}
