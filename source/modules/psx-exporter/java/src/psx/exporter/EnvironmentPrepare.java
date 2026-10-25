package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional preparation transaction; inspects installed bytes without defining PSX classes. */
final class EnvironmentPrepare {
    static final ClassDesc OBS=ClassDesc.of("psx.exporter.EnvironmentObserver");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    static final String PREFIX="$psxEnvironment$";
    record Read(int slot,String owner,String name,String descriptor,boolean configured,boolean instance){}
    final Prepare base;
    final List<Read> reads=new ArrayList<>();
    final Set<String> fields=new HashSet<>(),required=new TreeSet<>();
    final Set<Prepare.MethodId> guards=new HashSet<>();
    final List<String> evidence=new ArrayList<>();
    int capabilities;
    boolean context;
    EnvironmentPrepare(Prepare base){this.base=base;}
    void field(String owner,String name,String desc,boolean instance){
        ClassModel model=base.models.get(owner);if(model==null)throw new IllegalArgumentException("environment owner "+owner);
        FieldModel f=model.fields().stream().filter(v->v.fieldName().equalsString(name)&&v.fieldType().equalsString(desc)).findFirst().orElseThrow(()->new IllegalArgumentException("environment field "+owner+"."+name+desc));
        if(((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==instance||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("environment field visibility/staticness "+owner+"."+name);
        fields.add(owner+"."+name);evidence.add("field "+owner+"."+name+" "+desc);
    }
    void read(int slot,String owner,String name,String desc,boolean target){field(owner,name,desc,target);reads.add(new Read(slot,owner,name,desc,target,target));}
    void pattern(String owner,String name,String descriptor,String field,Opcode opcode,int minimum){
        MethodModel method=base.requireMethod(owner,name,descriptor);
        long count=Prepare.instructions(method).stream().filter(i->i instanceof FieldInstruction f&&f.opcode()==opcode&&(f.owner().asInternalName()+"."+f.name().stringValue()).equals(field)).count();
        if(count<minimum)throw new IllegalArgumentException("environment pattern "+owner+"."+name+" "+field+" "+opcode);
        evidence.add("qualified "+owner+"."+name+descriptor+" "+field+" "+opcode+" count="+count);
    }
    void qualify(){
        field("EarthTime","simMillis","J",false);field("Core","simSpeed","F",false);field("Core","paused","Z",false);field("Core","loaderPaused","Z",false);field("Core","globSimPaused","I",false);
        base.requireMethod("Core","runPresto","()V");base.requireMethod("Core","isPaused","()Z");
        pattern("EarthTime","runAllegro","()V","EarthTime.simMillis",Opcode.PUTSTATIC,1);
        pattern("EarthTime","runAllegro","()V","Core.simSpeed",Opcode.GETSTATIC,1);
        pattern("EarthTime","parseInjection","(I)V","EarthTime.simMillis",Opcode.PUTSTATIC,1);
        read(0,"Core","simSpeed","F",false);capabilities=1;
        Set<String> timeFields=Set.copyOf(fields);int timeReads=reads.size();
        try{
            field("Weather","currZone","LWeather$ZoneWx;",false);
            read(1,"Aerodynamics","realLat","D",false);read(2,"Aerodynamics","realLon","D",false);read(3,"Aerodynamics","altitudeReal","F",false);read(4,"Aerodynamics","height","F",false);
            String[] layers={"Hi","Lo","Cb"};String[] targetLayers={"hi","lo","cb"};int slot=5;
            for(int layer=0;layer<3;layer++)for(String suffix:List.of("Cov","Base","Top")){
                read(slot,"Weather","f"+layers[layer]+"Cloud"+suffix,"I",false);
                read(slot,"Weather$ZoneWx",targetLayers[layer]+"Cloud"+suffix,"I",true);slot++;
            }
            read(14,"Weather","fWindDir","F",false);read(15,"Weather","totalWindSpd","F",false);read(16,"Weather","fTemp","F",false);read(17,"Weather","fHpa","F",false);read(18,"Weather","fVisibMtrs","F",false);read(19,"Weather","fPrecipLevel","I",false);
            read(14,"Weather$ZoneWx","arptWindDir","I",true);read(15,"Weather$ZoneWx","arptWindSpd","I",true);read(16,"Weather$ZoneWx","surfaceTmp","I",true);read(17,"Weather$ZoneWx","fred","I",true);read(18,"Weather$ZoneWx","visibMtrs","I",true);read(19,"Weather$ZoneWx","precipLevel","I",true);
            read(22,"Weather$ZoneWx","elev","I",true);
            pattern("Weather","interpolate","()V","Aerodynamics.height",Opcode.GETSTATIC,1);
            for(String f:List.of("fHiCloudCov","fHiCloudBase","fHiCloudTop","fLoCloudCov","fLoCloudBase","fLoCloudTop","fCbCloudCov","fCbCloudBase","fCbCloudTop","fTemp","fHpa","fVisibMtrs","fPrecipLevel"))pattern("Weather","interpolate","()V","Weather."+f,Opcode.PUTSTATIC,1);
            pattern("Weather","runDelta","()V","Weather.totalWindSpd",Opcode.PUTSTATIC,1);
            pattern("Aerodynamics","runDelta","()V","Weather.fHpa",Opcode.GETSTATIC,1);
            pattern("Aerodynamics","runDelta","()V","Weather.totalWindSpd",Opcode.GETSTATIC,1);
            capabilities|=2;
        }catch(RuntimeException failure){fields.clear();fields.addAll(timeFields);reads.subList(timeReads,reads.size()).clear();evidence.add("weather unavailable "+failure);}
        if((capabilities&2)!=0){
            Set<String> lower=Set.copyOf(fields);int proof=evidence.size();
            try{
                field("Weather","wxBasic","LWeather$ZoneWx;",false);field("Weather","wx1","LWeather$ZoneWx;",false);
                field("Weather$ZoneWx","tabIdx","I",true);field("Weather$ZoneWx","arptIcao","Ljava/lang/String;",true);
                // Native object identities define target ownership; never infer it from edited UI tabs.
                long indexWriters=0;
                for(var entry:base.methods.entrySet())if(entry.getValue().code().isPresent())for(var ins:Prepare.instructions(entry.getValue()))if(ins instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals("Weather$ZoneWx")&&f.name().equalsString("tabIdx")){
                    if(!entry.getKey().name().equals("<init>"))throw new IllegalArgumentException("mutable weather tab identity");indexWriters++;
                }
                if(indexWriters!=1)throw new IllegalArgumentException("weather tab initialization");
                context=true;evidence.add("context qualified: currZone identity against wxBasic/wx1; others use immutable native tabIdx, all configured visibility/station writers guarded");
            }catch(RuntimeException failure){fields.clear();fields.addAll(lower);evidence.subList(proof,evidence.size()).clear();evidence.add("context unavailable "+failure);}
        }
        for(var entry:base.methods.entrySet())if(entry.getValue().code().isPresent()){
            for(var ins:Prepare.instructions(entry.getValue()))if(ins instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)&&fields.contains(f.owner().asInternalName()+"."+f.name().stringValue())){
                if(!entry.getKey().name().startsWith("<"))guards.add(entry.getKey());break;
            }
        }
        guards.forEach(id->{required.add(id.owner());evidence.add("writer "+id);});required.add("Core");if((capabilities&2)!=0)required.add("Weather");Collections.sort(evidence);
    }
    Map<String,byte[]> transform(Map<String,byte[]> input){
        Map<String,byte[]> output=new TreeMap<>(input);
        for(String name:required){ClassDesc owner=ClassDesc.ofInternalName(name);ClassModel model=base.cf.parse(input.getOrDefault(name,base.inputs.get(name)));
            byte[] bytes=base.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());boolean guard=guards.contains(id);
                    boolean root=(name.equals("Weather")&&id.name().equals("interpolate")&&id.descriptor().equals("()V"))||(name.equals("Core")&&id.name().equals("runPresto")&&id.descriptor().equals("()V"));
                    if(!guard&&!root){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel code)){mb.with(me);return;}mb.transformCode(code,new CodeTransform(){
                        Label begin,end,handler;
                        public void atStart(CodeBuilder cb){if(name.equals("Core")&&root)cb.invokestatic(owner,PREFIX+"state",VOID);if(guard)cb.invokestatic(owner,PREFIX+"enter",VOID);if(root)cb.invokestatic(owner,PREFIX+"begin",VOID);begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);}
                        public void accept(CodeBuilder cb,CodeElement ce){if(ce instanceof ReturnInstruction){if(root){cb.invokestatic(owner,PREFIX+"copy",VOID);cb.invokestatic(owner,PREFIX+"end",VOID);}if(guard)cb.invokestatic(owner,PREFIX+"leave",VOID);}cb.with(ce);}
                        public void atEnd(CodeBuilder cb){cb.labelBinding(end).labelBinding(handler);if(root)cb.invokestatic(owner,PREFIX+"end",VOID);if(guard)cb.invokestatic(owner,PREFIX+"leave",VOID);cb.athrow();cb.exceptionCatchAll(begin,end,handler);}
                    });});
                }
                public void atEnd(ClassBuilder b){
                    if(name.equals("Core"))safe(b,"state",cb->{Prepare.read(cb,"Core","loaderPaused","Z");cb.invokestatic(OBS,"systemState",MethodTypeDesc.of(CD_void,CD_boolean));});
                    if(guards.stream().anyMatch(id->id.owner().equals(name)))for(String method:List.of("enter","leave"))safe(b,method,cb->cb.invokestatic(OBS,method,VOID));
                    if(name.equals("Core")||name.equals("Weather")){
                      safe(b,"end",cb->cb.invokestatic(OBS,"end",VOID));
                      safe(b,"begin",cb->{
                        Prepare.read(cb,"Core","loaderPaused","Z");cb.ifThen(skip->skip.return_());
                        if(name.equals("Core")&&(capabilities&2)!=0){Prepare.read(cb,"Core","paused","Z");cb.ifThen(Opcode.IFEQ,skip->skip.return_());}
                        cb.invokestatic(OBS,"begin",VOID);
                      });
                      safe(b,"copy",cb->{
                        Prepare.read(cb,"Core","loaderPaused","Z");cb.ifThen(skip->skip.return_());
                        if(name.equals("Core")&&(capabilities&2)!=0){Prepare.read(cb,"Core","paused","Z");cb.ifThen(Opcode.IFEQ,skip->skip.return_());}
                        cb.invokestatic(OBS,"copy",MethodTypeDesc.of(CD_boolean));cb.ifThen(copy->{
                            for(Read r:reads){copy.loadConstant(r.slot);if(r.instance){Prepare.read(copy,"Weather","currZone","LWeather$ZoneWx;");copy.getfield(ClassDesc.ofInternalName(r.owner),r.name,ClassDesc.ofDescriptor(r.descriptor));}else Prepare.read(copy,r.owner,r.name,r.descriptor);
                                if(r.descriptor.equals("F"))copy.f2d();else if(r.descriptor.equals("I"))copy.i2d();copy.invokestatic(OBS,r.configured?"configured":"value",MethodTypeDesc.of(CD_void,CD_int,CD_double));}
                            Prepare.read(copy,"EarthTime","simMillis","J");Prepare.read(copy,"Core","paused","Z");Prepare.read(copy,"Core","globSimPaused","I");copy.iconst_2().iand();copy.invokestatic(OBS,"clock",MethodTypeDesc.of(CD_void,CD_long,CD_boolean,CD_int));
                            if(context)copy.invokestatic(owner,PREFIX+"context",VOID);
                            if(name.equals("Weather"))copy.trying(x->x.invokestatic(ClassDesc.of("psx.exporter.WeatherChangeObserver"),"consumed",VOID),c->c.catching(CD_Throwable,h->{h.pop();h.trying(x->x.invokestatic(ClassDesc.of("psx.exporter.WeatherChangeObserver"),"disable",VOID),d->d.catching(CD_Throwable,x->x.pop()));}));
                            copy.invokestatic(OBS,"publish",VOID);
                        });
                    });
                    if(context)b.withMethodBody(PREFIX+"context",VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                        cb.trying(body->{
                            Prepare.read(body,"Weather","currZone","LWeather$ZoneWx;");Prepare.read(body,"Weather","wxBasic","LWeather$ZoneWx;");Prepare.read(body,"Weather","wx1","LWeather$ZoneWx;");
                            body.invokestatic(ClassDesc.of("psx.exporter.EnvironmentContextObserver"),"begin",MethodTypeDesc.of(CD_void,CD_Object,CD_Object,CD_Object));
                            Prepare.read(body,"Weather","currZone","LWeather$ZoneWx;");body.getfield(ClassDesc.of("Weather$ZoneWx"),"tabIdx",CD_int);
                            Prepare.read(body,"Weather","wxBasic","LWeather$ZoneWx;");body.getfield(ClassDesc.of("Weather$ZoneWx"),"visibMtrs",CD_int);
                            Prepare.read(body,"Weather","wx1","LWeather$ZoneWx;");body.getfield(ClassDesc.of("Weather$ZoneWx"),"visibMtrs",CD_int);
                            Prepare.read(body,"Weather","wx1","LWeather$ZoneWx;");body.getfield(ClassDesc.of("Weather$ZoneWx"),"arptIcao",CD_String);
                            body.invokestatic(ClassDesc.of("psx.exporter.EnvironmentContextObserver"),"copy",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_int,CD_String));
                        },c->c.catching(CD_Throwable,h->{h.pop();h.trying(x->x.invokestatic(ClassDesc.of("psx.exporter.EnvironmentContextObserver"),"disable",VOID),nested->nested.catching(CD_Throwable,x->x.pop()));}));cb.return_();
                    });}
                }
            });var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("environment verify "+name+" "+errors.getFirst());output.put(name,bytes);
        }Map<String,byte[]> auditBefore=new TreeMap<>(input);for(String name:required)auditBefore.putIfAbsent(name,base.inputs.get(name));TerrainAccess.verifyAdded(auditBefore,output,base.inputs);return output;
    }
    static void safe(ClassBuilder b,String method,java.util.function.Consumer<CodeBuilder> body){b.withMethodBody(PREFIX+method,VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
        cb.trying(block->body.accept(block),catches->catches.catching(CD_Throwable,h->{h.pop();h.trying(x->{x.invokestatic(OBS,"disable",VOID);x.invokestatic(OBS,"end",VOID);},c->c.catching(CD_Throwable,x->x.pop()));}));cb.return_();
    });}
}
