package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Negative receiver roles require exclusive converged source provenance and
 * absence from actual binding. Shared native member names remain protected on
 * retained route/prediction receivers; inherited sites are contextual only. */
final class FmcBackgroundAdmissionRoles {
    private record Role(FmcQualification.Member root,FmcReconstructionArrayProof.ElementResult access){}
    private record Store(Prepare.MethodId method,int ordinal){}
    private static final Map<Prepare,List<Role>> CACHE=Collections.synchronizedMap(new WeakHashMap<>());
    private final Prepare data;private final List<Role> roles;private final FmcBackgroundAdmissionNavFold navFold;
    FmcBackgroundAdmissionRoles(Prepare data,FmcBackgroundAdmissionProfile.Plan plan){this.data=data;navFold=data.methods.containsKey(new Prepare.MethodId("Core","navFold","(Ljava/lang/String;)Ljava/lang/String;"))?new FmcBackgroundAdmissionNavFold(data):null;
        if(plan.ownedStatics().isEmpty()){roles=List.of();return;}
        var qualified=CACHE.computeIfAbsent(data,FmcBackgroundAdmissionRoles::qualify);
        roles=qualified.stream().filter(r->!plan.publishedReferences().contains(r.root)&&!plan.ownedStatics().contains(r.root)&&!plan.ownedChildren().contains(r.root)&&!plan.fields().contains(r.root)).toList();
    }
    private static List<Role> qualify(Prepare data){
        var result=new ArrayList<Role>();result.add(new Role(FmcReconstructionApproachEffects.FLAP_POINTS,FmcReconstructionApproachEffects.inspect(data,Set.of(),Set.of()).elementAccesses()));result.add(new Role(FmcBackgroundAdmissionMemoryEffects.REFERENCE,FmcBackgroundAdmissionMemoryEffects.inspect(data,Set.of(),Set.of(),Set.of()).elementAccesses()));
        var stores=new HashMap<FmcQualification.Member,List<Store>>();var readers=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var ops=Prepare.instructions(entry.getValue());for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction f&&Set.of("LCalc$PlaceBearingDistance;","LCalc$PlaceBearingPlaceBearing;").contains(f.type().stringValue())){var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(member!=null){if(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)stores.computeIfAbsent(member,k->new ArrayList<>()).add(new Store(entry.getKey(),at));else readers.computeIfAbsent(member,k->new HashSet<>()).add(entry.getKey());}}}
        for(var entry:data.models.entrySet())for(var field:entry.getValue().fields())for(String type:List.of("Calc$PlaceBearingDistance","Calc$PlaceBearingPlaceBearing"))if(field.fieldType().equalsString("L"+type+";")){
            var root=new FmcQualification.Member(entry.getKey(),field.fieldName().stringValue(),field.fieldType().stringValue());
            if(!fresh(data,root,type,stores.getOrDefault(root,List.of())))continue;
            try{var access=FmcReconstructionArrayProof.elementAccesses(data,Set.of(root),readers.getOrDefault(root,Set.of()),type);if(access.proof().mutations().isEmpty())result.add(new Role(root,access));}catch(IllegalArgumentException unresolved){/* A published or opaque object retains its native frontier. */}
        }return List.copyOf(result);
    }
    /** A different freshly constructed workspace cannot alias a retained FMC
     * workspace. Every use of its field reference is separately checked for
     * publication/escape; native type equality supplies no ownership. */
    private static boolean fresh(Prepare data,FmcQualification.Member root,String type,List<Store> stores){
        if(stores.size()!=1)return false;var store=stores.getFirst();var ops=Prepare.instructions(data.methods.get(store.method()));int at=store.ordinal();return store.method().owner().equals(root.owner())&&Set.of("<init>","<clinit>").contains(store.method().name())&&at>=3&&Prepare.call(ops.get(at-1),type,"<init>","()V")&&ops.get(at-2).opcode()==Opcode.DUP&&ops.get(at-3) instanceof NewObjectInstruction n&&n.className().asInternalName().equals(type);
    }
    Set<FmcBackgroundAdmissionProjection.Site> directFields(){var result=new HashSet<FmcBackgroundAdmissionProjection.Site>();for(var role:roles)for(var site:role.access.directAccesses())if(Prepare.instructions(data.methods.get(site.method())).get(site.ordinal()) instanceof FieldInstruction)result.add(new FmcBackgroundAdmissionProjection.Site(site.method(),site.ordinal()));return Set.copyOf(result);}
    boolean unboundCall(Prepare.MethodId parent,int ordinal,Prepare.MethodId target,Set<Integer> substantive){
        if(navFold!=null&&navFold.unboundCall(parent,ordinal,target,substantive))return true;
        for(var role:roles){boolean direct=role.access.directAccesses().stream().anyMatch(s->s.method().equals(parent)&&s.ordinal()==ordinal&&s.owner().equals(target.owner())&&s.name().equals(target.name())&&s.descriptor().equals(target.descriptor()));if(!direct)continue;
            var inherited=new HashSet<Integer>();for(var site:role.access.inheritedAccesses())if(site.method().equals(target))inherited.add(site.ordinal());
            if(!substantive.isEmpty()&&inherited.containsAll(substantive))return true;
        }return false;
    }
}
