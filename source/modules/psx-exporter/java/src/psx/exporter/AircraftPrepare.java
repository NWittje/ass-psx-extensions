package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional external aircraft inspection and verified load-time transformations. Never initializes PSX. */
final class AircraftPrepare {
    static final ClassDesc OBS=ClassDesc.of("psx.exporter.AircraftObserver");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    static final String PREFIX="$psxAircraft$";
    record Step(String owner,String name,String descriptor,boolean isStatic) {}
    record Value(int slot,List<Step> path,float scale,boolean positive) {}
    record Root(int group,String owner,String method,String descriptor,int engine,List<Value> values) {}
    final Prepare base;
    final ClassDesc observer;
    final String prefix;
    final int allMask;
    final List<Root> roots=new ArrayList<>();
    final Map<Prepare.MethodId,Integer> guards=new HashMap<>();
    final Map<String,Integer> fields=new HashMap<>();
    final Map<String,Integer> required=new TreeMap<>();
    final List<String> evidence=new ArrayList<>();
    int capabilities;
    AircraftPrepare(Prepare base){this(base,OBS,PREFIX,63);}
    AircraftPrepare(Prepare base,ClassDesc observer,String prefix,int allMask){
        this.base=base;this.observer=observer;this.prefix=prefix;this.allMask=allMask;
        if(observer==null||!prefix.matches("\\$[A-Za-z][A-Za-z0-9_$]*")||allMask<=0||(allMask&~63)!=0)
            throw new IllegalArgumentException("scalar observation configuration");
    }
    Step field(String owner,String name,boolean statik) {
        String actual=owner;
        for(;;){
            ClassModel c=base.models.get(actual);if(c==null)throw new IllegalArgumentException("aircraft class "+actual);
            for(FieldModel f:c.fields())if(f.fieldName().equalsString(name)){
                int flags=f.flags().flagsMask();
                if(((flags&ClassFile.ACC_STATIC)!=0)!=statik || (flags&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("aircraft access "+owner+"."+name);
                return new Step(actual,name,f.fieldType().stringValue(),statik);
            }
            actual=c.superclass().orElseThrow().asInternalName();
        }
    }
    Value value(int slot,String path,float scale,boolean positive) {
        String expected=path.startsWith("LightsExt.")?"C":path.endsWith("/extent")||path.endsWith(".stabSrfcDeg")?"I":path.endsWith("/doorOpen")?"Z":"F";
        return typedValue(slot,path,scale,positive,expected);
    }
    Value typedValue(int slot,String path,float scale,boolean positive,String expected) {
        String[] parts=path.split("/");int dot=parts[0].lastIndexOf('.');
        Step step=field(parts[0].substring(0,dot),parts[0].substring(dot+1),true);
        List<Step> result=new ArrayList<>();result.add(step);
        for(int i=1;i<parts.length;i++){String d=step.descriptor;if(!d.startsWith("L"))throw new IllegalArgumentException("aircraft chain "+path);step=field(d.substring(1,d.length()-1),parts[i],false);result.add(step);}
        if(!Set.of("F","D","I","Z","C").contains(step.descriptor))throw new IllegalArgumentException("aircraft scalar "+path);
        if(!step.descriptor.equals(expected))throw new IllegalArgumentException("aircraft source descriptor "+path+" expected "+expected);
        return new Value(slot,List.copyOf(result),scale,positive);
    }
    Value v(int slot,String path){return value(slot,path,1,false);}
    void root(int group,String owner,String method,int engine,List<Value> values){
        String descriptor=owner.equals("AirData")?"(FFFFFFFFFF)V":"()V";
        base.requireMethod(owner,method,descriptor);roots.add(new Root(group,owner,method,descriptor,engine,values));capabilities|=1<<group;
        for(Value value:values)for(Step f:value.path)fields.merge(f.owner+"."+f.name,1<<group,(a,b)->a|b);
        evidence.add("producer "+group+" "+owner+"."+method+"()V");
        for(Value value:values)evidence.add("field "+group+"/"+value.slot+" "+value.path+" scale="+value.scale+" positive="+value.positive);
    }
    void optional(int group,Runnable action){
        try{action.run();}catch(RuntimeException failure){capabilities&=~(1<<group);roots.removeIf(r->r.group==group);fields.replaceAll((k,v)->v&~(1<<group));evidence.add("unavailable "+group+" "+failure);}
    }
    void qualify() {
        optional(0,()->{
            String[] names={"aeroOutbdAilL","aeroInbdAilL","aeroInbdAilR","aeroOutbdAilR","aeroOutbdEleL","aeroInbdEleL","aeroInbdEleR","aeroOutbdEleR","aeroRudUpr","aeroRudLwr"};
            List<Value> values=new ArrayList<>();for(int i=0;i<10;i++)values.add(v(i,"FlightControls."+names[i]));
            values.add(value(10,"FlightControls.stabSrfcDeg",.001f,false));
            root(0,"Core","runPresto",-1,values);
            for(String n:List.of("yawDampUpr","yawDampLwr")){if(!field("FlightControls",n,true).descriptor.equals("F"))throw new IllegalArgumentException("rudder yaw descriptor");fields.put("FlightControls."+n,1);}
        });
        optional(1,()->{List<Value> values=new ArrayList<>();String[] names={"1","2","3","4","5","6_7","8","9","10","11","12"};for(int i=0;i<11;i++)values.add(v(i,"FlightControls.aeroSpl_"+names[i]));root(1,"Core","runPresto",-1,values);});
        optional(2,()->{List<Value> values=new ArrayList<>();String[] names={"leOuL","leMiL","leInL","leInR","leMiR","leOuR","teOutbdL","teInbdL","teInbdR","teOutbdR"};for(int i=0;i<10;i++)values.add(value(i,"Flaps."+names[i]+"/ext",i<6?1f/500:1f/2250,false));root(2,"Core","runPresto",-1,values);});
        optional(3,()->{List<Value> values=new ArrayList<>();String[] names={"nose","wingL","wingR","bodyL","bodyR"};for(int i=0;i<5;i++){values.add(value(i,"Gear."+names[i]+"/extent",1f/20000,false));values.add(v(i+5,"Gear."+names[i]+"/doorOpen"));}root(3,"Core","runPresto",-1,values);});
        optional(4,()->{List<Value> values=new ArrayList<>();for(int i=0;i<4;i++)values.add(value(i,"EngineAccessories.acc"+(i+1)+"/revFactor",1,true));root(4,"Core","runPresto",-1,values);});
        optional(5,()->{List<Value> values=new ArrayList<>();String[] names={"ldgObL","ldgObR","ldgIbL","ldgIbR","rwyTurnOffL","rwyTurnOffR","taxi","bcnU","bcnL","navL","navR","strobe","wing","logo"};for(int i=0;i<14;i++)values.add(v(i,"LightsExt."+names[i]));root(5,"Core","runPresto",-1,values);});
        qualifyWriters();
    }
    void qualifyWriters() {
        field("Option","engineModel",true);field("Option","variant",true);field("Core","paused",true);field("Core","loaderPaused",true);
        fields.put("Option.engineModel",allMask);fields.put("Option.variant",allMask);
        for(var entry:base.methods.entrySet()){
            var id=entry.getKey();if(entry.getValue().code().isEmpty())continue;int mask=0;
            for(var ins:Prepare.instructions(entry.getValue()))if(ins instanceof FieldInstruction f && (f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                Step resolved=resolveStore(f);if(resolved!=null)mask|=fields.getOrDefault(resolved.owner+"."+resolved.name,0);
            }
            if(mask!=0){
                evidence.add("writer "+mask+" "+id);
                // Constructors populate unpublished instances; reference publication stores are guarded.
                if(!id.name().startsWith("<"))guards.put(id,mask);
            }
        }
        for(Root r:roots)required.merge(r.owner,1<<r.group,(a,b)->a|b);
        guards.forEach((id,mask)->required.merge(id.owner(),mask,(a,b)->a|b));
        required.merge("Core",capabilities,(a,b)->a|b);
        Collections.sort(evidence);
    }
    Step resolveStore(FieldInstruction f){
        try{return field(f.owner().asInternalName(),f.name().stringValue(),f.opcode()==Opcode.PUTSTATIC);}
        catch(IllegalArgumentException ignored){return null;}
    }
    Map<String,byte[]> transform(Map<String,byte[]> pose){
        Map<String,byte[]> output=new TreeMap<>(pose);
        for(String name:required.keySet()){
            ClassDesc owner=ClassDesc.ofInternalName(name);ClassModel input=base.cf.parse(output.getOrDefault(name,base.inputs.get(name)));
            List<Root> own=roots.stream().filter(r->r.owner.equals(name)).toList();
            byte[] bytes=base.cf.transformClass(input,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());
                    List<Root> active=own.stream().filter(r->r.method.equals(id.name())&&id.descriptor().equals(r.descriptor)).toList();
                    List<Integer> activeGroups=active.stream().map(Root::group).distinct().toList();
                    int mask=guards.getOrDefault(id,0);
                    boolean state=name.equals("Core")&&id.name().equals("runPresto")&&id.descriptor().equals("()V");
                    if(active.isEmpty()&&mask==0&&!state){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{
                        if(!(me instanceof CodeModel code)){mb.with(me);return;}
                        mb.transformCode(code,new CodeTransform(){
                            Label begin,end,handler;
                            public void atStart(CodeBuilder cb){
                                if(state)cb.invokestatic(owner,prefix+"state",VOID);
                                for(int group:activeGroups)cb.invokestatic(owner,prefix+"begin"+group,VOID);
                                if(mask!=0)cb.invokestatic(owner,prefix+"enter"+mask,VOID);
                                begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);
                            }
                            public void accept(CodeBuilder cb,CodeElement ce){
                                if(ce instanceof ReturnInstruction){
                                    if(mask!=0)cb.invokestatic(owner,prefix+"leave"+mask,VOID);
                                    for(int group:activeGroups){cb.invokestatic(owner,prefix+"copy"+group,VOID);cb.invokestatic(owner,prefix+"end"+group,VOID);}
                                }cb.with(ce);
                            }
                            public void atEnd(CodeBuilder cb){
                                cb.labelBinding(end).labelBinding(handler);
                                if(mask!=0)cb.invokestatic(owner,prefix+"leave"+mask,VOID);
                                for(int group:activeGroups)cb.invokestatic(owner,prefix+"end"+group,VOID);
                                cb.athrow();cb.exceptionCatchAll(begin,end,handler);
                            }
                        });
                    });
                }
                public void atEnd(ClassBuilder b){
                    if(name.equals("Core"))safe(b,"state",allMask,cb->{Prepare.read(cb,"Core","loaderPaused","Z");cb.invokestatic(observer,"systemState",MethodTypeDesc.of(CD_void,CD_boolean));});
                    Set<Integer> masks=new TreeSet<>();guards.forEach((id,mask)->{if(id.owner().equals(name))masks.add(mask);});
                    for(int mask:masks)for(String action:List.of("enter","leave"))safe(b,action+mask,mask,cb->cb.loadConstant(mask).invokestatic(observer,action,MethodTypeDesc.of(CD_void,CD_int)));
                    var profiles=new TreeMap<Integer,List<Root>>();
                    for(Root profile:own)profiles.computeIfAbsent(profile.group,k->new ArrayList<>()).add(profile);
                    for(var entry:profiles.entrySet()){
                        Root r=entry.getValue().getFirst();
                        safe(b,"begin"+r.group,1<<r.group,cb->{Prepare.read(cb,"Core","loaderPaused","Z");cb.ifThen(skip->skip.return_());cb.loadConstant(r.group);Prepare.read(cb,"Option","engineModel","I");cb.iconst_1().iadd();Prepare.read(cb,"Option","variant","I");Prepare.read(cb,"Core","paused","Z");cb.invokestatic(observer,"begin",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_boolean));});
                        safe(b,"end"+r.group,1<<r.group,cb->cb.loadConstant(r.group).invokestatic(observer,"end",MethodTypeDesc.of(CD_void,CD_int)));
                        safe(b,"copy"+r.group,1<<r.group,cb->{
                            Prepare.read(cb,"Core","loaderPaused","Z");cb.ifThen(skip->skip.return_());
                            for(Root profile:entry.getValue()){
                                if(profile.engine>=0){Prepare.read(cb,"Option","engineModel","I");cb.loadConstant(profile.engine);cb.ifThen(Opcode.IF_ICMPEQ,copy->{emitCopy(copy,profile);copy.return_();});}
                                else {emitCopy(cb,profile);break;}
                            }
                        });
                    }
                }
            });
            var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("aircraft verify "+name+": "+errors.getFirst());output.put(name,bytes);
        }var before=new HashMap<>(base.inputs);before.putAll(pose);TerrainAccess.verifyAdded(before,output,base.inputs);return output;
    }
    void emitCopy(CodeBuilder cb,Root profile){
        cb.loadConstant(profile.group).invokestatic(observer,"copy",MethodTypeDesc.of(CD_boolean,CD_int));
        cb.ifThen(copy->{for(Value value:profile.values){copy.loadConstant(profile.group).loadConstant(value.slot);emit(copy,value);copy.invokestatic(observer,"value",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_float));}
            copy.loadConstant(profile.group);Prepare.read(copy,"Option","engineModel","I");copy.iconst_1().iadd();Prepare.read(copy,"Option","variant","I");Prepare.read(copy,"Core","paused","Z");copy.invokestatic(observer,"configuration",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_boolean));
            copy.loadConstant(profile.group).invokestatic(observer,"publish",MethodTypeDesc.of(CD_void,CD_int));});
    }
    void emit(CodeBuilder cb,Value v){
        for(Step f:v.path){ClassDesc owner=ClassDesc.ofInternalName(f.owner),type=ClassDesc.ofDescriptor(f.descriptor);if(f.isStatic)cb.getstatic(owner,f.name,type);else cb.getfield(owner,f.name,type);}
        String type=v.path.getLast().descriptor;if(type.equals("I")||type.equals("Z")||type.equals("C"))cb.i2f();else if(type.equals("D"))cb.d2f();
        Step last=v.path.getLast();if(last.owner.equals("LightsExt"))cb.loadConstant(-48f).fadd();
        if(last.name.equals("aeroRudUpr")||last.name.equals("aeroRudLwr")){Prepare.read(cb,"FlightControls",last.name.equals("aeroRudUpr")?"yawDampUpr":"yawDampLwr","F");cb.fadd();}
        if(v.scale!=1)cb.loadConstant(v.scale).fmul();if(v.positive)cb.invokestatic(observer,"positive",MethodTypeDesc.of(CD_float,CD_float));
    }
    void safe(ClassBuilder b,String name,int mask,java.util.function.Consumer<CodeBuilder> body){
        b.withMethodBody(prefix+name,VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            cb.trying(block->body.accept(block),catches->catches.catching(CD_Throwable,handler->{handler.pop();handler.trying(f->f.loadConstant(mask).invokestatic(observer,"disable",MethodTypeDesc.of(CD_void,CD_int)),c->c.catching(CD_Throwable,f->f.pop()));}));cb.return_();
        });
    }
}
