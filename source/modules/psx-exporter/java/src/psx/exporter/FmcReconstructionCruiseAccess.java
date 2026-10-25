package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Worker-bound ACTIVE/MOD prediction preimages. Only cruise altitude in feet
 * and the pilot's cancelled step change; both banks rebase on the current
 * ACTIVE speed/forecast suffix before native EXEC copies MOD back to ACTIVE.
 * Resolve and reject pending/replaced banks before any
 * route publication, then use the ordinary EXEC thread's mutation authority. */
final class FmcReconstructionCruiseAccess {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final FmcPublication.Authority authority;
    private final MethodHandle roots,modBody,state,raw,text,aliases,bank,rebase,rta,rtaResult,invalidatePoints,load,value,pending;private final Object[] bound;private final Object boundModBody;
    private final int[][] states;private final long[][] cache;private final String[][] texts;private final Object[][] children;
    private final String[] preimages;private final Set<String> candidates;
    private final int altitude;
    private FmcReconstructionCruiseAccess(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,int altitude,Object newModBody)throws Throwable{
        this.context=context;this.qualified=qualified;this.authority=authority;this.altitude=altitude;
        roots=method(context.owner("FmcVnav"),FmcReconstructionCruisePrepare.ROOTS,Object[].class);
        modBody=method(context.owner("FmcVnav"),FmcReconstructionCruisePrepare.MOD_BODY,Object.class);boundModBody=(Object)modBody.invokeExact();require(boundModBody==newModBody,"trusted current MOD body root");
        state=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.STATE,int[].class,Object.class);
        raw=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.RAW,long[].class,Object.class);
        text=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.TEXT,String[].class,Object.class);
        aliases=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.ALIASES,Object[].class,Object.class);
        bank=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.BANK,String.class,Object.class);
        rebase=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.REBASE,void.class,Object.class,Object.class);
        rta=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.RTA,void.class,Object.class,Object.class);
        rtaResult=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.RTA_RESULT,void.class,Object.class,Object.class);
        invalidatePoints=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.INVALIDATE_POINTS,void.class,Object.class);
        load=method(context.owner("FmcVnav$Pred"),FmcReconstructionCruisePrepare.LOAD,void.class,Object.class,Object.class,int.class,String.class);
        value=method(context.owner("Qs"),FmcReconstructionBanks.VALUE,String.class,Object.class);pending=method(context.owner("Q"),FmcReconstructionBanks.PENDING,boolean.class,Object.class);
        var accepts=method(context.owner("Qs"),FmcReconstructionBanks.ACCEPTS,boolean.class,Object.class,String.class);
        bound=(Object[])roots.invokeExact();require(bound.length==4&&bound[0]!=bound[1]&&bound[2]!=bound[3],"distinct ACTIVE/MOD roots/banks");
        states=new int[2][];cache=new long[2][];texts=new String[2][];children=new Object[2][];preimages=new String[2];
        var unique=new IdentityHashMap<Object,Boolean>();
        for(int i=0;i<2;i++){
            require(context.owner("FmcVnav$Pred").isInstance(bound[i])&&context.owner("Qs").isInstance(bound[i+2]),"exact prediction receiver types");
            states[i]=(int[])state.invokeExact(bound[i]);require(states[i].length==4,"complete prediction state");
            cache[i]=(long[])raw.invokeExact(bound[i]);texts[i]=(String[])text.invokeExact(bound[i]);children[i]=(Object[])aliases.invokeExact(bound[i]);
            require(cache[i].length==FmcReconstructionCruisePrepare.CELLS.stream().filter(c->!c.type().startsWith("L")).count()&&texts[i].length==3&&children[i].length==10,"complete current prediction controls/children");
            for(String value:texts[i])require(value!=null,"native current RTA text");
            for(int j=0;j<10;j++)require(children[i][j]!=null&&context.owner(j<6?"FmcVnavCalc$ProfilePoint":"FmcVnav$SpdAlt").isInstance(children[i][j])&&unique.put(children[i][j],Boolean.TRUE)==null,"distinct native prediction child aliases");
            preimages[i]=(String)value.invokeExact(bound[i+2]);
        }
        String canonical=(String)bank.invokeExact(bound[0]);
        candidates=Set.copyOf(List.of(FmcReconstructionCruisePrepare.prepareBank(canonical,altitude),FmcReconstructionCruisePrepare.prepareMissingRtaBank(canonical,altitude)));
        for(int i=0;i<2;i++){
            require(preimages[i]!=null&&!(boolean)pending.invokeExact(bound[i+2]),"pending prediction input");
            for(String candidate:candidates)require((boolean)accepts.invokeExact(bound[i+2],candidate),"prediction bank limit before publication");
        }
        require(states[0][0]!=states[1][0],"distinct native prediction bank indices");verifyPreimage();
    }
    static FmcReconstructionCruiseAccess bind(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,int altitude,Object newModBody)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(qualified);Objects.requireNonNull(authority);Objects.requireNonNull(newModBody);require(context.current()&&qualified.getAsBoolean(),"worker lifecycle");return new FmcReconstructionCruiseAccess(context,qualified,authority,altitude,newModBody);
    }
    private static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>... args)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,args);require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==result,"exact bridge");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result,args));
    }
    Object[] aliases(){var result=new ArrayList<Object>(24);result.addAll(Arrays.asList(bound));for(Object[] row:children)result.addAll(Arrays.asList(row));return result.toArray();}Object[] banks(){return new Object[]{bound[2],bound[3]};}
    void verifyPreimage()throws Throwable{
        require(context.current()&&qualified.getAsBoolean()&&(Object)modBody.invokeExact()==boundModBody,"preimage lifecycle/MOD body root");Object[] actual=(Object[])roots.invokeExact();require(actual.length==4,"root roster");for(int i=0;i<4;i++)require(actual[i]==bound[i],"prediction root/bank replaced");
        for(int i=0;i<2;i++){
            require(Arrays.equals(states[i],(int[])state.invokeExact(bound[i]))&&Arrays.equals(cache[i],(long[])raw.invokeExact(bound[i]))&&Arrays.equals(texts[i],(String[])text.invokeExact(bound[i]))&&preimages[i].equals((String)value.invokeExact(bound[i+2]))&&!(boolean)pending.invokeExact(bound[i+2]),"prediction state/input changed");
            Object[] current=(Object[])aliases.invokeExact(bound[i]);require(current.length==10,"prediction child roster");for(int j=0;j<10;j++)require(current[j]==children[i][j],"prediction child identity changed");
        }require(context.current()&&qualified.getAsBoolean(),"preimage lifecycle changed");
    }
    void load(Object newModBody)throws Throwable{
        verifyPreimage();require(newModBody==boundModBody,"trusted NEW MOD body");publication();rebase.invokeExact(bound[0],bound[1]);publication();rta.invokeExact(bound[1],newModBody);publication();rtaResult.invokeExact(bound[1],bound[0]);
        for(int i=0;i<2;i++){publication();invalidatePoints.invokeExact(bound[i]);}
        String rebuilt=FmcReconstructionCruisePrepare.prepareBank((String)bank.invokeExact(bound[1]),altitude);require(candidates.contains(rebuilt),"native RTA re-resolution exceeds prepared bank candidates");
        for(int i=0;i<2;i++){publication();load.invokeExact(bound[i],bound[i+2],altitude,rebuilt);}publication();
    }
    private void publication()throws Throwable{require(context.current()&&qualified.getAsBoolean(),"publication lifecycle");authority.check();}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC cruise access "+why);}
}
