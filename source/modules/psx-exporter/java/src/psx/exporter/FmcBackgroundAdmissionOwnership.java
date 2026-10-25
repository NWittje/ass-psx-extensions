package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Direct preparation-added ownership metadata, outside native FMC content.
 * Workers bind exact retained objects; callbacks read a volatile long without
 * hashing, equality calls, allocation, reflection or native input consumption.
 * Native instance-cell qualification precedes insertion of this exact field. */
final class FmcBackgroundAdmissionOwnership {
    static final String TAG="$psxExporter$fmcOwnershipScope",READ="$psxExporter$fmcOwnershipRead",WRITE="$psxExporter$fmcOwnershipWrite";
    static final int FLAGS=ClassFile.ACC_PUBLIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_TRANSIENT|ClassFile.ACC_SYNTHETIC;
    record Profile(Map<String,byte[]> changed,Set<String> owners){}
    record Target(String owner,Object object){}
    private FmcBackgroundAdmissionOwnership(){}
    static Profile transform(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> selected){
        var owners=new TreeSet<String>();
        for(var member:selected){
            var model=data.models.get(member.owner());require(model!=null,"native metadata owner");
            var field=model.fields().stream().filter(f->f.fieldName().equalsString(member.name())&&f.fieldType().equalsString(member.descriptor())).findFirst().orElseThrow();
            if((field.flags().flagsMask()&ClassFile.ACC_STATIC)==0)owners.add(member.owner());
        }
        var before=new HashMap<>(lower);var output=new HashMap<String,byte[]>();
        for(String owner:owners){
            var original=data.models.get(owner);require(original.fields().stream().noneMatch(f->f.fieldName().equalsString(TAG))
                &&original.methods().stream().noneMatch(m->Set.of(READ,WRITE).contains(m.methodName().stringValue())),"original metadata collision");
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);ClassDesc type=ClassDesc.of(owner);
            boolean tagged=model.fields().stream().anyMatch(f->f.fieldName().equalsString(TAG));
            if(tagged){qualify(data,owner,model);continue;}
            require(model.methods().stream().noneMatch(m->Set.of(READ,WRITE).contains(m.methodName().stringValue())),"partial metadata collision");
            byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){members(b,type);}
            });
            require(data.cf.verify(bytes).isEmpty(),"metadata final verifier");qualify(data,owner,data.cf.parse(bytes));output.put(owner,bytes);
        }
        TerrainAccess.verifyAdded(before,output,data.inputs);return new Profile(Map.copyOf(output),Set.copyOf(owners));
    }
    private static void members(ClassBuilder b,ClassDesc owner){
        b.withField(TAG,CD_long,FLAGS);
        b.withMethodBody(READ,MethodTypeDesc.of(CD_long,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var nonnull=c.newLabel();c.aload(0).ifnonnull(nonnull).lconst_0().lreturn().labelBinding(nonnull).aload(0).checkcast(owner).getfield(owner,TAG,CD_long).lreturn();
        });
        b.withMethodBody(WRITE,MethodTypeDesc.of(CD_void,CD_Object,CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,
            c->c.aload(0).checkcast(owner).lload(1).putfield(owner,TAG,CD_long).return_());
    }
    static void qualify(Prepare data,String owner,ClassModel model){
        var fields=model.fields().stream().filter(f->f.fieldName().equalsString(TAG)).toList();
        require(fields.size()==1&&fields.getFirst().fieldType().equalsString("J")&&fields.getFirst().flags().flagsMask()==FLAGS,"exact generated ownership field");
        var expected=data.cf.parse(data.cf.build(ClassDesc.of(owner),b->members(b,ClassDesc.of(owner))));
        for(String name:List.of(READ,WRITE)){
            var actual=model.methods().stream().filter(m->m.methodName().equalsString(name)).toList();var wanted=expected.methods().stream().filter(m->m.methodName().equalsString(name)).findFirst().orElseThrow();
            require(actual.size()==1&&actual.getFirst().methodType().equalsString(wanted.methodType().stringValue())&&actual.getFirst().flags().flagsMask()==wanted.flags().flagsMask(),"exact generated ownership bridge");
            require(Arrays.equals(normalized(data,owner,actual.getFirst()),normalized(data,owner,wanted)),"generated ownership bridge body provenance");
        }
    }
    private static byte[] normalized(Prepare data,String owner,MethodModel method){return data.cf.build(ClassDesc.of(owner),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),
        m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));}
    /** Caller has completed ordinary Pause and quiet external edit admission.
     * Existing source/job/poll witnesses protect binding-to-closure transition.
     * Targets come only from the validated live graph, never artifact objects. */
    static synchronized long bind(FmcInitialization.Context context,Profile profile,List<Target> targets,Collection<?> aliases)throws Throwable{
        Objects.requireNonNull(context);Objects.requireNonNull(profile);Objects.requireNonNull(targets);Objects.requireNonNull(aliases);
        require(context.current()&&!FmcExclusion.admissionClosed()&&!targets.isEmpty()&&targets.size()<=FmcReconstructionCapacity.MAX_RECEIVERS,"ownership binding context/bounds");
        long epoch=Observer.terrainEpoch();var handles=new HashMap<String,MethodHandle>();var owners=new HashMap<String,Class<?>>();
        var actualOwners=new TreeSet<String>();for(var target:targets){require(target.object!=null&&profile.owners.contains(target.owner),"actual target belongs to qualified metadata roster");actualOwners.add(target.owner);}
        for(String name:actualOwners){
            Class<?> owner=context.owner(name.replace('/','.'));var field=owner.getDeclaredField(TAG);
            require(field.getType()==long.class&&field.getModifiers()==FLAGS&&field.isSynthetic(),"initialized metadata field flags");
            var method=owner.getDeclaredMethod(WRITE,Object.class,long.class);
            require(method.getReturnType()==void.class&&method.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|ClassFile.ACC_SYNTHETIC)&&method.isSynthetic(),"initialized metadata setter flags");
            var reader=owner.getDeclaredMethod(READ,Object.class);
            require(reader.getReturnType()==long.class&&reader.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|ClassFile.ACC_SYNTHETIC)&&reader.isSynthetic(),"initialized metadata reader flags");
            handles.put(name,MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,WRITE,MethodType.methodType(void.class,Object.class,long.class)));owners.put(name,owner);
        }
        for(var target:targets)require(target.object!=null&&owners.containsKey(target.owner)&&owners.get(target.owner).isInstance(target.object),"exact owned target class/object");
        long scope=FmcBackgroundAdmission.nextScope();
        // A partial metadata update never becomes a published ownership scope.
        // Reject recall until a new simulator lifecycle if binding races a
        // closed window or any validated native metadata bridge fails.
        try{
            // Accept both old and new metadata IDs while open. A native field
            // callback cannot slip through the tag-write/publication interval;
            // its already acquired lifetime survives the final scope swap.
            FmcBackgroundAdmission.beginScope(epoch,scope);
            for(var target:targets){require(context.current()&&epoch==Observer.terrainEpoch()&&!FmcExclusion.admissionClosed(),"ownership changed during metadata binding");
                handles.get(target.owner).invokeExact(target.object,scope);}
            require(context.current()&&epoch==Observer.terrainEpoch()&&!FmcExclusion.admissionClosed(),"ownership changed before publication");
            FmcAdmissionReceivers.bind(epoch,aliases);FmcBackgroundAdmission.publishScope(epoch,scope);return scope;
        }catch(Throwable failure){FmcExclusion.failed();throw failure;}
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC ownership metadata "+why);}
}
