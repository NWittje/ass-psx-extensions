package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.nio.file.*;import java.util.*;
/** Exact native page pagination outputs. Page identities, input bank indices
 * and subsystem ownership never belong to this presentation partition. */
final class FmcReconstructionPageEffects {
    static final FmcQualification.Member PAGE=new FmcQualification.Member("CduHub$Page","pgOfPages","I"),FIRST=new FmcQualification.Member("CduHub$Page","firstPgLim","I");
    static final Set<FmcQualification.Member> OUTPUTS=Set.of(PAGE,FIRST);
    private static final String CDU="LControlDisplayUnit;";
    private static void add(Set<Prepare.MethodId> set,String owners,String name,String descriptor){for(String owner:owners.split(","))set.add(new Prepare.MethodId(owner,name,descriptor));}
    private static Set<Prepare.MethodId> processes(){var set=new HashSet<Prepare.MethodId>();add(set,"CduPgArrivals,CduPgAtcLog,CduPgDepartures,CduPgHold,CduPgIrsLegs,CduPgLegsL,CduPgLegsR,CduPgRteDataL,CduPgRteDataR,CduPgRteL,CduPgRteR,CduPgVerifyEmergency,CduPgVerifyRequest,CduPgWindsL,CduPgWindsR","processAll","()V");return set;}
    private static Set<Prepare.MethodId> firstProcesses(){var set=new HashSet<Prepare.MethodId>();add(set,"CduPgIrsLegs,CduPgLegsL,CduPgLegsR,CduPgRteDataL,CduPgRteDataR,CduPgRteL,CduPgRteR","processAll","()V");return set;}
    private static Map<FmcQualification.Member,Set<Prepare.MethodId>> wholeWriters(){
        var page=new HashSet<>(processes());page.add(new Prepare.MethodId("CduHub$Page","<init>","(II)V"));
        String scrolling="CduPgAltnArpt,CduPgArrivals,CduPgAtcLog,CduPgAtcTransmitted,CduPgCrossLoadNg,CduPgDepartures,CduPgDesiredWptL,CduPgDesiredWptR,CduPgDiscretes,CduPgFixInfo,CduPgFmcComm,CduPgHold,CduPgIrsLegs,CduPgLegsL,CduPgLegsR,CduPgRteDataL,CduPgRteDataR,CduPgRteL,CduPgRteR,CduPgSensors,CduPgVerifyEmergency,CduPgVerifyRequest,CduPgWindsL,CduPgWindsR";
        for(String name:List.of("pageNext","pagePrev"))add(page,scrolling,name,"("+CDU+")V");
        add(page,"CduPgArrivals,CduPgAtcAltRequest,CduPgAtcOffsetRequest,CduPgAtcRequest,CduPgAtcRteRequest,CduPgAtcSpdRequest,CduPgAtcTransmitted,CduPgEmergencyRep,CduPgLegsL,CduPgLegsR","right6","("+CDU+")V");
        add(page,"CduPgAtcIndex","left4","("+CDU+")V");add(page,"CduPgFmcComm","left3","("+CDU+")V");add(page,"CduPgFmcComm","left5","("+CDU+")V");add(page,"CduPgInitRef","right3","("+CDU+")V");for(String name:List.of("left1","right1","right2"))add(page,"CduPgMaintIndex",name,"("+CDU+")V");
        add(page,"CduPgAltnFour","selectArptPage","("+CDU+"I)V");add(page,"CduPgRteDataL,CduPgRteDataR","gotoWinds","("+CDU+"I)V");add(page,"CduPgAtcTransmitted","specialJob","(ILjava/lang/String;I)V");add(page,"ControlDisplayUnit","selectCduPage","(LCduHub$Page;I)V");
        add(page,"CduPgEmergencyRep$ThreadL1,CduPgEmergencyRep$ThreadR1","run","()V");add(page,"ControlDisplayUnit$45,ControlDisplayUnit$49,ControlDisplayUnit$50,ControlDisplayUnit$52","process","()V");add(page,"Fans","pageIt","("+CDU+"II)V");
        add(page,"FmcDesiredWpt","add","(I)V");add(page,"FmcDesiredWpt","returnToTargetPage","()V");add(page,"FmcDesiredWpt","set","(Ljava/lang/String;ILRoutes$RteBody;IDD)I");add(page,"FmcHold","selectHoldByCdu","("+CDU+")V");add(page,"FmcNg","readGlobMiscPgsOf","()V");add(page,"FmcNg","updateNearestAltn","(Z)V");add(page,"FmcRte","fmcVersionChangeProcess","()V");add(page,"FmcRte","readGlobPgsOf","()V");add(page,"Routes$RteBody","addHoldPpos","()V");for(String name:List.of("addInRteHold","setHold"))add(page,"Routes$RteBody",name,"(I)V");add(page,"RoutesStby$Cdu","selectLegsPage","()V");add(page,"RoutesStby","readGlobStby","()V");
        var first=new HashSet<>(firstProcesses());add(first,"CduPgIrsLegs,CduPgLegsL,CduPgLegsR,CduPgRteDataL,CduPgRteDataR,CduPgRteL,CduPgRteR","<init>","(II)V");add(first,"CduPgLegsL$ThreadL6,CduPgLegsR$ThreadL6,CduPgRteL$ThreadL6,CduPgRteR$ThreadL6","run","()V");return Map.of(PAGE,Set.copyOf(page),FIRST,Set.copyOf(first));
    }
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        if(!Collections.disjoint(OUTPUTS,construction)||!Collections.disjoint(OUTPUTS,currentInputs))throw new IllegalArgumentException("native pagination overlaps reconstruction/current input");
        var all=writers(data,OUTPUTS);if(!all.equals(wholeWriters()))throw new IllegalArgumentException("native pagination original writer drift actual="+all);
        var potential=FmcReconstructionExecAudit.inspect(data);var expected=new HashSet<>(processes());add(expected,"CduPgAtcTransmitted","specialJob","(ILjava/lang/String;I)V");add(expected,"ControlDisplayUnit","selectCduPage","(LCduHub$Page;I)V");add(expected,"FmcRte","readGlobPgsOf","()V");
        if(!potential.writers().getOrDefault(PAGE,Set.of()).equals(expected)||!potential.writers().getOrDefault(FIRST,Set.of()).equals(firstProcesses()))throw new IllegalArgumentException("ordinary EXEC pagination publisher drift");
        for(var group:List.of(FmcNarrowContent.HEADER_INPUTS,FmcNarrowContent.PERF_INPUTS,FmcNarrowContent.WAYPOINT_INPUTS))for(var input:group)if(OUTPUTS.contains(new FmcQualification.Member(input.owner(),input.name(),input.descriptor())))throw new IllegalArgumentException("pagination cannot be saved input");
        return OUTPUTS;
    }
    static FmcReconstructionNormalExec.Profile apply(Prepare data,FmcReconstructionNormalExec.Profile normal,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){var fields=new HashSet<>(normal.fields());fields.addAll(inspect(data,construction,currentInputs));return new FmcReconstructionNormalExec.Profile(Set.copyOf(fields),normal.banks(),normal.retainedInputs(),normal.bankReaders(),normal.references(),normal.dynamicScratchpads(),normal.atomicCurrentOutputs());}
    static Map<FmcQualification.Member,Set<Prepare.MethodId>> writers(Prepare data,Set<FmcQualification.Member> fields){
        var result=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();for(var e:data.methods.entrySet())if(e.getValue().code().isPresent())for(var op:Prepare.instructions(e.getValue()))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){
            var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field!=null&&fields.contains(field))result.computeIfAbsent(field,k->new HashSet<>()).add(e.getKey());
        }var frozen=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();result.forEach((k,v)->frozen.put(k,Set.copyOf(v)));return Map.copyOf(frozen);
    }
    public static void main(String[] args)throws Exception{
        var data=new Prepare(Prepare.readJar(Path.of(args[0])));var members=new TreeSet<FmcQualification.Member>();for(var field:data.models.get("CduHub$Page").fields())members.add(new FmcQualification.Member("CduHub$Page",field.fieldName().stringValue(),field.fieldType().stringValue()));for(String name:List.of("ldgAlt","ldgAltPending","ldgAltSetTimer"))members.add(new FmcQualification.Member("FmcRte",name,name.endsWith("Timer")?"F":"I"));
        var all=writers(data,members);var potential=FmcReconstructionExecAudit.inspect(data);var recipe=FmcReconstructionProfile.qualify(data,Map.of());for(var f:members)if(all.containsKey(f))System.out.println("FIELD "+f+" writers="+all.get(f).stream().map(Object::toString).sorted().toList()+" potential="+potential.writers().getOrDefault(f,Set.of())+" recipeInput="+recipe.currentInputs().contains(f)+" recipeWrite="+recipe.writes().contains(f));
    }
}
