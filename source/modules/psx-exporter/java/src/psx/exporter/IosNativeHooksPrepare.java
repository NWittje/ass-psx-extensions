package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional native-boundary instrumentation as DATA. Startup does not select it.
 * Qualification of outer activity/edit guards remains a separate prerequisite. */
public final class IosNativeHooksPrepare {
    public static final String POLL="$psxIos$ownerPoll",SELECT="$psxIos$selected",APPLIED="$psxIos$weatherApplied",DYNAMICS="$psxIos$dynamics";
    private static final ClassDesc HOOK=ClassDesc.of("psx.exporter.IosExecutionHooks"),A=ClassDesc.of("Aerodynamics"),W=ClassDesc.of("Weather"),
        O=ClassDesc.of("psx.exporter.Observer"),B=ClassDesc.of("psx.exporter.BridgeNative");
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    private static final Map<String,String> POSE_FIELDS=Map.of("realLat","D","realLon","D","trueHeading","D",
        "altitudeReal","F","pitch","F","bank","F","tasReal","F","gsReal","F","vtVel","F");
    public record Site(String owner,String method,String descriptor,int offset,String event){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    public static List<Site> select(Map<String,ClassModel> models){
        var sites=new ArrayList<Site>();
        for(String owner:List.of("Core","Weather","Aerodynamics")){
            var c=models.get(owner);require(c!=null,"Missing native hook owner");
            require(c.methods().stream().noneMatch(m->m.methodName().stringValue().startsWith("$psxIos$")),"Native hooks already present");
            for(var m:c.methods())if(m.code().isPresent()){
                int integrationStart=owner.equals("Aerodynamics")&&m.methodName().equalsString("runDelta")?integrationStart(m):-1;
                if(owner.equals("Core")&&m.methodName().equalsString("runPresto")){
                    require(m.methodType().equalsString("()V")&&(m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"Changed owner callback");
                    sites.add(new Site(owner,m.methodName().stringValue(),"()V",0,"owner-before-activity"));
                }
                if(owner.equals("Weather")&&m.methodName().equalsString("runModerato"))
                    sites.add(new Site(owner,"runModerato","()V",selectionJoin(m),"successful-destination-selection"));
                int offset=0;for(var e:m.code().get())if(e instanceof Instruction i){
                    if(owner.equals("Weather")&&m.methodName().equalsString("interpolate")&&i instanceof ReturnInstruction){
                        require(m.methodType().equalsString("()V"),"Changed interpolation return");sites.add(new Site(owner,"interpolate","()V",offset,"successful-applied-weather"));
                    }
                    if(owner.equals("Aerodynamics")&&!m.methodName().stringValue().startsWith("psxExporterIos")&&i instanceof InvokeInstruction call&&
                        call.owner().asInternalName().equals(owner)&&Set.of("pitchProgress","bankProgress","yawProgressOfRudderAndAileron","lateralProgress","tasProgress","headingProgress").contains(call.name().stringValue())){
                        require(call.type().equalsString("()V"),"Changed integration descriptor");sites.add(new Site(owner,m.methodName().stringValue(),m.methodType().stringValue(),offset,"native-motion-integration"));
                    }
                    if(integrationStart>=0&&offset>=integrationStart&&i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&
                        f.owner().asInternalName().equals("Aerodynamics")&&POSE_FIELDS.containsKey(f.name().stringValue())){
                        require(f.type().equalsString(POSE_FIELDS.get(f.name().stringValue())),"Changed native pose integration storage");
                        sites.add(new Site(owner,m.methodName().stringValue(),m.methodType().stringValue(),offset,"native-pose-integration-write"));
                    }
                    offset+=i.sizeInBytes();
                }
            }
        }
        require(sites.stream().filter(s->s.event.equals("owner-before-activity")).count()==1&&
            sites.stream().filter(s->s.event.equals("successful-destination-selection")).count()==1&&
            sites.stream().filter(s->s.event.equals("successful-applied-weather")).count()==1&&
            sites.stream().filter(s->s.event.equals("native-motion-integration")).count()>=4&&
            sites.stream().anyMatch(s->s.event.equals("native-pose-integration-write")),"Incomplete native boundary selection");
        return List.copyOf(sites);
    }
    public static Map<String,byte[]> prepare(ClassFile cf,Map<String,ClassModel> models,List<Site> selection){
        require(select(models).equals(selection),"Native boundary selection changed");var out=new TreeMap<String,byte[]>();
        for(String owner:List.of("Core","Weather","Aerodynamics")){
            var c=models.get(owner);ClassDesc type=c.thisClass().asSymbol();
            byte[] bytes=cf.transformClass(c,(builder,element)->{
                if(element instanceof MethodModel m&&m.code().isPresent()){
                    var chosen=selection.stream().filter(s->s.owner.equals(owner)&&s.method.equals(m.methodName().stringValue())&&s.descriptor.equals(m.methodType().stringValue())).toList();
                    if(!chosen.isEmpty()){builder.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                        int offset;
                        public void atStart(CodeBuilder cb){if(chosen.stream().anyMatch(s->s.event.equals("owner-before-activity")))cb.invokestatic(type,POLL,VOID);}
                        public void accept(CodeBuilder cb,CodeElement ce){
                            if(ce instanceof Instruction i){for(var s:chosen)if(s.offset==offset&&!s.event.equals("owner-before-activity"))cb.invokestatic(type,
                                s.event.equals("successful-destination-selection")?SELECT:s.event.equals("successful-applied-weather")?APPLIED:DYNAMICS,VOID);
                                offset+=i.sizeInBytes();}
                            cb.with(ce);
                        }
                    }));return;}
                }
                builder.with(element);
            });
            bytes=cf.transformClass(cf.parse(bytes),ClassTransform.endHandler(b->helpers(b,owner)));
            require(cf.verify(bytes).isEmpty(),"Native hook verifier failure "+owner);out.put(owner,bytes);
        }return Collections.unmodifiableMap(out);
    }
    private static void helpers(ClassBuilder b,String owner){
                if(owner.equals("Core"))safe(b,POLL,cb->cb.invokestatic(HOOK,"ownerPoll",VOID));
                if(owner.equals("Aerodynamics"))safe(b,DYNAMICS,cb->cb.invokestatic(HOOK,"dynamics",VOID));
                if(owner.equals("Weather")){
                    // These are the coordinates actually consumed by the seven
                    // native zone checks, rather than later airframe reads.
                    safe(b,SELECT,cb->{cb.invokestatic(O,"terrainEpoch",MethodTypeDesc.of(CD_long)).invokestatic(B,"qpc",MethodTypeDesc.of(CD_long))
                        .getstatic(W,"tmpLat",CD_double).getstatic(W,"tmpLon",CD_double).getstatic(A,"altitudeReal",CD_float);
                        cb.invokestatic(HOOK,"selected",MethodTypeDesc.of(CD_void,CD_long,CD_long,CD_double,CD_double,CD_float));});
                    safe(b,APPLIED,cb->{position(cb);cb.getstatic(W,"fTemp",CD_float).getstatic(W,"fHpa",CD_float)
                        .getstatic(A,"totalShear",CD_float).getstatic(A,"rollBack",CD_float)
                        .invokestatic(HOOK,"applied",MethodTypeDesc.of(CD_void,CD_long,CD_long,CD_double,CD_double,CD_float,CD_float,CD_float,CD_float,CD_float));});
                }
    }
    /** Rechecks the actual composed result, including omitted callbacks and
     * original catch ordering. Verification of classfile stack types alone cannot
     * detect accidentally dropping the transformation during bundle merging. */
    public static void verifyPrepared(Map<String,ClassModel> before,List<Site> sites,Map<String,ClassModel> after){
        require(select(before).equals(sites),"Changed hook preimages");
        for(String owner:List.of("Core","Weather","Aerodynamics")){
            var original=before.get(owner);var result=after.get(owner);require(result!=null,"Missing hook output "+owner);
            require(original.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList().equals(
                result.fields().stream().map(f->f.fieldName()+":"+f.fieldType()+":"+f.flags().flagsMask()).toList()),"Changed native hook storage");
            for(var m:original.methods()){
                var observed=result.methods().stream().filter(x->x.methodName().equals(m.methodName())&&x.methodType().equals(m.methodType())).findFirst().orElseThrow();
                var expected=sites.stream().filter(s->s.owner.equals(owner)&&s.method.equals(m.methodName().stringValue())&&s.descriptor.equals(m.methodType().stringValue())).toList();
                var calls=new ArrayList<String>();var originalOps=new ArrayList<Instruction>();
                if(m.code().isPresent())for(var e:m.code().get())if(e instanceof Instruction i)originalOps.add(i);
                int ordinal=0,nativeOffset=0;
                if(observed.code().isPresent())for(var e:observed.code().get())if(e instanceof Instruction i){
                    if(i instanceof InvokeInstruction c&&c.owner().asInternalName().equals(owner)&&c.name().stringValue().startsWith("$psxIos$")){
                        require(c.opcode()==Opcode.INVOKESTATIC&&c.type().equalsString("()V"),"Changed callback invocation");calls.add(c.name()+"@"+nativeOffset);
                    }else{require(ordinal<originalOps.size(),"Unexpected native instruction");nativeOffset+=originalOps.get(ordinal++).sizeInBytes();}
                }
                require(calls.equals(expected.stream().sorted(Comparator.comparingInt(Site::offset)).map(s->(s.event.equals("owner-before-activity")?POLL:s.event.equals("successful-destination-selection")?SELECT:s.event.equals("successful-applied-weather")?APPLIED:DYNAMICS)+"@"+s.offset).toList()),"Missing/reordered native hooks "+owner+"."+m.methodName());
                require(IosMethodIdentity.of(m,i->false).equals(IosMethodIdentity.of(observed,i->i instanceof InvokeInstruction c&&c.owner().asInternalName().equals(owner)&&c.name().stringValue().startsWith("$psxIos$"))),"Original native instructions/catches changed "+owner+"."+m.methodName());
            }
            for(String name:owner.equals("Core")?List.of(POLL):owner.equals("Weather")?List.of(SELECT,APPLIED):List.of(DYNAMICS)){
                var helper=result.methods().stream().filter(m->m.methodName().equalsString(name)&&m.methodType().equalsString("()V")).findFirst().orElseThrow();
                require(helper.flags().flagsMask()==(ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC)&&helper.code().isPresent()&&helper.code().get().exceptionHandlers().size()==2,"Changed optional hook isolation");
                var reference=ClassFile.of().parse(ClassFile.of().build(ClassDesc.of(owner),b->helpers(b,owner))).methods().stream().filter(m->m.methodName().equalsString(name)).findFirst().orElseThrow();
                require(IosMethodIdentity.of(helper,i->false).equals(IosMethodIdentity.of(reference,i->false)),"Changed optional hook body/cleanup identity");
            }
        }
    }
    private static void position(CodeBuilder cb){cb.invokestatic(O,"terrainEpoch",MethodTypeDesc.of(CD_long)).invokestatic(B,"qpc",MethodTypeDesc.of(CD_long))
        .getstatic(A,"realLat",CD_double).getstatic(A,"realLon",CD_double).getstatic(A,"altitudeReal",CD_float);}
    /** The native nonzero pause/freeze and loader branch returns before the
     * dynamics region. Observe actual pose integration writes within that region,
     * rather than treating a pause flag or unchanged numbers as motion evidence.
     * Ordinary paused damper/indication processing is a separate source obligation. */
    private static int integrationStart(MethodModel method){
        var ops=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int offset=0;
        for(var e:method.code().orElseThrow()){
            if(e instanceof LabelTarget l)labels.put(l.label(),offset);
            if(e instanceof Instruction i){ops.add(i);offsets.add(offset);offset+=i.sizeInBytes();}
        }
        int found=-1;
        for(int n=0;n+4<ops.size();n++)if(ops.get(n) instanceof FieldInstruction pause&&pause.opcode()==Opcode.GETSTATIC&&
            pause.owner().asInternalName().equals("Core")&&pause.name().equalsString("globSimPaused")&&pause.type().equalsString("I")&&
            ops.get(n+1) instanceof BranchInstruction paused&&paused.opcode()==Opcode.IFNE&&
            ops.get(n+2) instanceof FieldInstruction loader&&loader.opcode()==Opcode.GETSTATIC&&loader.owner().asInternalName().equals("Core")&&
            loader.name().equalsString("loaderPaused")&&loader.type().equalsString("Z")&&
            ops.get(n+3) instanceof BranchInstruction running&&running.opcode()==Opcode.IFEQ){
            require(found<0&&Objects.equals(labels.get(paused.target()),offsets.get(n+4)),"Changed native pause/loader integration gate");
            int start=labels.getOrDefault(running.target(),-1);require(start>offsets.get(n+4),"Missing bounded paused processing region");
            for(int p=0;p<n;p++)if(ops.get(p) instanceof FieldInstruction field&&field.opcode()==Opcode.PUTSTATIC&&
                field.owner().asInternalName().equals("Aerodynamics")&&POSE_FIELDS.containsKey(field.name().stringValue()))
                throw new IllegalArgumentException("Pose write precedes qualified native integration gate");
            int end=offsets.indexOf(start);require(end>0&&ops.get(end-1) instanceof ReturnInstruction,"Paused processing must return before dynamics");
            for(int p=n+4;p<end;p++)if(ops.get(p) instanceof BranchInstruction branch)
                require(labels.getOrDefault(branch.target(),-1)<start,"Paused processing escapes into integration region");
            found=start;
        }
        require(found>=0,"Missing native pause/loader dynamics boundary");return found;
    }
    /** Native zone selection runs in Moderato, independently of Allegro's
     * interpolation. The join is reached even when the selected zone is unchanged.
     * Locates the actual join rather than crediting an interpolation invocation. */
    private static int selectionJoin(MethodModel method){
        require(method.methodType().equalsString("()V")&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"Changed destination selection context");
        var ops=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int offset=0;
        for(var e:method.code().orElseThrow()){
            if(e instanceof LabelTarget l)labels.put(l.label(),offset);
            if(e instanceof Instruction i){ops.add(i);offsets.add(offset);offset+=i.sizeInBytes();}
        }
        int join=-1;var zones=new ArrayList<String>();boolean latitude=false,longitude=false,assigned=false,unchanged=false;
        for(int n=0;n<ops.size();n++){
            var i=ops.get(n);
            if(i instanceof FieldInstruction f){
                if(f.owner().asInternalName().equals("Weather")&&i.opcode()==Opcode.PUTSTATIC){
                    latitude|=f.name().equalsString("tmpLat")&&f.type().equalsString("D");
                    longitude|=f.name().equalsString("tmpLon")&&f.type().equalsString("D");
                    if(f.name().equalsString("currZone")){
                        require(latitude&&longitude&&zones.equals(List.of("wx1","wx2","wx3","wx4","wx5","wx6","wx7")),"Changed zone selection ordering");assigned=true;
                    }
                }
                if(f.owner().asInternalName().equals("EarthTime")&&f.name().equalsString("simDayOfYear")){
                    require(join<0&&assigned&&i.opcode()==Opcode.GETSTATIC&&f.type().equalsString("I"),"Changed zone selection join");join=offsets.get(n);break;
                }
            }
            if(i instanceof InvokeInstruction c&&c.owner().asInternalName().equals("Weather$ZoneWx")&&c.name().equalsString("checkZone")){
                require(c.opcode()==Opcode.INVOKEVIRTUAL&&c.type().equalsString("()V")&&n>0&&ops.get(n-1) instanceof FieldInstruction,"Changed zone receiver");
                var f=(FieldInstruction)ops.get(n-1);require(f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("Weather")&&f.type().equalsString("LWeather$ZoneWx;"),"Changed zone identity");zones.add(f.name().stringValue());
            }
        }
        require(join>=0,"Missing completed destination selection join");
        for(int n=2;n<ops.size();n++)if(ops.get(n) instanceof BranchInstruction b&&b.opcode()==Opcode.IF_ACMPEQ&&Objects.equals(labels.get(b.target()),join)&&
            ops.get(n-2) instanceof FieldInstruction a&&a.owner().asInternalName().equals("Weather")&&a.name().equalsString("currZone")&&
            ops.get(n-1) instanceof FieldInstruction t&&t.owner().asInternalName().equals("Weather")&&t.name().equalsString("tmpZone"))unchanged=true;
        require(unchanged,"Unchanged zone must join successful selection");return join;
    }
    private static void safe(ClassBuilder b,String name,java.util.function.Consumer<CodeBuilder> body){b.withMethodBody(name,VOID,
        ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            cb.trying(block->body.accept(block),catches->catches.catching(CD_Throwable,h->{h.pop();h.trying(c->c.invokestatic(HOOK,"fault",VOID),nested->nested.catching(CD_Throwable,c->c.pop()));}));cb.return_();
        });}
    private IosNativeHooksPrepare(){}
}
