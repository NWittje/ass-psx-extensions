package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound readonly native destination-zone query. This does not calculate or
 * apply destination pressure/temperature/wind, change cached selection, or
 * qualify the atmosphere/IAS relocation adapter. Requires exact receivers and
 * complete coherent weather input capture/exclusion before use. */
public final class IosWeatherZonePrepare {
    public static final String QUERY="psxExporterIosDestinationZone";
    private static final ClassDesc MATH=ClassDesc.of("java.lang.Math"),DOUBLE=ClassDesc.of("java.lang.Double"),FLOAT=ClassDesc.of("java.lang.Float");
    private static final MethodTypeDesc UNARY=MethodTypeDesc.of(CD_double,CD_double),DISTANCE=MethodTypeDesc.of(CD_double,CD_double,CD_double,CD_double,CD_double);
    public record Layout(ClassDesc weather,ClassDesc zone,ClassDesc aero,ClassDesc calc){}
    public record Selection(Layout layout,int selectionBegin,int selectionEnd,int zonePrefixEnd){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static void field(ClassModel model,String name,ClassDesc type,boolean statik,boolean external){
        var all=model.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing/duplicate destination-zone field "+name);var f=all.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&ClassFile.ACC_FINAL)==0&&(!external||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible destination-zone field "+name);
    }
    private static MethodModel method(ClassModel model,String name,MethodTypeDesc type,boolean statik){
        var all=model.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/overloaded destination-zone method "+name);var m=all.getFirst();
        require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_PRIVATE))==(statik?ClassFile.ACC_STATIC:0),"Changed destination-zone method context "+name);return m;
    }
    private record Body(List<Instruction> instructions,List<Integer> offsets,Map<Label,Integer> labels){}
    private static Body body(CodeModel code){
        var instructions=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int offset=0;
        for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),instructions.size());if(e instanceof Instruction i){instructions.add(i);offsets.add(offset);offset+=i.sizeInBytes();}}
        offsets.add(offset);return new Body(List.copyOf(instructions),List.copyOf(offsets),Map.copyOf(labels));
    }
    private static List<String> identity(Body b,int start,int end){
        var result=new ArrayList<String>();for(int n=start;n<end;n++){var i=b.instructions.get(n);
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof LoadInstruction l)result.add("load:"+l.typeKind()+":"+l.slot());
            else if(i instanceof StoreInstruction s)result.add("store:"+s.typeKind()+":"+s.slot());
            else if(i instanceof BranchInstruction branch)result.add(i.opcode()+":"+(b.labels.get(branch.target())-start));
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else if(i instanceof InvokeInstruction call)result.add(i.opcode()+":"+call.owner().asInternalName()+":"+call.name()+":"+call.type());
            else result.add(i.opcode().name());
        }return List.copyOf(result);
    }
    public static byte[] distanceReference(ClassFile cf,Layout l){return cf.build(l.calc,b->b.withMethodBody("orthodromicDistNm",DISTANCE,ClassFile.ACC_STATIC,c->
        c.dload(0).dload(4).dsub().ldc(2.0).ddiv().invokestatic(MATH,"sin",UNARY).dstore(8)
            .dload(2).dload(6).dsub().ldc(2.0).ddiv().invokestatic(MATH,"sin",UNARY).dstore(10)
            .ldc(6875.493541569878).dload(8).dload(8).dmul().dload(0).invokestatic(MATH,"cos",UNARY)
            .dload(4).invokestatic(MATH,"cos",UNARY).dmul().dload(10).dmul().dload(10).dmul().dadd()
            .invokestatic(MATH,"sqrt",UNARY).invokestatic(MATH,"asin",UNARY).dmul().dreturn()));}
    public static byte[] selectionReference(ClassFile cf,Layout l){return cf.build(l.weather,b->b.withMethodBody("select",MethodTypeDesc.of(CD_void),ClassFile.ACC_STATIC,c->{
        c.iconst_0().putstatic(l.weather,"wxLocked",CD_boolean).getstatic(l.weather,"wxBasic",l.zone).putstatic(l.weather,"tmpZone",l.zone)
            .ldc(320.0).putstatic(l.weather,"lastCheckZoneDist",CD_double).getstatic(l.aero,"realLat",CD_double).putstatic(l.weather,"tmpLat",CD_double)
            .getstatic(l.aero,"realLon",CD_double).putstatic(l.weather,"tmpLon",CD_double);
        for(int n=1;n<=7;n++)c.getstatic(l.weather,"wx"+n,l.zone).invokevirtual(l.zone,"checkZone",MethodTypeDesc.of(CD_void));c.return_();
    }));}
    /** Native physical prefix including icon branch labels, without UI tail. */
    public static byte[] zoneReference(ClassFile cf,Layout l){return cf.build(l.zone,b->b.withMethodBody("checkZone",MethodTypeDesc.of(CD_void),0,c->{
        var active=c.newLabel();var locked=c.newLabel();var nearest=c.newLabel();var iconNear=c.newLabel();var iconValue=c.newLabel();var inactive=c.newLabel();var done=c.newLabel();
        c.aload(0).getfield(l.zone,"lat",CD_float).fconst_0().fcmpl().ifne(active)
            .aload(0).getfield(l.zone,"lon",CD_float).fconst_0().fcmpl().ifne(active).aload(0).getfield(l.zone,"elev",CD_int).ifeq(inactive).labelBinding(active)
            .getstatic(l.weather,"wxLocked",CD_boolean).ifne(locked).getstatic(l.weather,"wxLock",CD_String).aload(0).getfield(l.zone,"arptIcao",CD_String)
            .invokevirtual(CD_String,"equals",MethodTypeDesc.of(CD_boolean,CD_Object)).ifeq(locked)
            .iconst_1().putstatic(l.weather,"wxLocked",CD_boolean).aload(0).putstatic(l.weather,"tmpZone",l.zone).labelBinding(locked)
            .aload(0).aload(0).getfield(l.zone,"lat",CD_float).f2d().aload(0).getfield(l.zone,"lon",CD_float).f2d()
            .getstatic(l.weather,"tmpLat",CD_double).getstatic(l.weather,"tmpLon",CD_double).invokestatic(l.calc,"orthodromicDistNm",DISTANCE).putfield(l.zone,"distToAcft",CD_double)
            .getstatic(l.weather,"lastCheckZoneDist",CD_double).aload(0).getfield(l.zone,"distToAcft",CD_double).dcmpl().ifle(nearest)
            .aload(0).getfield(l.zone,"distToAcft",CD_double).putstatic(l.weather,"lastCheckZoneDist",CD_double).getstatic(l.weather,"wxLocked",CD_boolean).ifne(nearest)
            .aload(0).putstatic(l.weather,"tmpZone",l.zone).labelBinding(nearest)
            .aload(0).aload(0).getfield(l.zone,"distToAcft",CD_double).ldc(320.0).dcmpl().ifle(iconNear).iconst_0().goto_(iconValue)
            .labelBinding(iconNear).iconst_1().labelBinding(iconValue).putfield(l.zone,"iconMode",CD_int).goto_(done)
            .labelBinding(inactive).aload(0).iconst_0().putfield(l.zone,"iconMode",CD_int).labelBinding(done).return_();
    }));}
    public static Selection select(ClassModel weather,ClassModel zone,ClassModel aero,ClassModel calc){
        var l=new Layout(weather.thisClass().asSymbol(),zone.thisClass().asSymbol(),aero.thisClass().asSymbol(),calc.thisClass().asSymbol());
        for(var model:List.of(weather,zone,aero,calc))require((model.flags().flagsMask()&ClassFile.ACC_INTERFACE)==0,"Zone dependency changed to interface");
        for(var type:List.of(l.zone,l.aero,l.calc))require(pkg(type).equals(pkg(l.weather)),"Changed zone dependency package");
        for(var m:weather.methods())require(!m.methodName().equalsString(QUERY),"Existing destination-zone query");
        for(String name:List.of("wxBasic","tmpZone"))field(weather,name,l.zone,true,false);for(int n=1;n<=7;n++)field(weather,"wx"+n,l.zone,true,false);
        field(weather,"wxLock",CD_String,true,false);field(weather,"wxLocked",CD_boolean,true,false);
        for(String name:List.of("lastCheckZoneDist","tmpLat","tmpLon"))field(weather,name,CD_double,true,false);
        for(String name:List.of("realLat","realLon"))field(aero,name,CD_double,true,true);
        field(zone,"lat",CD_float,false,true);field(zone,"lon",CD_float,false,true);field(zone,"elev",CD_int,false,true);field(zone,"arptIcao",CD_String,false,true);
        field(zone,"distToAcft",CD_double,false,true);field(zone,"iconMode",CD_int,false,false);
        var distance=method(calc,"orthodromicDistNm",DISTANCE,true).code().orElseThrow();require(distance.exceptionHandlers().isEmpty(),"Changed geometry exceptional effects");
        var a=body(distance);var r=body(ClassFile.of().parse(distanceReference(ClassFile.of(),l)).methods().getFirst().code().orElseThrow());
        require(identity(a,0,a.instructions.size()).equals(identity(r,0,r.instructions.size())),"Changed complete native spherical geometry");
        var check=method(zone,"checkZone",MethodTypeDesc.of(CD_void),false).code().orElseThrow();require(check.exceptionHandlers().isEmpty(),"Changed zone selection exceptional context");
        var z=body(check);var zr=body(ClassFile.of().parse(zoneReference(ClassFile.of(),l)).methods().getFirst().code().orElseThrow());
        int end=zr.instructions.size()-1;require(z.instructions.size()>=end&&identity(z,0,end).equals(identity(zr,0,end)),"Changed native zone validity/lock/nearest/icon branch order");
        var moderate=method(weather,"runModerato",MethodTypeDesc.of(CD_void),true).code().orElseThrow();require(moderate.exceptionHandlers().isEmpty(),"Changed weather selection exceptional context");
        var w=body(moderate);var wr=body(ClassFile.of().parse(selectionReference(ClassFile.of(),l)).methods().getFirst().code().orElseThrow());int count=wr.instructions.size()-1,begin=-1;
        for(int n=0;n+count<=w.instructions.size();n++)if(w.instructions.get(n).opcode()==Opcode.ICONST_0&&identity(w,n,n+count).equals(identity(wr,0,count))){require(begin<0,"Ambiguous native selection prefix");begin=n;}
        require(begin>=0,"Missing/reordered native seven-zone selection prefix");return new Selection(l,w.offsets.get(begin),w.offsets.get(begin+count),z.offsets.get(end));
    }
    public static byte[] prepare(ClassFile cf,ClassModel weather,ClassModel zone,ClassModel aero,ClassModel calc,Selection s){
        require(select(weather,zone,aero,calc).equals(s),"Destination-zone preparation differs from qualification");var l=s.layout;
        return cf.transformClass(weather,ClassTransform.endHandler(b->b.withMethodBody(QUERY,MethodTypeDesc.of(l.zone,CD_double,CD_double),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var invalid=c.newLabel();
            c.dload(0).invokestatic(DOUBLE,"isFinite",MethodTypeDesc.of(CD_boolean,CD_double)).ifeq(invalid)
                .dload(2).invokestatic(DOUBLE,"isFinite",MethodTypeDesc.of(CD_boolean,CD_double)).ifeq(invalid)
                .dload(0).ldc(-Math.PI/2).dcmpg().iflt(invalid).dload(0).ldc(Math.PI/2).dcmpl().ifgt(invalid)
                .dload(2).ldc(-Math.PI).dcmpg().iflt(invalid).dload(2).ldc(Math.PI).dcmpl().ifgt(invalid)
                .getstatic(l.weather,"wxBasic",l.zone).astore(4).aload(4).ifnull(invalid)
                .getstatic(l.weather,"wxLock",CD_String).astore(11).aload(11).ifnull(invalid)
                .aload(11).invokevirtual(CD_String,"length",MethodTypeDesc.of(CD_int)).bipush(32).if_icmpgt(invalid)
                .ldc(320.0).dstore(5).iconst_0().istore(7);
            for(int n=1;n<=7;n++){
                var active=c.newLabel();var skip=c.newLabel();var noLock=c.newLabel();var noNear=c.newLabel();var stationValid=c.newLabel();
                c.getstatic(l.weather,"wx"+n,l.zone).astore(8).aload(8).ifnull(invalid)
                    .aload(8).getfield(l.zone,"lat",CD_float).fconst_0().fcmpl().ifne(active)
                    .aload(8).getfield(l.zone,"lon",CD_float).fconst_0().fcmpl().ifne(active).aload(8).getfield(l.zone,"elev",CD_int).ifeq(skip).labelBinding(active);
                for(var bound:List.of(new Object[]{"lat",(float)(Math.PI/2)},new Object[]{"lon",(float)Math.PI})){
                    String name=(String)bound[0];float limit=(float)bound[1];
                    c.aload(8).getfield(l.zone,name,CD_float).invokestatic(FLOAT,"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(invalid)
                        .aload(8).getfield(l.zone,name,CD_float).ldc(-limit).fcmpg().iflt(invalid)
                        .aload(8).getfield(l.zone,name,CD_float).ldc(limit).fcmpl().ifgt(invalid);
                }
                c.aload(8).getfield(l.zone,"arptIcao",CD_String).ifnull(stationValid)
                    .aload(8).getfield(l.zone,"arptIcao",CD_String).invokevirtual(CD_String,"length",MethodTypeDesc.of(CD_int)).bipush(32).if_icmpgt(invalid).labelBinding(stationValid)
                    .iload(7).ifne(noLock).aload(11).aload(8).getfield(l.zone,"arptIcao",CD_String).invokevirtual(CD_String,"equals",MethodTypeDesc.of(CD_boolean,CD_Object)).ifeq(noLock)
                    .iconst_1().istore(7).aload(8).astore(4).labelBinding(noLock)
                    .aload(8).getfield(l.zone,"lat",CD_float).f2d().aload(8).getfield(l.zone,"lon",CD_float).f2d().dload(0).dload(2).invokestatic(l.calc,"orthodromicDistNm",DISTANCE).dstore(9)
                    .dload(9).invokestatic(DOUBLE,"isFinite",MethodTypeDesc.of(CD_boolean,CD_double)).ifeq(invalid).dload(9).dconst_0().dcmpg().iflt(invalid)
                    .dload(5).dload(9).dcmpl().ifle(noNear).dload(9).dstore(5).iload(7).ifne(noNear).aload(8).astore(4).labelBinding(noNear).labelBinding(skip);
            }
            c.aload(4).areturn().labelBinding(invalid).aconst_null().areturn();
        })));
    }
    private IosWeatherZonePrepare(){}
}
