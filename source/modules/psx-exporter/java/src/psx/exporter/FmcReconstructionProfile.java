package psx.exporter;

import java.util.*;

/** Trusted source-side roster for the assembled ordinary-thread store guards
 * and receiver-qualified background admission. Artifact bytes cannot select
 * a mutator, store or current-state input. The assembled header/control and
 * ordinary EXEC profiles are joined by the trusted driver before admission. */
final class FmcReconstructionProfile {
    record Qualified(Map<String,byte[]> changed,Set<Prepare.MethodId> roots,Set<FmcQualification.Member> writes,
                     Set<FmcQualification.Member> currentInputs,Set<FmcQualification.Member> preservedObjects){}
    private FmcReconstructionProfile(){}
    static Set<Prepare.MethodId> roots(){
        var result=new HashSet<Prepare.MethodId>();
        result.add(new Prepare.MethodId("RouteWpt",FmcReconstructionPrepare.RESET,"(Ljava/lang/Object;Ljava/lang/Object;)V"));
        result.add(new Prepare.MethodId("RouteWpt",FmcReconstructionPrepare.ENTERED,"(Ljava/lang/Object;[J[Ljava/lang/String;)V"));
        result.add(new Prepare.MethodId("Routes$RteBody",FmcReconstructionPrepare.COUNT,"(Ljava/lang/Object;I)V"));
        result.add(new Prepare.MethodId("Routes$RteBody",FmcReconstructionPrepare.CURRENT_POSITION,"(Ljava/lang/Object;I)V"));
        for(var clone:FmcReconstructionEffects.CLONES)result.add(new Prepare.MethodId(clone.original().owner(),clone.generated(),clone.original().descriptor()));
        result.addAll(FmcReconstructionBodyPrepare.roots());result.addAll(FmcReconstructionPerfPrepare.roots());result.addAll(FmcReconstructionBanks.roots());
        result.addAll(FmcReconstructionGeometryPrepare.roots());
        result.addAll(FmcReconstructionBodyDefaults.roots());
        return Set.copyOf(result);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> before)throws Exception{
        var cells=FmcReconstructionPrepare.transform(data,before);var courses=FmcReconstructionEffects.transform(data,cells.changed());
        var changed=FmcReconstructionPrepare.routeSetters(data,courses.changed());
        changed=FmcReconstructionBanks.transform(data,changed);changed=FmcReconstructionPerfPrepare.transform(data,changed);
        var geometry=FmcReconstructionGeometryPrepare.transform(data,changed);changed=geometry.changed();
        var body=FmcReconstructionBodyPrepare.transform(data,changed);changed=body.changed();
        changed=FmcReconstructionBodyDefaults.transform(data,changed);
        var writes=new HashSet<>(cells.entered());writes.addAll(cells.calculated());writes.addAll(courses.writes());
        writes.add(new FmcQualification.Member("Routes$RteBody","legsAmo","I"));
        writes.addAll(body.writes());writes.addAll(FmcReconstructionBanks.writes());writes.addAll(FmcReconstructionPerfPrepare.writes());
        writes.addAll(geometry.writes());
        writes.addAll(FmcReconstructionBodyDefaults.writes());
        var inputs=new HashSet<>(courses.currentInputs());inputs.add(new FmcQualification.Member("Routes","nrp","LFmcGlobal$Pos;"));
        inputs.add(new FmcQualification.Member("FmcGlobal$Pos","lat","D"));inputs.add(new FmcQualification.Member("FmcGlobal$Pos","lon","D"));
        inputs.addAll(body.inputs());inputs.add(new FmcQualification.Member("FmcProgress2","refTotFuelQty1000","F"));inputs.add(new FmcQualification.Member("FmcPerf","ballast","I"));
        // Native manual/sensed fuel control and its displayed history are
        // current inputs. Ordinary EXEC does not write these cells. Watch
        // outside changes throughout restore even when no load kernel reads
        // a particular preserved field for its own arithmetic.
        for(String name:FmcPerfInputsPrepare.PRESERVED){var field=data.models.get("FmcPerf").fields().stream().filter(f->f.fieldName().equalsString(name)).toList();if(field.size()!=1||(field.getFirst().flags().flagsMask()&java.lang.classfile.ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("native current fuel preservation field "+name);inputs.add(new FmcQualification.Member("FmcPerf",name,field.getFirst().fieldType().stringValue()));}
        inputs.addAll(geometry.inputs());
        // Native hold symbols are rebuilt to the current left/right symbol.
        // Their waypoint membership survives, while that derived reference is
        // owned by the native hold cache; forecast object membership survives.
        var preserved=new HashSet<>(cells.preserved());preserved.remove(new FmcQualification.Member("RouteWpt","pHold","Ljava/awt/geom/GeneralPath;"));
        return new Qualified(changed,roots(),Set.copyOf(writes),Set.copyOf(inputs),Set.copyOf(preserved));
    }
}
