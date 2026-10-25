package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound readonly destination-temperature/QNH arithmetic. Native fields,
 * smoothing, timers, SIGMET and corridor state are never mutated by the query.
 * Immutable inputs require separately qualified coherent native capture. */
public final class IosAtmospherePrepare {
    public static final String TEMPERATURE="psxExporterIosDestinationTemperature",QNH="psxExporterIosDestinationQnh";
    private static final ClassDesc MATH=ClassDesc.of("java.lang.Math"),FLOAT=ClassDesc.of("java.lang.Float");
    private static final MethodTypeDesc ROUND=MethodTypeDesc.of(CD_int,CD_float),MAX=MethodTypeDesc.of(CD_int,CD_int);
    public record Layout(ClassDesc weather,ClassDesc zone,ClassDesc aero,ClassDesc earth,ClassDesc sigmet,ClassDesc corridor,ClassDesc calc,ClassDesc input){}
    public record Window(int begin,int end,int beginOffset,int endOffset){}
    public record Selection(Layout layout,Window seasonal,Window lapse,Window cap,Window corridor,Window pressure){}
    private record Body(List<Instruction> instructions,List<Integer> offsets,Map<Label,Integer> labels){}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException(why);}
    private static MethodModel method(ClassModel c,String name,MethodTypeDesc type){
        var all=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/overloaded atmosphere method "+name);var m=all.getFirst();
        require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,"Changed atmosphere method context "+name);
        require(m.code().orElseThrow().exceptionHandlers().isEmpty(),"Changed atmosphere exceptional context "+name);return m;
    }
    private static Body body(CodeModel code){var is=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int p=0;
        for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),is.size());if(e instanceof Instruction i){is.add(i);offsets.add(p);p+=i.sizeInBytes();}}offsets.add(p);return new Body(List.copyOf(is),List.copyOf(offsets),Map.copyOf(labels));}
    private static List<String> identity(Body b,int start,int end){var out=new ArrayList<String>();for(int n=start;n<end;n++){var i=b.instructions.get(n);
        if(i instanceof FieldInstruction f)out.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
        else if(i instanceof InvokeInstruction c)out.add(i.opcode()+":"+c.owner().asInternalName()+":"+c.name()+":"+c.type());
        else if(i instanceof ConstantInstruction c)out.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
        else if(i instanceof LoadInstruction l)out.add("load:"+l.typeKind()+":"+l.slot());
        else if(i instanceof StoreInstruction s)out.add("store:"+s.typeKind()+":"+s.slot());
        else if(i instanceof BranchInstruction branch){Integer p=b.labels.get(branch.target());require(p!=null,"Unbound atmosphere branch");out.add(i.opcode()+":"+(p-start));}
        else out.add(i.opcode().name());}return List.copyOf(out);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean external){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing atmosphere field "+name);var f=all.getFirst();
        int flags=f.flags().flagsMask();require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(!external||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible atmosphere field "+name);
    }
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    /** Independent authored instruction references, not recovered native source. */
    public static byte[] reference(ClassFile cf,Layout l){return cf.build(l.weather,b->{
        var scratch=MethodTypeDesc.of(CD_void,Collections.nCopies(14,CD_float).toArray(ClassDesc[]::new));
        b.withMethodBody("seasonal",scratch,ClassFile.ACC_STATIC,c->{var south=c.newLabel();
            c.getstatic(l.aero,"realLat",CD_double).invokestatic(MATH,"abs",MethodTypeDesc.of(CD_double,CD_double)).ldc(.01745329238474369).ddiv().d2f().fstore(8)
                .getstatic(l.earth,"simDayOfYear",CD_int).sipush(182).isub().invokestatic(MATH,"abs",MAX).i2f().fstore(9)
                .getstatic(l.aero,"realLat",CD_double).dconst_0().dcmpl().ifle(south).ldc(182f).fload(9).fsub().fstore(9).labelBinding(south)
                .fload(9).fload(8).fmul().ldc(2000f).fdiv().fstore(9).ldc(-52f).fload(9).fadd().fload(8).fload(8).fmul().ldc(300f).fdiv().fsub().putstatic(l.weather,"targTemp",CD_float).return_();});
        b.withMethodBody("lapse",scratch,ClassFile.ACC_STATIC,c->{
            var done=c.newLabel();var above=c.newLabel();var cold=c.newLabel();var belowValue=c.newLabel();var ordinary=c.newLabel();var topOK=c.newLabel();var depthOK=c.newLabel();var iceOK=c.newLabel();var within=c.newLabel();var inversionDone=c.newLabel();
            c.fload(8).getstatic(l.weather,"fTropopause",CD_float).fcmpg().ifge(done)
                .fload(8).getstatic(l.weather,"gndElev",CD_float).fsub().fstore(9).getstatic(l.weather,"fTropopause",CD_float).getstatic(l.weather,"gndElev",CD_float).fsub().fstore(10)
                .fload(9).fconst_0().fcmpg().ifge(above).getstatic(l.weather,"gndTmp",CD_float).getstatic(l.weather,"gndElev",CD_float).fload(8).fsub().ldc(.0019812f).fmul().fadd().fstore(11)
                .fload(11).ldc(50f).fcmpl().ifle(cold).ldc(50f).goto_(belowValue).labelBinding(cold).fload(11).labelBinding(belowValue).putstatic(l.weather,"targTemp",CD_float).goto_(done).labelBinding(above)
                .getstatic(l.weather,"currZone",l.zone).getfield(l.zone,"inversionOn",CD_boolean).ifeq(ordinary)
                .getstatic(l.weather,"currZone",l.zone).getfield(l.zone,"inversionTop",CD_int).i2f().fstore(12).fload(12).ldc(500f).fcmpg().ifge(topOK).ldc(500f).fstore(12).labelBinding(topOK)
                .fload(10).fload(12).ldc(1000f).fadd().fcmpg().ifge(depthOK).fload(12).ldc(1000f).fadd().fstore(10).labelBinding(depthOK)
                .getstatic(l.weather,"currZone",l.zone).getfield(l.zone,"inversionTmp",CD_int).i2f().fstore(13).getstatic(l.sigmet,"inIce",CD_boolean).ifeq(iceOK)
                .fload(13).fconst_0().fcmpl().ifle(iceOK).fconst_0().fstore(13).labelBinding(iceOK)
                .fload(9).fload(12).fcmpl().ifle(within).fload(9).fload(12).fsub().fload(10).fload(12).fsub().fdiv().fstore(11)
                .fload(13).fload(11).fload(13).getstatic(l.weather,"targTemp",CD_float).fsub().fmul().fsub().putstatic(l.weather,"targTemp",CD_float).goto_(inversionDone).labelBinding(within)
                .fload(9).fload(12).fdiv().fstore(11).getstatic(l.weather,"gndTmp",CD_float).fload(11).getstatic(l.weather,"gndTmp",CD_float).fload(13).fsub().fmul().fsub().putstatic(l.weather,"targTemp",CD_float)
                .getstatic(l.weather,"targTemp",CD_float).ldc(-80f).fcmpg().ifge(inversionDone).ldc(-80f).putstatic(l.weather,"targTemp",CD_float).labelBinding(inversionDone)
                .sipush(30000).putstatic(l.weather,"invertionReleaseTimer",CD_int).goto_(done).labelBinding(ordinary)
                .fload(9).fload(10).fdiv().fstore(11).getstatic(l.weather,"gndTmp",CD_float).fload(11).getstatic(l.weather,"gndTmp",CD_float).getstatic(l.weather,"targTemp",CD_float).fsub().fmul().fsub().putstatic(l.weather,"targTemp",CD_float).labelBinding(done).return_();});
        b.withMethodBody("cap",scratch,ClassFile.ACC_STATIC,c->{var done=c.newLabel();c.fload(8).invokestatic(MATH,"round",ROUND).invokestatic(l.calc,"maxOatAlt",MAX).istore(9)
            .getstatic(l.weather,"targTemp",CD_float).iload(9).i2f().fcmpl().ifle(done).iload(9).i2f().putstatic(l.weather,"targTemp",CD_float).labelBinding(done).return_();});
        b.withMethodBody("corridor",scratch,ClassFile.ACC_STATIC,c->c.getstatic(l.weather,"targTemp",CD_float).getstatic(l.corridor,"acftOa",CD_float).getstatic(l.weather,"targTemp",CD_float).fsub().getstatic(l.corridor,"abmFac",CD_float).fmul().fadd().putstatic(l.weather,"targTemp",CD_float).return_());
        b.withMethodBody("pressure",scratch,ClassFile.ACC_STATIC,c->c.getstatic(l.weather,"currZone",l.zone).getfield(l.zone,"fred",CD_int).i2f().ldc(.33863887f).fmul().fstore(11).return_());
        b.withMethodBody("maximum",MAX,ClassFile.ACC_STATIC,c->{var below=c.newLabel();var done=c.newLabel();c.iload(0).ldc(45100).if_icmple(below).bipush(-40).goto_(done).labelBinding(below)
            .ldc(55f).iload(0).i2f().ldc(45100f).fdiv().ldc(95f).fmul().fsub().invokestatic(MATH,"round",ROUND).labelBinding(done).ireturn();});
    });}
    private static Window find(Body nativeBody,Body reference){int count=reference.instructions.size()-1,found=-1;var expected=identity(reference,0,count);
        for(int n=0;n+count<=nativeBody.instructions.size();n++)if(identity(nativeBody,n,n+count).equals(expected)){require(found<0,"Ambiguous atmosphere arithmetic");found=n;}
        require(found>=0,"Missing/reordered/changed atmosphere arithmetic");return new Window(found,found+count,nativeBody.offsets.get(found),nativeBody.offsets.get(found+count));}
    public static Selection select(ClassModel weather,ClassModel zone,ClassModel aero,ClassModel earth,ClassModel sigmet,ClassModel corridor,ClassModel calc,ClassModel input){
        var l=new Layout(weather.thisClass().asSymbol(),zone.thisClass().asSymbol(),aero.thisClass().asSymbol(),earth.thisClass().asSymbol(),sigmet.thisClass().asSymbol(),corridor.thisClass().asSymbol(),calc.thisClass().asSymbol(),input.thisClass().asSymbol());
        for(var c:List.of(weather,zone,aero,earth,sigmet,corridor,calc)){require((c.flags().flagsMask()&ClassFile.ACC_INTERFACE)==0&&pkg(c.thisClass().asSymbol()).equals(pkg(l.weather)),"Changed native atmosphere class/package");}
        require((input.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_INTERFACE))==(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL),"Atmosphere operands must be public/final concrete class");
        for(var f:input.fields())require((f.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_STATIC))==(ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL),"Atmosphere operand storage must be immutable public instance");
        for(String n:List.of("gndElev","gndTmp","fTropopause","targTemp"))field(weather,n,CD_float,true,false);
        field(weather,"currZone",l.zone,true,false);field(weather,"invertionReleaseTimer",CD_int,true,false);
        for(String n:List.of("inversionTop","inversionTmp","fred"))field(zone,n,CD_int,false,true);field(zone,"inversionOn",CD_boolean,false,true);
        field(aero,"realLat",CD_double,true,true);field(earth,"simDayOfYear",CD_int,true,true);field(sigmet,"inIce",CD_boolean,true,true);
        field(corridor,"acftOa",CD_float,true,true);field(corridor,"abmFac",CD_float,true,true);
        field(input,"latitude",CD_double,false,true);for(String n:List.of("day","inversionTop","inversionTmp"))field(input,n,CD_int,false,true);
        for(String n:List.of("altitudeMsl","groundC","zoneElevation","tropopause","tropopauseTemperature","corridorOat","corridorFactor"))field(input,n,CD_float,false,true);
        for(String n:List.of("fixedTropopauseTemperature","inversionOn","icing"))field(input,n,CD_boolean,false,true);
        for(var m:weather.methods())require(!Set.of(TEMPERATURE,QNH).contains(m.methodName().stringValue()),"Existing destination atmosphere query");
        var ref=ClassFile.of().parse(reference(ClassFile.of(),l));var maxBody=body(method(calc,"maxOatAlt",MAX).code().orElseThrow());var mr=body(ref.methods().get(5).code().orElseThrow());
        require(identity(maxBody,0,maxBody.instructions.size()).equals(identity(mr,0,mr.instructions.size())),"Changed complete native temperature cap");
        var nativeBody=body(method(weather,"interpolate",MethodTypeDesc.of(CD_void)).code().orElseThrow());var windows=new ArrayList<Window>();
        for(int n=0;n<5;n++)windows.add(find(nativeBody,body(ref.methods().get(n).code().orElseThrow())));
        for(int n=1;n<windows.size();n++)require(windows.get(n-1).end<=windows.get(n).begin,"Changed temperature/pressure ordering");
        return new Selection(l,windows.get(0),windows.get(1),windows.get(2),windows.get(3),windows.get(4));
    }
    private static void emit(CodeBuilder c,Body b,Window window,Layout l){var labels=new HashMap<Label,Label>();
        for(var e:b.labels.entrySet())if(e.getValue()>=window.begin&&e.getValue()<=window.end)labels.put(e.getKey(),c.newLabel());
        for(int n=window.begin;n<=window.end;n++){
            for(var e:b.labels.entrySet())if(e.getValue()==n&&labels.containsKey(e.getKey()))c.labelBinding(labels.get(e.getKey()));
            if(n==window.end)break;var i=b.instructions.get(n);
            if(i instanceof BranchInstruction branch){require(labels.containsKey(branch.target()),"Atmosphere branch escapes qualified window");c.branch(i.opcode(),labels.get(branch.target()));}
            else if(i instanceof FieldInstruction f){String name=f.name().stringValue();var owner=f.owner().asSymbol();
                if(owner.equals(l.weather)&&name.equals("targTemp")){if(i.opcode()==Opcode.GETSTATIC)c.fload(1);else c.fstore(1);}
                else if(owner.equals(l.weather)&&name.equals("invertionReleaseTimer")){require(i.opcode()==Opcode.PUTSTATIC,"Unexpected timer read");c.pop();}
                else if(owner.equals(l.weather)&&name.equals("currZone"))c.aload(0);
                else if(owner.equals(l.zone))c.getfield(l.input,name,f.typeSymbol());
                else {String dest=owner.equals(l.aero)?"latitude":owner.equals(l.earth)?"day":owner.equals(l.sigmet)?"icing":owner.equals(l.corridor)?(name.equals("acftOa")?"corridorOat":"corridorFactor"):switch(name){case "gndTmp"->"groundC";case "gndElev"->"zoneElevation";case "fTropopause"->"tropopause";default->throw new IllegalArgumentException("Unexpected atmosphere input");};
                    require(i.opcode()==Opcode.GETSTATIC,"Unexpected atmosphere write");c.aload(0).getfield(l.input,dest,f.typeSymbol());}
            }else c.with(i);
        }
    }
    public static byte[] prepare(ClassFile cf,ClassModel weather,ClassModel zone,ClassModel aero,ClassModel earth,ClassModel sigmet,ClassModel corridor,ClassModel calc,ClassModel input,Selection s){
        require(select(weather,zone,aero,earth,sigmet,corridor,calc,input).equals(s),"Atmosphere preparation differs from selection");var l=s.layout;var b=body(method(weather,"interpolate",MethodTypeDesc.of(CD_void)).code().orElseThrow());
        return cf.transformClass(weather,ClassTransform.endHandler(builder->{
            builder.withMethodBody(TEMPERATURE,MethodTypeDesc.of(CD_float,l.input),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                var valid=c.newLabel();var seasonal=c.newLabel();var initialized=c.newLabel();var finite=c.newLabel();c.aload(0).ifnonnull(valid).ldc(Float.NaN).freturn().labelBinding(valid)
                    .aload(0).getfield(l.input,"fixedTropopauseTemperature",CD_boolean).ifeq(seasonal).aload(0).getfield(l.input,"tropopauseTemperature",CD_float).fstore(1).goto_(initialized).labelBinding(seasonal);
                emit(c,b,s.seasonal,l);c.labelBinding(initialized).aload(0).getfield(l.input,"altitudeMsl",CD_float).fstore(8);
                emit(c,b,s.lapse,l);emit(c,b,s.cap,l);emit(c,b,s.corridor,l);
                c.fload(1).invokestatic(FLOAT,"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifne(finite).ldc(Float.NaN).freturn().labelBinding(finite).fload(1).freturn();
            });
            builder.withMethodBody(QNH,MethodTypeDesc.of(CD_float,CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var valid=c.newLabel();c.iload(0).ifgt(valid).ldc(Float.NaN).freturn().labelBinding(valid).iload(0).i2f().ldc(.33863887f).fmul().freturn();});
        }));
    }
    private IosAtmospherePrepare(){}
}
