package psx.exporter;

import java.util.*;
import java.util.function.*;

/** Worker binding for the exact narrow member profile. Private module-checked
 * lookup is necessary for installed package-private owners. Native fields and
 * constructors are never reflected here. All write permissions are empty.
 * The preparation proof and coherent lifecycle source remain mandatory. */
final class FmcNarrowBindings implements FmcNarrowSource.Native {
    static final String[] OWNERS={"FmcRte$Rte","FmcRte$Rwy","FmcDepArr$DepArrSelections","FmcPerf","RouteWpt","Routes$RteBody","FmcRte","Routes"};
    private static final Set<Integer> REFERENCES=Set.of(1,2,3,4,5,6,7,8,9,10,11,35,36,37,45,52,53,54,55,56,58,60,61,62,63,64,65);
    private final FmcInitialization.Context context;
    private final FmcInitialization.Source lifecycle;
    private final LongSupplier generation,clock;
    private final BooleanSupplier qualification;
    private final FmcBridgeAccess[] bridges;
    private FmcNarrowBindings(FmcInitialization.Context context,FmcInitialization.Source lifecycle,
        LongSupplier generation,LongSupplier clock,BooleanSupplier qualification,FmcBridgeAccess[] bridges){
        this.context=context;this.lifecycle=lifecycle;this.generation=generation;this.clock=clock;this.qualification=qualification;this.bridges=bridges;
    }
    static int owner(int member){
        if(member<1||member>66)throw new IllegalArgumentException("FMC narrow member identifier");
        if(member<=3||member>=52&&member<=55||member==59)return 0;
        if(member==4)return 1;if(member<=13)return 2;if(member<=24)return 3;if(member<=51)return 4;
        if(member<=58)return 5;return member==60||member==62||member==63||member==66?6:7;
    }
    static boolean reference(int member){owner(member);return REFERENCES.contains(member);}
    static FmcNarrowBindings bind(FmcInitialization.Context context,FmcInitialization.Source lifecycle,
        LongSupplier generation,LongSupplier clock,BooleanSupplier qualification)throws ReflectiveOperationException {
        Objects.requireNonNull(context);Objects.requireNonNull(lifecycle);Objects.requireNonNull(generation);Objects.requireNonNull(clock);Objects.requireNonNull(qualification);
        if(!context.current()||!qualification.getAsBoolean())throw new IllegalStateException("FMC narrow binding qualification/lifecycle");
        var bridges=new FmcBridgeAccess[OWNERS.length];
        for(int index=0;index<bridges.length;index++){
            var raw=new ArrayList<Integer>();var refs=new ArrayList<Integer>();
            for(int member=1;member<=66;member++)if(owner(member)==index)(reference(member)?refs:raw).add(member);
            bridges[index]=FmcBridgeAccess.bind(context.owner(OWNERS[index]),raw.stream().mapToInt(Integer::intValue).toArray(),new int[0],
                refs.stream().mapToInt(Integer::intValue).toArray(),new int[0],()->context.current()&&qualification.getAsBoolean());
        }
        if(!context.current()||!qualification.getAsBoolean())throw new IllegalStateException("FMC narrow binding changed during resolution");
        return new FmcNarrowBindings(context,lifecycle,generation,clock,qualification,bridges);
    }
    public boolean qualified(){return context.current()&&qualification.getAsBoolean();}
    public long lifecycle(){return qualified()?lifecycle.qualifiedLifecycle():0;}
    public long generation(){return qualified()?generation.getAsLong():0;}
    public long qpc(){return clock.getAsLong();}
    public Object reference(Object receiver,int member)throws Throwable{
        if(!reference(member))throw new IllegalArgumentException("FMC narrow reference kind");return bridges[owner(member)].readRef(receiver,member);
    }
    public long bits(Object receiver,int member)throws Throwable{
        if(reference(member))throw new IllegalArgumentException("FMC narrow scalar kind");return bridges[owner(member)].readBits(receiver,member);
    }
}
