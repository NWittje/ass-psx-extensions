package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Exact route-cell publication kernels. Resetting a live waypoint with native
 * blank() would also erase its live winds; replacing it would break aliases.
 * These kernels retain every object/array slot and reset calculated scalar and
 * String cells from a freshly constructed native default before entered cells
 * are assigned. The complete adapter must bind those defaults to the qualified
 * fresh constructor and hold ordinary-EXEC mutation authority at every call. */
final class FmcReconstructionPrepare {
    static final String RESET="$psxExporter$fmcResetCalculatedCells";
    static final String ENTERED="$psxExporter$fmcWriteEnteredCells";
    static final String COUNT="$psxExporter$fmcWriteEnteredCount";
    static final String CURRENT_POSITION="$psxExporter$fmcWriteCurrentPosition";
    static final String DEFAULTS="$psxExporter$fmcCreateWaypointDefaults";
    static final String OWNER="RouteWpt";
    private static final List<String> INTS=List.of("eta","fuel","modBit","holdExitTime","holdExitTime10");
    private static final List<String> FLOATS=List.of("ndx","ndy","flovLat","flovLon","flovLat2","flovLon2","flovRadius","flovDeg",
        "brgDeg","recipBrgDeg","brgDiffDeg","rteOffsetLat","rteOffsetLon","rteOffsetRecipBrgDeg","rteOffsetBrgDiffDeg",
        "altConstrNdX","holdCrsRadian","clawAngle","clawLat1","clawLon1","clawLat2","clawLon2","clawEndDist");
    private static final List<String> DOUBLES=List.of("brg","recipBrg","brgDiff","dst","dstNd","flyByDst","flyByRadius",
        "rteOffsetDst","rteOffsetBrg","rteOffsetRecipBrg","rteOffsetBrgDiff","rteOffsetFlyByRadius","rteOffsetFlyByDst","dstTrkd","vecLength");
    private static final List<String> BOOLEANS=List.of("showSymbol","holding","flyOver","chg135","chg135ng","terminateManually",
        "noNdSymbol","showRadOrDmeInName","showAltInName","priorRfLeg","unableAlt");
    private static final List<String> STRINGS=List.of("altConstraintDisplay","etaDisplay","altName","latlonStr","cssStr");
    static final List<FmcQualification.Member> PRESERVED=List.of(
        new FmcQualification.Member(OWNER,"winds","LFmcWindOat;"),new FmcQualification.Member(OWNER,"pHold","Ljava/awt/geom/GeneralPath;"));
    record Qualified(Map<String,byte[]> changed,List<FmcQualification.Member> entered,
                     List<FmcQualification.Member> calculated,List<FmcQualification.Member> preserved){}
    private FmcReconstructionPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction "+why);}
    static List<FmcQualification.Member> calculated(){
        var fields=new ArrayList<FmcQualification.Member>();
        for(var group:List.of(Map.entry("I",INTS),Map.entry("F",FLOATS),Map.entry("D",DOUBLES),Map.entry("Z",BOOLEANS),Map.entry("Ljava/lang/String;",STRINGS)))
            for(String name:group.getValue())fields.add(new FmcQualification.Member(OWNER,name,group.getKey()));
        return List.copyOf(fields);
    }
    static List<FmcQualification.Member> entered(){return FmcNarrowContent.WAYPOINT_INPUTS.stream()
        .map(f->new FmcQualification.Member(f.owner(),f.name(),f.descriptor())).toList();}
    private static void qualifyFields(ClassModel model){
        require(model!=null&&model.superclass().orElseThrow().asInternalName().equals("java/lang/Object"),"waypoint owner/hierarchy");
        var expected=new HashSet<FmcQualification.Member>();
        for(var fields:List.of(entered(),calculated(),PRESERVED))for(var member:fields)require(expected.add(member),"disjoint complete cells");
        var actual=new HashSet<FmcQualification.Member>();
        for(var field:model.fields())if((field.flags().flagsMask()&ClassFile.ACC_STATIC)==0){
            require((field.flags().flagsMask()&ClassFile.ACC_FINAL)==0,"mutable native cell");
            actual.add(new FmcQualification.Member(OWNER,field.fieldName().stringValue(),field.fieldType().stringValue()));
        }
        require(actual.equals(expected),"complete waypoint cell roster: unexpected "+difference(actual,expected)+"; absent "+difference(expected,actual));
    }
    private static Set<FmcQualification.Member> difference(Set<FmcQualification.Member>a,Set<FmcQualification.Member>b){var r=new TreeSet<>(a);r.removeAll(b);return r;}
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);qualifyFields(data.models.get(OWNER));qualifyFields(models.get(OWNER));
        ClassDesc owner=ClassDesc.of(OWNER);byte[] input=lower.getOrDefault(OWNER,data.inputs.get(OWNER));var model=data.cf.parse(input);
        require(model.methods().stream().noneMatch(m->m.methodName().equalsString(RESET)||m.methodName().equalsString(ENTERED)),"kernel collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(RESET,MethodTypeDesc.of(CD_void,CD_Object,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.aload(0).checkcast(owner).astore(0);c.aload(1).checkcast(owner).astore(1);
                    for(var field:calculated()){var type=ClassDesc.ofDescriptor(field.descriptor());c.aload(0).aload(1).getfield(owner,field.name(),type).putfield(owner,field.name(),type);}
                    // turnDirValid belongs to the serialized table but is a
                    // calculated validity witness, never authoritative input.
                    c.aload(0).iconst_0().putfield(owner,"turnDirValid",CD_boolean).return_();
                });
                b.withMethodBody(ENTERED,MethodTypeDesc.of(CD_void,CD_Object,CD_long.arrayType(),CD_String.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();c.aload(1).arraylength().loadConstant(27).if_icmpne(bad);
                    c.aload(2).arraylength().loadConstant(27).if_icmpne(bad);c.aload(0).checkcast(owner).astore(0);
                    for(int i=0;i<entered().size();i++){
                        var field=entered().get(i);if(field.name().equals("turnDirValid"))continue;
                        c.aload(0);switch(field.descriptor()){
                            case "Ljava/lang/String;"->c.aload(2).loadConstant(i).aaload();
                            case "F"->c.aload(1).loadConstant(i).laload().l2i().invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));
                            case "C"->c.aload(1).loadConstant(i).laload().l2i().i2c();
                            case "I","Z"->c.aload(1).loadConstant(i).laload().l2i();
                            default->throw new IllegalArgumentException("FMC reconstruction entered descriptor");
                        }
                        c.putfield(owner,field.name(),ClassDesc.ofDescriptor(field.descriptor()));
                    }
                    c.return_();c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC reconstruction complete entered cells")
                        .invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
                });
            }
        });
        require(data.cf.verify(output).isEmpty(),"kernel verifier");
        var changed=new HashMap<>(lower);changed.put(OWNER,output);var before=new HashMap<>(lower);before.put(OWNER,input);
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        // This audit operates on generated bytecode, rather than trusting the
        // generator's intent or a broader native mutator's effect list.
        var resetWrites=new HashSet<FmcQualification.Member>();var enteredWrites=new HashSet<FmcQualification.Member>();
        for(var method:data.cf.parse(output).methods())if(method.methodName().equalsString(RESET)||method.methodName().equalsString(ENTERED)){
            var writes=method.methodName().equalsString(RESET)?resetWrites:enteredWrites;
            for(var ins:Prepare.instructions(method)){
                require(ins.opcode()!=Opcode.PUTSTATIC&&ins.opcode()!=Opcode.AASTORE,"root/static publication in waypoint kernel");
                if(ins instanceof FieldInstruction field&&field.opcode()==Opcode.PUTFIELD)require(writes.add(new FmcQualification.Member(field.owner().asInternalName(),field.name().stringValue(),field.type().stringValue())),"duplicate calculated/entered write");
                if(ins instanceof InvokeInstruction call)require(call.owner().asInternalName().equals("java/lang/Float")||call.owner().asInternalName().equals("java/lang/IllegalArgumentException"),"external native call in waypoint kernel");
            }
        }
        var resetExpected=new HashSet<>(calculated());resetExpected.add(new FmcQualification.Member(OWNER,"turnDirValid","Z"));
        var enteredExpected=new HashSet<>(entered());enteredExpected.remove(new FmcQualification.Member(OWNER,"turnDirValid","Z"));
        require(resetWrites.equals(resetExpected)&&enteredWrites.equals(enteredExpected),"complete exact generated write sets");
        return new Qualified(Map.copyOf(changed),entered(),calculated(),PRESERVED);
    }
    /** Count/current-position setters contain no parser, bank transfer or root
     * replacement. Current coordinates use exactly the installed loader's
     * qualified double-to-float expression. The private fresh factory proof is
     * reused as data; that factory is never invoked by preparation. */
    static Map<String,byte[]> routeSetters(Prepare data,Map<String,byte[]> lower){
        var current=FmcFreshOwnshipPrepare.transform(data,lower).changed();var changed=new HashMap<>(current);
        String name="Routes$RteBody";var body=data.cf.parse(current.get(name));ClassDesc b=ClassDesc.of(name),w=ClassDesc.of(OWNER),r=ClassDesc.of("Routes"),p=ClassDesc.of("FmcGlobal$Pos");
        require(body.methods().stream().noneMatch(m->m.methodName().equalsString(COUNT)||m.methodName().equalsString(CURRENT_POSITION)),"route setter collision");
        byte[] output=data.cf.transformClass(body,new ClassTransform(){
            public void accept(ClassBuilder builder,ClassElement element){builder.with(element);}
            public void atEnd(ClassBuilder builder){
                builder.withMethodBody(COUNT,MethodTypeDesc.of(CD_void,CD_Object,CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();c.iload(1).iconst_3().if_icmplt(bad);c.iload(1).loadConstant(253).if_icmpgt(bad);
                    c.aload(0).checkcast(b).iload(1).putfield(b,"legsAmo",CD_int).return_();bad(c,bad,"FMC reconstruction entered count");
                });
                builder.withMethodBody(CURRENT_POSITION,MethodTypeDesc.of(CD_void,CD_Object,CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();c.iload(1).iflt(bad);c.iload(1).loadConstant(253).if_icmpge(bad);c.aload(0).checkcast(b).astore(0);
                    for(String coordinate:List.of("lat","lon"))c.aload(0).getfield(b,"leg",w.arrayType()).iload(1).aaload()
                        .getstatic(r,"nrp",p).getfield(p,coordinate,CD_double).d2f().putfield(w,coordinate,CD_float);
                    c.return_();bad(c,bad,"FMC reconstruction current-position index");
                });
            }
        });require(data.cf.verify(output).isEmpty(),"route setters verifier");changed.put(name,output);
        var waypoint=data.cf.parse(changed.getOrDefault(OWNER,data.inputs.get(OWNER)));require(waypoint.methods().stream().noneMatch(m->m.methodName().equalsString(DEFAULTS)),"default factory collision");
        byte[] defaults=data.cf.transformClass(waypoint,new ClassTransform(){
            public void accept(ClassBuilder builder,ClassElement element){builder.with(element);}
            public void atEnd(ClassBuilder builder){builder.withMethodBody(DEFAULTS,MethodTypeDesc.of(CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                var loop=c.newLabel();var done=c.newLabel();c.loadConstant(253).anewarray(w).astore(0).iconst_0().istore(1).labelBinding(loop).iload(1).loadConstant(253).if_icmpge(done);
                c.aload(0).iload(1).new_(w).dup().invokespecial(w,"<init>",MethodTypeDesc.of(CD_void)).aastore().iinc(1,1).goto_(loop).labelBinding(done).aload(0).areturn();
            });}
        });require(data.cf.verify(defaults).isEmpty(),"default factory verifier");changed.put(OWNER,defaults);
        TerrainAccess.verifyAdded(current,changed,data.inputs);return Map.copyOf(changed);
    }
    private static void bad(CodeBuilder c,Label bad,String reason){c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant(reason)
        .invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();}
}
