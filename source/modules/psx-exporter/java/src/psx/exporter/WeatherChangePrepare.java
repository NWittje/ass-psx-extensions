package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Accepted native weather commands, with input origin retained through the native consumer. */
final class WeatherChangePrepare {
    static final String PREFIX="$psxWeatherChange$";
    private static final ClassDesc OBS=ClassDesc.of("psx.exporter.WeatherChangeObserver");
    final Prepare base;
    final Set<String> required=new TreeSet<>();
    final Set<Prepare.MethodId> ui=new HashSet<>();
    final List<String> packets=new ArrayList<>(),evidence=new ArrayList<>();
    WeatherChangePrepare(Prepare base){this.base=base;}
    private long calls(MethodModel m,String owner,String name,String descriptor){return Prepare.instructions(m).stream().filter(i->Prepare.call(i,owner,name,descriptor)).count();}
    void qualify(){
        var transfer=base.requireMethod("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;");
        if(Prepare.instructions(transfer).stream().filter(i->i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.name().equalsString("sVar")).count()!=1)
            throw new IllegalArgumentException("manual WX accepted packet store");
        var server=base.requireMethod("NetServerThread","run","()V");
        var instructions=Prepare.instructions(server);int ingress=-1;for(int i=0;i<instructions.size();i++)if(Prepare.call(instructions.get(i),"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"))ingress=i;
        if(calls(server,"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")!=1||ingress<1||instructions.get(ingress-1).opcode()!=Opcode.INEG)
            throw new IllegalArgumentException("manual WX negative Main ingress provenance");
        var passive=base.requireMethod("SlideParameter","setActValue","(I)I");
        if(calls(passive,"SwitchEvent","switching","(I)V")!=0)throw new IllegalArgumentException("passive slider invokes input callback");
        for(var id:List.of(new Prepare.MethodId("SlideParameter","parse","()V"),new Prepare.MethodId("SlideParameter","slideIt","(I)V"))){
            if(calls(base.requireMethod(id.owner(),id.name(),id.descriptor()),"SwitchEvent","switching","(I)V")!=1)throw new IllegalArgumentException("manual slider boundary "+id);
            ui.add(id);required.add(id.owner());
        }
        for(var entry:base.methods.entrySet()){
            var id=entry.getKey();boolean weatherUi=id.owner().matches("(Weather\\$.*|InstructorWx.*\\$.*)");
            boolean callback=(id.name().equals("actionPerformed")&&id.descriptor().equals("(Ljava/awt/event/ActionEvent;)V"))||
                (id.name().equals("caretUpdate")&&id.descriptor().equals("(Ljavax/swing/event/CaretEvent;)V"))||
                (id.name().equals("itemStateChanged")&&id.descriptor().equals("(Ljava/awt/event/ItemEvent;)V"));
            if(weatherUi&&callback&&entry.getValue().code().isPresent()){ui.add(id);required.add(id.owner());}
        }
        if(ui.size()<10||ui.size()>96)throw new IllegalArgumentException("manual WX input coverage "+ui.size());
        for(var f:base.models.get("Glob").fields()){
            var name=f.fieldName().stringValue();if(!name.matches("qs(Wx(Aloft|Basic|[1-7]|Mode[1-7]|Clust|CorridorSel|CorridorTxt|Sigmet)|Metar[1-7]|MetarsUp)"))continue;
            if(!f.fieldType().equalsString("I")||(f.flags().flagsMask()&ClassFile.ACC_STATIC)==0||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("manual WX binding "+name);
            packets.add(name);
        }
        Collections.sort(packets);if(packets.size()<23||packets.size()>40)throw new IllegalArgumentException("manual WX record coverage "+packets.size());
        if(calls(base.requireMethod("Weather","runAllegro","()V"),"Weather","interpolate","()V")!=1)throw new IllegalArgumentException("manual WX consumed phase");
        if(calls(base.requireMethod("Weather$ZoneWx","parseMetarIfNewTxt","()V"),"MetarParser","parseMetar","(Ljava/lang/String;LWeather$ZoneWx;)V")!=1)throw new IllegalArgumentException("manual METAR completion");
        if(calls(base.requireMethod("SituationFiler","load","(Ljava/lang/String;)Z"),"Core","unpauseLoader","()V")!=1)throw new IllegalArgumentException("manual WX successful load seam");
        required.addAll(List.of("Qs","Weather","Weather$ZoneWx","SituationFiler"));
        evidence.add("Manual WX: "+ui.size()+" native input boundaries, "+packets.size()+" exact Glob record bindings; accepted Qs.sVar store only; negative Main origins; automatic cycle excluded; native interpolation confirms consumption");
    }
    private static void guarded(CodeBuilder c,java.util.function.Consumer<CodeBuilder> body){
        c.trying(block->body.accept(block),x->x.catching(CD_Throwable,h->{h.pop();h.trying(d->d.invokestatic(OBS,"disable",MethodTypeDesc.of(CD_void)),f->f.catching(CD_Throwable,d->d.pop()));}));
    }
    private static void leave(CodeBuilder c,int entered,boolean success){guarded(c,x->x.iload(entered).ifThen(Opcode.IFNE,y->y.loadConstant(success?1:0).invokestatic(OBS,"uiEnd",MethodTypeDesc.of(CD_void,CD_boolean))));}
    Map<String,byte[]> transform(Map<String,byte[]> input){
        Map<String,byte[]> output=new TreeMap<>(input),before=new TreeMap<>(input);
        for(String name:required){before.putIfAbsent(name,base.inputs.get(name));ClassDesc owner=ClassDesc.of(name);
            var model=base.cf.parse(input.getOrDefault(name,base.inputs.get(name)));
            byte[] bytes=base.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());
                    boolean inputMethod=ui.contains(id),cycle=name.equals("Weather")&&id.name().equals("runAllegro")&&id.descriptor().equals("()V"),transfer=name.equals("Qs")&&id.name().equals("transfer")&&id.descriptor().equals("(Ljava/lang/String;CI)Ljava/lang/String;"),load=name.equals("SituationFiler")&&id.name().equals("load")&&id.descriptor().equals("(Ljava/lang/String;)Z"),metar=name.equals("Weather$ZoneWx")&&id.name().equals("parseMetarIfNewTxt")&&id.descriptor().equals("()V");
                    if(!inputMethod&&!cycle&&!transfer&&!load&&!metar){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel code)){mb.with(me);return;}mb.transformCode(code,new CodeTransform(){
                        int entered,exception;Label start,end,handler;
                        public void atStart(CodeBuilder c){
                            if(inputMethod){entered=c.allocateLocal(TypeKind.INT);c.iconst_0().istore(entered);guarded(c,x->{Prepare.read(x,"Core","loaderPaused","Z");x.ifThen(Opcode.IFEQ,y->y.invokestatic(OBS,"uiEnter",MethodTypeDesc.of(CD_boolean)).istore(entered));});}
                            if(cycle)guarded(c,x->x.invokestatic(OBS,"cycle",MethodTypeDesc.of(CD_void)));
                            if(inputMethod||cycle){exception=c.allocateLocal(TypeKind.REFERENCE);start=c.newLabel();end=c.newLabel();handler=c.newLabel();c.labelBinding(start);}
                        }
                        public void accept(CodeBuilder c,CodeElement ce){
                            if(ce instanceof ReturnInstruction){if(inputMethod)leave(c,entered,true);if(cycle)guarded(c,x->x.invokestatic(OBS,"cycleEnd",MethodTypeDesc.of(CD_void)));}
                            c.with(ce);
                            if(transfer&&ce instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.name().equalsString("sVar"))guarded(c,x->x.aload(0).iload(3).invokestatic(owner,PREFIX+"packet",MethodTypeDesc.of(CD_void,owner,CD_int)));
                            if(load&&ce instanceof Instruction ins&&Prepare.call(ins,"Core","unpauseLoader","()V"))guarded(c,x->x.iconst_3().invokestatic(OBS,"changed",MethodTypeDesc.of(CD_void,CD_int)));
                            if(metar&&ce instanceof Instruction ins&&Prepare.call(ins,"MetarParser","parseMetar","(Ljava/lang/String;LWeather$ZoneWx;)V"))guarded(c,x->x.invokestatic(OBS,"direct",MethodTypeDesc.of(CD_void)));
                        }
                        public void atEnd(CodeBuilder c){if(!inputMethod&&!cycle)return;c.labelBinding(end).labelBinding(handler).astore(exception);
                            if(inputMethod)leave(c,entered,false);if(cycle)guarded(c,x->x.invokestatic(OBS,"cycleEnd",MethodTypeDesc.of(CD_void)));
                            c.aload(exception).athrow();c.exceptionCatchAll(start,end,handler);}
                    });});
                }
                public void atEnd(ClassBuilder b){if(!name.equals("Qs"))return;
                    b.withMethodBody(PREFIX+"packet",MethodTypeDesc.of(CD_void,owner,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                        guarded(c,x->x.iload(1).invokestatic(OBS,"interested",MethodTypeDesc.of(CD_boolean,CD_int)).ifThen(Opcode.IFNE,yes->{
                            yes.aload(0).getfield(ClassDesc.of("Q"),"serialNr",CD_int).istore(2);
                            for(String key:packets)yes.iload(2).getstatic(ClassDesc.of("Glob"),key,CD_int).ifThen(Opcode.IF_ICMPEQ,match->match.iload(1).invokestatic(OBS,"packet",MethodTypeDesc.of(CD_void,CD_int)).return_());
                        }));c.return_();
                    });
                }
            });
            var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("manual WX verify "+name+" "+errors.getFirst());output.put(name,bytes);
        }
        TerrainAccess.verifyAdded(before,output,base.inputs);return output;
    }
}
