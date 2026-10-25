package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Resolves current native roots on the operation worker, then retains private
 * complete header vectors for the original EXEC-thread publisher. All aliases,
 * bank limits and pending inputs are checked before the first live write. */
final class FmcReconstructionHeaderAccess implements FmcReconstructionAccess.Headers {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final FmcPublication.Authority authority;
    private final FmcNavigationCatalog catalog;private final MethodHandle load,aliases,banks,options,value,accepts,pending,forecast,pages,freshPacks,bindPacks,packAliases,menuReset,menuAliases;
    private final IdentityHashMap<Object,FmcReconstructionHeaderPrepare.Data> prepared=new IdentityHashMap<>();
    private final IdentityHashMap<Object,Object[]> roots=new IdentityHashMap<>();
    private final IdentityHashMap<Object,String> bankPreimages=new IdentityHashMap<>();
    private Object[] menuRoots;private final MethodHandle menuLinks;
    private final IdentityHashMap<Object,Object[]> privatePacks=new IdentityHashMap<>();
    private FmcReconstructionHeaderAccess(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,FmcNavigationCatalog catalog)throws Throwable{
        this.context=context;this.qualified=qualified;this.authority=authority;this.catalog=catalog;
        var h=context.owner("FmcRte$Rte");load=method(h,FmcReconstructionHeaderPrepare.NAME,void.class,Object.class,long[].class,String[].class,Object[].class);
        aliases=method(h,FmcReconstructionHeaderPrepare.ALIASES,Object[].class,Object.class);banks=method(h,FmcReconstructionHeaderPrepare.BANKS,Object[].class,Object.class);
        options=method(h,FmcReconstructionHeaderPrepare.OPTIONS,int[].class);forecast=method(h,FmcReconstructionHeaderPrepare.FORECAST,void.class,Object.class,Object.class);pages=method(h,FmcReconstructionHeaderPrepare.PAGES,void.class,Object.class);
        freshPacks=method(h,FmcReconstructionHeaderPrepare.FRESH_PACKS,Object[].class);bindPacks=method(h,FmcReconstructionHeaderPrepare.BIND_PACKS,void.class,Object.class,Object[].class);packAliases=method(h,FmcReconstructionHeaderPrepare.PACK_ALIASES,Object[].class,Object[].class);
        menuReset=method(h,FmcReconstructionMenuPrepare.RESET,void.class,Object.class);menuAliases=method(h,FmcReconstructionMenuPrepare.ALIASES,Object[].class);menuLinks=method(h,FmcReconstructionMenuPrepare.LINKS,boolean.class,Object.class);context.owner("FmcDepArr");context.owner("FmcDepArr$DepTransPack");context.owner("FmcDepArr$ArrTransPack");
        for(String owner:List.of("FmcDepArr$SidPack","FmcDepArr$SidList","FmcDepArr$ArrPack","FmcDepArr$ArrList","FmcDepArr$AppPack","FmcDepArr$AppList","FmcDepArr$StringList"))context.owner(owner);
        value=method(context.owner("Qs"),FmcReconstructionBanks.VALUE,String.class,Object.class);accepts=method(context.owner("Qs"),FmcReconstructionBanks.ACCEPTS,boolean.class,Object.class,String.class);pending=method(context.owner("Q"),FmcReconstructionBanks.PENDING,boolean.class,Object.class);
    }
    static FmcReconstructionHeaderAccess bind(FmcInitialization.Context context,BooleanSupplier qualified,FmcPublication.Authority authority,FmcNavigationCatalog catalog)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(qualified);Objects.requireNonNull(authority);Objects.requireNonNull(catalog);require(context.current()&&qualified.getAsBoolean(),"worker qualification");
        var result=new FmcReconstructionHeaderAccess(context,qualified,authority,catalog);require(context.current()&&qualified.getAsBoolean(),"worker lifecycle changed");return result;
    }
    private static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>... args)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,args);require(m.isSynthetic()&&Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&m.getReturnType()==result,"exact bridge properties");
        return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(result,args));
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC header access "+why);}
    private void check()throws Throwable{require(context.current()&&qualified.getAsBoolean(),"lifecycle/qualification replaced");authority.check();}
    public Object[] banks(FmcReconstructionAccess.Route route)throws Throwable{require(context.current()&&qualified.getAsBoolean(),"bank binding lifecycle");Object[] result=(Object[])banks.invokeExact(route.header());require(result.length==5,"bank roster");return result;}
    public void validate(FmcReconstructionPlan plan,FmcReconstructionAccess.Route target,FmcReconstructionAccess.Route mod)throws Throwable{
        require(prepared.isEmpty(),"one private recipe per binding");require(context.current()&&qualified.getAsBoolean(),"validation lifecycle");
        menuRoots=(Object[])menuAliases.invokeExact();require(menuRoots.length==4,"global menu cache roster");for(int i=0;i<4;i++)require(menuRoots[i]!=null&&context.owner(i<2?"FmcDepArr$DepTransPack":"FmcDepArr$ArrTransPack").isInstance(menuRoots[i]),"actual menu cache receivers");
        int[] currentOptions=(int[])options.invokeExact();Object targetBank=banks(target)[0];String current=(String)value.invokeExact(targetBank);
        var data=FmcReconstructionHeaderPrepare.prepare(plan,catalog,currentOptions,current);
        var unique=new IdentityHashMap<Object,Boolean>();
        for(var route:List.of(target,mod)){
            require((boolean)menuLinks.invokeExact(route.header()),"native startup transition links");
            Object[] selected=(Object[])aliases.invokeExact(route.header());require(selected.length==FmcReconstructionHeaderPrepare.ALIAS_FIELDS.size()+5,"alias roster");
            for(int i=0;i<selected.length;i++){
                Object object=selected[i];if(i>=FmcReconstructionHeaderPrepare.ALIAS_FIELDS.size()||i>=5&&i<=9||i==18||i==19)continue;require(object!=null&&unique.put(object,Boolean.TRUE)==null,"distinct owned header aliases");
                if(i==3||i==4){require(object instanceof Object[]&&((Object[])object).length==30,"native runway storage");for(Object row:(Object[])object)require(row!=null&&context.owner("ScanRunway$RwyInfo").isInstance(row)&&unique.put(row,Boolean.TRUE)==null,"distinct native runway rows");}
            }
            Object[] fresh=(Object[])freshPacks.invokeExact();require(fresh.length==5,"private native menu pack roster");Object[] packRoots=(Object[])packAliases.invokeExact(fresh);require(packRoots.length==10,"private menu alias roster");for(int i=0;i<10;i++){Object object=packRoots[i];require(object!=null&&unique.put(object,Boolean.TRUE)==null,"private distinct menu roots");if(i>=5){require(object instanceof Object[]&&((Object[])object).length==200,"native menu storage");for(Object row:(Object[])object)require(row!=null&&context.owner("FmcDepArr$StringList").isInstance(row)&&unique.put(row,Boolean.TRUE)==null,"private distinct native menu rows");}}privatePacks.put(route.header(),fresh);
            Object[] nativeBanks=banks(route);for(int i=0;i<nativeBanks.length;i++){Object bank=nativeBanks[i];require(context.owner("Qs").isInstance(bank)&&!(boolean)pending.invokeExact(bank)&&(boolean)accepts.invokeExact(bank,data.text()[14+i]),"bank type/pending/length limits");bankPreimages.put(bank,(String)value.invokeExact(bank));}
            prepared.put(route.header(),data);roots.put(route.header(),selected);
        }
        require(context.current()&&qualified.getAsBoolean(),"validation lifecycle changed");
    }
    private void same(FmcReconstructionAccess.Route route)throws Throwable{Object[] currentMenus=(Object[])menuAliases.invokeExact();require(menuRoots!=null&&currentMenus.length==menuRoots.length,"current menu cache roster");for(int i=0;i<currentMenus.length;i++)require(currentMenus[i]==menuRoots[i],"current menu cache replaced");Object[] now=(Object[])aliases.invokeExact(route.header()),before=roots.get(route.header());require(before!=null&&now.length==before.length,"bound alias vector");for(int i=0;i<now.length;i++)require(now[i]==before[i],"header alias replaced");}
    public void verifyPreimage(FmcReconstructionAccess.Route route)throws Throwable{require(context.current()&&qualified.getAsBoolean(),"preimage lifecycle");same(route);for(Object bank:banks(route))require(bankPreimages.containsKey(bank)&&Objects.equals(bankPreimages.get(bank),(String)value.invokeExact(bank))&&!(boolean)pending.invokeExact(bank),"header bank preimage changed before mutation");require(context.current()&&qualified.getAsBoolean(),"preimage lifecycle changed");}
    public void load(FmcReconstructionPlan plan,FmcReconstructionAccess.Route route)throws Throwable{check();same(route);var data=prepared.get(route.header());require(data!=null,"private validated header data");Object[] fresh=privatePacks.get(route.header());Object[] newAliases=(Object[])packAliases.invokeExact(fresh);bindPacks.invokeExact(route.header(),fresh);var expected=roots.get(route.header()).clone();for(int i=0;i<5;i++){expected[5+i]=newAliases[i];expected[FmcReconstructionHeaderPrepare.ALIAS_FIELDS.size()+i]=newAliases[5+i];}menuReset.invokeExact(route.header());roots.put(route.header(),expected);check();load.invokeExact(route.header(),data.raw(),data.text(),data.lists());check();same(route);}
    public void copyCurrentForecast(FmcReconstructionAccess.Route mod,FmcReconstructionAccess.Route target)throws Throwable{check();same(target);same(mod);forecast.invokeExact(target.header(),mod.header());check();}
    public void reconcile(FmcReconstructionAccess.Route route)throws Throwable{check();same(route);pages.invokeExact(route.header());check();}
    public Object[] ownedAliases()throws Throwable{var objects=new ArrayList<Object>();if(menuRoots!=null)objects.addAll(Arrays.asList(menuRoots));for(var list:roots.values())for(int i=0;i<list.length;i++){if(i>=5&&i<=9||i>=FmcReconstructionHeaderPrepare.ALIAS_FIELDS.size())continue;Object o=list[i];if(o==null)continue;objects.add(o);if(o instanceof Object[] rows)objects.addAll(Arrays.asList(rows));}for(var list:privatePacks.values())for(Object o:(Object[])packAliases.invokeExact(list)){objects.add(o);if(o instanceof Object[] rows)objects.addAll(Arrays.asList(rows));}return objects.toArray();}
}
