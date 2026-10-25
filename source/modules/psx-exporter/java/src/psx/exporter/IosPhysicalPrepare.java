package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Isolated, direct physical bindings. Preparation does not activate a writer.
 * Complete input/receiver/lock/effect qualification and authoritative destination
 * support are required in addition to these member/ordering selectors. */
public final class IosPhysicalPrepare {
    public static final String MODE="psxExporterIosConsumeRelocationMode",POSE="psxExporterIosApplyRelocationPose",
        READ="psxExporterIosObservePhysical",REGISTER="psxExporterIosGroundRegister";
    private static final ClassDesc A=ClassDesc.of("Aerodynamics"),G=ClassDesc.of("Gear"),E=ClassDesc.of("Elevation"),
        C=ClassDesc.of("Calc"),ATT=ClassDesc.of("GearAttitude"),VALUE=ClassDesc.of("GearAttitude$Attitude"),
        IR=ClassDesc.of("InertialReference"),UNIT=ClassDesc.of("InertialReference$InertialReferenceUnit"),
        Q=ClassDesc.of("Qi"),QB=ClassDesc.of("Q"),GL=ClassDesc.of("Glob"),MATH=ClassDesc.of("java.lang.Math"),
        T=ClassDesc.of("psx.ios.IosTwoStageRelocation$Target"),P=ClassDesc.of("psx.ios.IosPhysicalReadback");
    private static final Map<String,String> DOUBLE=Map.of("latitude","realLat","longitude","realLon","trueHeading","trueHeading",
        "previousLatitude","realLatEx","previousLongitude","realLonEx");
    private static final Map<String,String> FLOAT=Map.ofEntries(Map.entry("altitude","altitudeReal"),Map.entry("pitch","pitch"),
        Map.entry("totalPitch","totalPitch"),Map.entry("bank","bank"),Map.entry("tas","tasReal"),Map.entry("groundSpeed","gsReal"),
        Map.entry("verticalSpeed","vtVel"),Map.entry("elevation","elev"),Map.entry("support","realElevWithStrut"),Map.entry("strut","strut"),
        Map.entry("targetPitch","targPitch"),Map.entry("targetBank","targBank"),Map.entry("targetAltitude","altitudeDamper"),
        Map.entry("targetTas","tasDamper"),Map.entry("magneticVariation","magVar"));
    public record Selection(List<String> fields,List<String> nativeMethods){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static String internal(ClassDesc d){String s=d.descriptorString();return s.substring(1,s.length()-1);}
    private static void field(Map<String,ClassModel> models,ClassDesc owner,String name,ClassDesc type,boolean statik,boolean external){
        var c=models.get(internal(owner));require(c!=null,"Missing physical storage owner "+owner);
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing physical field "+owner+"."+name);
        var f=all.getFirst();require(f.fieldTypeSymbol().equals(type)&&((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==statik&&
            (f.flags().flagsMask()&ClassFile.ACC_FINAL)==0&&(!external||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible physical field "+owner+"."+name);
    }
    private static void method(Map<String,ClassModel> models,ClassDesc owner,String name,String descriptor){
        var m=models.get(internal(owner)).methods().stream().filter(x->x.methodName().equalsString(name)&&x.methodType().equalsString(descriptor)).findFirst().orElseThrow();
        require(m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,"Changed physical native dependency "+name);
    }
    public static Selection select(Map<String,ClassModel> models){
        for(var c:models.values())for(var m:c.methods())require(!Set.of(MODE,POSE,READ,REGISTER).contains(m.methodName().stringValue()),"Physical binding already exists");
        var fields=new TreeSet<String>();
        for(String name:DOUBLE.values())field(models,A,name,CD_double,true,false);
        for(String name:FLOAT.values())field(models,A,name,CD_float,true,false);
        for(String name:List.of("ground","groundEx","nanStop","gridNorthOn","gridSouthOn"))field(models,A,name,CD_boolean,true,false);
        for(String name:List.of("targTrueHeading","driftAngByWind","gsFeet"))field(models,A,name,CD_double,true,false);
        for(String name:List.of("noseGearOsc","noseGearOscSine","noseGearCompress","noseGearAmpl","lastAltitudeRealForFpv","thrustTas",
            "targVtVel","ywVel","targYwVel","ptVel","bkVel","targBkVel","longiHdg","publicLongiHdg","gearPitchDn","gearPitchUp","magVarNew",
            "sinPitch","cosPitch","sinBank","cosBank","absBank","height"))field(models,A,name,CD_float,true,false);
        field(models,A,"topDown",CD_boolean,true,false);field(models,A,"topDnFac",CD_int,true,false);
        for(String name:List.of("realElev","targRealElev","injectedElev"))field(models,E,name,CD_float,true,true);
        field(models,E,"sceneryElev",CD_double,true,true);
        for(String name:List.of("aeroGround","noseGearTouchedDown","noseFuselageTouchedDown","allMainUp"))field(models,G,name,CD_boolean,true,true);
        field(models,G,"nose",ClassDesc.of("Gear$NoseGroup"),true,true);field(models,ClassDesc.of("Gear$Group"),"down",CD_boolean,false,true);
        field(models,ATT,"now",VALUE,true,true);
        for(String name:List.of("strut","pLimDn","pLimUp"))field(models,VALUE,name,CD_float,false,true);
        field(models,IR,"iru",UNIT.arrayType(),true,true);
        field(models,GL,"qi",Q.arrayType(),true,true);field(models,GL,"qiOnGround",CD_int,true,true);
        for(String name:List.of("min","max","iVar"))field(models,Q,name,CD_int,false,false);
        field(models,QB,"serialNr",CD_int,false,true);
        for(String name:List.of("fresh","updated"))field(models,QB,name,CD_boolean,false,true);
        method(models,A,"readStartPiBaHeAlVsTas","()V");method(models,A,"runDelta","()V");method(models,C,"getMagvar","(DD)F");
        // Native source sites corroborate storage ownership; not complete effect/receiver proof.
        for(var owner:List.of(A,E,G,VALUE,Q,QB,IR))for(var f:models.get(internal(owner)).fields())
            if(!f.fieldName().stringValue().startsWith("Copyright"))fields.add(internal(owner)+"."+f.fieldName()+":"+f.fieldType());
        return new Selection(List.copyOf(fields),List.of("Aerodynamics.readStartPiBaHeAlVsTas()V","Aerodynamics.runDelta()V","Calc.getMagvar(DD)F"));
    }
    public static byte[] register(ClassFile cf,ClassModel qi){return cf.transformClass(qi,ClassTransform.endHandler(b->b.withMethodBody(REGISTER,
        MethodTypeDesc.of(CD_boolean,CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->{
            var bad=c.newLabel();c.getstatic(GL,"qi",Q.arrayType()).ifnull(bad)
                .getstatic(GL,"qiOnGround",CD_int).sipush(257).if_icmpne(bad)
                .getstatic(GL,"qi",Q.arrayType()).arraylength().sipush(257).if_icmple(bad)
                .getstatic(GL,"qi",Q.arrayType()).sipush(257).aaload().aload(0).if_acmpne(bad)
                .aload(0).getfield(QB,"serialNr",CD_int).sipush(257).if_icmpne(bad)
                .aload(0).getfield(Q,"min",CD_int).ifne(bad).aload(0).getfield(Q,"max",CD_int).iconst_1().if_icmpne(bad)
                .aload(0).iload(1).putfield(Q,"iVar",CD_int).aload(0).iconst_0().putfield(QB,"fresh",CD_boolean)
                .aload(0).iconst_0().putfield(QB,"updated",CD_boolean).iconst_1().ireturn().labelBinding(bad).iconst_0().ireturn();
        })));
    }
    private static void target(CodeBuilder c,String name,ClassDesc result){c.aload(0).invokevirtual(T,name,MethodTypeDesc.of(result));}
    private static void floats(CodeBuilder c,int slot,String... names){for(String name:names)c.fload(slot).putstatic(A,name,CD_float);}
    private static void zeros(CodeBuilder c,String... names){for(String name:names)c.fconst_0().putstatic(A,name,CD_float);}
    public static byte[] aerodynamics(ClassFile cf,ClassModel aero){return cf.transformClass(aero,ClassTransform.endHandler(b->{
        b.withMethodBody(MODE,MethodTypeDesc.of(CD_void,CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var fail=c.newLabel();c.getstatic(GL,"qi",Q.arrayType()).getstatic(GL,"qiOnGround",CD_int).aaload().iload(0)
                .invokevirtual(Q,REGISTER,MethodTypeDesc.of(CD_boolean,CD_boolean)).ifeq(fail)
                .iload(0).putstatic(A,"ground",CD_boolean).iload(0).putstatic(A,"groundEx",CD_boolean).return_();
            c.labelBinding(fail).new_(ClassDesc.of("java.lang.IllegalStateException")).dup().ldc("Changed ground register")
                .invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
        });
        b.withMethodBody(POSE,MethodTypeDesc.of(CD_boolean,T,CD_float,CD_float,CD_float,CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var failed=c.newLabel();var air=c.newLabel();var attitude=c.newLabel();var north=c.newLabel();var south=c.newLabel();var heading=c.newLabel();
            c.aload(0).ifnull(failed).fload(1).invokestatic(ClassDesc.of("java.lang.Float"),"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(failed)
                .fload(2).invokestatic(ClassDesc.of("java.lang.Float"),"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(failed)
                .fload(1).fconst_0().fcmpg().iflt(failed);
            target(c,"ground",CD_boolean);c.getstatic(A,"ground",CD_boolean).if_icmpne(failed);
            // Pre-resolved destination variation avoids native exceptional logging
            // and lazy magnetic-table initialization inside owner exclusion.
            c.fload(3).fstore(4).fload(4).invokestatic(ClassDesc.of("java.lang.Float"),"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(failed);
            c.getstatic(ATT,"now",VALUE).ifnull(failed);
            target(c,"pitch",CD_float);c.fstore(5);target(c,"bank",CD_float);c.fstore(6);target(c,"altitudeMsl",CD_float);c.fstore(7);
            target(c,"latitude",CD_double);c.putstatic(A,"realLat",CD_double);target(c,"longitude",CD_double);c.putstatic(A,"realLon",CD_double);
            c.getstatic(A,"realLat",CD_double).putstatic(A,"realLatEx",CD_double).getstatic(A,"realLon",CD_double).putstatic(A,"realLonEx",CD_double);
            target(c,"trueHeading",CD_double);c.invokestatic(A,IosHeadingPrepare.APPLY,MethodTypeDesc.of(CD_void,CD_double));
            c.fload(2).putstatic(E,"realElev",CD_float).fload(2).putstatic(E,"targRealElev",CD_float).fload(2).f2d().putstatic(E,"sceneryElev",CD_double)
                .ldc(-999999f).putstatic(E,"injectedElev",CD_float).fload(2).putstatic(A,"elev",CD_float);
            target(c,"ground",CD_boolean);c.ifeq(air);
            zeros(c,"pitch","targPitch","totalPitch","bank","targBank","gsReal");
            c.getstatic(ATT,"now",VALUE).invokevirtual(VALUE,IosGroundContactPrepare.CONTACT,MethodTypeDesc.of(CD_void))
                .getstatic(ATT,"now",VALUE).fconst_0().invokevirtual(VALUE,IosGroundContactPrepare.BANK,MethodTypeDesc.of(CD_float,CD_float)).fstore(6)
                .getstatic(ATT,"now",VALUE).getfield(VALUE,"pLimDn",CD_float).fstore(5)
                .getstatic(ATT,"now",VALUE).getfield(VALUE,"strut",CD_float).putstatic(A,"strut",CD_float)
                .fload(2).getstatic(A,"strut",CD_float).fadd().fstore(7).fconst_0().fstore(1).goto_(attitude);
            c.labelBinding(air).fconst_0().putstatic(A,"strut",CD_float);
            c.labelBinding(attitude);floats(c,5,"pitch","targPitch","totalPitch","gearPitchDn");floats(c,6,"bank","targBank");
            c.getstatic(ATT,"now",VALUE).getfield(VALUE,"pLimUp",CD_float).putstatic(A,"gearPitchUp",CD_float);
            floats(c,7,"altitudeReal","altitudeDamper","lastAltitudeRealForFpv");floats(c,1,"tasReal","tasDamper","thrustTas");floats(c,4,"magVar","magVarNew");
            zeros(c,"vtVel","targVtVel","ywVel","targYwVel","ptVel","bkVel","targBkVel","noseGearOsc","noseGearOscSine","noseGearCompress","noseGearAmpl");
            c.getstatic(A,"elev",CD_float).getstatic(A,"strut",CD_float).fadd().putstatic(A,"realElevWithStrut",CD_float)
                .fload(7).fload(2).fsub().putstatic(A,"height",CD_float)
                .fload(6).invokestatic(ClassDesc.of("java.lang.Math"),"abs",MethodTypeDesc.of(CD_float,CD_float)).putstatic(A,"absBank",CD_float)
                .iconst_0().putstatic(A,"topDown",CD_boolean).iconst_1().putstatic(A,"topDnFac",CD_int);
            for(String kind:List.of("Pitch","Bank"))for(String trig:List.of("sin","cos"))c.fload(kind.equals("Pitch")?5:6).f2d()
                .invokestatic(MATH,trig,MethodTypeDesc.of(CD_double,CD_double)).d2f().putstatic(A,trig+kind,CD_float);
            target(c,"ground",CD_boolean);c.putstatic(G,"aeroGround",CD_boolean);
            target(c,"ground",CD_boolean);c.getstatic(G,"nose",ClassDesc.of("Gear$NoseGroup")).getfield(ClassDesc.of("Gear$Group"),"down",CD_boolean)
                .iand().putstatic(G,"noseGearTouchedDown",CD_boolean);
            // Native contact geometry; omit touchdown failure/steering/audio
            // publication. All-up native attitude has +3 degrees, so its nose
            // fuselage remains clear. No existing failure is reset or generated.
            var noFuselage=c.newLabel();var contactDone=c.newLabel();
            target(c,"ground",CD_boolean);c.ifeq(noFuselage).getstatic(G,"allMainUp",CD_boolean).ifeq(noFuselage)
                .getstatic(G,"nose",ClassDesc.of("Gear$NoseGroup")).getfield(ClassDesc.of("Gear$Group"),"down",CD_boolean).ifne(noFuselage)
                .getstatic(A,"pitch",CD_float).fconst_0().fcmpg().ifgt(noFuselage)
                .iconst_1().goto_(contactDone).labelBinding(noFuselage).iconst_0().labelBinding(contactDone)
                .putstatic(G,"noseFuselageTouchedDown",CD_boolean);
            target(c,"ground",CD_boolean);var moving=c.newLabel();c.ifeq(moving);zeros(c,"gsReal");c.dconst_0().putstatic(A,"gsFeet",CD_double).labelBinding(moving);
            c.iconst_1().ireturn().labelBinding(failed).iconst_0().ireturn();
        });
        b.withMethodBody(READ,MethodTypeDesc.of(CD_void,P),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var done=c.newLabel();c.aload(0).ifnull(done).aload(0).iconst_0().putfield(P,"availability",CD_int);
            for(var pair:DOUBLE.entrySet()){
                c.aload(0);
                if(pair.getKey().equals("trueHeading")){
                    var geographic=c.newLabel();var observed=c.newLabel();
                    c.getstatic(A,"gridNorthOn",CD_boolean).ifne(geographic).getstatic(A,"gridSouthOn",CD_boolean).ifne(geographic)
                        .getstatic(A,"trueHeading",CD_double).goto_(observed).labelBinding(geographic)
                        .getstatic(A,"longiHdg",CD_float).f2d().labelBinding(observed);
                }else c.getstatic(A,pair.getValue(),CD_double);
                c.putfield(P,pair.getKey(),CD_double);
            }
            for(var pair:FLOAT.entrySet())c.aload(0).getstatic(A,pair.getValue(),CD_float).putfield(P,pair.getKey(),CD_float);
            for(String name:List.of("ground","groundEx","nanStop"))c.aload(0).getstatic(A,name,CD_boolean).putfield(P,name,CD_boolean);
            c.aload(0).getstatic(G,"aeroGround",CD_boolean).putfield(P,"gearGround",CD_boolean)
                .aload(0).getstatic(G,"noseGearTouchedDown",CD_boolean).putfield(P,"noseContact",CD_boolean)
                .aload(0).getstatic(G,"noseFuselageTouchedDown",CD_boolean).putfield(P,"noseFuselageContact",CD_boolean)
                .aload(0).getstatic(ATT,"now",VALUE).putfield(P,"attitude",CD_Object);
            for(var pair:List.of(Map.entry("irsLeft",0),Map.entry("irsCenter",2),Map.entry("irsRight",1)))
                c.aload(0).getstatic(IR,"iru",UNIT.arrayType()).loadConstant(pair.getValue()).aaload().putfield(P,pair.getKey(),CD_Object);
            c.aload(0).iconst_1().putfield(P,"availability",CD_int).labelBinding(done).return_();
        });
    }));}
    private IosPhysicalPrepare(){}
}
