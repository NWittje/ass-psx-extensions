package psx.exporter;
import java.lang.invoke.*;import java.lang.reflect.Modifier;import java.util.*;import java.util.function.BooleanSupplier;
/** Current config polling stays live. This worker binding proves its actual
 * bank receivers remain outside every reconstruction ownership target. No
 * captured artifact value, bank freshness or transport state is read or set. */
final class FmcBackgroundAdmissionOptionAccess {
 private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final MethodHandle getter;private final Object[] bound,scratch;private final int[] indices,currentIndices;private final IdentityHashMap<Object,Boolean> owned;
 private FmcBackgroundAdmissionOptionAccess(FmcInitialization.Context context,BooleanSupplier qualified,Collection<?> actualOwned)throws Throwable{
  this.context=Objects.requireNonNull(context);this.qualified=Objects.requireNonNull(qualified);Objects.requireNonNull(actualOwned);current();require(!FmcExclusion.admissionClosed(),"open worker binding");var owner=context.owner("Option");context.owner("Glob");var qi=context.owner("Qi");var qs=context.owner("Qs");var countMethod=owner.getDeclaredMethod(FmcBackgroundAdmissionOptionPrepare.CONFIG_COUNT);require(countMethod.isSynthetic()&&countMethod.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|0x1000)&&countMethod.getReturnType()==int.class,"exact readonly configuration count");int count=(int)MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,FmcBackgroundAdmissionOptionPrepare.CONFIG_COUNT,MethodType.methodType(int.class)).invokeExact();require(count>0&&count<=256,"qualified finite configuration bank count");
  var m=owner.getDeclaredMethod(FmcBackgroundAdmissionOptionPrepare.CONFIG,Object[].class,int[].class);require(m.isSynthetic()&&m.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|0x1000)&&m.getReturnType()==void.class,"exact readonly configuration bridge");getter=MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,FmcBackgroundAdmissionOptionPrepare.CONFIG,MethodType.methodType(void.class,Object[].class,int[].class));
  bound=new Object[count+2];scratch=new Object[bound.length];indices=new int[count];currentIndices=new int[indices.length];owned=new IdentityHashMap<>();for(Object o:actualOwned)if(o!=null)owned.put(o,Boolean.TRUE);getter.invokeExact(bound,indices);require(qi.arrayType().isInstance(bound[0])&&qs.arrayType().isInstance(bound[1])&&bound[0]!=bound[1],"actual distinct native bank array roots");var unique=new IdentityHashMap<Object,Boolean>();
  for(int i=0;i<count;i++){Object bank=bound[i+2];require((qi.isInstance(bank)||qs.isInstance(bank))&&unique.put(bank,Boolean.TRUE)==null&&indices[i]>=0,"actual distinct config bank receiver/type/index");require(!owned.containsKey(bank),"configuration bank aliases reconstruction-owned receiver");}current();verify();
 }
 static FmcBackgroundAdmissionOptionAccess bind(FmcInitialization.Context context,BooleanSupplier qualified,Collection<?> allActualOwned)throws Throwable{return new FmcBackgroundAdmissionOptionAccess(context,qualified,allActualOwned);}
 /** Call on the service worker/authorized dispatcher before each mutation
  * stage. Buffers and the complete negative ownership set were prepared once. */
 void verify()throws Throwable{current();getter.invokeExact(scratch,currentIndices);for(int i=0;i<bound.length;i++)require(scratch[i]==bound[i],"configuration array/bank identity replaced");for(int i=0;i<indices.length;i++){require(currentIndices[i]==indices[i],"configuration bank index replaced");require(!owned.containsKey(scratch[i+2]),"configuration bank became reconstruction-owned");}current();}
 private void current(){require(context.current()&&qualified.getAsBoolean(),"qualified initialized lifecycle replaced");}
 private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC configuration bank binding "+why);}

}

