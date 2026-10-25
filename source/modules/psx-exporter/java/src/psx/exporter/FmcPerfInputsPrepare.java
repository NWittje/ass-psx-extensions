package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Narrow PERF INIT member qualification. The artifact owns entered values
 * and their entered/unset flags, not mass, sensed/manual fuel, predictions or
 * the ballast-fuel control. Values retain native units and raw bits.
 *
 * This preparation component neither advertises recall nor invokes installed
 * classes. Complete ordinary-thread exclusion, source-bank reconciliation and
 * the route adapter remain prerequisites to using its legal setters. */
public final class FmcPerfInputsPrepare {
    static final List<FmcMemberPrepare.Field> INPUTS=List.of(
        new FmcMemberPrepare.Field(1,"FmcPerf","zfWeight","F",true),
        new FmcMemberPrepare.Field(2,"FmcPerf","fuelReserves","I",true),
        new FmcMemberPrepare.Field(3,"FmcPerf","costIndex","I",true),
        new FmcMemberPrepare.Field(4,"FmcPerf","crzAlt","I",true),
        new FmcMemberPrepare.Field(5,"FmcPerf","crzCg","I",true),
        new FmcMemberPrepare.Field(6,"FmcPerf","stepSize","I",true),
        new FmcMemberPrepare.Field(7,"FmcPerf","stepSizeNg","I",true),
        new FmcMemberPrepare.Field(8,"FmcPerf","minFuelTemp","I",true),
        new FmcMemberPrepare.Field(9,"FmcPerf","grWtBig","Z",true),
        new FmcMemberPrepare.Field(10,"FmcPerf","zfWtBig","Z",true),
        new FmcMemberPrepare.Field(11,"FmcPerf","cgBig","Z",true));
    static final Set<String> PRESERVED=Set.of("ballast","fuelWtMode","fuelWeightEntry","fuelWeightSens");
    static final Set<String> DERIVED=Set.of("grWeight","fuelReservesInUse","costIndexFac","crzCgAltAdd");
    record Qualified(Map<String,byte[]> changed,Map<String,Set<Prepare.MethodId>> writers){}
    private FmcPerfInputsPrepare(){}
    private static void require(boolean yes,String reason){
        if(!yes)throw new IllegalArgumentException("FMC PERF INIT inputs "+reason);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower)throws Exception{
        byte[] bytes=lower.getOrDefault("FmcPerf",data.inputs.get("FmcPerf"));
        require(bytes!=null,"missing owner");var owner=data.cf.parse(bytes);
        var finalModels=FmcQualification.finalModels(data,lower);
        var declared=new HashSet<FmcQualification.Member>();
        var writers=new TreeMap<String,Set<Prepare.MethodId>>();
        for(var input:INPUTS){
            var fields=owner.fields().stream().filter(f->f.fieldName().equalsString(input.name())).toList();
            require(fields.size()==1&&fields.getFirst().fieldType().equalsString(input.descriptor()),"descriptor "+input.name());
            require((fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,
                "mutable static input "+input.name());
            declared.add(new FmcQualification.Member(input.owner(),input.name(),input.descriptor()));
            writers.put(input.name(),new HashSet<>());
        }
        // Resolve inherited spellings. This establishes direct writer coverage,
        // not a claim that all writer callers are deferrable or already gated.
        var owners=new HashSet<>(data.models.keySet());owners.addAll(lower.keySet());
        for(String name:owners){
          var model=data.cf.parse(lower.getOrDefault(name,data.inputs.get(name)));
          for(var method:model.methods()){
            if(method.code().isEmpty())continue;
            var id=new Prepare.MethodId(name,method.methodName().stringValue(),method.methodType().stringValue());
            for(var instruction:Prepare.instructions(method))
                if(instruction instanceof FieldInstruction field&&field.opcode()==Opcode.PUTSTATIC){
                    var member=FmcQualification.resolve(finalModels,declared,field.owner().asInternalName(),field.name().stringValue(),field.type().stringValue());
                    if(member!=null)writers.get(member.name()).add(id);
                }
          }
        }
        for(var input:INPUTS){
            String method=switch(input.name()){
                case "stepSizeNg"->"readStepSizeNg";
                case "minFuelTemp"->"runLento";
                default->"readGlobAll";
            };
            require(writers.get(input.name()).equals(Set.of(new Prepare.MethodId("FmcPerf",method,"()V"))),
                "writer coverage "+input.name()+" "+writers.get(input.name()));
        }
        var changed=new HashMap<>(lower);
        changed.putAll(FmcMemberPrepare.transform(data,lower,INPUTS));
        changed.putAll(FmcContentReconcilePrepare.transform(data,changed));
        changed.putAll(FmcCostIndexReconcilePrepare.transform(data,changed));
        var frozen=new TreeMap<String,Set<Prepare.MethodId>>();writers.forEach((name,value)->frozen.put(name,Set.copyOf(value)));
        return new Qualified(Map.copyOf(changed),Collections.unmodifiableMap(frozen));
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report required");var jar=Path.of(arguments[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var result=qualify(data,Map.of());
        var out=new StringBuilder("{\"schema\":1,\"state\":\"narrow_perf_init_member_component\",\"captureQualified\":false,\"recallQualified\":false,\"inputs\":[");
        boolean comma=false;
        for(var input:INPUTS){if(comma)out.append(',');comma=true;
            out.append("{\"id\":").append(input.id()).append(",\"name\":").append(FmcQualification.quote(input.name()))
                .append(",\"descriptor\":").append(FmcQualification.quote(input.descriptor()))
                .append(",\"writer\":").append(FmcQualification.quote(result.writers().get(input.name()).iterator().next().toString())).append('}');}
        out.append("],\"preserved\":").append(FmcQualification.quote(new TreeSet<>(PRESERVED).toString()))
            .append(",\"derived\":").append(FmcQualification.quote(new TreeSet<>(DERIVED).toString()))
            .append(",\"inputSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get("FmcPerf"))))
            .append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(result.changed().get("FmcPerf"))))
            .append(",\"limit\":\"No native definition, initialization or invocation. Native input banks, current fuel validity, CG/reserve derivatives, route profile, ordinary refresh, exclusion, JNI and service remain qualification gates.\"}\n");
        Files.writeString(Path.of(arguments[1]),out,StandardOpenOption.CREATE_NEW);
        System.out.println("Narrow PERF INIT member component qualified as data; native FMC recall remains unavailable");
    }
}
