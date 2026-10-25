package psx.exporter;

import java.lang.classfile.*;
import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import java.util.function.BooleanSupplier;

/** Worker-only scan of preparation-added primitive failure flags. Native page
 * classes may initialize lazily: resolve without initialization, probe the VM
 * in bounded batches, and read only ordinarily initialized owners. Repeat
 * after native lifetimes retire so newly initialized owners cannot conceal a
 * helper failure. Never clear flags or define/initialize vendor classes. */
final class FmcBackgroundAdmissionFaults {
    private static final int BATCH=64,MAX_OWNERS=2048,MAX_FIELDS=8192;
    private static final Set<String> NAMES=Set.of(FmcBackgroundAdmissionPrepare.FAULT,FmcBackgroundAdmissionRegionPrepare.FAULT,
        FmcExclusionPrepare.FAULT,FmcJobPrepare.RECALL_FAULT,FmcExecPrepare.FAULT,FmcBackgroundAdmissionBankPrepare.FAULT,FmcBackgroundAdmissionInputPrepare.FAULT,FmcBackgroundAdmissionTransportPrepare.FAULT,FmcRenderingPrepare.FAULT,FmcBackgroundAdmissionAfdsPrepare.FAULT,"$psxExporter$fmcMutationFailure");
    record Owner(String name,List<String> fields){}
    record Roster(List<Owner> owners,int fields){}
    record Result(int initializedOwners,int skippedOwners,int readFields,boolean clean){}
    private FmcBackgroundAdmissionFaults(){}
    static Roster roster(Prepare data,Map<String,byte[]> prepared){
        var result=new ArrayList<Owner>();int count=0;
        for(var entry:new TreeMap<>(FmcQualification.finalModels(data,prepared)).entrySet()){
            var fields=new ArrayList<String>();
            for(var f:entry.getValue().fields())if(NAMES.contains(f.fieldName().stringValue())){
                int mask=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC;
                require(f.fieldType().equalsString("Z")&&(f.flags().flagsMask()&mask)==mask&&(f.flags().flagsMask()&ClassFile.ACC_FINAL)==0,"prepared fault flags/type");
                fields.add(f.fieldName().stringValue());count++;
            }
            if(!fields.isEmpty()){fields.sort(String::compareTo);result.add(new Owner(entry.getKey().replace('/','.'),List.copyOf(fields)));}
        }
        require(!result.isEmpty()&&result.size()<=MAX_OWNERS&&count<=MAX_FIELDS,"prepared fault roster bounds");
        return new Roster(List.copyOf(result),count);
    }
    static Result scan(ClassLoader loader,Roster roster,FmcInitialization.Source source,BooleanSupplier retired)throws Throwable{
        return scan(loader,roster,source,retired,(owners,states)->FmcNative.abiVersion()==1&&FmcNative.classStates(owners,states));
    }
    static Result scan(ClassLoader loader,Roster roster,FmcInitialization.Source source,BooleanSupplier retired,FmcInitialization.Probe probe)throws Throwable{
        Objects.requireNonNull(loader);Objects.requireNonNull(roster);Objects.requireNonNull(source);Objects.requireNonNull(retired);Objects.requireNonNull(probe);
        require(!roster.owners.isEmpty()&&roster.owners.size()<=MAX_OWNERS&&roster.fields<=MAX_FIELDS,"fault scan roster bounds");
        long lifecycle=source.qualifiedLifecycle();current(source,retired,lifecycle);
        int initialized=0,skipped=0,reads=0;boolean clean=true;var names=new HashSet<String>();
        for(int start=0;start<roster.owners.size();start+=BATCH){
            int size=Math.min(BATCH,roster.owners.size()-start);var owners=new Class<?>[size];var states=new int[size];
            for(int i=0;i<size;i++){
                var owner=roster.owners.get(start+i);require(names.add(owner.name)&&!owner.name.isEmpty()&&owner.name.indexOf('/')<0&&!owner.name.startsWith("["),"fault scan exact owner identity");
                Class<?> klass=Class.forName(owner.name,false,loader);
                require(klass.getClassLoader()==loader&&klass.getName().equals(owner.name)&&!klass.isArray()&&!klass.isPrimitive()&&!klass.isInterface()&&!klass.isHidden(),"fault scan prepared class loader/type");owners[i]=klass;
            }
            current(source,retired,lifecycle);require(probe.states(owners,states),"fault VM state evidence unavailable");current(source,retired,lifecycle);
            for(int i=0;i<size;i++){
                int status=states[i];require((status&~FmcNative.KNOWN)==0&&(status&(FmcNative.ERROR|FmcNative.ARRAY|FmcNative.PRIMITIVE))==0,"fault VM status invalid");
                if(!FmcNative.initialized(status)){skipped++;continue;}
                initialized++;Class<?> owner=owners[i];var lookup=MethodHandles.privateLookupIn(owner,MethodHandles.lookup());
                for(String name:roster.owners.get(start+i).fields){
                    require(NAMES.contains(name),"fault field outside prepared generated allowlist");
                    var field=owner.getDeclaredField(name);int flags=field.getModifiers();
                    require(field.getType()==boolean.class&&Modifier.isPublic(flags)&&Modifier.isStatic(flags)&&Modifier.isVolatile(flags)&&!Modifier.isFinal(flags)&&field.isSynthetic(),"fault field identity/flags");
                    current(source,retired,lifecycle);var getter=lookup.findStaticGetter(owner,name,boolean.class);
                    boolean fault=(boolean)getter.invokeExact();reads++;if(fault)clean=false;current(source,retired,lifecycle);
                }
            }
        }
        current(source,retired,lifecycle);return new Result(initialized,skipped,reads,clean);
    }
    private static void current(FmcInitialization.Source source,BooleanSupplier retired,long lifecycle){
        if(lifecycle<=0||source.qualifiedLifecycle()!=lifecycle||!retired.getAsBoolean())throw new IllegalStateException("FMC fault scan requires same-lifecycle retired native work");
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC admission fault "+why);}
}
