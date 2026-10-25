package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;
/** Two scalar ordinary-EXEC display publications. This is not permission
 * to omit PERF/Core/route members by type, or any preserved/current input.
 * Complete native writer rosters and native display producer bytes are
 * sealed. The two cells are never reconstruction or artifact input. */
final class FmcReconstructionDisplayStateEffects {
    // Native visibility/style flags; neither cell contains gross weight or
    // route geometry. Ordinary EXEC recalculates them from current native data.
    static final FmcQualification.Member WEIGHTS=new FmcQualification.Member("FmcPerf","gwOnEicas","Z"),DASHES=new FmcQualification.Member("Core","denyDashedCurves","Z");
    static final Set<FmcQualification.Member> FIELDS=Set.of(WEIGHTS,DASHES);
    private record Site(Prepare.MethodId method,int bci){}
    private static Prepare.MethodId id(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    private static Site site(String owner,String name,String type,int bci){return new Site(id(owner,name,type),bci);}
    private static final Map<FmcQualification.Member,Set<Site>> WRITERS=Map.of(
        WEIGHTS,Set.of(site("FmcPerf","makeDisplays1B2B","()V",95),site("FmcPerf","makeDisplays1B2B","()V",130),site("FmcPerf","makeDisplays1B2B","()V",439),site("FmcPerf","makeDisplays1B2B","()V",461),site("FmcPerf","makeDisplays1B2B","()V",496),site("FmcPerf","makeDisplays1B2B","()V",526),site("FmcPerf","makeDisplays1B2B","()V",572)),
        DASHES,Set.of(site("Core","unpauseLoader","()V",634),site("IntegratedDisplaySystem","runDelta","()V",250),site("Routes$RteBody","readGlob","(Ljava/lang/String;)V",15134)));
    private static final Map<String,String> OWNERS=Map.of(
        "FmcPerf","20A94BEF3544D992C4B39F73158A585E1D280163B747372886EFB56B501C67AD",
        "Core","D522384326E98914FB2D2028588A6C2C8D1AA8DC7FFDFB7582AEE5BE49C7E037",
        "IntegratedDisplaySystem","418702AD71957A2C1CFC76E950B0CF805379B68CD1829E268F749C1BB2A282D0",
        "Routes$RteBody","C32FC40F6939128EFCEF8622D522684FD85011E1980513E4A9F6CD1EF1F50280");
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native display state "+why);}
    private static boolean source(byte[] bytes,String expected){try{return bytes!=null&&Identity.hash(bytes).equalsIgnoreCase(expected);}catch(Exception failure){throw new IllegalArgumentException("FMC display source digest",failure);}}
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
        var weights=id("FmcPerf","makeDisplays1B2B","()V");var route=id("Routes$RteBody","readGlob","(Ljava/lang/String;)V");require(actual.methods().contains(weights)&&actual.methods().contains(route),"source-qualified ordinary EXEC display producers");
        for(var field:FIELDS)for(var site:WRITERS.get(field))if(site.method().equals(weights)||site.method().equals(route)){
            var ops=Prepare.instructions(data.methods.get(site.method()));int bci=0,ordinal=-1;for(int at=0;at<ops.size();at++){if(bci==site.bci())ordinal=at;bci+=ops.get(at).sizeInBytes();}
            require(ordinal>=0&&actual.instructions().getOrDefault(site.method(),Set.of()).contains(ordinal),"exact display publication occurs in ordinary EXEC dispatch");
        }
        return FIELDS;
    }
    private FmcReconstructionDisplayStateEffects(){}
}