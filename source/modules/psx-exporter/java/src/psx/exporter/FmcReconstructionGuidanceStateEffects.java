package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;
/** Three scalar ordinary-EXEC guidance publications. This is not permission
 * to omit VNAV/Pred/ThrustLim members by type, or any preserved/current input.
 * Complete native writer rosters and activation/auto-CON producer bytes are
 * sealed. The three cells are never reconstruction or artifact input. */
final class FmcReconstructionGuidanceStateEffects {
    static final FmcQualification.Member EARLY=new FmcQualification.Member("FmcVnav","isEarlyDesAct","Z"),TIMER=new FmcQualification.Member("FmcVnavCalc","execTimer","F"),THRUST=new FmcQualification.Member("FmcThrustLim","selThrLimMode","I");
    static final Set<FmcQualification.Member> FIELDS=Set.of(EARLY,TIMER,THRUST);
    private record Site(Prepare.MethodId method,int bci){}
    private static Prepare.MethodId id(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    private static Site site(String owner,String name,String type,int bci){return new Site(id(owner,name,type),bci);}
    private static final Map<FmcQualification.Member,Set<Site>> WRITERS=Map.of(
        EARLY,Set.of(site("FmcGlobal$MemBits","trans","(ZZI)V",418),site("AfdsHub","runDelta","()V",1949),site("AfdsHub","runDelta","()V",3229),site("FmcVnav","activateEarlyDescent","()V",263)),
        TIMER,Set.of(site("FmcGlobal$MemBits","trans","(ZZI)V",616),site("FmcGlobal$MemBits","trans","(ZZI)V",623),site("FmcVnav","activateEarlyDescent","()V",6),site("FmcVnavCalc","runDelta","()V",201)),
        THRUST,Set.of(site("FmcThrustLim","runModerato","()V",94),site("FmcThrustLim","manageConThrustLimitAfterExec","()V",61),site("FmcThrustLim","manageConThrustLimitAfterExec","()V",124),site("FmcThrustLim","<clinit>","()V",1)));
    private static final Map<String,String> OWNERS=Map.of(
        "FmcVnav","A764CF1D893A9C9FD281B8612C7D97E0E4BF3E82B71842DC64D6EE90680F4709",
        "FmcVnavCalc","A3D8231E1BD677331805A009931003B8A48F1BF0854F1F68351319AB15FBEACA",
        "FmcThrustLim","A54B5FB221C214763C28565C1431CF4C4C8605064BF43D84A26123B050BD48E0",
        "FmcGlobal$MemBits","C6BF2C151728B21926DC1B030E9DB1020AD897A8023FF2D69DD569707E6EC324",
        "AfdsHub","DDD319B48A760708502C417B57F8999F4D2A5C413B63C2C904C469EC683A0A8E");
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native guidance state "+why);}
    private static boolean source(byte[] bytes,String expected){try{return bytes!=null&&Identity.hash(bytes).equalsIgnoreCase(expected);}catch(Exception failure){throw new IllegalArgumentException("FMC guidance source digest",failure);}}
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){return inspect(data,construction,currentInputs,null);}
    /** Production passes its trusted same-original-model scope snapshot; the
     * isolated source test may derive it here after all collision/writer checks. */
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs,FmcReconstructionExecScope.Profile actual){
        require(Collections.disjoint(FIELDS,construction)&&Collections.disjoint(FIELDS,currentInputs),"construction/current preservation collision");
        for(var owner:OWNERS.entrySet())require(source(data.inputs.get(owner.getKey()),owner.getValue()),"exact producer/status source changed "+owner.getKey());
        var writers=new HashMap<FmcQualification.Member,Set<Site>>();for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field!=null&&FIELDS.contains(field)){require(f.opcode()==Opcode.PUTSTATIC,"atomic static scalar publication");writers.computeIfAbsent(field,k->new HashSet<>()).add(new Site(entry.getKey(),bci));}}bci+=op.sizeInBytes();}}
        require(writers.equals(WRITERS),"whole original scalar writer roster changed");
        for(var field:FmcNarrowContent.HEADER_INPUTS)require(!FIELDS.contains(new FmcQualification.Member(field.owner(),field.name(),field.descriptor())),"captured header collision");
        for(var field:FmcNarrowContent.PERF_INPUTS)require(!FIELDS.contains(new FmcQualification.Member(field.owner(),field.name(),field.descriptor())),"captured PERF collision");
        for(var field:FmcNarrowContent.WAYPOINT_INPUTS)require(!FIELDS.contains(new FmcQualification.Member(field.owner(),field.name(),field.descriptor())),"captured waypoint collision");
        if(actual==null)actual=FmcReconstructionExecScope.inspect(data);require(actual.writes().containsAll(FIELDS),"actual ordinary EXEC output frontier");
        var early=id("FmcVnav","activateEarlyDescent","()V");var thrust=id("FmcThrustLim","manageConThrustLimitAfterExec","()V");require(actual.methods().contains(early)&&actual.methods().contains(thrust),"source-qualified ordinary EXEC producers");
        return FIELDS;
    }
    private FmcReconstructionGuidanceStateEffects(){}
}