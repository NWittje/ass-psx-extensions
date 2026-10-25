package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import java.util.function.Consumer;
import static java.lang.constant.ConstantDescs.*;

/** Structural qualification of the common-plane native support extension. */
final class TerrainPrepare {
    static final String PREFIX="$psxTerrain$";
    static final ClassDesc AERO=ClassDesc.of("Aerodynamics"),ELEV=ClassDesc.of("Elevation"),T=ClassDesc.of("psx.exporter.Terrain"),OBS=ClassDesc.of("psx.exporter.Observer");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void),FLOAT=MethodTypeDesc.of(CD_float),F_F=MethodTypeDesc.of(CD_float,CD_float);
    final Prepare base;final Set<String> required=new TreeSet<>();final List<String> evidence=new ArrayList<>();
    int capabilities;boolean constraintsQualified;
    final Map<String,Integer> constraintSites=new TreeMap<>();
    TerrainPrepare(Prepare base){this.base=base;}
    private static boolean get(Instruction i,String owner,String name){return Prepare.field(i,Opcode.GETSTATIC,owner,name);}
    private static boolean put(Instruction i,String owner,String name){return Prepare.field(i,Opcode.PUTSTATIC,owner,name);}
    private static boolean fieldName(FieldInstruction f,String owner,String name){return f.owner().asInternalName().equals(owner)&&f.name().equalsString(name);}
    private void field(String owner,String name,String descriptor){
        var model=base.models.get(owner);if(model==null||model.fields().stream().noneMatch(f->f.fieldName().equalsString(name)&&f.fieldType().equalsString(descriptor)&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0))throw new IllegalArgumentException("terrain field "+owner+"."+name);
    }
    void qualify() throws Exception {
        // Current installed audit. Pose/audio remain independently available on an unqualified terrain build.
        Map<String,String> audited=Map.of(
            "Aerodynamics","e68d4f2d2b6850c10b4162a12e55c97451a6263da7e2c424b497d94336cf2e53",
            "Core","d522384326e98914fb2d2028588a6c2c8d1aa8dc7ffdfb7582aee5be49c7e037",
            "Elevation","c4790a012a1e1c06cfb65204553586d717cf8ee34598b11721e4e77b9eccd87b",
            "Gear","11a9db837b095cd0abd26997094f70ad44a4b41b67429c57af1a28390f0ef48a",
            "GearAttitude","9a5cc66d7dff1e95636acceb382a4c0b249707f17e4c2199c6672671aa7b6095",
            "GearAttitude$Attitude","2989ca0a1a8232115f0279450ff778f1eb68ab9318ce34a67ebf1ff86c4bd7ff");
        for(var e:audited.entrySet())if(!base.inputs.containsKey(e.getKey())||!Identity.hash(base.inputs.get(e.getKey())).equals(e.getValue()))throw new IllegalArgumentException("terrain current support audit identity "+e.getKey());
        for(String f:List.of("elev","height","strut","realElevWithStrut","altitudeDamper","altitudeReal","mainGearStrut","noseGearOscSine","noseGearCompress","fpv","bank","targBank","absBank","gearPitchDn","gearPitchUp"))field("Aerodynamics",f,"F");
        field("Aerodynamics","situInitLoop","I");
        for(String f:List.of("realLat","realLon","trueHeading"))field("Aerodynamics",f,"D");
        field("Aerodynamics","ground","Z");field("Gear","noseGearTouchedDown","Z");field("Elevation","realElev","F");field("GearAttitude","now","LGearAttitude$Attitude;");
        var attitude=base.models.get("GearAttitude$Attitude");if(attitude==null||attitude.fields().stream().noneMatch(f->f.fieldName().equalsString("id")&&f.fieldType().equalsString("I")))throw new IllegalArgumentException("terrain support configuration");
        var bank=Prepare.instructions(base.requireStatic("Aerodynamics","bankProgress"));
        long culmination=bank.stream().filter(i->Prepare.call(i,"Aerodynamics","bankCulminator","()V")).count();
        long bankWrites=bank.stream().filter(i->put(i,"Aerodynamics","bank")).count();
        long absWrites=bank.stream().filter(i->put(i,"Aerodynamics","absBank")).count();
        long profileCalls=bank.stream().filter(i->Prepare.call(i,"GearAttitude$Attitude","getBank","(F)F")).count();
        if(culmination!=1||bankWrites!=2||absWrites!=1||profileCalls!=1)throw new IllegalArgumentException("terrain bank support shape");
        var pitch=Prepare.instructions(base.requireStatic("Aerodynamics","pitchProgress"));
        long grades=pitch.stream().filter(i->get(i,"Aerodynamics","fpv")).count();
        long upper=pitch.stream().filter(i->i instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD&&fieldName(f,"GearAttitude$Attitude","pLimUp")).count();
        long lower=pitch.stream().filter(i->i instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD&&fieldName(f,"GearAttitude$Attitude","pLimDn")).count();
        if(grades!=3||upper!=3||lower!=3)throw new IllegalArgumentException("terrain pitch support shape grades="+grades+" limits="+upper+"/"+lower);
        for(String method:List.of("runDelta","tasProgress","pitchProgress","bankProgress"))base.requireStatic("Aerodynamics",method);
        base.requireStatic("Gear","checkTouchDown");
        qualifySupportShape();qualifyConstraintShape();
        for(var entry:base.models.entrySet()){
            String owner=entry.getKey();
            for(MethodModel method:entry.getValue().methods())if(method.code().isPresent())for(Instruction instruction:Prepare.instructions(method)){
                if(get(instruction,"Elevation","realElev")&&!owner.equals("Elevation"))evidence.add("audited native elevation reader "+owner+"."+method.methodName().stringValue());
            }
        }
        required.addAll(List.of("Core","Aerodynamics","Elevation","Gear","GearAttitude","GearAttitude$Attitude"));
        evidence.add("native bank integration/pitch constraints and Gear.checkTouchDown structurally qualified");
        evidence.add("normal=geographic ENU; Aerodynamics reference not labelled CG; no five reaction equations");
        capabilities=2;
    }
    /** Every installed constraint reader/writer and producer arithmetic is inventoried. */
    void qualifyConstraintShape(){
        Map<String,Integer> reads=new TreeMap<>(),writes=new TreeMap<>();constraintSites.clear();
        for(var entry:base.models.entrySet())for(MethodModel method:entry.getValue().methods())if(method.code().isPresent()){
            int down=0,up=0;var code=Prepare.instructions(method);String name=method.methodName().stringValue(),owner=entry.getKey();
            for(int n=0;n<code.size();n++)for(int f=0;f<2;f++){
                String field=f==0?"gearPitchDn":"gearPitchUp",key=owner+"."+name+"."+field;
                if(get(code.get(n),"Aerodynamics",field)){
                    int occurrence=f==0?down++:up++;constraintSites.put(key+"."+occurrence,constraintSites.size());reads.merge(key,1,Integer::sum);
                }
                if(put(code.get(n),"Aerodynamics",field)){
                    writes.merge(key,1,Integer::sum);
                    if(!name.equals("<clinit>")){
                        boolean linear=n>0&&Prepare.call(code.get(n-1),"Calc","getLinear","(FFF)F");
                        boolean profile=n>0&&code.get(n-1) instanceof FieldInstruction fi&&fi.opcode()==Opcode.GETFIELD&&fieldName(fi,"GearAttitude$Attitude",f==0?"pLimDn":"pLimUp");
                        if(!owner.equals("Aerodynamics")||!name.equals("pitchProgress")||(!linear&&!profile))throw new IllegalArgumentException("terrain cached constraint writer arithmetic "+key);
                    }
                }
            }
        }
        Map<String,Integer> expectedReads=Map.of("Aerodynamics.bankProgress.gearPitchDn",1,"Aerodynamics.pitchProgress.gearPitchDn",6,"Aerodynamics.pitchProgress.gearPitchUp",3,"Aerodynamics.headingProgress.gearPitchDn",2,"Gear.firstTouchDown.gearPitchDn",1,"Gear.checkTouchDown.gearPitchDn",2);
        Map<String,Integer> expectedWrites=Map.of("Aerodynamics.pitchProgress.gearPitchDn",3,"Aerodynamics.pitchProgress.gearPitchUp",3,"Aerodynamics.<clinit>.gearPitchUp",1);
        if(!reads.equals(expectedReads)||!writes.equals(expectedWrites)||constraintSites.size()!=15)throw new IllegalArgumentException("terrain cached constraint complete inventory "+reads+" / "+writes);
        var core=base.requireStatic("Core","runPresto");var ci=Prepare.instructions(core);int gear=-1,aero=-1,boost=-1;
        for(int i=0;i<ci.size();i++){if(Prepare.call(ci.get(i),"Gear","runDelta","()V")){if(gear>=0)throw new IllegalArgumentException("terrain gear frame duplicate");gear=i;}if(Prepare.call(ci.get(i),"Aerodynamics","runDelta","()V"))aero=i;if(Prepare.call(ci.get(i),"NetBooster","runDelta","()V"))boost=i;}
        if(gear<0||aero<=gear||boost<=aero||!dominates(core,gear,aero)||!dominates(core,aero,boost))throw new IllegalArgumentException("terrain complete native frame order");
        constraintsQualified=true;evidence.add("cached constraints:15 reads/seven writes incl native initializer; ground Calc.getLinear vs airborne profile basis qualified; Gear precedes Aerodynamics precedes Boost");
        constraintSites.forEach((k,v)->evidence.add("constraint site "+v+" "+k));
    }
    static void constraintRead(CodeBuilder cb,int field,int site){cb.loadConstant(field).loadConstant(site).invokestatic(T,"constraint",MethodTypeDesc.of(CD_float,CD_float,CD_int,CD_int));}
    static void constraintWrite(CodeBuilder cb,int field,boolean includesGrade){cb.loadConstant(field).loadConstant(includesGrade?1:0).invokestatic(T,"constraintWrite",MethodTypeDesc.of(CD_float,CD_float,CD_int,CD_int));}
    /** Independent of identities so fixtures can exercise each rejected structural mutation. */
    void qualifySupportShape(){
        int producers=0;
        for(MethodModel m:base.models.get("Aerodynamics").methods())if(m.code().isPresent()){
            var code=Prepare.instructions(m);
            for(int n=0;n<code.size();n++)if(put(code.get(n),"Aerodynamics","realElevWithStrut")){
                String method=m.methodName().stringValue();producers++;
                if(method.equals("pitchProgress")){
                    if(n<3||!get(code.get(n-3),"Aerodynamics","elev")||!get(code.get(n-2),"Aerodynamics","strut")||code.get(n-1).opcode()!=Opcode.FADD)
                        throw new IllegalArgumentException("terrain ordinary support producer");
                }else if(method.equals("readStartPiBaHeAlVsTas")){
                    if(n<1||!get(code.get(n-1),"Elevation","realElev"))throw new IllegalArgumentException("terrain initialization support producer");
                    int altitude=-1;for(int j=0;j<n;j++)if(put(code.get(j),"Aerodynamics","altitudeReal"))altitude=j;
                    if(altitude<0||!dominates(m,altitude,n))throw new IllegalArgumentException("terrain initialization reset order");
                }else throw new IllegalArgumentException("terrain unknown support producer "+method);
            }
        }
        if(producers!=2)throw new IllegalArgumentException("terrain support producer count");
        MethodModel method=base.requireStatic("Aerodynamics","runDelta");var delta=Prepare.instructions(method);
        Map<Label,Integer> labels=labels(method);int paused=-1,ordinary=-1;
        for(int n=0;n+3<delta.size();n++)if(get(delta.get(n),"Core","globSimPaused")){
            if(paused>=0||!(delta.get(n+1) instanceof BranchInstruction first)||first.opcode()!=Opcode.IFNE
                ||!get(delta.get(n+2),"Core","loaderPaused")||!(delta.get(n+3) instanceof BranchInstruction second)||second.opcode()!=Opcode.IFEQ)
                throw new IllegalArgumentException("terrain native pause branch");
            paused=labels.get(first.target());ordinary=labels.get(second.target());
        }
        if(paused<0||ordinary<=paused)throw new IllegalArgumentException("terrain native phase boundaries");
        int count=0,pitch=-1,smooth=0,pitchCalls=0;
        for(int n=0;n<delta.size();n++){
            if(Prepare.call(delta.get(n),"Aerodynamics","pitchProgress","()V")){pitch=n;pitchCalls++;}
            if(get(delta.get(n),"Aerodynamics","realElevWithStrut")){
                if((count<2&&(n<paused||n>=ordinary))||(count>=2&&n<ordinary))throw new IllegalArgumentException("terrain paused/ordinary consumer scope");
                if(count>=2&&(pitch<0||!dominates(method,pitch,n)))throw new IllegalArgumentException("terrain support phase order");
                count++;
            }
            if(preSmooth(delta,n))smooth++;
        }
        if(count!=8||smooth!=2||pitchCalls!=2)throw new IllegalArgumentException("terrain support consumers/smoothers "+count+"/"+smooth);
        evidence.add("two exact native support producers; eight consumers (two paused/six ordinary); two pre-smoothing phase boundaries");
    }
    private static boolean preSmooth(List<Instruction> code,int n){
        return n+4<code.size()&&get(code.get(n),"Aerodynamics","altitudeDamper")&&get(code.get(n+1),"Aerodynamics","altitudeReal")
            &&Prepare.call(code.get(n+4),"Calc","getSmoothExpo","(FFFF)F");
    }
    private static Map<Label,Integer> labels(MethodModel method){
        Map<Label,Integer> result=new HashMap<>();int ordinal=0;
        for(CodeElement e:method.code().orElseThrow()){if(e instanceof LabelTarget l)result.put(l.label(),ordinal);if(e instanceof Instruction)ordinal++;}return result;
    }
    private static boolean dominates(MethodModel method,int barrier,int target){
        CodeModel code=method.code().orElseThrow();Map<Label,Integer> labels=new HashMap<>();List<Instruction> ins=new ArrayList<>();
        for(CodeElement element:code){if(element instanceof LabelTarget label)labels.put(label.label(),ins.size());if(element instanceof Instruction instruction)ins.add(instruction);}
        boolean[] visited=new boolean[ins.size()+1];ArrayDeque<Integer> pending=new ArrayDeque<>();pending.add(0);
        while(!pending.isEmpty()){
            int index=pending.removeFirst();if(index==target)return false;if(index==barrier||index>=ins.size()||visited[index])continue;visited[index]=true;
            // Conservative exception edges establish that catch paths cannot bypass the producer.
            for(var handler:code.exceptionHandlers())if(index>=labels.get(handler.tryStart())&&index<labels.get(handler.tryEnd()))pending.add(labels.get(handler.handler()));
            Instruction instruction=ins.get(index);
            if(instruction instanceof BranchInstruction branch){
                pending.add(labels.get(branch.target()));if(instruction.opcode()==Opcode.JSR||instruction.opcode()==Opcode.JSR_W)throw new IllegalArgumentException("terrain subroutine flow");
                if(instruction.opcode()==Opcode.GOTO||instruction.opcode()==Opcode.GOTO_W)continue;
            }else if(instruction instanceof LookupSwitchInstruction branch){pending.add(labels.get(branch.defaultTarget()));for(var entry:branch.cases())pending.add(labels.get(entry.target()));continue;
            }else if(instruction instanceof TableSwitchInstruction branch){pending.add(labels.get(branch.defaultTarget()));for(var entry:branch.cases())pending.add(labels.get(entry.target()));continue;
            }else if(instruction instanceof ReturnInstruction||instruction.opcode()==Opcode.ATHROW)continue;
            pending.add(index+1);
        }
        return true;
    }
    private void position(CodeBuilder cb){cb.invokestatic(AERO,PREFIX+"position",VOID);}
    private void scalar(CodeBuilder cb,String method){cb.invokestatic(T,method,F_F);}
    static final ClassDesc TRACE=ClassDesc.of("psx.exporter.FrameTrace");
    /** The protected region contains injected work only, never a native dynamics call. */
    static void guardedVoid(ClassBuilder b,ClassDesc owner,String name,MethodTypeDesc type,int flags,Consumer<CodeBuilder> body,ClassDesc failure){
        b.withMethodBody(name,type,flags,cb->{
            cb.trying(code->body.accept(code),catches->catches.catching(CD_Throwable,handler->{
                handler.pop();
                handler.trying(cleanup->cleanup.invokestatic(failure,"failed",VOID),nested->nested.catching(CD_Throwable,ignored->ignored.pop()));
            }));
            cb.return_();
        });
    }
    static void addLatch(ClassBuilder b,ClassDesc owner){
        guardedVoid(b,owner,PREFIX+"latch",VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            Label done=cb.newLabel();cb.invokestatic(T,"frameReady",MethodTypeDesc.of(CD_boolean)).ifeq(done).invokestatic(AERO,PREFIX+"begin",VOID).labelBinding(done);
        },T);
    }
    static void addFrameCallbacks(ClassBuilder b,ClassDesc owner){
        int flags=ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
        guardedVoid(b,owner,PREFIX+"traceEnter",VOID,flags,cb->cb.invokestatic(TRACE,"enter",VOID),TRACE);
        guardedVoid(b,owner,PREFIX+"traceStage",MethodTypeDesc.of(CD_void,CD_int),flags,cb->cb.iload(0).invokestatic(TRACE,"stage",MethodTypeDesc.of(CD_void,CD_int)),TRACE);
        for(boolean failed:List.of(false,true))guardedVoid(b,owner,PREFIX+(failed?"traceFail":"traceExit"),VOID,flags,
            cb->cb.getstatic(owner,"delta",CD_long).l2d().getstatic(owner,"deltaAcc",CD_float).f2d().loadConstant(failed?1:0).invokestatic(TRACE,"exit",MethodTypeDesc.of(CD_void,CD_double,CD_double,CD_boolean)),TRACE);
    }
    Map<String,byte[]> transform(Map<String,byte[]> existing){
        Map<String,byte[]> output=new TreeMap<>(existing);
        for(String name:required){
            byte[] input=output.getOrDefault(name,base.inputs.get(name));ClassModel model=base.cf.parse(input);ClassDesc owner=ClassDesc.ofInternalName(name);
            byte[] changed=base.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement element){
                    if(!(element instanceof MethodModel m)||m.code().isEmpty()){b.with(element);return;}
                    String method=m.methodName().stringValue();if(method.startsWith(PREFIX)){b.with(element);return;}
                    List<Instruction> instructions=Prepare.instructions(m);
                    int bankCutoff=-1,clearanceStart=-1,clearanceEnd=-1,pitchContactStart=-1,pitchContactEnd=-1;
                    if(name.equals("Aerodynamics")&&method.equals("bankProgress")){
                        for(int i=0;i<instructions.size();i++){
                            if(put(instructions.get(i),"Aerodynamics","absBank"))bankCutoff=i-2;
                            if(get(instructions.get(i),"Aerodynamics","height")&&clearanceStart<0)clearanceStart=i;
                            if(put(instructions.get(i),"Aerodynamics","crabBankAdd"))clearanceEnd=i;
                        }
                        // crabBankAdd is cleared early, then written in the airborne clearance branch.
                        if(bankCutoff<0||clearanceStart<0||clearanceEnd<=clearanceStart)throw new IllegalArgumentException("terrain bank clearance boundaries");
                    }
                    if(name.equals("Aerodynamics")&&method.equals("pitchProgress")){
                        for(int i=0;i<instructions.size();i++){
                            if(get(instructions.get(i),"Aerodynamics","fpvOnGndCount"))pitchContactStart=i;
                            if(get(instructions.get(i),"Aerodynamics","stalling")&&pitchContactStart>=0&&pitchContactEnd<0)pitchContactEnd=i;
                        }
                        if(pitchContactStart<0||pitchContactEnd<=pitchContactStart)throw new IllegalArgumentException("terrain nose support boundaries");
                    }
                    final int cutoff=bankCutoff,clearStart=clearanceStart,clearEnd=clearanceEnd,noseStart=pitchContactStart,noseEnd=pitchContactEnd;
                    b.transformMethod(m,(mb,me)->{
                        if(!(me instanceof CodeModel code)){mb.with(me);return;}
                        mb.transformCode(code,new CodeTransform(){
                            int ordinal,supportSite,downReads,upReads;Label frameStart,frameEnd,frameHandler;
                            public void atStart(CodeBuilder cb){if(constraintsQualified&&name.equals("Core")&&method.equals("runPresto")){cb.invokestatic(owner,PREFIX+"traceEnter",VOID);frameStart=cb.newLabel();frameEnd=cb.newLabel();frameHandler=cb.newLabel();cb.labelBinding(frameStart);}if(name.equals("Aerodynamics")&&(method.equals("bankProgress")||method.equals("pitchProgress"))){position(cb);if(method.equals("bankProgress"))cb.invokestatic(T,"bankBegin",VOID);}}
                            public void accept(CodeBuilder cb,CodeElement e){
                                if(!(e instanceof Instruction i)){cb.with(e);return;}
                                int index=ordinal++;
                                if(name.equals("Aerodynamics")&&method.equals("runDelta")&&preSmooth(instructions,index))
                                    cb.getstatic(AERO,"altitudeDamper",CD_float).invokestatic(T,"preSmooth",MethodTypeDesc.of(CD_void,CD_float));
                                if(name.equals("Core")&&method.equals(Prepare.PREFIX+"frame")&&Prepare.call(i,"Aerodynamics","runDelta","()V")){
                                    if(!constraintsQualified)cb.invokestatic(AERO,PREFIX+"begin",VOID);
                                    if(constraintsQualified)cb.loadConstant(1).invokestatic(ClassDesc.of("Core"),PREFIX+"traceStage",MethodTypeDesc.of(CD_void,CD_int));
                                    cb.with(e);if(constraintsQualified)cb.loadConstant(2).invokestatic(ClassDesc.of("Core"),PREFIX+"traceStage",MethodTypeDesc.of(CD_void,CD_int));cb.invokestatic(AERO,PREFIX+"end",VOID);return;
                                }
                                if(constraintsQualified&&name.equals("Core")&&method.equals("runPresto")){
                                    if(Prepare.call(i,"Gear","runDelta","()V"))cb.invokestatic(owner,PREFIX+"latch",VOID);
                                    if(Prepare.call(i,"NetBooster","runDelta","()V")){cb.loadConstant(5).invokestatic(owner,PREFIX+"traceStage",MethodTypeDesc.of(CD_void,CD_int));cb.with(e);cb.loadConstant(6).invokestatic(owner,PREFIX+"traceStage",MethodTypeDesc.of(CD_void,CD_int));return;}
                                    if(i instanceof ReturnInstruction)cb.invokestatic(owner,PREFIX+"traceExit",VOID);
                                }
                                if(constraintsQualified&&(get(i,"Aerodynamics","gearPitchDn")||get(i,"Aerodynamics","gearPitchUp"))){
                                    int field=get(i,"Aerodynamics","gearPitchDn")?0:1,occurrence=field==0?downReads++:upReads++;
                                    Integer site=constraintSites.get(name+"."+method+"."+(field==0?"gearPitchDn":"gearPitchUp")+"."+occurrence);
                                    if(site==null)throw new IllegalArgumentException("terrain unmapped constraint consumer");
                                    position(cb);cb.with(e);constraintRead(cb,field,site);
                                    if(field==0&&((name.equals("Aerodynamics")&&method.equals("pitchProgress")&&index>=noseStart&&index<noseEnd)||(name.equals("Gear")&&method.equals("checkTouchDown"))))scalar(cb,"relativePitch");return;
                                }
                                if(constraintsQualified&&name.equals("Aerodynamics")&&method.equals("pitchProgress")&&(put(i,"Aerodynamics","gearPitchDn")||put(i,"Aerodynamics","gearPitchUp"))){
                                    position(cb);constraintWrite(cb,put(i,"Aerodynamics","gearPitchDn")?0:1,index>0&&Prepare.call(instructions.get(index-1),"Calc","getLinear","(FFF)F"));cb.with(e);return;
                                }
                                if(name.equals("Aerodynamics")&&method.equals("bankProgress")){
                                    if(i instanceof ReturnInstruction)cb.invokestatic(T,"bankEnd",VOID);
                                    boolean clearance=index>=clearStart&&index<=clearEnd;
                                    if(index<cutoff&&(get(i,"Aerodynamics","bank")||get(i,"Aerodynamics","targBank"))){cb.with(e);scalar(cb,clearance?"clearanceBank":"relativeBank");return;}
                                    if(index<cutoff&&get(i,"Aerodynamics","absBank")){cb.with(e).getstatic(AERO,"bank",CD_float).invokestatic(T,"relativeAbsBank",MethodTypeDesc.of(CD_float,CD_float,CD_float));return;}
                                    if(index<cutoff&&(put(i,"Aerodynamics","bank")||put(i,"Aerodynamics","targBank"))){scalar(cb,clearance?"clearanceAbsoluteBank":"absoluteBank");cb.with(e);return;}
                                }
                                if(name.equals("Aerodynamics")&&method.equals("pitchProgress")&&get(i,"Aerodynamics","fpv")){cb.with(e);scalar(cb,"grade");return;}
                                if(name.equals("Aerodynamics")&&method.equals("tasProgress")&&get(i,"Aerodynamics","fpv")){cb.with(e);scalar(cb,"grade");return;}
                                if(name.equals("Aerodynamics")&&method.equals("pitchProgress")&&index>=noseStart&&index<noseEnd){
                                    if(get(i,"Aerodynamics","pitch")){cb.with(e);scalar(cb,"relativePitch");return;}
                                    if(get(i,"Aerodynamics","gearPitchDn")){cb.with(e);scalar(cb,"relativePitch");return;}
                                    if(put(i,"Aerodynamics","pitch")||put(i,"Aerodynamics","targPitch")){
                                        scalar(cb,"absolutePitch");
                                        cb.with(e);return;
                                    }
                                }
                                if(name.equals("Gear")&&method.equals("checkTouchDown")&&get(i,"Aerodynamics","pitch")){cb.with(e);scalar(cb,"relativePitch");return;}
                                if(name.equals("Gear")&&method.equals("checkTouchDown")&&get(i,"Aerodynamics","gearPitchDn")){cb.with(e);scalar(cb,"relativePitch");return;}
                                if(name.equals("Aerodynamics")&&method.equals("runDelta")&&get(i,"Elevation","realElev")){cb.invokestatic(ELEV,PREFIX+"height",FLOAT);return;}
                                if(name.equals("Aerodynamics")&&(method.equals("runDelta")||method.equals("pitchProgress"))&&get(i,"Aerodynamics","elev")){cb.invokestatic(AERO,PREFIX+"height",FLOAT);return;}
                                if(name.equals("Aerodynamics")&&method.equals("runDelta")&&get(i,"Aerodynamics","realElevWithStrut")){cb.loadConstant(supportSite++).invokestatic(AERO,PREFIX+"support",MethodTypeDesc.of(CD_float,CD_int));return;}
                                cb.with(e);
                            }
                            public void atEnd(CodeBuilder cb){if(frameStart!=null){cb.labelBinding(frameEnd).labelBinding(frameHandler).invokestatic(owner,PREFIX+"traceFail",VOID).athrow();cb.exceptionCatchAll(frameStart,frameEnd,frameHandler);}}
                        });
                    });
                }
                public void atEnd(ClassBuilder b){
                    if(constraintsQualified&&name.equals("Core")){
                        addLatch(b,owner);addFrameCallbacks(b,owner);
                    }
                    if(name.equals("Elevation"))b.withMethodBody(PREFIX+"height",FLOAT,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                        position(cb);cb.getstatic(ELEV,"realElev",CD_float).getstatic(AERO,"realLat",CD_double).getstatic(AERO,"realLon",CD_double).invokestatic(T,"height",MethodTypeDesc.of(CD_float,CD_float,CD_double,CD_double)).freturn();
                    });
                    if(name.equals("Aerodynamics")){
                        guardedVoid(b,owner,PREFIX+"position",VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->cb.getstatic(AERO,"realLat",CD_double).getstatic(AERO,"realLon",CD_double).getstatic(AERO,"trueHeading",CD_double).invokestatic(T,"position",MethodTypeDesc.of(CD_void,CD_double,CD_double,CD_double)),T);
                        b.withMethodBody(PREFIX+"height",FLOAT,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{position(cb);cb.getstatic(AERO,"elev",CD_float).getstatic(AERO,"realLat",CD_double).getstatic(AERO,"realLon",CD_double).invokestatic(T,"height",MethodTypeDesc.of(CD_float,CD_float,CD_double,CD_double)).freturn();});
                        b.withMethodBody(PREFIX+"support",MethodTypeDesc.of(CD_float,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{position(cb);cb.getstatic(AERO,"realElevWithStrut",CD_float).getstatic(AERO,"strut",CD_float).getstatic(AERO,"realLat",CD_double).getstatic(AERO,"realLon",CD_double).iload(0).invokestatic(T,"supportSite",MethodTypeDesc.of(CD_float,CD_float,CD_float,CD_double,CD_double,CD_int)).freturn();});
                        guardedVoid(b,owner,PREFIX+"begin",VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->cb.invokestatic(OBS,"terrainEpoch",MethodTypeDesc.of(CD_long)).getstatic(ClassDesc.of("Core"),"deltaAcc",CD_float).f2d().getstatic(AERO,"realLat",CD_double).getstatic(AERO,"realLon",CD_double).getstatic(AERO,"trueHeading",CD_double).getstatic(AERO,"elev",CD_float).getstatic(AERO,"fpv",CD_float).getstatic(AERO,"ground",CD_boolean).getstatic(ELEV,"realElev",CD_float).getstatic(AERO,"realElevWithStrut",CD_float).getstatic(AERO,"situInitLoop",CD_int).invokestatic(T,"begin",MethodTypeDesc.of(CD_void,CD_long,CD_double,CD_double,CD_double,CD_double,CD_float,CD_float,CD_boolean,CD_float,CD_float,CD_int)),T);
                        guardedVoid(b,owner,PREFIX+"end",VOID,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->cb.invokestatic(OBS,"terrainFrame",MethodTypeDesc.of(CD_long)).getstatic(AERO,"mainGearStrut",CD_float).getstatic(AERO,"noseGearOscSine",CD_float).getstatic(AERO,"noseGearCompress",CD_float).getstatic(ClassDesc.of("GearAttitude"),"now",ClassDesc.of("GearAttitude$Attitude")).getfield(ClassDesc.of("GearAttitude$Attitude"),"id",CD_int).getstatic(AERO,"ground",CD_boolean).getstatic(ClassDesc.of("Gear"),"noseGearTouchedDown",CD_boolean).invokestatic(T,"end",MethodTypeDesc.of(CD_void,CD_long,CD_float,CD_float,CD_float,CD_int,CD_boolean,CD_boolean)),T);
                    }
                }
            });
            var errors=base.cf.verify(changed);if(!errors.isEmpty())throw new IllegalArgumentException("terrain bytecode verification "+name+": "+errors.getFirst());
            output.put(name,changed);
        }
        postconditions(output);
        TerrainAccess.verifyAdded(existing,output,base.inputs);
        evidence.add("verified every terrain-added member reference against final vendor/helper bytecode; accessibility, descriptor and staticness; no class initialization");
        evidence.add("transformed classes "+String.join(",",required));return output;
    }
    void postconditions(Map<String,byte[]> output){
        ClassModel aero=base.cf.parse(output.get("Aerodynamics"));
        var delta=Prepare.instructions(aero.methods().stream().filter(m->m.methodName().equalsString("runDelta")).findFirst().orElseThrow());
        int count=0,smooth=0;
        for(int n=0;n<delta.size();n++){
            if(get(delta.get(n),"Aerodynamics","realElevWithStrut"))throw new IllegalArgumentException("terrain untransformed support read");
            if(Prepare.call(delta.get(n),"Aerodynamics",PREFIX+"support","(I)F")){
                if(n==0||!(delta.get(n-1) instanceof ConstantInstruction c)||!(c.constantValue() instanceof Integer site)||site!=count++)throw new IllegalArgumentException("terrain support site postcondition");
            }
            if(Prepare.call(delta.get(n),"psx/exporter/Terrain","preSmooth","(F)V"))smooth++;
        }
        if(count!=8||smooth!=2)throw new IllegalArgumentException("terrain complete support postcondition "+count+"/"+smooth);
        var support=Prepare.instructions(aero.methods().stream().filter(m->m.methodName().equalsString(PREFIX+"support")).findFirst().orElseThrow());
        if(support.stream().anyMatch(i->get(i,"Aerodynamics","elev"))||support.stream().filter(i->get(i,"Aerodynamics","strut")).count()!=1)
            throw new IllegalArgumentException("terrain explicit ordinary strut postcondition");
        var begin=Prepare.instructions(aero.methods().stream().filter(m->m.methodName().equalsString(PREFIX+"begin")).findFirst().orElseThrow());
        if(begin.stream().filter(i->get(i,"Aerodynamics","elev")).count()!=1||begin.stream().filter(i->get(i,"Elevation","realElev")).count()!=1)
            throw new IllegalArgumentException("terrain activation consumed/shadow height postcondition");
        if(constraintsQualified){
            int reads=0,writes=0,seen=0;for(var entry:output.entrySet())for(MethodModel m:base.cf.parse(entry.getValue()).methods())if(m.code().isPresent()){
                var code=Prepare.instructions(m);int dn=0,up=0;
                for(int n=0;n<code.size();n++){
                    Instruction i=code.get(n);
                    if(get(i,"Aerodynamics","gearPitchDn")||get(i,"Aerodynamics","gearPitchUp")){
                        int f=get(i,"Aerodynamics","gearPitchDn")?0:1,ordinal=f==0?dn++:up++;
                        Integer site=constraintSites.get(entry.getKey()+"."+m.methodName().stringValue()+"."+(f==0?"gearPitchDn":"gearPitchUp")+"."+ordinal);
                        if(site==null||n+3>=code.size()||!(code.get(n+1) instanceof ConstantInstruction cf)||!cf.constantValue().equals(f)||!(code.get(n+2) instanceof ConstantInstruction cs)||!cs.constantValue().equals(site)||!Prepare.call(code.get(n+3),"psx/exporter/Terrain","constraint","(FII)F")||(seen&(1<<site))!=0)
                            throw new IllegalArgumentException("terrain exact constraint site postcondition");
                        seen|=1<<site;reads++;
                    }
                    if(put(i,"Aerodynamics","gearPitchDn")||put(i,"Aerodynamics","gearPitchUp")){
                        if(m.methodName().equalsString("<clinit>"))continue;
                        int f=put(i,"Aerodynamics","gearPitchDn")?0:1;
                        if(n<3||!(code.get(n-3) instanceof ConstantInstruction cf)||!cf.constantValue().equals(f)||!(code.get(n-2) instanceof ConstantInstruction cg)||!(cg.constantValue().equals(0)||cg.constantValue().equals(1))||!Prepare.call(code.get(n-1),"psx/exporter/Terrain","constraintWrite","(FII)F"))
                            throw new IllegalArgumentException("terrain constraint writer postcondition");
                        writes++;
                    }
                }
            }
            if(reads!=15||writes!=6||seen!=32767)throw new IllegalArgumentException("terrain complete constraint transform "+reads+"/"+writes);
            var core=base.cf.parse(output.get("Core"));var frame=core.methods().stream().filter(m->m.methodName().equalsString("runPresto")).findFirst().orElseThrow();var code=Prepare.instructions(frame);int latch=-1,gear=-1;
            for(int n=0;n<code.size();n++){if(Prepare.call(code.get(n),"Core",PREFIX+"latch","()V"))latch=n;if(Prepare.call(code.get(n),"Gear","runDelta","()V"))gear=n;}
            if(latch<0||gear!=latch+1||!dominates(frame,latch,gear)||code.stream().filter(i->Prepare.call(i,"Core",PREFIX+"traceEnter","()V")).count()!=1)
                throw new IllegalArgumentException("terrain pre-Gear coherent latch postcondition");
            evidence.add("verified all15 tagged cached constraint consumers/six runtime writers and guarded pre-Gear plane latch");
        }
        MethodModel init=aero.methods().stream().filter(m->m.methodName().equalsString("readStartPiBaHeAlVsTas")).findFirst().orElseThrow();
        var initCode=Prepare.instructions(init);int reset=-1;
        for(int n=0;n<initCode.size();n++){
            if(Prepare.call(initCode.get(n),"Aerodynamics",Prepare.PREFIX+"lifecycle","()V"))reset=n;
            if(put(initCode.get(n),"Aerodynamics","realElevWithStrut")&&(reset<0||!dominates(init,reset,n)))
                throw new IllegalArgumentException("terrain native initialization reset postcondition");
        }
        for(var e:output.entrySet())if(!base.cf.verify(e.getValue()).isEmpty())throw new IllegalArgumentException("terrain whole optional transform verification "+e.getKey());
    }
}
