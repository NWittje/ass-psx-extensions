package psx.exporter;

import java.util.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.lang.classfile.*;

/** Worker-side restore ownership, separate from the historical raw capture
 * table. Serialization is evidence of storage, not permission to restore a
 * private native field. Rebound fields require the qualified navigation/anchor
 * reconstruction; calculated fields require native readback after EXEC.
 * Nothing in this profile advertises recall or writes installed PSX state. */
final class FmcRecallProfile {
    enum Ownership { ENTERED, REBIND, RECALCULATE, PRESERVE }
    record Field(String owner,String name,String descriptor,Ownership ownership){}
    enum LegRole { DEFINITION, ORIGIN_RUNWAY, DESTINATION_RUNWAY, HOLD_QUADRANT,
        ANCHOR, DYNAMIC_COURSE, STATIC_COURSE, COURSE_FROM_1, COURSE_FROM_2, COURSE_FROM_3 }
    // These are the installed serialized characters, not guessed row types or
    // navigation indices. A role is evidence of meaning, not a restore recipe.
    private static final Map<String,Character> MARKERS=Map.of(
        "RES_OFF",'*',"RES_ORIGRWY",'O',"RES_DESTRWY",'D',"RES_HOLD_QUAD_ON",'H',
        "RES_ANCHOR",'A',"RES_DYN_CRS",'C',"RES_STATIC_CRS",'S',
        "RES_CRSFROM_1",'E',"RES_CRSFROM_2",'F',"RES_CRSFROM_3",'G');
    private static final Set<String> CALCULATED=Set.of("turnDirValid");
    static final Set<String> PRESERVED=Set.of(
        "aircraftPosition","fmcPosition","navigationBias","irs","actualFuel",
        "navigationSources","manualIlsTuning","manualFmcFuel","manualFmcFuelHistory","flightNumber","inactiveRoute");
    // Caller contract, not an inferred frozen-input guarantee. Neither capture
    // nor recall imports scratchpad text. No external edits are allowed during
    // restore; independent native jobs/readers still require drain/exclusion.
    static final String INPUT_CONTRACT="quiet-external-edits;scratchpad-not-saved-restored-or-preserved";
    private FmcRecallProfile(){}
    static LegRole role(char marker){return switch(marker){
        case '*'->LegRole.DEFINITION;case 'O'->LegRole.ORIGIN_RUNWAY;
        case 'D'->LegRole.DESTINATION_RUNWAY;case 'H'->LegRole.HOLD_QUADRANT;
        case 'A'->LegRole.ANCHOR;case 'C'->LegRole.DYNAMIC_COURSE;
        case 'S'->LegRole.STATIC_COURSE;case 'E'->LegRole.COURSE_FROM_1;
        case 'F'->LegRole.COURSE_FROM_2;case 'G'->LegRole.COURSE_FROM_3;
        default->throw new IllegalArgumentException("FMC unqualified native leg role "+(int)marker);
    };}
    /** Preparation reads constant-pool data; it never defines RouteWpt. The
     * installed constants independently qualify the discriminator table. */
    static void qualifyRoles(Prepare data){
        var owner=data.models.get("RouteWpt");
        if(owner==null)throw new IllegalArgumentException("FMC leg-role owner");
        for(var expected:MARKERS.entrySet()){
            var found=owner.fields().stream().filter(f->f.fieldName().equalsString(expected.getKey())).toList();
            if(found.size()!=1||!found.getFirst().fieldType().equalsString("C")
                ||(found.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))!=(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL)
                ||!found.getFirst().findAttribute(Attributes.constantValue()).orElseThrow().constant().constantValue().equals((int)expected.getValue()))
                throw new IllegalArgumentException("FMC leg-role native constant drift "+expected.getKey());
        }
    }
    static List<Field> fields(){
        var result=new ArrayList<Field>();
        for(var group:List.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS))
            for(var input:group){
                Ownership policy=Ownership.ENTERED;
                if(input.owner().equals("RouteWpt")){
                    if(CALCULATED.contains(input.name()))policy=Ownership.RECALCULATE;
                }
                result.add(new Field(input.owner(),input.name(),input.descriptor(),policy));
            }
        return List.copyOf(result);
    }
    /** Comparison graph for the selected entered data. Raw artifact integrity
     * remains a separate SHA-256 check. Binding and derived-state verification
     * are mandatory independent witnesses; equality here cannot prove them.
     * The service must use this projection consistently at source and readback
     * before joining its existing restored_state/hash contract. */
    static FmcNarrowContent.Content entered(FmcNarrowContent.Content source){
        Objects.requireNonNull(source);
        int h=FmcNarrowContent.HEADER_INPUTS.size(),p=FmcNarrowContent.PERF_INPUTS.size(),w=FmcNarrowContent.WAYPOINT_INPUTS.size();
        long[] header=new long[h],perf=new long[p];String[] texts=new String[h];
        for(int i=0;i<h;i++){header[i]=source.headerBits(i+1);texts[i]=source.headerText(i+1);}
        for(int i=0;i<p;i++)perf[i]=source.perfBits(i+1);
        long[][] raw=new long[source.waypoints()][w];String[][] text=new String[source.waypoints()][w];
        for(int i=0;i<raw.length;i++)for(int j=0;j<w;j++){
            var input=FmcNarrowContent.WAYPOINT_INPUTS.get(j);
            text[i][j]=source.waypointText(i,j+1);
            raw[i][j]=CALCULATED.contains(input.name())?0:source.waypointBits(i,j+1);
        }
        for(int i=0;i<raw.length;i++){
            var role=role((char)source.waypointBits(i,7));boolean course=FmcReconstructionPlan.courseFrom(role);
            boolean current="PPOS".equals(text[i][10]),runway=role==LegRole.ORIGIN_RUNWAY||role==LegRole.DESTINATION_RUNWAY;
            if(course){if(i>1||i==1&&FmcReconstructionPlan.courseFrom(role((char)source.waypointBits(0,7))))throw new IllegalArgumentException("FMC comparison course lineage unresolved");raw[i][6]='F';}
            if(runway||current||course&&i==1)raw[i][13]=raw[i][14]=Integer.toUnsignedLong(Float.floatToRawIntBits(9f));
            else {float lat=Float.intBitsToFloat((int)raw[i][13]);if(lat!=9f){if(lat>1.5707945814656445d)lat=1.5707946f;else if(lat< -1.5707945814656445d)lat=-1.5707946f;raw[i][13]=Integer.toUnsignedLong(Float.floatToRawIntBits(lat));}}
            if(course||role==LegRole.DYNAMIC_COURSE)raw[i][25]=raw[i][26]=Integer.toUnsignedLong(Float.floatToRawIntBits(9f));
            if(role==LegRole.DYNAMIC_COURSE){raw[i][15]=Integer.toUnsignedLong(-1);raw[i][2]='E';}
            if((int)raw[i][23]==-2)raw[i][23]=Integer.toUnsignedLong(-1);
        }
        return FmcNarrowContent.of(header,texts,perf,raw,text);
    }
    static byte[] enteredBytes(FmcNarrowContent.Content source)throws Exception{return FmcNarrowContent.encode(entered(source));}
    static byte[] contractHash()throws Exception{
        var text=new StringBuilder("FMC-recall-entered-v5;active-or-RTE1;native-EXEC-after-load;native-guidance-effects-accepted;pause-preserve-freeze;no-angular-reset;synthetic0/1-current;terminal-retained;EFG-canonical-F-editable-slots2/3-only;slot2-nonPPOS-base-entered;slot3-course-endpoint-rebuilt;runway-PPOS-coordinates-rebound;C-course-turn-centres-rebuilt;EFG-centres-rebuilt;native-polar-clamp;EFC-minus2-history-to-minus1;binding-and-calculation-readback-required;").append(INPUT_CONTRACT).append(';');
        for(var f:fields())text.append(f.owner).append('.').append(f.name).append(':').append(f.descriptor).append('@').append(f.ownership).append(';');
        for(String name:new TreeSet<>(PRESERVED))text.append("preserve:").append(name).append(';');
        return MessageDigest.getInstance("SHA-256").digest(text.toString().getBytes(StandardCharsets.UTF_8));
    }
}
