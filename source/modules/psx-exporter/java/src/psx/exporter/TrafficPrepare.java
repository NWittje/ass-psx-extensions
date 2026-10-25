package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Byte-data qualification and an all-or-nothing optional startup overlay. */
final class TrafficPrepare {
    static final String PREFIX="$psxTraffic$";
    static final ClassDesc OBS=ClassDesc.of("psx.exporter.TrafficObserver"),INTR=ClassDesc.of("Traffic$Intruder"),TRAFFIC=ClassDesc.of("Traffic");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void),ONE=MethodTypeDesc.of(CD_void,INTR);
    static final List<String> VALUES=List.of("latA","lonA","altA","trk","pitch","bank","gsKts","fpmAct","fpm","trkRate");
    final Prepare base;
    final Set<String> fields=new HashSet<>(),required=new TreeSet<>();
    final Set<Prepare.MethodId> guards=new HashSet<>();
    final List<String> evidence=new ArrayList<>();
    TrafficPrepare(Prepare base){this.base=base;}
    static boolean constant(Instruction i,double value){return i instanceof ConstantInstruction v&&v.constantValue() instanceof Number n&&n.doubleValue()==value;}
    static void scale(List<Instruction> code,String field,double value,boolean degrees){
        for(int i=0;i+3<code.size();i++)if(Prepare.field(code.get(i),Opcode.GETFIELD,"Traffic$Intruder",field)&&constant(code.get(i+1),value)&&code.get(i+2).opcode()==Opcode.DMUL){
            if(!degrees&&code.get(i+3).opcode()==Opcode.D2I)return;
            if(degrees&&i+5<code.size()&&constant(code.get(i+3),Math.PI/180)&&code.get(i+4).opcode()==Opcode.DDIV&&code.get(i+5).opcode()==Opcode.D2I)return;
        }
        throw new IllegalArgumentException("traffic unqualified serialized units "+field);
    }
    void field(String owner,String name,String descriptor,boolean instance){
        var c=base.models.get(owner);if(c==null)throw new IllegalArgumentException("traffic owner "+owner);
        var f=c.fields().stream().filter(v->v.fieldName().equalsString(name)&&v.fieldType().equalsString(descriptor)).findFirst().orElseThrow(()->new IllegalArgumentException("traffic field "+owner+"."+name));
        if(((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==instance||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("traffic field access/staticness "+owner+"."+name);
        fields.add(owner+"."+name);evidence.add("field "+owner+"."+name+descriptor);
    }
    void qualify() throws Exception {
        field("Traffic","in","[LTraffic$Intruder;",false);field("Traffic","simMode","I",false);
        for(String v:VALUES)field("Traffic$Intruder",v,v.equals("fpmAct")?"F":"D",true);
        field("Traffic$Intruder","id","I",true);
        for(String v:List.of("on","allOn","altInfoOn","airbus"))field("Traffic$Intruder",v,"Z",true);
        for(String v:List.of("paused","loaderPaused"))field("Core",v,"Z",false);
        field("Core","globSimPaused","I",false);field("Core","simSpeed","F",false);field("Core","delta","J",false);
        var root=Prepare.instructions(base.requireStatic("Core","runPresto"));int a=-1,t=-1,c=-1,n=-1;
        for(int i=0;i<root.size();i++){
            if(Prepare.call(root.get(i),"Aerodynamics","runDelta","()V")){if(a>=0)throw new IllegalArgumentException("traffic duplicate dynamics");a=i;}
            if(Prepare.call(root.get(i),"Traffic","runDelta","()V")){if(t>=0)throw new IllegalArgumentException("traffic duplicate sample phase");t=i;}
            if(Prepare.call(root.get(i),"Tcas","runDelta","()V")){if(c>=0)throw new IllegalArgumentException("traffic duplicate TCAS");c=i;}
            if(Prepare.call(root.get(i),"NetBooster","runDelta","()V"))n=i;
        }
        if(!(a>=0&&t>a&&c>t&&n>c))throw new IllegalArgumentException("traffic native phase order");
        evidence.add("phase Aerodynamics -> Traffic -> observer -> Tcas -> NetBooster; distinct original observation QPC");
        var init=Prepare.instructions(base.requireStatic("Traffic","init"));
        if(init.stream().noneMatch(i->i instanceof ConstantInstruction v&&Integer.valueOf(7).equals(v.constantValue()))||init.stream().filter(i->i.opcode()==Opcode.AASTORE).count()!=1||init.stream().filter(i->Prepare.call(i,"Traffic$Intruder","<init>","(I)V")).count()!=1)throw new IllegalArgumentException("traffic seven fresh slots");
        var delta=Prepare.instructions(base.requireStatic("Traffic","runDelta"));
        if(delta.stream().noneMatch(i->Prepare.field(i,Opcode.GETSTATIC,"Core","paused"))||delta.stream().noneMatch(i->Prepare.field(i,Opcode.GETSTATIC,"Traffic","simMode")))throw new IllegalArgumentException("traffic pause/mode branches");
        var intr=Prepare.instructions(base.requireMethod("Traffic$Intruder","runDelta","()V"));int marker=-1;
        for(int i=0;i<intr.size();i++)if(Prepare.field(intr.get(i),Opcode.PUTSTATIC,"Traffic","velUpd")){
            if(marker>=0||i==0||intr.get(i-1).opcode()!=Opcode.ICONST_1)throw new IllegalArgumentException("traffic recycle latch");marker=i;
        }
        if(marker<0)throw new IllegalArgumentException("traffic missing recycle boundary");
        for(String name:List.of("latA","lonA")){
            int before=0,after=0;for(int i=0;i<intr.size();i++)if(Prepare.field(intr.get(i),Opcode.PUTFIELD,"Traffic$Intruder",name)){if(i<marker)before++;else after++;}
            if(before<1||after!=1)throw new IllegalArgumentException("traffic recycle/integration boundary "+name);
        }
        for(String name:List.of("fpmAct","trkRate","pitch","bank"))if(intr.stream().noneMatch(i->Prepare.field(i,Opcode.GETFIELD,"Traffic$Intruder",name)))throw new IllegalArgumentException("traffic actual integration "+name);
        for(String name:List.of("sendGlobPos","sendGlobVel","readGlobPos","readGlobVel","readGlobBits","situLoadInit"))base.requireMethod("Traffic",name,name.startsWith("readGlob")&&!name.equals("readGlobBits")?"(I)V":"()V");
        // Unit/reference arithmetic is admitted as the inspected installed instruction sequence,
        // with semantic constants/calls checked; no historical byte offsets or version whitelist.
        var pos=Prepare.instructions(base.requireStatic("Traffic","sendGlobPos"));
        for(String name:List.of("latA","lonA","altA","trk"))if(pos.stream().noneMatch(i->Prepare.field(i,Opcode.GETFIELD,"Traffic$Intruder",name)))throw new IllegalArgumentException("traffic position serializer "+name);
        for(double value:new double[]{10,100})if(pos.stream().noneMatch(i->i instanceof ConstantInstruction v&&v.constantValue() instanceof Double d&&d==value))throw new IllegalArgumentException("traffic serializer units");
        scale(pos,"altA",10,false);scale(pos,"trk",100,true);
        boolean actualFpm=false;for(int i=0;i+6<intr.size();i++)if(Prepare.field(intr.get(i),Opcode.GETFIELD,"Traffic$Intruder","fpmAct")&&Prepare.field(intr.get(i+1),Opcode.GETSTATIC,"Core","delta")&&intr.get(i+2).opcode()==Opcode.L2F&&intr.get(i+3).opcode()==Opcode.FMUL&&intr.get(i+4).opcode()==Opcode.F2D&&constant(intr.get(i+5),60000)&&intr.get(i+6).opcode()==Opcode.DDIV)actualFpm=true;
        if(!actualFpm)throw new IllegalArgumentException("traffic actual vertical integration units");
        int bankLocal=-1;for(int i=0;i+4<intr.size();i++)if(Prepare.field(intr.get(i),Opcode.GETFIELD,"Traffic$Intruder","bank")&&Prepare.call(intr.get(i+1),"java/lang/Math","sin","(D)D")&&constant(intr.get(i+2),100)&&intr.get(i+3).opcode()==Opcode.DMUL&&intr.get(i+4) instanceof StoreInstruction s)bankLocal=s.slot();
        boolean left=false,right=false;for(int i=0;i+3<intr.size();i++)if(intr.get(i+1) instanceof LoadInstruction l&&l.slot()==bankLocal){
            if(Prepare.field(intr.get(i),Opcode.GETFIELD,"Traffic$Intruder","altL")&&intr.get(i+2).opcode()==Opcode.DADD&&Prepare.field(intr.get(i+3),Opcode.PUTFIELD,"Traffic$Intruder","altL"))left=true;
            if(Prepare.field(intr.get(i),Opcode.GETFIELD,"Traffic$Intruder","altR")&&intr.get(i+2).opcode()==Opcode.DSUB&&Prepare.field(intr.get(i+3),Opcode.PUTFIELD,"Traffic$Intruder","altR"))right=true;
        }
        if(bankLocal<0||!left||!right||intr.stream().noneMatch(i->constant(i,160)))throw new IllegalArgumentException("traffic bank sign/graphic anchor geometry");
        evidence.add("native lat/lon/track/pitch/bank radians; anchor revision1; altitude feet; GS knots; actual fpmAct feet/min; target fpm diagnostic; trackRate radians/native-ms; positive-right wrapped bank");
        for(var e:base.methods.entrySet())if(e.getValue().code().isPresent()){
            var ins=Prepare.instructions(e.getValue());boolean writer=false;
            if(ins.stream().anyMatch(i->Prepare.field(i,Opcode.GETSTATIC,"Traffic","in"))&&ins.stream().anyMatch(i->i.opcode()==Opcode.AASTORE)&&!e.getKey().equals(new Prepare.MethodId("Traffic","init","()V")))throw new IllegalArgumentException("traffic unqualified array store "+e.getKey());
            for(var i:ins){
                if(i instanceof FieldInstruction f&&fields.contains(f.owner().asInternalName()+"."+f.name().stringValue())&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){
                    evidence.add("writer "+e.getKey()+" "+f.owner().asInternalName()+"."+f.name().stringValue());writer=true;
                    if(f.owner().asInternalName().equals("Traffic$Intruder")&&!Set.of("Traffic","Traffic$Intruder","Tcas").contains(e.getKey().owner()))throw new IllegalArgumentException("traffic unqualified writer owner "+e.getKey());
                    if(f.name().equalsString("id")&&!e.getKey().equals(new Prepare.MethodId("Traffic$Intruder","<init>","(I)V")))throw new IllegalArgumentException("traffic mutable slot identity");
                    if(f.owner().asInternalName().equals("Traffic")&&f.name().equalsString("in")&&!e.getKey().equals(new Prepare.MethodId("Traffic","<clinit>","()V")))throw new IllegalArgumentException("traffic array replacement");
                }
                if(Prepare.call(i,"Traffic$Intruder","<init>","(I)V")&&!e.getKey().equals(new Prepare.MethodId("Traffic","init","()V")))throw new IllegalArgumentException("traffic escaping construction");
            }
            if(writer&&!e.getKey().name().startsWith("<")){guards.add(e.getKey());required.add(e.getKey().owner());}
        }
        // Fresh-object constructor/static initialization are before native readiness. The only
        // qualified publication of Intruder instances is Traffic.init's guarded array fill.
        guards.add(new Prepare.MethodId("Traffic","init","()V"));required.addAll(List.of("Traffic","Traffic$Intruder","Core"));
        for(String name:required)evidence.add("class "+name+" SHA256="+Identity.hash(base.inputs.get(name)));
        Collections.sort(evidence);
    }
    Map<String,byte[]> transform(Map<String,byte[]> lower){
        Map<String,byte[]> out=new TreeMap<>(lower),before=new TreeMap<>(lower);
        // Earlier layers may wrap/rename Core.runPresto. Qualify the actual
        // composed native call site rather than assuming its historical name.
        var core=base.cf.parse(lower.getOrDefault("Core",base.inputs.get("Core")));
        Prepare.MethodId phase=null;
        for(var method:core.methods())if(method.code().isPresent()){
            var code=Prepare.instructions(method);int traffic=-1,tcas=-1;
            for(int i=0;i<code.size();i++){
                if(Prepare.call(code.get(i),"Traffic","runDelta","()V")){if(traffic>=0)throw new IllegalArgumentException("traffic duplicate composed call");traffic=i;}
                if(Prepare.call(code.get(i),"Tcas","runDelta","()V"))tcas=i;
            }
            if(traffic>=0){
                if(phase!=null||tcas<=traffic||!method.methodType().equalsString("()V")||(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("traffic composed phase unqualified");
                phase=new Prepare.MethodId("Core",method.methodName().stringValue(),"()V");
            }
        }
        if(phase==null)throw new IllegalArgumentException("traffic composed phase missing");
        final Prepare.MethodId samplePhase=phase;
        evidence.add("composed phase "+phase.name()+": Traffic -> observer -> Tcas");
        for(String name:required){ClassDesc owner=ClassDesc.ofInternalName(name);byte[] input=lower.getOrDefault(name,base.inputs.get(name));before.putIfAbsent(name,input);
            byte[] bytes=base.cf.transformClass(base.cf.parse(input),new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());
                    boolean guard=guards.contains(id),root=id.equals(samplePhase);
                    boolean intr=id.equals(new Prepare.MethodId("Traffic$Intruder","runDelta","()V"));
                    if(!guard&&!root){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel code)){mb.with(me);return;}mb.transformCode(code,new CodeTransform(){
                        Label begin,end,handler;int object,value,flag;boolean recycled=false;
                        public void atStart(CodeBuilder cb){object=cb.allocateLocal(TypeKind.REFERENCE);value=cb.allocateLocal(TypeKind.DOUBLE);flag=cb.allocateLocal(TypeKind.INT);cb.invokestatic(owner,PREFIX+"enter",VOID);begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);if(id.equals(new Prepare.MethodId("Traffic","init","()V")))cb.invokestatic(owner,PREFIX+"replaced",VOID);}
                        public void accept(CodeBuilder cb,CodeElement ce){
                            if(ce instanceof FieldInstruction f){
                                if(f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals("Traffic$Intruder")&&f.name().equalsString("latA")&&!intr){cb.dstore(value).astore(object).aload(object).invokestatic(owner,PREFIX+"replace",ONE).aload(object).dload(value);}
                                if(f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals("Traffic$Intruder")&&f.name().equalsString("on")){cb.istore(flag).astore(object).aload(object).iload(flag).invokestatic(owner,PREFIX+"activation",MethodTypeDesc.of(CD_void,INTR,CD_boolean)).aload(object).iload(flag);}
                                if(f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("Traffic")&&f.name().equalsString("simMode"))cb.invokestatic(owner,PREFIX+"replaced",VOID);
                                if(intr&&Prepare.field(f,Opcode.PUTSTATIC,"Traffic","velUpd")){cb.aload(0).invokestatic(owner,PREFIX+"replace",ONE);recycled=true;}
                            }
                            if(ce instanceof ReturnInstruction)cb.invokestatic(owner,PREFIX+"leave",VOID);
                            cb.with(ce);
                            if(root&&ce instanceof InvokeInstruction i&&Prepare.call(i,"Traffic","runDelta","()V"))cb.invokestatic(owner,PREFIX+"sample",VOID);
                        }
                        public void atEnd(CodeBuilder cb){if(intr&&!recycled)throw new IllegalArgumentException("traffic composed recycle marker missing");cb.labelBinding(end).labelBinding(handler).invokestatic(owner,PREFIX+"leave",VOID).athrow();cb.exceptionCatchAll(begin,end,handler);}
                    });});
                }
                public void atEnd(ClassBuilder b){
                    for(String op:List.of("enter","leave","replaced"))safe(b,op,VOID,cb->cb.invokestatic(OBS,op,VOID));
                    safe(b,"replace",ONE,cb->{cb.aload(0).getfield(INTR,"id",CD_int).invokestatic(OBS,"changed",MethodTypeDesc.of(CD_void,CD_int));});
                    safe(b,"activation",MethodTypeDesc.of(CD_void,INTR,CD_boolean),cb->{cb.iload(1).ifThen(c->c.aload(0).getfield(INTR,"on",CD_boolean).ifThen(Opcode.IFEQ,x->x.aload(0).getfield(INTR,"id",CD_int).invokestatic(OBS,"changed",MethodTypeDesc.of(CD_void,CD_int))));});
                    if(name.equals("Core"))safe(b,"sample",VOID,cb->cb.invokestatic(TRAFFIC,PREFIX+"copy",VOID));
                    if(name.equals("Traffic")){
                        safe(b,"copy",VOID,cb->{cb.invokestatic(OBS,"begin",MethodTypeDesc.of(CD_boolean));cb.ifThen(c->{
                            Prepare.read(c,"Core","delta","J");Prepare.read(c,"Core","simSpeed","F");Prepare.read(c,"Core","paused","Z");Prepare.read(c,"Core","globSimPaused","I");Prepare.read(c,"Core","loaderPaused","Z");Prepare.read(c,"Traffic","simMode","I");
                            c.invokestatic(OBS,"header",MethodTypeDesc.of(CD_void,CD_long,CD_float,CD_boolean,CD_int,CD_boolean,CD_int));
                            for(int i=0;i<7;i++)c.loadConstant(i).invokestatic(TRAFFIC,PREFIX+"slot",MethodTypeDesc.of(CD_void,CD_int));
                            c.invokestatic(OBS,"publish",VOID);
                        });cb.invokestatic(OBS,"end",VOID);});
                        safe(b,"slot",MethodTypeDesc.of(CD_void,CD_int),cb->{
                            cb.getstatic(TRAFFIC,"in",INTR.arrayType()).dup().arraylength().loadConstant(7).ifThen(Opcode.IF_ICMPNE,c->{c.pop();c.invokestatic(OBS,"disable",VOID);c.return_();});
                            cb.iload(0).aaload().astore(1).iload(0);cb.aload(1).getfield(INTR,"id",CD_int);
                            for(String flag:List.of("on","allOn","altInfoOn","airbus"))cb.aload(1).getfield(INTR,flag,CD_boolean);
                            for(String v:VALUES)cb.aload(1).getfield(INTR,v,v.equals("fpmAct")?CD_float:CD_double);
                            cb.invokestatic(OBS,"aircraft",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_boolean,CD_boolean,CD_boolean,CD_boolean,CD_double,CD_double,CD_double,CD_double,CD_double,CD_double,CD_double,CD_float,CD_double,CD_double));
                        });
                    }
                }
            });var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("traffic verify "+name+" "+errors.getFirst());out.put(name,bytes);
        }
        TerrainAccess.verifyAdded(before,out,base.inputs);return out;
    }
    static void safe(ClassBuilder b,String name,MethodTypeDesc type,java.util.function.Consumer<CodeBuilder> body){
        // Package visibility permits the default-package Core->Traffic bridge legally.
        b.withMethodBody(PREFIX+name,type,ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{cb.trying(x->body.accept(x),c->c.catching(CD_Throwable,h->{h.pop();h.trying(x->{x.invokestatic(OBS,"disable",VOID);x.invokestatic(OBS,"end",VOID);},cc->cc.catching(CD_Throwable,x->x.pop()));}));cb.return_();});
    }
}
