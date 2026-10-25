package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional external audio inspection and verified load-time transformations. Never initializes PSX. */
final class AudioPrepare {
    static final ClassDesc OBS=ClassDesc.of("psx.exporter.AudioObserver");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    static final String PREFIX="$psxAudio$";
    record Step(String owner,String name,String descriptor,boolean isStatic) {}
    record Value(int slot,List<Step> path,float scale,boolean positive) {}
    record Root(int group,String owner,String method,String descriptor,int engine,List<Value> values) {}
    final Prepare base;
    final List<Root> roots=new ArrayList<>();
    final Map<Prepare.MethodId,Integer> guards=new HashMap<>();
    final Map<String,Integer> fields=new HashMap<>();
    final Map<String,Integer> required=new TreeMap<>();
    final List<String> evidence=new ArrayList<>();
    int capabilities;
    AudioPrepare(Prepare base){this.base=base;}
    Step field(String owner,String name,boolean statik) {
        String actual=owner;
        for(;;){
            ClassModel c=base.models.get(actual);if(c==null)throw new IllegalArgumentException("audio class "+actual);
            for(FieldModel f:c.fields())if(f.fieldName().equalsString(name)){
                int flags=f.flags().flagsMask();
                if(((flags&ClassFile.ACC_STATIC)!=0)!=statik || (flags&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("audio access "+owner+"."+name);
                return new Step(actual,name,f.fieldType().stringValue(),statik);
            }
            actual=c.superclass().orElseThrow().asInternalName();
        }
    }
    Value value(int slot,String path,float scale,boolean positive) {
        String[] parts=path.split("/");int dot=parts[0].lastIndexOf('.');
        Step step=field(parts[0].substring(0,dot),parts[0].substring(dot+1),true);
        List<Step> result=new ArrayList<>();result.add(step);
        for(int i=1;i<parts.length;i++){String d=step.descriptor;if(!d.startsWith("L"))throw new IllegalArgumentException("audio chain "+path);step=field(d.substring(1,d.length()-1),parts[i],false);result.add(step);}
        if(!Set.of("F","D","I","Z").contains(step.descriptor))throw new IllegalArgumentException("audio scalar "+path);
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
        optional(0,()->{for(int model=0;model<3;model++){
            String owner=new String[]{"EngModCf6","EngModPw4000","EngModRb211"}[model];
            String source="Core."+new String[]{"engCf6","engPw4000","engRb211"}[model];List<Value> values=new ArrayList<>();
            for(int e=0;e<4;e++){
                String p=source+"/eng"+(e+1)+"/";
                values.add(v(e*6,p+"n1"));values.add(v(e*6+1,p+"n2"));if(model==2)values.add(v(e*6+2,p+"n3"));
                values.add(v(e*6+3,p+"lightOn"));
                // Actual SAV travel is not a verified starter-running boolean; slot4 remains unavailable.
                values.add(v(e*6+5,p+"accessory/revFactor"));
            }root(0,owner,"runDelta",model,values);
        }
        });
        optional(1,()->{List<Value> gear=new ArrayList<>();String[] gears={"nose","wingL","wingR","bodyL","bodyR"};
        for(int i=0;i<5;i++)gear.add(value(i,"Gear."+gears[i]+"/extent",1f/20000f,false));root(1,"Gear","runModerato",-1,gear);});
        optional(2,()->{List<Value> flaps=new ArrayList<>();String[] fs={"leOuL","leMiL","leInL","leInR","leMiR","leOuR","teOutbdL","teInbdL","teInbdR","teOutbdR"};
        for(int i=0;i<10;i++){flaps.add(v(i,"Flaps."+fs[i]+"/ext"));flaps.add(v(i+10,"Flaps."+fs[i]+"/inTransit"));}root(2,"Flaps","runAllegro",-1,flaps);});
        optional(3,()->root(3,"Apu","runAllegro",-1,List.of(v(0,"Apu.n1"),v(1,"Apu.n2"),v(2,"Apu.runningWell"))));
        // Actual open state is sampled, with no claim of pack flow amount.
        optional(4,()->root(4,"BleedAir","runModerato",-1,List.of(v(0,"BleedAir.pack1/open"),v(1,"BleedAir.pack2/open"),v(2,"BleedAir.pack3/open"),v(3,"BleedAir.pack1/dct/psi"),v(4,"BleedAir.pack2/dct/psi"),v(5,"BleedAir.pack3/dct/psi"))));
        optional(5,()->root(5,"Hydraulics","runAllegro",-1,List.of(v(0,"Hydraulics.sys1/actPsi"),v(1,"Hydraulics.sys2/actPsi"),v(2,"Hydraulics.sys3/actPsi"),v(3,"Hydraulics.sys4/actPsi"))));
        optional(6,()->{String[] pumps={"pumpMnFw1","pumpMnAf1","pumpMnFw2","pumpMnAf2","pumpMnFw3","pumpMnAf3","pumpMnFw4","pumpMnAf4","pumpOvJtFw2","pumpOvJtAf2","pumpOvJtFw3","pumpOvJtAf3","pumpOvJtCtrL","pumpOvJtCtrR","pumpStabL","pumpStabR","pumpAuxL","pumpAuxR","pumpScavenge"};
        List<Value> fuel=new ArrayList<>();for(int i=0;i<pumps.length;i++)fuel.add(value(i,"Fuel."+pumps[i]+"/rpmAct",1,true));root(6,"Fuel","runModerato",-1,fuel);});
        optional(7,()->root(7,"AirData","setData",-1,List.of(value(0,"AirData.nTasWithGust",1852f/3600f,false),v(1,"AirData.nOat"))));
        field("Option","engineModel",true);field("Option","variant",true);field("Core","paused",true);
        fields.put("Option.engineModel",255);fields.put("Option.variant",255);
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
                    Root root=own.stream().filter(r->r.method.equals(id.name())&&id.descriptor().equals(r.descriptor)).findFirst().orElse(null);
                    int mask=guards.getOrDefault(id,0);
                    boolean state=name.equals("Core")&&id.name().equals("runPresto")&&id.descriptor().equals("()V");
                    if(root==null&&mask==0&&!state){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{
                        if(!(me instanceof CodeModel code)){mb.with(me);return;}
                        mb.transformCode(code,new CodeTransform(){
                            Label begin,end,handler;
                            public void atStart(CodeBuilder cb){
                                if(state)cb.invokestatic(owner,PREFIX+"state",VOID);
                                if(root!=null)cb.invokestatic(owner,PREFIX+"begin"+root.group,VOID);
                                if(mask!=0)cb.invokestatic(owner,PREFIX+"enter"+mask,VOID);
                                begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);
                            }
                            public void accept(CodeBuilder cb,CodeElement ce){
                                if(ce instanceof ReturnInstruction){
                                    if(mask!=0)cb.invokestatic(owner,PREFIX+"leave"+mask,VOID);
                                    if(root!=null){cb.invokestatic(owner,PREFIX+"copy"+root.group,VOID);cb.invokestatic(owner,PREFIX+"end"+root.group,VOID);}
                                }cb.with(ce);
                            }
                            public void atEnd(CodeBuilder cb){
                                cb.labelBinding(end).labelBinding(handler);
                                if(mask!=0)cb.invokestatic(owner,PREFIX+"leave"+mask,VOID);
                                if(root!=null)cb.invokestatic(owner,PREFIX+"end"+root.group,VOID);
                                cb.athrow();cb.exceptionCatchAll(begin,end,handler);
                            }
                        });
                    });
                }
                public void atEnd(ClassBuilder b){
                    if(name.equals("Core"))safe(b,"state",255,cb->{Prepare.read(cb,"Core","paused","Z");Prepare.read(cb,"Core","loaderPaused","Z");cb.ior().invokestatic(OBS,"systemState",MethodTypeDesc.of(CD_void,CD_boolean));});
                    Set<Integer> masks=new TreeSet<>();guards.forEach((id,mask)->{if(id.owner().equals(name))masks.add(mask);});
                    for(int mask:masks)for(String action:List.of("enter","leave"))safe(b,action+mask,mask,cb->cb.loadConstant(mask).invokestatic(OBS,action,MethodTypeDesc.of(CD_void,CD_int)));
                    for(Root r:own){
                        safe(b,"begin"+r.group,1<<r.group,cb->{cb.loadConstant(r.group);Prepare.read(cb,"Option","engineModel","I");cb.iconst_1().iadd();Prepare.read(cb,"Option","variant","I");Prepare.read(cb,"Core","paused","Z");cb.invokestatic(OBS,"begin",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_boolean));});
                        safe(b,"end"+r.group,1<<r.group,cb->cb.loadConstant(r.group).invokestatic(OBS,"end",MethodTypeDesc.of(CD_void,CD_int)));
                        safe(b,"copy"+r.group,1<<r.group,cb->{
                            if(r.engine>=0){Prepare.read(cb,"Option","engineModel","I");cb.loadConstant(r.engine);cb.ifThen(Opcode.IF_ICMPNE,skip->skip.return_());}
                            cb.loadConstant(r.group).invokestatic(OBS,"copy",MethodTypeDesc.of(CD_boolean,CD_int));
                            cb.ifThen(copy->{for(Value v:r.values){copy.loadConstant(r.group).loadConstant(v.slot);emit(copy,v);copy.invokestatic(OBS,"value",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_float));}
                                copy.loadConstant(r.group);Prepare.read(copy,"Option","engineModel","I");copy.iconst_1().iadd();Prepare.read(copy,"Option","variant","I");Prepare.read(copy,"Core","paused","Z");copy.invokestatic(OBS,"configuration",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_boolean));
                                copy.loadConstant(r.group).invokestatic(OBS,"publish",MethodTypeDesc.of(CD_void,CD_int));});
                        });
                    }
                }
            });
            var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("audio verify "+name+": "+errors.getFirst());output.put(name,bytes);
        }return output;
    }
    static void emit(CodeBuilder cb,Value v){
        for(Step f:v.path){ClassDesc owner=ClassDesc.ofInternalName(f.owner),type=ClassDesc.ofDescriptor(f.descriptor);if(f.isStatic)cb.getstatic(owner,f.name,type);else cb.getfield(owner,f.name,type);}
        String type=v.path.getLast().descriptor;if(type.equals("I")||type.equals("Z"))cb.i2f();else if(type.equals("D"))cb.d2f();
        if(v.scale!=1)cb.loadConstant(v.scale).fmul();if(v.positive)cb.invokestatic(OBS,"positive",MethodTypeDesc.of(CD_float,CD_float));
    }
    static void safe(ClassBuilder b,String name,int mask,java.util.function.Consumer<CodeBuilder> body){
        b.withMethodBody(PREFIX+name,VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            cb.trying(block->body.accept(block),catches->catches.catching(CD_Throwable,handler->{handler.pop();handler.loadConstant(mask).invokestatic(OBS,"disable",MethodTypeDesc.of(CD_void,CD_int));}));cb.return_();
        });
    }
}
