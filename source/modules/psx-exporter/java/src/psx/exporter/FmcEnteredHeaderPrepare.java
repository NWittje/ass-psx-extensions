package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Explicit entered route-header/procedure members. Airport coordinates,
 * navigation position, predictions, global flight number and unused instance
 * runway/flight-number buffers are absent. Rwy.name has separate origin and
 * destination receiver roles; its field ID alone never chooses a live object.
 *
 * Mutable header buffers need coherent deep copying and mutator/alias coverage
 * in the full adapter. This component only qualifies legal member access and
 * direct field writers; it does not authorize a native capture or publication. */
public final class FmcEnteredHeaderPrepare {
    static final String HEADER="FmcRte$Rte",SELECT="FmcDepArr$DepArrSelections";
    static final List<FmcMemberPrepare.Field> INPUTS=List.of(
        field(1,HEADER,"sbOrig","Ljava/lang/StringBuilder;"),field(2,HEADER,"sbDest","Ljava/lang/StringBuilder;"),
        field(3,HEADER,"sbCoRte","Ljava/lang/StringBuilder;"),field(4,"FmcRte$Rwy","name","Ljava/lang/String;"),
        field(5,SELECT,"selSid","Ljava/lang/String;"),field(6,SELECT,"selSidTrans","Ljava/lang/String;"),field(7,SELECT,"selSidRwy","Ljava/lang/String;"),
        field(8,SELECT,"selStar","Ljava/lang/String;"),field(9,SELECT,"selStarTrans","Ljava/lang/String;"),
        field(10,SELECT,"selApp","Ljava/lang/String;"),field(11,SELECT,"selAppTrans","Ljava/lang/String;"),
        field(12,SELECT,"rwyExtNm10","I"),field(13,SELECT,"rwyExtFpaDeg10","I"));
    private static FmcMemberPrepare.Field field(int id,String owner,String name,String type){return new FmcMemberPrepare.Field(id,owner,name,type,true);}
    record Qualified(Map<String,byte[]> changed,Map<FmcQualification.Member,Set<Prepare.MethodId>> writers){}
    private FmcEnteredHeaderPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC entered header "+why);}
    private static Prepare.MethodId method(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    static Set<Prepare.MethodId> expected(FmcMemberPrepare.Field field){
        if(field.owner().equals(HEADER))return Set.of(method(HEADER,"<init>","(IIIIILRoutes$RteBody;)V"));
        if(field.owner().equals("FmcRte$Rwy"))return Set.of(method("FmcRte$Rwy","<init>","()V"),method(HEADER,"deleteDepRwy","()V"),
            method(HEADER,"makeArrRwy","(Ljava/lang/String;)V"),method(HEADER,"readGlobRteHeader","()V"),method(HEADER,"situLoadInit","()V"));
        if(field.descriptor().equals("I"))return Set.of(method(SELECT,"readGlob","()V"));
        var result=new HashSet<Prepare.MethodId>();result.add(method(SELECT,"<init>","(I)V"));result.add(method(SELECT,"readGlob","()V"));
        result.add(method("FmcDepArr","resetDepArrPacksAndLists","(LFmcRte$Rte;)V"));result.add(method("Routes$RteBody","readGlob","(Ljava/lang/String;)V"));
        if(field.name().startsWith("selSid"))result.add(method("FmcDepArr$DepTransPack","set","(LFmcRte$Rte;LFmcDepArr$SidPack;IILControlDisplayUnit;)V"));
        else result.add(method("FmcDepArr$ArrTransPack","set","(LFmcRte$Rte;LFmcDepArr$ArrPack;LFmcDepArr$AppPack;IILControlDisplayUnit;)V"));
        if(!field.name().startsWith("selApp"))result.add(method("RouteFiler","loadAerowinx","(Ljava/io/RandomAccessFile;)V"));
        return Set.copyOf(result);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var selected=new HashSet<FmcQualification.Member>();var writers=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();
        for(var input:INPUTS){
            var model=models.get(input.owner());require(model!=null,"owner");var members=model.fields().stream().filter(f->f.fieldName().equalsString(input.name())).toList();
            require(members.size()==1&&members.getFirst().fieldType().equalsString(input.descriptor())&&(members.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0,"mutable instance descriptor");
            var member=new FmcQualification.Member(input.owner(),input.name(),input.descriptor());selected.add(member);writers.put(member,new HashSet<>());
        }
        for(var entry:models.entrySet())for(var method:entry.getValue().methods())if(method.code().isPresent())
            for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                var member=FmcQualification.resolve(models,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(member!=null)writers.get(member).add(method(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()));
            }
        for(var input:INPUTS)require(writers.get(new FmcQualification.Member(input.owner(),input.name(),input.descriptor())).equals(expected(input)),"direct writer frontier "+input.name());
        var changed=new HashMap<>(lower);changed.putAll(FmcMemberPrepare.transform(data,lower,INPUTS));
        var frozen=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();writers.forEach((k,v)->frozen.put(k,Set.copyOf(v)));
        return new Qualified(Map.copyOf(changed),Collections.unmodifiableMap(frozen));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var result=qualify(data,Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"entered_header_member_component_only\",\"captureQualified\":false,\"recallQualified\":false,\"members\":13,\"directWriters\":"+result.writers().values().stream().flatMap(Set::stream).distinct().count()+",\"limits\":\"Header buffer mutators/aliases, receiver roles, route-only native reconstruction, exclusion and publication remain unqualified. No installed classes defined/initialized/invoked.\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("Entered route header/procedure members qualify as data; native FMC capability unavailable");
    }
}
