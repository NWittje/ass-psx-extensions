package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Fresh scalar/body-cache defaults in existing live storage. Native aliases
 * and current pilot sequence/history counters remain current. Uncaptured
 * offset fields begin at native fresh defaults; body parsing recalculates the
 * offset end reference, and the separate cancellation kernel retires MOD's
 * old offset-entry cache before ordinary EXEC. The
 * worker factory creates one private body using the independently qualified
 * pure native constructor; no artifact can provide that defaults object. */
final class FmcReconstructionBodyDefaults {
    static final String FACTORY="$psxExporter$fmcBodyDefaults",RESET="$psxExporter$fmcResetBodyCaches";
    static final Set<String> CURRENT=Set.of("globIdx","legsAmo","lastUsedPilotEnteredSeqId","lento3");
    private static final String INTS="viasAmo wptAddMode legsAmoNew offsetNm offsetEndRefNm lastOffsetIdx offsetAmo holdAmo lastRteIdx firstConstraintIdx firstActAltConstrIdx lastWptIdx destRwyIdx firstApproachIdx crsToEnteredDeg intCrsToAct cduDistToAct intCrsToActEx cduDistToActEx ndDistToAct10 ndDistToAct actWptNameUpdCode destIdx noPathDestIdx rtaSegmentInLegs speedSegmIdx1 speedSegmIdx2 rte0FirstIdxNd highestAltConstr maxCssAltMsgTest termAtAltitudeSafetyTimer pbpbStep missedAppIdx";
    private static final String DOUBLES="ndSpread ndExtent crsToAct distToAct origCrsTo origRecipCrsTo distToActTrkd pbdRequestedBrg pbdRequestedDst pbpbRequestedBrg1 pbpbRequestedBrg2 pbpbLa1 pbpbLo1 pbpbLa2 pbpbLo2 actRadius lastRadius modRadius";
    private static final String BOOLEANS="origRwyInLegs actGreater135 beenBeforeActWpt isBeforeActWpt busy boxedDirTo isHoldRequest onFlyOverArc dirToStraight dirToStraightEx";
    private static final String STRINGS="actIlsStr pbpbFixName pbpbNavName";
    static Set<FmcQualification.Member> writes(){var result=new HashSet<FmcQualification.Member>();for(var group:List.of(Map.entry(INTS,"I"),Map.entry(DOUBLES,"D"),Map.entry(BOOLEANS,"Z"),Map.entry(STRINGS,"Ljava/lang/String;")))for(String name:group.getKey().split(" "))result.add(new FmcQualification.Member("Routes$RteBody",name,group.getValue()));return Set.copyOf(result);}
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("Routes$RteBody",RESET,"(Ljava/lang/Object;Ljava/lang/Object;)V"));}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var owner=ClassDesc.of("Routes$RteBody");byte[] input=lower.getOrDefault("Routes$RteBody",data.inputs.get("Routes$RteBody"));var model=data.cf.parse(input);var fields=writes();
        var seen=new HashSet<FmcQualification.Member>();for(var field:model.fields())if((field.flags().flagsMask()&ClassFile.ACC_STATIC)==0){
            String name=field.fieldName().stringValue(),type=field.fieldType().stringValue();var selected=new FmcQualification.Member("Routes$RteBody",name,type);
            if(type.equals("Ljava/lang/String;")||!type.startsWith("L")&&!type.startsWith("[")){if(!CURRENT.contains(name)&&!fields.contains(selected))throw new IllegalArgumentException("FMC body cache roster drift "+selected);if(fields.contains(selected))seen.add(selected);}
        }if(!seen.equals(fields))throw new IllegalArgumentException("FMC body cache native scalar/descriptor roster");
        if(model.methods().stream().anyMatch(m->Set.of(FACTORY,RESET).contains(m.methodName().stringValue())))throw new IllegalArgumentException("FMC body cache bridge collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
            b.withMethodBody(FACTORY,MethodTypeDesc.of(CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.new_(owner).dup().iconst_m1().invokespecial(owner,"<init>",MethodTypeDesc.of(CD_void,CD_int)).areturn());
            b.withMethodBody(RESET,MethodTypeDesc.of(CD_void,CD_Object,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{for(var field:fields.stream().sorted().toList())c.aload(0).checkcast(owner).aload(1).checkcast(owner).getfield(owner,field.name(),ClassDesc.ofDescriptor(field.descriptor())).putfield(owner,field.name(),ClassDesc.ofDescriptor(field.descriptor()));c.return_();});
        }});if(!data.cf.verify(output).isEmpty())throw new IllegalArgumentException("FMC body cache bridge verifier");var changed=new HashMap<>(lower);changed.put("Routes$RteBody",output);var before=new HashMap<>(lower);before.put("Routes$RteBody",input);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
