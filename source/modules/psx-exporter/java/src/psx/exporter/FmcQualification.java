package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Data-only FMC ownership discovery. Discovery never advertises a native
 * capability: an inventory is not a proof of object ownership or exclusion. */
public final class FmcQualification {
    static final String INSTALLED_JAR = "96b7504a58920794394ff0b7b6a4e33189a37965e836119e845a34164ede8285";
    // These are semantic roots, not a prefix rule for copying private memory.
    static final List<String> ROOTS = List.of("FmcGlobal", "FmcRte", "Routes", "RoutesStby",
        "RoutesStbyProg", "FmcPerf", "FmcVnav", "FmcNg", "FmcThrustLim", "FmcTkofRef",
        "FmcProgress1", "FmcProgress2", "FmcRefNavData", "FmcHold", "FmcFixInfo",
        "FmcDepArr", "FmcMaintenance", "FmcMessages", "FmcAppRef", "FmcCrzPower",
        "FmcDesiredWpt", "FmcTkofV", "FmcVnavCalc", "FmcVnavDes", "FmcWindOat",
        "FmcDataLink", "FmcValidEntry", "FmcPrint");
    record Member(String owner, String name, String descriptor) implements Comparable<Member> {
        public int compareTo(Member b) {
            int c=owner.compareTo(b.owner); if(c!=0)return c;
            c=name.compareTo(b.name); return c!=0?c:descriptor.compareTo(b.descriptor);
        }
        public String toString(){return owner+"."+name+descriptor;}
    }
    record Access(Prepare.MethodId method, Opcode opcode) {}
    record Inventory(Map<String,String> classHashes, Map<Member,List<Access>> accesses,
                     List<Member> fields, Map<Member,Integer> flags, Map<Prepare.MethodId,String> bodyHashes,
                     Map<Prepare.MethodId,List<Prepare.MethodId>> callers, Set<String> referencedTypes,
                     Set<Prepare.MethodId> asyncEntries, List<String> boundaries) {}
    private FmcQualification(){}

    static Inventory inventory(Prepare data) throws Exception {
        var selected=new TreeSet<String>();
        for(String root:ROOTS) {
            if(!data.models.containsKey(root))throw new IllegalArgumentException("missing FMC root "+root);
            selected.add(root);
            for(String name:data.models.keySet())if(name.startsWith(root+"$"))selected.add(name);
        }
        // RouteWpt is an owned data candidate whose aliases need separate proof.
        if(!data.models.containsKey("RouteWpt"))throw new IllegalArgumentException("missing RouteWpt");
        selected.add("RouteWpt");
        var hashes=new TreeMap<String,String>(); var fields=new ArrayList<Member>();
        var references=new TreeSet<String>(); var boundary=new ArrayList<String>();
        var flags=new TreeMap<Member,Integer>();
        var bodies=new TreeMap<Prepare.MethodId,String>(Comparator.comparing(Prepare.MethodId::toString));
        var callers=new TreeMap<Prepare.MethodId,List<Prepare.MethodId>>(Comparator.comparing(Prepare.MethodId::toString));
        for(String name:selected) {
            hashes.put(name,Identity.hash(data.inputs.get(name)));
            for(var field:data.models.get(name).fields()) {
                // Constant tables and compiler links still appear in the inventory;
                // the future profile must classify them rather than silently copy.
                var member=new Member(name,field.fieldName().stringValue(),field.fieldType().stringValue());
                fields.add(member);flags.put(member,field.flags().flagsMask());
                String type=member.descriptor();while(type.startsWith("["))type=type.substring(1);
                if(type.startsWith("L")) {
                    String target=type.substring(1,type.length()-1); references.add(target);
                    if(!selected.contains(target)&&!target.equals("java/lang/String"))
                        boundary.add(member+" -> "+target);
                }
            }
        }
        Collections.sort(fields);
        var declared=new HashSet<>(fields); var accesses=new TreeMap<Member,List<Access>>();
        var async=new TreeSet<Prepare.MethodId>(Comparator.comparing(Prepare.MethodId::toString));
        for(var item:data.methods.entrySet()) {
            if(item.getValue().code().isEmpty())continue;
            for(var instruction:Prepare.instructions(item.getValue())) {
                if(instruction instanceof FieldInstruction f) {
                    var member=resolve(data,declared,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                    if(member!=null)accesses.computeIfAbsent(member,k->new ArrayList<>())
                        .add(new Access(item.getKey(),f.opcode()));
                }
                if(instruction instanceof InvokeInstruction invoke) {
                    var target=new Prepare.MethodId(invoke.owner().asInternalName(),invoke.name().stringValue(),invoke.type().stringValue());
                    callers.computeIfAbsent(target,k->new ArrayList<>()).add(item.getKey());
                }
                if(instruction instanceof InvokeInstruction call && call.name().equalsString("start")
                    && call.type().equalsString("()V") && selected.contains(item.getKey().owner()))
                    async.add(item.getKey());
            }
        }
        for(var list:accesses.values())for(var access:list)bodies.put(access.method(),Identity.hash(data.inputs.get(access.method().owner())));
        callers.values().forEach(v->{v.sort(Comparator.comparing(Prepare.MethodId::toString));});
        accesses.values().forEach(v->v.sort(Comparator.comparing(a->a.method().toString()+a.opcode())));
        Collections.sort(boundary);
        return new Inventory(Collections.unmodifiableMap(hashes),Collections.unmodifiableMap(accesses),
            List.copyOf(fields),Collections.unmodifiableMap(flags),Collections.unmodifiableMap(bodies),Collections.unmodifiableMap(callers),Collections.unmodifiableSet(references),Collections.unmodifiableSet(async),List.copyOf(boundary));
    }

    // JVMS field resolution starts at the named owner, then interfaces and
    // superclass. An inherited access must not escape the writer inventory.
    static Member resolve(Prepare data,Set<Member> declared,String owner,String name,String type) {
        return resolve(data.models,declared,owner,name,type);
    }
    // Resolve final instructions against final declarations. A lower optional
    // layer may change a shadow field or inheritance path during preparation.
    static Map<String,ClassModel> finalModels(Prepare data,Map<String,byte[]> lower) {
        var models=new HashMap<>(data.models);
        lower.forEach((name,bytes)->{
            var model=data.cf.parse(bytes);
            if(!name.equals(model.thisClass().asInternalName()))throw new IllegalArgumentException("FMC final class name mismatch");
            models.put(name,model);
        });
        return Map.copyOf(models);
    }
    static Member resolve(Map<String,ClassModel> models,Set<Member> declared,String owner,String name,String type) {
        Member actual=resolveField(models,owner,name,type,new HashSet<>());
        return actual!=null&&declared.contains(actual)?actual:null;
    }
    static Member resolveField(Map<String,ClassModel> models,String owner,String name,String type,Set<String> seen) {
        if(!seen.add(owner))return null;var model=models.get(owner);if(model==null)return null;
        if(declares(model,name,type))return new Member(owner,name,type);
        for(var iface:model.interfaces()) {
            Member found=resolveField(models,iface.asInternalName(),name,type,seen);if(found!=null)return found;
        }
        return model.superclass().isPresent()?resolveField(models,model.superclass().get().asInternalName(),name,type,seen):null;
    }

    static String quote(String s) {
        var b=new StringBuilder("\"");
        for(int i=0;i<s.length();i++){char c=s.charAt(i);switch(c){
            case '"'->b.append("\\\"");case '\\'->b.append("\\\\");case '\n'->b.append("\\n");
            case '\r'->b.append("\\r");case '\t'->b.append("\\t");
            default->{if(c<32)b.append(String.format(Locale.ROOT,"\\u%04x",(int)c));else b.append(c);}
        }}return b.append('"').toString();
    }
    static String report(Inventory i,String jarHash) {
        var b=new StringBuilder("{\"schema\":1,\"state\":\"inventory_only\",\"jarSha256\":").append(quote(jarHash))
            .append(",\"captureQualified\":false,\"recallQualified\":false,\"classes\":[");
        boolean comma=false;
        for(var e:i.classHashes().entrySet()){if(comma)b.append(',');comma=true;
            b.append("{\"name\":").append(quote(e.getKey())).append(",\"sha256\":").append(quote(e.getValue())).append('}');}
        b.append("],\"fields\":[");comma=false;
        for(var f:i.fields()){if(comma)b.append(',');comma=true;
            b.append("{\"owner\":").append(quote(f.owner())).append(",\"name\":").append(quote(f.name()))
                .append(",\"descriptor\":").append(quote(f.descriptor())).append(",\"flags\":").append(i.flags().get(f)).append(",\"accesses\":[");
            boolean ac=false;for(var a:i.accesses().getOrDefault(f,List.of())){if(ac)b.append(',');ac=true;
                b.append("{\"method\":").append(quote(a.method().toString())).append(",\"opcode\":").append(quote(a.opcode().name())).append('}');}
            b.append("]}");}
        b.append("],\"referenceBoundaries\":[");comma=false;
        for(String v:i.boundaries()){if(comma)b.append(',');comma=true;b.append(quote(v));}
        b.append("],\"asynchronousStartCallers\":[");comma=false;
        for(var v:i.asyncEntries()){if(comma)b.append(',');comma=true;b.append(quote(v.toString()));}
        b.append("],\"accessorClasses\":[");comma=false;
        for(var v:i.bodyHashes().entrySet()){if(comma)b.append(',');comma=true;
            b.append("{\"method\":").append(quote(v.getKey().toString())).append(",\"classSha256\":").append(quote(v.getValue())).append(",\"callers\":[");
            boolean ac=false;for(var c:i.callers().getOrDefault(v.getKey(),List.of())){if(ac)b.append(',');ac=true;b.append(quote(c.toString()));}b.append("]}");}
        return b.append("]}").toString();
    }
    public static void main(String[] arguments)throws Exception {
        if(arguments.length!=2)throw new IllegalArgumentException("FmcQualification <installed jar> <report>");
        Path jar=Path.of(arguments[0]);String hash=Identity.hash(jar);
        if(!INSTALLED_JAR.equals(hash))throw new IllegalArgumentException("unqualified PSX JAR identity");
        var result=inventory(new Prepare(Prepare.readJar(jar)));
        Files.writeString(Path.of(arguments[1]),report(result,hash),StandardOpenOption.CREATE_NEW);
        System.out.println("FMC inventory classes="+result.classHashes().size()+" fields="+result.fields().size()
            +" reference-boundaries="+result.boundaries().size()+" asynchronous-starts="+result.asyncEntries().size()
            +"; discovery only, no native capability advertised");
    }

    // Preparation only. ClassModel declarations are immutable; final/lower
    // replacements have different identities. Do not retain hierarchy results,
    // selected-member decisions, exceptions or any partially inspected field.
    // Models inflate lazily, so retain the original cold scan and thread use.
    private static final int DECLARED_QUERY_LIMIT=32_768;
    private static final ThreadLocal<DeclaredFieldCache> DECLARED_FIELDS=
        ThreadLocal.withInitial(DeclaredFieldCache::new);
    private record FieldQuery(String name,String descriptor) {}
    private static final class DeclaredFieldCache {
        final IdentityHashMap<ClassModel,Map<FieldQuery,Boolean>> models=new IdentityHashMap<>();
        int entries;
    }
    private static boolean declares(ClassModel model,String name,String type) {
        DeclaredFieldCache cache=DECLARED_FIELDS.get();
        FieldQuery query=new FieldQuery(name,type);
        Map<FieldQuery,Boolean> known=cache.models.get(model);
        Boolean answer=known==null?null:known.get(query);
        if(answer!=null)return answer;
        boolean found=model.fields().stream().anyMatch(f->f.fieldName().equalsString(name)&&f.fieldType().equalsString(type));
        // Only a completed scan reaches admission. Eviction changes cost only;
        // the map remains bounded even for many missing/hostile field queries.
        if(cache.entries>=DECLARED_QUERY_LIMIT){cache.models.clear();cache.entries=0;}
        cache.models.computeIfAbsent(model,k->new HashMap<>()).put(query,found);
        ++cache.entries;
        return found;
    }
}
