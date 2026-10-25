package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound owner-only air-data cache finalizer. Requires destination atmosphere,
 * wind/mode/pose inputs already coherently established under complete exclusion.
 * It calculates into locals and validates before any native field is written.
 * Its Boolean is a local application result, never full relocation completion. */
public final class IosAirDataFinalizerPrepare {
    public static final String REFRESH="psxExporterIosRefreshRelocationAirData";
    private static final ClassDesc MATH=ClassDesc.of("java.lang.Math"),FLOAT=ClassDesc.of("java.lang.Float"),DOUBLE=ClassDesc.of("java.lang.Double");
    private static final MethodTypeDesc ONE=MethodTypeDesc.of(CD_float,CD_float),TWO=MethodTypeDesc.of(CD_float,CD_float,CD_float),UNARY=MethodTypeDesc.of(CD_double,CD_double);
    private static final List<String> OUTPUTS=List.of("pressAlt","oat","tat","isaOatDev","relativeWindDir","cosRelWindDir","sinRelWindDir","headWind","crossWind","totalShear","movingBackwards","tasWithGust","machReal","iasReal");
    public record Layout(ClassDesc aero,ClassDesc weather,ClassDesc calc){}
    public record Selection(Layout layout,int begin,int end,int beginOffset,int endOffset,List<String> outputs){}
    private record Body(List<Instruction> instructions,List<Integer> offsets,Map<Label,Integer> labels){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static MethodModel method(ClassModel c,String name,MethodTypeDesc type){var all=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/overloaded air-data finalizer dependency "+name);var m=all.getFirst();
        require(m.methodTypeSymbol().equals(type)&&m.code().isPresent()&&(m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_PRIVATE))==ClassFile.ACC_STATIC,"Changed air-data finalizer method context");return m;}
    private static Body body(CodeModel code){var ins=new ArrayList<Instruction>();var offsets=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int p=0;
        for(var e:code){if(e instanceof LabelTarget t)labels.put(t.label(),ins.size());if(e instanceof Instruction i){ins.add(i);offsets.add(p);p+=i.sizeInBytes();}}offsets.add(p);return new Body(List.copyOf(ins),List.copyOf(offsets),Map.copyOf(labels));}
    private static List<String> identity(Body b,int start,int end){var out=new ArrayList<String>();for(int n=start;n<end;n++){var i=b.instructions.get(n);
        if(i instanceof FieldInstruction f)out.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
        else if(i instanceof InvokeInstruction c)out.add(i.opcode()+":"+c.owner().asInternalName()+":"+c.name()+":"+c.type());
        else if(i instanceof ConstantInstruction c)out.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
        else if(i instanceof LoadInstruction l)out.add("load:"+l.typeKind()+":"+l.slot());else if(i instanceof StoreInstruction s)out.add("store:"+s.typeKind()+":"+s.slot());
        else if(i instanceof BranchInstruction br){Integer p=b.labels.get(br.target());require(p!=null,"Missing air-data branch target");out.add(i.opcode()+":"+(p-start));}else out.add(i.opcode().name());}return List.copyOf(out);}
    private static String pkg(ClassDesc c){String s=c.descriptorString();return s.substring(0,s.lastIndexOf('/')+1);}
    private static boolean environmentCleanup(Body body,ExceptionCatch handler,ClassDesc aero,int begin,int end){
        // The composed environment observer adds one whole-method cleanup. It is
        // not a native arithmetic catch. Accept only its exact entry/range/tail;
        // the original observer remains intact and is never copied into REFRESH.
        Integer start=body.labels.get(handler.tryStart()),finish=body.labels.get(handler.tryEnd()),target=body.labels.get(handler.handler());
        if(handler.catchType().isPresent()||start==null||finish==null||target==null||start!=1||finish.intValue()!=target.intValue()||
            target+2!=body.instructions.size()||start>begin||finish<end)return false;
        var first=body.instructions.getFirst();var cleanup=body.instructions.get(target);
        return first instanceof InvokeInstruction entry&&entry.opcode()==Opcode.INVOKESTATIC&&entry.owner().asSymbol().equals(aero)&&
            entry.name().equalsString("$psxEnvironment$enter")&&entry.type().equalsString("()V")&&
            cleanup instanceof InvokeInstruction leave&&leave.opcode()==Opcode.INVOKESTATIC&&leave.owner().asSymbol().equals(aero)&&
            leave.name().equalsString("$psxEnvironment$leave")&&leave.type().equalsString("()V")&&body.instructions.get(target+1).opcode()==Opcode.ATHROW;
    }
    public static byte[] reference(ClassFile cf,Layout l){return cf.build(l.aero,b->{
        b.withMethodBody("runDelta",MethodTypeDesc.of(CD_void),ClassFile.ACC_STATIC,c->{
            var tatOK=c.newLabel();var groundWind=c.newLabel();var windDone=c.newLabel();var air=c.newLabel();var forwards=c.newLabel();var moving=c.newLabel();var forwardSpeed=c.newLabel();var speed=c.newLabel();var nativeSpeed=c.newLabel();var chosen=c.newLabel();var gustDone=c.newLabel();
            c.getstatic(l.aero,"altitudeReal",CD_float).getstatic(l.weather,"fHpa",CD_float).ldc(1013.25f).fsub().ldc(27f).fmul().fsub().putstatic(l.aero,"pressAlt",CD_float)
                .getstatic(l.weather,"fTemp",CD_float).putstatic(l.aero,"oat",CD_float)
                .getstatic(l.aero,"oat",CD_float).getstatic(l.aero,"tasReal",CD_float).getstatic(l.aero,"tasReal",CD_float).fmul().ldc(7592f).fdiv().fadd().putstatic(l.aero,"tat",CD_float)
                .getstatic(l.aero,"tat",CD_float).ldc(99f).fcmpl().ifle(tatOK).ldc(99f).putstatic(l.aero,"tat",CD_float).labelBinding(tatOK)
                .getstatic(l.aero,"oat",CD_float).getstatic(l.aero,"altitudeReal",CD_float).invokestatic(l.calc,"getIsaOat",ONE).fsub().putstatic(l.aero,"isaOatDev",CD_float)
                .getstatic(l.aero,"ground",CD_boolean).ifeq(groundWind).getstatic(l.aero,"tasReal",CD_float).ldc(80f).fcmpg().ifge(groundWind)
                .getstatic(l.weather,"fWindDir",CD_float).ldc(.017453292f).fmul().getstatic(l.aero,"longiHdg",CD_float).fsub().putstatic(l.aero,"relativeWindDir",CD_float).goto_(windDone).labelBinding(groundWind)
                .getstatic(l.weather,"fWindDir",CD_float).ldc(.017453292f).fmul().f2d().getstatic(l.aero,"longiHdg",CD_float).f2d().getstatic(l.aero,"driftAngByWind",CD_double).dadd().dsub().d2f().putstatic(l.aero,"relativeWindDir",CD_float).labelBinding(windDone)
                .getstatic(l.aero,"relativeWindDir",CD_float).f2d().invokestatic(MATH,"cos",UNARY).d2f().putstatic(l.aero,"cosRelWindDir",CD_float)
                .getstatic(l.aero,"relativeWindDir",CD_float).f2d().invokestatic(MATH,"sin",UNARY).d2f().putstatic(l.aero,"sinRelWindDir",CD_float)
                .getstatic(l.aero,"cosRelWindDir",CD_float).getstatic(l.weather,"totalWindSpd",CD_float).fmul().putstatic(l.aero,"headWind",CD_float)
                .getstatic(l.aero,"sinRelWindDir",CD_float).getstatic(l.weather,"totalWindSpd",CD_float).fmul().putstatic(l.aero,"crossWind",CD_float)
                .getstatic(l.aero,"smoothImpulseTas",CD_float).getstatic(l.aero,"actHeadShear",CD_float).fadd().putstatic(l.aero,"totalShear",CD_float)
                .getstatic(l.aero,"tasReal",CD_float).getstatic(l.aero,"totalShear",CD_float).fadd().fstore(2)
                .getstatic(l.aero,"ground",CD_boolean).ifeq(air).getstatic(l.aero,"driftAngByWind",CD_double).invokestatic(MATH,"abs",UNARY).ldc(1.5707963705062866).dcmpl().ifle(forwards)
                .iconst_1().goto_(moving).labelBinding(forwards).iconst_0().labelBinding(moving).putstatic(l.aero,"movingBackwards",CD_boolean)
                .getstatic(l.aero,"headWind",CD_float).getstatic(l.aero,"totalShear",CD_float).fadd().getstatic(l.aero,"movingBackwards",CD_boolean).ifeq(forwardSpeed)
                .getstatic(l.aero,"gsReal",CD_float).fneg().goto_(speed).labelBinding(forwardSpeed).getstatic(l.aero,"gsReal",CD_float).labelBinding(speed).fadd().fstore(3)
                .fload(2).fload(3).fcmpg().ifge(nativeSpeed).fload(3).goto_(chosen).labelBinding(nativeSpeed).fload(2).labelBinding(chosen).putstatic(l.aero,"tasWithGust",CD_float).goto_(gustDone).labelBinding(air)
                .iconst_0().putstatic(l.aero,"movingBackwards",CD_boolean).fload(2).putstatic(l.aero,"tasWithGust",CD_float).labelBinding(gustDone)
                .getstatic(l.aero,"tasWithGust",CD_float).getstatic(l.aero,"rollBack",CD_float).fsub().getstatic(l.aero,"oat",CD_float).invokestatic(l.calc,"getMach",TWO).putstatic(l.aero,"machReal",CD_float)
                .getstatic(l.aero,"pressAlt",CD_float).getstatic(l.aero,"machReal",CD_float).invokestatic(l.calc,"getIas",TWO).putstatic(l.aero,"iasReal",CD_float).return_();
        });
        b.withMethodBody("isa",ONE,ClassFile.ACC_STATIC,c->{var below=c.newLabel();var done=c.newLabel();c.fload(0).ldc(36089f).fcmpl().ifle(below).ldc(-56.5f).goto_(done).labelBinding(below).ldc(15f).fload(0).ldc(.0019812f).fmul().fsub().labelBinding(done).freturn();});
    });}
    public static Selection select(ClassModel aero,ClassModel weather,ClassModel calc){
        var l=new Layout(aero.thisClass().asSymbol(),weather.thisClass().asSymbol(),calc.thisClass().asSymbol());require(pkg(l.aero).equals(pkg(l.weather))&&pkg(l.aero).equals(pkg(l.calc)),"Changed native air-data dependency package");
        for(var m:aero.methods())require(!m.methodName().equalsString(REFRESH),"Existing relocation air-data refresh");
        IosAirDataPrepare.select(calc);var refs=ClassFile.of().parse(reference(ClassFile.of(),l));var isa=method(calc,"getIsaOat",ONE).code().orElseThrow();require(isa.exceptionHandlers().isEmpty(),"Changed ISA exceptional effects");
        var i=body(isa);var r=body(refs.methods().get(1).code().orElseThrow());require(identity(i,0,i.instructions.size()).equals(identity(r,0,r.instructions.size())),"Changed complete native ISA arithmetic");
        var code=method(aero,"runDelta",MethodTypeDesc.of(CD_void)).code().orElseThrow();var b=body(code);var ref=body(refs.methods().getFirst().code().orElseThrow());int count=ref.instructions.size()-1,found=-1;var expected=identity(ref,0,count);
        for(int n=0;n+count<=b.instructions.size();n++)if(identity(b,n,n+count).equals(expected)){require(found<0,"Ambiguous native air-data refresh");found=n;}
        require(found>=0,"Missing/reordered/changed native air-data refresh");
        for(var h:code.exceptionHandlers()){int start=b.labels.get(h.tryStart()),end=b.labels.get(h.tryEnd());require(end<=found||start>=found+count||environmentCleanup(b,h,l.aero,found,found+count),"Air-data window intersects unqualified native/composed handler");}
        for(int n=found;n<found+count;n++)if(b.instructions.get(n) instanceof FieldInstruction f){
            var model=f.owner().asSymbol().equals(l.aero)?aero:weather;require(f.owner().asSymbol().equals(l.aero)||f.owner().asSymbol().equals(l.weather),"Foreign native air-data field");
            var fields=model.fields().stream().filter(x->x.fieldName().equals(f.name())).toList();require(fields.size()==1,"Missing air-data storage");var field=fields.getFirst();int flags=field.flags().flagsMask();
            require(field.fieldTypeSymbol().equals(f.typeSymbol())&&(flags&ClassFile.ACC_STATIC)!=0&&((model==aero)||(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible air-data storage");
            if(f.opcode()==Opcode.PUTSTATIC)require(model==aero&&OUTPUTS.contains(f.name().stringValue())&&(flags&ClassFile.ACC_FINAL)==0,"Unqualified native air-data write");
        }
        return new Selection(l,found,found+count,b.offsets.get(found),b.offsets.get(found+count),OUTPUTS);
    }
    public static byte[] prepare(ClassFile cf,ClassModel aero,ClassModel weather,ClassModel originalCalc,ClassModel preparedCalc,Selection selection){
        require(select(aero,weather,originalCalc).equals(selection),"Air-data finalizer differs from qualification");
        require(preparedCalc.thisClass().asSymbol().equals(originalCalc.thisClass().asSymbol()),"Prepared arithmetic has wrong owner");
        var expected=cf.parse(IosAirDataPrepare.transform(cf,originalCalc));
        for(String name:List.of(IosAirDataPrepare.MACH,IosAirDataPrepare.IAS)){
            var a=body(method(expected,name,TWO).code().orElseThrow());var b=body(method(preparedCalc,name,TWO).code().orElseThrow());require(identity(a,0,a.instructions.size()).equals(identity(b,0,b.instructions.size())),"Prepared native pure accessor differs");
        }
        var isaExpected=body(method(originalCalc,"getIsaOat",ONE).code().orElseThrow());var isaActual=body(method(preparedCalc,"getIsaOat",ONE).code().orElseThrow());
        require(identity(isaExpected,0,isaExpected.instructions.size()).equals(identity(isaActual,0,isaActual.instructions.size())),"Prepared ISA arithmetic differs");
        var l=selection.layout;var source=body(method(aero,"runDelta",MethodTypeDesc.of(CD_void)).code().orElseThrow());
        return cf.transformClass(aero,ClassTransform.endHandler(builder->builder.withMethodBody(REFRESH,MethodTypeDesc.of(CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var failed=c.newLabel();var labels=new HashMap<Label,Label>();var slots=new HashMap<String,Integer>();for(int n=0;n<OUTPUTS.size();n++)slots.put(OUTPUTS.get(n),4+n);
            // Initialize locals so branch joins never expose an uninitialized output.
            for(String name:OUTPUTS){if(name.equals("movingBackwards"))c.iconst_0().istore(slots.get(name));else c.fconst_0().fstore(slots.get(name));}
            var inputs=new LinkedHashMap<String,FieldInstruction>();
            for(int n=selection.begin;n<selection.end;n++)if(source.instructions.get(n) instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&
                    !(f.owner().asSymbol().equals(l.aero)&&slots.containsKey(f.name().stringValue()))&&(f.typeSymbol().equals(CD_float)||f.typeSymbol().equals(CD_double)))
                inputs.putIfAbsent(f.owner().asInternalName()+":"+f.name(),f);
            for(var f:inputs.values()){c.getstatic(f.owner().asSymbol(),f.name().stringValue(),f.typeSymbol());
                if(f.typeSymbol().equals(CD_float))c.invokestatic(FLOAT,"isFinite",MethodTypeDesc.of(CD_boolean,CD_float));else c.invokestatic(DOUBLE,"isFinite",MethodTypeDesc.of(CD_boolean,CD_double));c.ifeq(failed);}
            c.getstatic(l.weather,"fHpa",CD_float).fconst_0().fcmpg().ifle(failed).getstatic(l.aero,"tasReal",CD_float).fconst_0().fcmpg().iflt(failed);
            for(var e:source.labels.entrySet())if(e.getValue()>=selection.begin&&e.getValue()<=selection.end)labels.put(e.getKey(),c.newLabel());
            var available=new HashSet<String>();
            for(int n=selection.begin;n<=selection.end;n++){
                for(var e:source.labels.entrySet())if(e.getValue()==n&&labels.containsKey(e.getKey()))c.labelBinding(labels.get(e.getKey()));
                if(n==selection.end)break;var instruction=source.instructions.get(n);
                if(instruction instanceof BranchInstruction br){require(labels.containsKey(br.target()),"Air-data branch escapes qualified window");c.branch(instruction.opcode(),labels.get(br.target()));}
                else if(instruction instanceof FieldInstruction f&&f.owner().asSymbol().equals(l.aero)&&slots.containsKey(f.name().stringValue())){
                    String name=f.name().stringValue();int slot=slots.get(name);boolean flag=name.equals("movingBackwards");
                    if(f.opcode()==Opcode.PUTSTATIC){if(flag)c.istore(slot);else c.fstore(slot);available.add(name);}
                    else {require(available.contains(name),"Native refresh reads stale output before calculation");if(flag)c.iload(slot);else c.fload(slot);}
                }else if(instruction instanceof InvokeInstruction call&&call.owner().asSymbol().equals(l.calc)&&(call.name().equalsString("getMach")||call.name().equalsString("getIas")))
                    c.invokestatic(l.calc,call.name().equalsString("getMach")?IosAirDataPrepare.MACH:IosAirDataPrepare.IAS,TWO);
                else c.with(instruction);
            }
            require(available.size()==OUTPUTS.size(),"Incomplete native air-data result");
            for(String name:OUTPUTS)if(!name.equals("movingBackwards"))c.fload(slots.get(name)).invokestatic(FLOAT,"isFinite",MethodTypeDesc.of(CD_boolean,CD_float)).ifeq(failed);
            c.fload(slots.get("oat")).ldc(-273.1f).fcmpg().ifle(failed).fload(slots.get("iasReal")).fconst_0().fcmpg().iflt(failed);
            for(String name:OUTPUTS){int slot=slots.get(name);if(name.equals("movingBackwards"))c.iload(slot).putstatic(l.aero,name,CD_boolean);else c.fload(slot).putstatic(l.aero,name,CD_float);}
            c.iconst_1().ireturn().labelBinding(failed).iconst_0().ireturn();
        })));
    }
    private IosAirDataFinalizerPrepare(){}
}
