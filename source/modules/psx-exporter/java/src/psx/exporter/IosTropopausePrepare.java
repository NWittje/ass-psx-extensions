package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound destination query. Fixed mode uses the configured target, not the
 * departure smoothing cache. Destination native support and coherent settings
 * capture must be qualified separately before this query can be used. */
public final class IosTropopausePrepare {
    public static final String QUERY="psxExporterIosDestinationTropopause";
    public static final MethodTypeDesc TYPE=MethodTypeDesc.of(CD_float,CD_double,CD_int,CD_float,CD_boolean,CD_int);
    public record Layout(ClassDesc weather,ClassDesc aero,ClassDesc earth){}
    public record Window(int first,int end,int beginOffset,int endOffset){}
    public record Selection(Layout layout,Window inputs,Window seasonal,Window support){}
    private record Body(List<Instruction> instructions,List<Integer> offsets,Map<Label,Integer> labels){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static Body body(CodeModel code){var is=new ArrayList<Instruction>();var os=new ArrayList<Integer>();var ls=new HashMap<Label,Integer>();int p=0;
        for(var e:code){if(e instanceof LabelTarget t)ls.put(t.label(),is.size());if(e instanceof Instruction i){is.add(i);os.add(p);p+=i.sizeInBytes();}}os.add(p);return new Body(List.copyOf(is),List.copyOf(os),Map.copyOf(ls));}
    private static List<String> identity(Body b,int first,int end){var out=new ArrayList<String>();for(int n=first;n<end;n++){var i=b.instructions.get(n);
        if(i instanceof FieldInstruction f)out.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
        else if(i instanceof InvokeInstruction v)out.add(i.opcode()+":"+v.owner().asInternalName()+":"+v.name()+":"+v.type());
        else if(i instanceof ConstantInstruction v)out.add("constant:"+v.constantValue().getClass().getName()+":"+v.constantValue());
        else if(i instanceof LoadInstruction v)out.add("load:"+v.typeKind()+":"+v.slot());
        else if(i instanceof StoreInstruction v)out.add("store:"+v.typeKind()+":"+v.slot());
        else if(i instanceof BranchInstruction v){Integer t=b.labels.get(v.target());require(t!=null,"Unbound tropopause branch");out.add(i.opcode()+":"+(t-first));}
        else out.add(i.opcode().name());}return List.copyOf(out);}
    private static void field(ClassModel m,String name,ClassDesc type,boolean external){var all=m.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing tropopause field "+name);var f=all.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&(flags&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC&&(!external||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible tropopause field "+name);}
    private static CodeModel code(ClassModel m){var all=m.methods().stream().filter(v->v.methodName().equalsString("runModerato")).toList();require(all.size()==1,"Missing/ambiguous native tropopause caller");var v=all.getFirst();
        require(v.methodTypeSymbol().equals(MethodTypeDesc.of(CD_void))&&(v.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==ClassFile.ACC_STATIC&&v.code().isPresent(),"Changed tropopause execution context");var c=v.code().get();require(c.exceptionHandlers().isEmpty(),"Changed tropopause exceptional context");return c;}
    /** Authored arithmetic selector references; no vendor class is executed. */
    public static byte[] reference(ClassFile cf,Layout l){var scratch=MethodTypeDesc.of(CD_void,Collections.nCopies(11,CD_float).toArray(ClassDesc[]::new));return cf.build(l.weather,b->{
        b.withMethodBody("inputs",scratch,ClassFile.ACC_STATIC,c->c.getstatic(l.earth,"simDayOfYear",CD_int).sipush(183).isub().invokestatic(ClassDesc.of("java.lang.Math"),"abs",MethodTypeDesc.of(CD_int,CD_int)).i2f().fstore(6)
            .getstatic(l.aero,"realLat",CD_double).ldc(.01745329238474369).ddiv().d2f().fstore(7).return_());
        b.withMethodBody("seasonal",scratch,ClassFile.ACC_STATIC,c->{var south=c.newLabel();var done=c.newLabel();c.fload(7).fconst_0().fcmpl().ifle(south)
            .fload(9).ldc(50000f).ldc(26100f).fload(6).ldc(.03f).fdiv().fsub().fsub().fload(7).ldc(90f).fdiv().fmul().fsub().fstore(9).goto_(done).labelBinding(south)
            .fload(9).ldc(50000f).ldc(20000f).fload(6).ldc(.03f).fdiv().fadd().fsub().fload(7).fneg().ldc(90f).fdiv().fmul().fsub().fstore(9).labelBinding(done).return_();});
        b.withMethodBody("support",scratch,ClassFile.ACC_STATIC,c->{var done=c.newLabel();c.fload(9).fload(10).ldc(1000f).fadd().fcmpg().ifge(done).fload(10).ldc(1000f).fadd().fstore(9).labelBinding(done).return_();});
    });}
    private static Window find(Body b,Body reference){int count=reference.instructions.size()-1,found=-1;var expected=identity(reference,0,count);for(int n=0;n+count<=b.instructions.size();n++)if(identity(b,n,n+count).equals(expected)){require(found<0,"Ambiguous tropopause arithmetic");found=n;}
        require(found>=0,"Missing/changed/reordered tropopause arithmetic");return new Window(found,found+count,b.offsets.get(found),b.offsets.get(found+count));}
    public static Selection select(ClassModel weather,ClassModel aero,ClassModel earth){var l=new Layout(weather.thisClass().asSymbol(),aero.thisClass().asSymbol(),earth.thisClass().asSymbol());
        for(var m:List.of(weather,aero,earth)){require((m.flags().flagsMask()&ClassFile.ACC_INTERFACE)==0,"Changed native class");String p=m.thisClass().asInternalName();String w=weather.thisClass().asInternalName();require(p.substring(0,p.lastIndexOf('/')+1).equals(w.substring(0,w.lastIndexOf('/')+1)),"Changed native package");}
        field(weather,"tropoAlt",CD_int,false);field(weather,"tropoAltFixed",CD_boolean,false);field(weather,"tropoAltSmoo",CD_float,false);field(weather,"fTropopause",CD_float,false);field(aero,"realLat",CD_double,true);field(earth,"simDayOfYear",CD_int,true);
        require(weather.methods().stream().noneMatch(m->m.methodName().equalsString(QUERY)),"Destination tropopause already prepared");var nativeBody=body(code(weather));var r=ClassFile.of().parse(reference(ClassFile.of(),l));
        var windows=new ArrayList<Window>();for(var m:r.methods())windows.add(find(nativeBody,body(m.code().orElseThrow())));
        require(windows.get(0).end<=windows.get(1).first&&windows.get(1).end==windows.get(2).first,"Changed destination input/season/support ordering");
        int mode=0,units=0;for(int n=windows.get(0).end;n+2<windows.get(1).first;n++){
            var i=nativeBody.instructions.get(n);
            if(i instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asSymbol().equals(l.weather)&&f.name().equalsString("tropoAltFixed")&&f.typeSymbol().equals(CD_boolean)){
                require(nativeBody.instructions.get(n+1) instanceof BranchInstruction branch&&branch.opcode()==Opcode.IFEQ&&Objects.equals(nativeBody.labels.get(branch.target()),windows.get(1).first),"Changed fixed/seasonal selection");mode++;}
            if(i instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asSymbol().equals(l.weather)&&f.name().equalsString("tropoAlt")&&f.typeSymbol().equals(CD_int)&&nativeBody.instructions.get(n+1).opcode()==Opcode.I2F&&nativeBody.instructions.get(n+2) instanceof FieldInstruction out&&out.opcode()==Opcode.PUTSTATIC&&out.owner().asSymbol().equals(l.weather)&&out.name().equalsString("tropoAltSmoo")&&out.typeSymbol().equals(CD_float))units++;
        }
        require(mode==1&&units==1,"Missing/ambiguous configured target units/selection");int end=windows.get(2).end;
        require(end+1<nativeBody.instructions.size()&&nativeBody.instructions.get(end) instanceof LoadInstruction load&&load.typeKind()==TypeKind.FLOAT&&load.slot()==9&&nativeBody.instructions.get(end+1) instanceof FieldInstruction out&&out.opcode()==Opcode.PUTSTATIC&&out.owner().asSymbol().equals(l.weather)&&out.name().equalsString("fTropopause")&&out.typeSymbol().equals(CD_float),"Changed native tropopause result consumption");
        return new Selection(l,windows.get(0),windows.get(1),windows.get(2));}
    private static void emit(CodeBuilder c,Body b,Window w,Layout l){var labels=new HashMap<Label,Label>();for(var e:b.labels.entrySet())if(e.getValue()>=w.first&&e.getValue()<=w.end)labels.put(e.getKey(),c.newLabel());
        for(int n=w.first;n<=w.end;n++){for(var e:b.labels.entrySet())if(e.getValue()==n&&labels.containsKey(e.getKey()))c.labelBinding(labels.get(e.getKey()));if(n==w.end)break;var i=b.instructions.get(n);
            if(i instanceof FieldInstruction f&&f.owner().asSymbol().equals(l.earth)&&f.name().equalsString("simDayOfYear"))c.iload(2);
            else if(i instanceof FieldInstruction f&&f.owner().asSymbol().equals(l.aero)&&f.name().equalsString("realLat"))c.dload(0);
            else if(i instanceof BranchInstruction br){require(labels.containsKey(br.target()),"Escaping tropopause branch");c.branch(i.opcode(),labels.get(br.target()));}else c.with(i);}}
    public static byte[] prepare(ClassFile cf,ClassModel weather,ClassModel aero,ClassModel earth,Selection s){require(select(weather,aero,earth).equals(s),"Changed tropopause selection");var b=body(code(weather));return cf.transformClass(weather,ClassTransform.endHandler(builder->builder.withMethodBody(QUERY,TYPE,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
        var bad=c.newLabel();var seasonal=c.newLabel();var support=c.newLabel();
        c.dload(0).invokestatic(ClassDesc.of("java.lang.Double"),"isFinite",MethodTypeDesc.of(CD_boolean,CD_double)).ifeq(bad).dload(0).invokestatic(ClassDesc.of("java.lang.Math"),"abs",MethodTypeDesc.of(CD_double,CD_double)).ldc(Math.PI/2).dcmpl().ifgt(bad)
            .iload(2).iconst_1().if_icmplt(bad).iload(2).sipush(366).if_icmpgt(bad).fload(3).invokestatic(ClassDesc.of("java.lang.Float"),"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(bad)
            .fload(3).ldc(-2500f).fcmpg().iflt(bad).fload(3).ldc(55000f).fcmpl().ifgt(bad).fload(3).fstore(10).ldc(50000f).fstore(9)
            .iload(4).ifeq(seasonal).iload(5).iflt(bad).iload(5).ldc(60000).if_icmpgt(bad).iload(5).i2f().fstore(9).goto_(support).labelBinding(seasonal);
        emit(c,b,s.inputs,s.layout);emit(c,b,s.seasonal,s.layout);c.labelBinding(support);emit(c,b,s.support,s.layout);c.fload(9).freturn().labelBinding(bad).ldc(Float.NaN).freturn();
    })));}
    private IosTropopausePrepare(){}
}
