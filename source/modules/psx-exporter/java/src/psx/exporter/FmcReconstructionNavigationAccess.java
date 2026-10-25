package psx.exporter;
import java.lang.invoke.*;import java.lang.reflect.Modifier;import java.nio.file.*;import java.util.function.BooleanSupplier;

/** Current navigation source provenance. File hashing belongs on the worker;
 * original EXEC/dispatcher threads only compare the non-consuming directory. */
final class FmcReconstructionNavigationAccess {
    private final FmcInitialization.Context context;private final BooleanSupplier qualified;private final MethodHandle value;private final String directory;private final FmcNavigationCatalog catalog;
    private FmcReconstructionNavigationAccess(FmcInitialization.Context context,BooleanSupplier qualified,FmcNavigationCatalog catalog)throws Throwable{
        this.context=context;this.qualified=qualified;this.catalog=catalog;var owner=context.owner("Core");var method=owner.getDeclaredMethod(FmcReconstructionNavigationPrepare.VALUE);require(method.isSynthetic()&&Modifier.isPublic(method.getModifiers())&&Modifier.isStatic(method.getModifiers())&&method.getReturnType()==String.class,"exact source getter");value=MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,FmcReconstructionNavigationPrepare.VALUE,MethodType.methodType(String.class));directory=(String)value.invokeExact();require(directory!=null&&Path.of(directory).toAbsolutePath().normalize().equals(catalog.sourceRoot()),"catalog uses current native navigation directory");verifyCatalog();
    }
    static FmcReconstructionNavigationAccess bind(FmcInitialization.Context context,BooleanSupplier qualified,FmcNavigationCatalog catalog)throws Throwable{require(context.current()&&qualified.getAsBoolean()&&!FmcExclusion.admissionClosed(),"open worker binding");return new FmcReconstructionNavigationAccess(context,qualified,catalog);}
    void verify()throws Throwable{require(context.current()&&qualified.getAsBoolean()&&directory.equals((String)value.invokeExact()),"current navigation directory changed");}
    void verifyCatalog()throws Throwable{require(!FmcExclusion.currentAdmitted(),"catalog hashing is worker-only");verify();require(catalog.stillMatches(catalog.sourceRoot()),"navigation source file identity changed");verify();}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC navigation source "+why);}
}
