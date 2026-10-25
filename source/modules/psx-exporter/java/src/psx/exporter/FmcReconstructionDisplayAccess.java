package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;

/** Worker-only current LCD consumer/bank binding. No freshness is consumed. */
final class FmcReconstructionDisplayAccess {
    private static MethodHandle getter(Class<?> owner,String name,Class<?>... arguments)throws ReflectiveOperationException{
        var m=owner.getDeclaredMethod(name,arguments);if(m.getModifiers()!=(Modifier.PUBLIC|Modifier.STATIC|0x1000)||!m.isSynthetic()||m.getReturnType()!=Object[].class)throw new IllegalStateException("FMC LCD exact generated getter");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,MethodType.methodType(Object[].class,arguments));
    }
    static Object[] read(FmcInitialization.Context context)throws Throwable{
        if(!context.current())throw new IllegalStateException("FMC LCD lifecycle");var units=getter(context.owner("Core"),FmcReconstructionDisplayPrepare.UNITS);var lines=getter(context.owner("ControlDisplayUnit"),FmcReconstructionDisplayPrepare.LINES,Object.class);var banks=getter(context.owner("ControlDisplayUnit$ScreenLine"),FmcReconstructionDisplayPrepare.BANKS,Object.class);
        Object[] actual=(Object[])units.invokeExact();if(actual.length!=3)throw new IllegalStateException("FMC LCD unit roster");var result=new ArrayList<Object>();var unique=new IdentityHashMap<Object,Boolean>();
        for(Object unit:actual){if(unit==null||!context.owner("ControlDisplayUnit").isInstance(unit))throw new IllegalStateException("FMC LCD current unit");result.add(unit);Object[] rows=(Object[])lines.invokeExact(unit);if(rows.length!=13)throw new IllegalStateException("FMC LCD line roster");for(Object row:rows){if(row==null||!context.owner("ControlDisplayUnit$ScreenLine").isInstance(row)||unique.put(row,true)!=null)throw new IllegalStateException("FMC LCD line identity");result.add(row);Object[] bound=(Object[])banks.invokeExact(row);if(bound.length!=2)throw new IllegalStateException("FMC LCD bank roster");for(Object bank:bound){if(bank==null||!context.owner("Qs").isInstance(bank))throw new IllegalStateException("FMC LCD exact bank type/index");result.add(bank);}}}
        if(!context.current())throw new IllegalStateException("FMC LCD lifecycle changed");return result.toArray();
    }
    static Object[] readPages(FmcInitialization.Context context)throws Throwable{
        if(!context.current())throw new IllegalStateException("FMC page lifecycle");var units=getter(context.owner("Core"),FmcReconstructionDisplayPrepare.UNITS);var pages=getter(context.owner("ControlDisplayUnit"),FmcReconstructionDisplayPrepare.PAGES,Object.class);Object[] actual=(Object[])units.invokeExact();if(actual.length!=3)throw new IllegalStateException("FMC three current page roots");var result=new Object[3];
        for(int i=0;i<3;i++){if(actual[i]==null||!context.owner("ControlDisplayUnit").isInstance(actual[i]))throw new IllegalStateException("FMC current page unit");Object[] page=(Object[])pages.invokeExact(actual[i]);if(page.length!=1||page[0]==null||!context.owner("CduHub$Page").isInstance(page[0]))throw new IllegalStateException("FMC exact current page receiver");result[i]=page[0];}if(!context.current())throw new IllegalStateException("FMC page lifecycle changed");return result;
    }
}
