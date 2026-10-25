package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Explicit company-route definition candidates. Native serialization gives
 * field authority, not permission to restore dynamic anchors, active progress
 * or pending MOD memory. Those distinctions require the complete adapter.
 * ETA, fuel, winds, ND paths and turn geometry are deliberately absent. */
public final class FmcRouteInputsPrepare {
    static final List<FmcMemberPrepare.Field> INPUTS=List.of(
        field(1,"rteType","C"),field(2,"pathTerm","C"),field(3,"turnDir","C"),field(4,"turnDirValid","Z"),
        field(5,"spdLimType","C"),field(6,"altLimType","C"),field(7,"reserved","C"),
        field(8,"wptCol41","C"),field(9,"wptCol42","C"),field(10,"wptCol43","C"),
        field(11,"identStrFix","Ljava/lang/String;"),field(12,"identStrVia","Ljava/lang/String;"),field(13,"optionStr","Ljava/lang/String;"),
        field(14,"lat","F"),field(15,"lon","F"),field(16,"crs10","I"),field(17,"spdLim","I"),field(18,"alt1","I"),field(19,"alt2","I"),
        field(20,"rnp","I"),field(21,"identStrReqNav","Ljava/lang/String;"),field(22,"univ","I"),field(23,"legDist","I"),field(24,"efc","I"),
        field(25,"vertAngle","I"),field(26,"ctrFixLat","F"),field(27,"ctrFixLon","F"));
    private static FmcMemberPrepare.Field field(int id,String name,String descriptor){return new FmcMemberPrepare.Field(id,"RouteWpt",name,descriptor,true);}
    record Qualified(Map<String,byte[]> changed,Map<FmcQualification.Member,Set<Prepare.MethodId>> writers){}
    private FmcRouteInputsPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC route input "+why);}
    private static Map<FmcQualification.Member,Set<Prepare.MethodId>> writers(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> fields){
        var models=FmcQualification.finalModels(data,lower);var result=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();fields.forEach(f->result.put(f,new HashSet<>()));
        for(var entry:models.entrySet())for(var method:entry.getValue().methods()){
            if(method.code().isEmpty())continue;
            for(var i:Prepare.instructions(method))if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){
                var owner=FmcQualification.resolve(models,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(owner!=null)result.get(owner).add(new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()));
            }
        }
        return result;
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var wpt=models.get("RouteWpt");require(wpt!=null,"missing owner");var selected=new HashSet<FmcQualification.Member>();
        for(var input:INPUTS){
            var members=wpt.fields().stream().filter(f->f.fieldName().equalsString(input.name())).toList();
            require(members.size()==1&&members.getFirst().fieldType().equalsString(input.descriptor())&&(members.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0,"mutable instance descriptor "+input.name());
            selected.add(new FmcQualification.Member(input.owner(),input.name(),input.descriptor()));
        }
        var filer=models.get("RouteFiler");require(filer!=null,"native company-route writer");
        var saves=filer.methods().stream().filter(m->m.methodName().equalsString("save")&&m.methodType().equalsString("(Ljava/lang/String;LRoutes$RteBody;II)V")).toList();
        require(saves.size()==1&&saves.getFirst().code().isPresent()&&(saves.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==ClassFile.ACC_STATIC,"native company-route method");
        var reads=new HashSet<FmcQualification.Member>();
        for(var i:Prepare.instructions(saves.getFirst()))if(i instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD){
            var resolved=FmcQualification.resolve(models,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
            if(resolved!=null)reads.add(resolved);
            else if(f.owner().asInternalName().equals("RouteWpt"))throw new IllegalArgumentException("FMC unexpected serialized waypoint field "+f.name());
        }
        require(reads.equals(selected),"complete native serialized field coverage");
        var original=writers(data,Map.of(),selected);var current=writers(data,lower,selected);
        require(original.values().stream().allMatch(v->!v.isEmpty()),"native writer evidence");
        require(original.equals(current),"final writer coverage changed");
        var changed=new HashMap<>(lower);changed.putAll(FmcMemberPrepare.transform(data,lower,INPUTS));
        var result=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();current.forEach((f,v)->result.put(f,Set.copyOf(v)));
        return new Qualified(Map.copyOf(changed),Collections.unmodifiableMap(result));
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report");Path jar=Path.of(arguments[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var result=qualify(data,Map.of());var out=new StringBuilder("{\"schema\":1,\"state\":\"route_definition_member_component\",\"captureQualified\":false,\"recallQualified\":false,\"fields\":[");boolean comma=false;
        for(var input:INPUTS){if(comma)out.append(',');comma=true;out.append("{\"id\":").append(input.id()).append(",\"name\":").append(FmcQualification.quote(input.name())).append(",\"descriptor\":").append(FmcQualification.quote(input.descriptor())).append(",\"directWriters\":").append(result.writers().get(new FmcQualification.Member(input.owner(),input.name(),input.descriptor())).size()).append('}');}
        out.append("],\"inputSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get("RouteWpt")))).append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(result.changed().get("RouteWpt"))))
            .append(",\"limit\":\"Serialized-member evidence is not input ownership/exclusion: native dynamic anchors, progress-related fields, header/procedures, MOD clearing, bank coherence, refresh, complete JNI/service remain unqualified. No installed class definition/init/invocation.\"}\n");
        Files.writeString(Path.of(arguments[1]),out,StandardOpenOption.CREATE_NEW);System.out.println("27 route-definition member candidates qualify as data; no FMC capability");
    }
}
