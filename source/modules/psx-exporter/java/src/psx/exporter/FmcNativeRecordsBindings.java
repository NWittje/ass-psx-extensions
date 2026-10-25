package psx.exporter;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Path;
import java.util.Map;

/** Worker binding to prepared, already initialized native owners. These are
 * legal bridges into the original routines, not copies of native FMC logic. */
final class FmcNativeRecordsBindings implements FmcNativeRecordsExecutor.NativeCalls {
    private final FmcInitialization.Context context;
    private final FmcNativeRecordsProfile.Profile profile;
    private final MethodHandle save,pause,initialize,isPaused,importValues,finish;
    private FmcNativeRecordsBindings(FmcInitialization.Context context,FmcNativeRecordsProfile.Profile profile,
                                    MethodHandle save,MethodHandle pause,MethodHandle initialize,
                                    MethodHandle isPaused,MethodHandle importValues,MethodHandle finish) {
        this.context=context;this.profile=profile;this.save=save;this.pause=pause;
        this.initialize=initialize;this.isPaused=isPaused;this.importValues=importValues;
        this.finish=finish;
    }
    static FmcNativeRecordsBindings bind(FmcInitialization.Context context,Path nativeInput,
                                        FmcNativeRecordsProfile.Profile profile)throws Exception {
        worker();
        for(String name:profile.initializedOwners())if(!Agent.sameInputSource(context.owner(name),nativeInput))
            throw new IllegalArgumentException("Native FMC owner source: "+name);
        var glob=context.owner("Glob");var core=context.owner("Core");
        return new FmcNativeRecordsBindings(context,profile,
            method(context.owner("SituationFiler"),SituationPrepare.PREFIX+"Save",void.class,String.class,String.class),
            method(core,FmcNativeRecordsPrepare.PAUSE,void.class),
            method(context.owner("CduHub"),FmcNativeRecordsPrepare.INITIALIZE,void.class),
            method(core,FmcNativeRecordsPrepare.IS_PAUSED,boolean.class),
            method(glob,FmcNativeRecordsPrepare.IMPORT,void.class,int[].class,int[].class,String[].class),
            method(core,FmcNativeRecordsPrepare.FINISH,void.class));
    }
    static MethodHandle method(Class<?> owner,String name,Class<?> result,Class<?>...arguments)throws Exception {
        worker();Method method=owner.getDeclaredMethod(name,arguments);
        int required=Modifier.PUBLIC|Modifier.STATIC|0x1000;
        if(method.getReturnType()!=result||method.getModifiers()!=required||!method.isSynthetic()
                ||method.isVarArgs()||Modifier.isAbstract(method.getModifiers())||Modifier.isNative(method.getModifiers())
                ||!method.trySetAccessible())throw new IllegalAccessException("Native FMC bridge: "+owner.getName()+"."+name);
        return MethodHandles.lookup().unreflect(method);
    }
    private static void worker() { if(java.awt.EventQueue.isDispatchThread())throw new IllegalStateException("Native FMC binding worker required"); }
    public void check() { if(!context.current())throw new IllegalStateException("Native FMC initialized lifecycle replaced"); }
    public void save(String filename,String description)throws Throwable { save.invokeExact(filename,description); }
    public void pause()throws Throwable { pause.invokeExact(); }
    public void initialize()throws Throwable { initialize.invokeExact(); }
    public void finishReload()throws Throwable { finish.invokeExact(); }
    public boolean paused()throws Throwable { return (boolean)isPaused.invokeExact(); }
    FmcCommandDispatch.Action prepareImport(Map<String,String> values) {
        worker();check();
        var qis=profile.slots(FmcNativeRecordsProfile.Bank.QI);var qhs=profile.slots(FmcNativeRecordsProfile.Bank.QH);var qss=profile.slots(FmcNativeRecordsProfile.Bank.QS);
        if(values.size()!=profile.slots().size())throw new IllegalArgumentException("Incomplete native FMC records");
        int[] qi=new int[qis.size()],qh=new int[qhs.size()];String[] qs=new String[qss.size()];
        for(int i=0;i<qi.length;i++)qi[i]=Integer.parseInt(required(values,qis.get(i).key()));
        for(int i=0;i<qh.length;i++)qh[i]=Integer.parseInt(required(values,qhs.get(i).key()));
        for(int i=0;i<qs.length;i++)qs[i]=required(values,qss.get(i).key());
        // Statement context gives invokeExact the bridge's void return type.
        return ()->{importValues.invokeExact(qi,qh,qs);};
    }
    private static String required(Map<String,String> values,String key) {
        String value=values.get(key);if(value==null)throw new IllegalArgumentException("Missing native FMC record: "+key);return value;
    }
}
