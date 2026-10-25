package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only, nonconsuming IRU observation. No native monitor, geometry setter,
 * result allocation or target argument. Receiver/access coverage remains required. */
public final class IosIrsReadbackPrepare {
    public static final String READ="psxExporterIosObserveIrs";
    private static final ClassDesc U=ClassDesc.of("InertialReference$InertialReferenceUnit"),
        POINT=ClassDesc.of("Calc$PlaceBearingDistance"),P=ClassDesc.of("psx.ios.IosIrsReadback");
    private static final Map<String,String> DOUBLES=Map.ofEntries(
        Map.entry("latitude","lat"),Map.entry("longitude","lon"),Map.entry("bearing","iruBrg"),Map.entry("distance","iruDist"),
        Map.entry("driftBearing","driftBrg"),Map.entry("driftSpeed","driftSpd"),Map.entry("distanceError","currDistError"),
        Map.entry("quality","qualityFac"),Map.entry("lastLatitude","lastRealLat"),Map.entry("lastLongitude","lastRealLon"));
    private static final Map<String,String> INTS=Map.of("mode","mode","lastMode","lastMode","alignedCount","decisAligned",
        "requestedMode","modeRequested","switchTimer","modeSwitchTimer","alignPhase","alignPhase");
    private static final Map<String,String> BOOLS=Map.of("aligned","isAligned","good","good","aligning","aligning","quickAlign","quickAlign",
        "headingEntered","isHdgEntered","powered","isPowered","onDc","onDc","changed","hasChanged");
    private static void field(ClassModel owner,String name,ClassDesc type,boolean external){
        var found=owner.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
        if(found.size()!=1)throw new IllegalArgumentException("Missing/ambiguous IRS observation field "+name);
        var f=found.getFirst();if(!f.fieldTypeSymbol().equals(type)||(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0||
            (external&&(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)!=0))throw new IllegalArgumentException("Changed/inaccessible IRS observation field "+name);
    }
    public static void select(ClassModel unit,ClassModel point){
        if(!unit.thisClass().asSymbol().equals(U)||!point.thisClass().asSymbol().equals(POINT))throw new IllegalArgumentException("Wrong IRS observation owner");
        for(var m:unit.methods())if(m.methodName().equalsString(READ))throw new IllegalArgumentException("IRS observation already prepared");
        for(String n:DOUBLES.values())field(unit,n,CD_double,false);
        for(String n:INTS.values())field(unit,n,CD_int,false);
        for(String n:BOOLS.values())field(unit,n,CD_boolean,false);
        field(unit,"pbd",POINT,false);field(unit,"pbdM",POINT,false);
        field(point,"lat",CD_double,true);field(point,"lon",CD_double,true);
    }
    public static byte[] prepare(ClassFile cf,ClassModel unit,ClassModel point){
        select(unit,point);
        return cf.transformClass(unit,ClassTransform.endHandler(b->b.withMethodBody(READ,MethodTypeDesc.of(CD_void,P),
            ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->{
                var done=c.newLabel();c.aload(1).ifnull(done).aload(1).iconst_0().putfield(P,"available",CD_int)
                    .aload(0).getfield(U,"pbd",POINT).astore(2).aload(0).getfield(U,"pbdM",POINT).astore(3)
                    .aload(2).ifnull(done).aload(3).ifnull(done);
                c.aload(1).aload(0).putfield(P,"unit",CD_Object).aload(1).aload(2).putfield(P,"point",CD_Object)
                    .aload(1).aload(3).putfield(P,"movementPoint",CD_Object);
                for(var e:DOUBLES.entrySet())c.aload(1).aload(0).getfield(U,e.getValue(),CD_double).putfield(P,e.getKey(),CD_double);
                for(var e:INTS.entrySet())c.aload(1).aload(0).getfield(U,e.getValue(),CD_int).putfield(P,e.getKey(),CD_int);
                for(var e:BOOLS.entrySet())c.aload(1).aload(0).getfield(U,e.getValue(),CD_boolean).putfield(P,e.getKey(),CD_boolean);
                for(int slot=2;slot<=3;slot++)for(String kind:List.of("lat","lon"))c.aload(1).aload(slot).getfield(POINT,kind,CD_double)
                    .putfield(P,(slot==2?"point":"movement")+(kind.equals("lat")?"Latitude":"Longitude"),CD_double);
                c.aload(1).iconst_1().putfield(P,"available",CD_int).labelBinding(done).return_();
            })));
    }
    private IosIrsReadbackPrepare(){}
}
