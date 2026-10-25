package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Cancels only the uncaptured offset in NEW MOD. The original EXEC publishes
 * the empty bank; its freshness remains available to the native consumer. */
final class FmcReconstructionOffsetAccess {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final FmcPublication.Authority authority;
    private final MethodHandle roots,on,body,cancel,value,pending;private final Object[] bound,bodies;private final int[][] before;
    private final boolean oldOn;private final String oldBank;
    private FmcReconstructionOffsetAccess(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,Object target,Object mod,Object inactive)throws Throwable{
        this.context=context;this.qualified=qualified;this.authority=authority;bodies=new Object[]{target,mod,inactive};
        var ng=context.owner("FmcNg");context.owner("Glob");context.owner("Routes");
        roots=method(ng,FmcReconstructionOffsetPrepare.ROOTS,Object[].class);on=method(ng,FmcReconstructionOffsetPrepare.ON,boolean.class,Object.class);
        body=method(ng,FmcReconstructionOffsetPrepare.BODY,int[].class,Object.class);cancel=method(ng,FmcReconstructionOffsetPrepare.CANCEL,void.class,Object.class,Object.class);
        value=method(context.owner("Qs"),FmcReconstructionBanks.VALUE,String.class,Object.class);pending=method(context.owner("Q"),FmcReconstructionBanks.PENDING,boolean.class,Object.class);
        var accepts=method(context.owner("Qs"),FmcReconstructionBanks.ACCEPTS,boolean.class,Object.class,String.class);
        bound=(Object[])roots.invokeExact();require(bound.length==3&&context.owner("FmcNg$OffsEntry").isInstance(bound[0])&&context.owner("Qs").isInstance(bound[1])&&bound[2]==mod,"current MOD offset roots");
        oldOn=(boolean)on.invokeExact(bound[0]);oldBank=(String)value.invokeExact(bound[1]);require(oldBank!=null&&!(boolean)pending.invokeExact(bound[1])&&(boolean)accepts.invokeExact(bound[1],""),"offset bank input/empty output limits");
        before=new int[3][];var unique=new IdentityHashMap<Object,Boolean>();for(int i=0;i<3;i++){require(context.owner("Routes$RteBody").isInstance(bodies[i])&&unique.put(bodies[i],Boolean.TRUE)==null,"distinct route body roles");before[i]=state(bodies[i]);}verifyPreimage();
    }
    static FmcReconstructionOffsetAccess bind(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,Object target,Object mod,Object inactive)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(qualified);Objects.requireNonNull(authority);require(context.current()&&qualified.getAsBoolean(),"worker lifecycle");return new FmcReconstructionOffsetAccess(context,qualified,authority,target,mod,inactive);
    }
    private static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>... args)throws ReflectiveOperationException{var m=owner.getDeclaredMethod(name,args);require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==result,"exact bridge");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result,args));}
    private void current()throws Throwable{require(context.current()&&qualified.getAsBoolean(),"lifecycle/qualification");Object[] now=(Object[])roots.invokeExact();require(now.length==3,"root roster");for(int i=0;i<3;i++)require(now[i]==bound[i],"offset root/bank replaced");}
    private int[] state(Object receiver)throws Throwable{int[] result=(int[])body.invokeExact(receiver);require(result.length==2,"complete body offset state");return result;}
    void verifyPreimage()throws Throwable{current();require((boolean)on.invokeExact(bound[0])==oldOn&&oldBank.equals((String)value.invokeExact(bound[1]))&&!(boolean)pending.invokeExact(bound[1]),"offset input changed");for(int i=0;i<3;i++)require(Arrays.equals(before[i],state(bodies[i])),"offset body preimage changed");current();}
    void cancel()throws Throwable{current();authority.check();for(int i=0;i<2;i++)require(Arrays.equals(state(bodies[i]),new int[]{0,5}),"NEW parsed zero offset/five NM end reference");require(Arrays.equals(before[2],state(bodies[2])),"inactive offset changed");cancel.invokeExact(bound[0],bodies[1]);current();require(!(boolean)on.invokeExact(bound[0]),"MOD offset cancellation");authority.check();}
    void verifyFinished()throws Throwable{current();for(int i=0;i<2;i++)require(Arrays.equals(state(bodies[i]),new int[]{0,5}),"completed selected offset state");require(Arrays.equals(before[2],state(bodies[2])),"inactive offset preserved");}
    Object[] aliases(){return new Object[]{bound[0],bound[1],bound[2],bodies[0],bodies[2]};}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC offset access "+why);}
}
