package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Read-only Vref metadata on the existing navigation worker, never the native frame. */
final class PerformanceMetadata {
    private PerformanceMetadata() {}
    /** Installed UP marker, not a new Vref calculation or a flap-zero landing speed. */
    static List<String> qualifyUp(Map<String,byte[]> inputs) throws Exception {
        Map<String,String> hashes=Map.of(
            "FmcAppRef","121af9d2696b0cad5b49079a35adea347c8fc117941a62208937e4aa1c8ea6a6",
            "PrimaryFlightDisplay","bd9bf034e3e84da1520eb41ee9d1ac005901f07d7954aca33ba152389f61f055");
        ClassFile cf=ClassFile.of();
        for(var e:hashes.entrySet())if(!e.getValue().equalsIgnoreCase(Identity.hash(inputs.getOrDefault(e.getKey(),new byte[0]))))
            throw new IllegalArgumentException("UP marker unsupported owner "+e.getKey());
        FieldModel marker=field(cf.parse(inputs.get("FmcAppRef")),"vRef00","I",true);
        if((marker.flags().flagsMask()&ClassFile.ACC_FINAL)!=0)throw new IllegalArgumentException("UP marker mutability");
        List<String> writers=new ArrayList<>();
        for(var e:inputs.entrySet())for(var m:cf.parse(e.getValue()).methods())if(m.code().isPresent())
            for(var op:Prepare.instructions(m))if(op instanceof FieldInstruction f&&f.owner().asInternalName().equals("FmcAppRef")&&f.name().equalsString("vRef00")) {
                if(!f.type().equalsString("I")||(f.opcode()!=Opcode.GETSTATIC&&f.opcode()!=Opcode.PUTSTATIC))throw new IllegalArgumentException("UP marker access");
                if(f.opcode()==Opcode.PUTSTATIC)writers.add(e.getKey()+"."+m.methodName()+m.methodType());
            }
        Collections.sort(writers);
        if(!writers.equals(List.of("FmcAppRef.runLento()V","FmcAppRef.updateVref()V","FmcAppRef.updateVref()V","FmcAppRef.updateVref()V")))
            throw new IllegalArgumentException("UP marker writer coverage");
        return List.of("UP marker vRef00: native integer knots used by PrimaryFlightDisplay.yVref00; four qualified writes, no calculation invocation");
    }
    static FieldModel field(ClassModel m,String name,String type,boolean isStatic) {
        FieldModel f=m.fields().stream().filter(v->v.fieldName().equalsString(name)&&v.fieldType().equalsString(type)).findFirst().orElseThrow();
        if(((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)!=isStatic)throw new IllegalArgumentException("performance field staticness");
        return f;
    }
    static MethodModel method(ClassModel m,String name,String descriptor) {
        return m.methods().stream().filter(v->v.methodName().equalsString(name)&&v.methodType().equalsString(descriptor)).findFirst().orElseThrow();
    }
    static boolean member(Instruction i,Opcode opcode,String owner,String name,String descriptor) {
        return i instanceof FieldInstruction f&&f.opcode()==opcode&&f.owner().asInternalName().equals(owner)&&f.name().equalsString(name)&&f.type().equalsString(descriptor);
    }
    static List<String> qualify(Map<String,byte[]> inputs) throws Exception {
        ClassFile cf=ClassFile.of();Map<String,ClassModel> models=new TreeMap<>();
        for(String name:List.of("FmcAppRef","FmcAppRef$V","Aerodynamics","CabinPress","Core")) {
            byte[] bytes=inputs.get(name);if(bytes==null)throw new IllegalArgumentException("performance owner "+name);
            ClassModel m=cf.parse(bytes);if(!m.thisClass().asInternalName().equals(name))throw new IllegalArgumentException("performance class name");models.put(name,m);
        }
        field(models.get("Aerodynamics"),"grWeightReal","F",true);field(models.get("CabinPress"),"ldgAltDisp","I",true);field(models.get("Core"),"loaderPaused","Z",true);
        field(models.get("FmcAppRef$V"),"ref25","I",false);field(models.get("FmcAppRef$V"),"ref30","I",false);
        if(models.get("FmcAppRef$V").methods().stream().anyMatch(v->v.methodName().equalsString("<clinit>")))throw new IllegalArgumentException("performance result initializer");
        var ctor=Prepare.instructions(method(models.get("FmcAppRef$V"),"<init>","()V"));
        if(ctor.size()!=3||ctor.get(0).opcode()!=Opcode.ALOAD_0||!(ctor.get(1) instanceof InvokeInstruction c)||c.opcode()!=Opcode.INVOKESPECIAL||!c.owner().asInternalName().equals("java/lang/Object")||!c.name().equalsString("<init>")||!c.type().equalsString("()V")||ctor.get(2).opcode()!=Opcode.RETURN)throw new IllegalArgumentException("performance result constructor");
        MethodModel getter=method(models.get("FmcAppRef"),"getVref","(II)LFmcAppRef$V;");
        if((getter.flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("performance getter staticness");
        List<Instruction> ops=Prepare.instructions(getter);Map<Label,Integer> labels=new HashMap<>();int cursor=0;
        for(CodeElement e:getter.code().orElseThrow()){if(e instanceof LabelTarget l)labels.put(l.label(),cursor);if(e instanceof Instruction)cursor++;if(e instanceof ExceptionCatch)throw new IllegalArgumentException("performance getter handler");}
        if(ops.size()<20||ops.size()>512||!(ops.get(0) instanceof NewObjectInstruction n)||!n.className().asInternalName().equals("FmcAppRef$V")||ops.get(1).opcode()!=Opcode.DUP||!(ops.get(2) instanceof InvokeInstruction init)||init.opcode()!=Opcode.INVOKESPECIAL||!init.owner().asInternalName().equals("FmcAppRef$V")||!init.name().equalsString("<init>")||!init.type().equalsString("()V")||ops.get(3).opcode()!=Opcode.ASTORE_2||ops.get(4).opcode()!=Opcode.ILOAD_0||!(ops.get(5) instanceof ConstantInstruction kg)||!Integer.valueOf(1000).equals(kg.constantValue())||ops.get(6).opcode()!=Opcode.IDIV||ops.get(7).opcode()!=Opcode.ISTORE_0||ops.get(ops.size()-2).opcode()!=Opcode.ALOAD_2||ops.getLast().opcode()!=Opcode.ARETURN)throw new IllegalArgumentException("performance weight/result pattern");
        int writes25=0,writes30=0;boolean altitude=false;
        Set<Opcode> numeric=Set.of(Opcode.DUP,Opcode.ILOAD_0,Opcode.ILOAD_1,Opcode.ISTORE_0,Opcode.ISTORE_1,Opcode.ALOAD_2,Opcode.IADD,Opcode.ISUB,Opcode.IMUL,Opcode.IDIV,Opcode.I2F,Opcode.FMUL,Opcode.FADD,Opcode.FSUB,Opcode.FDIV);
        for(int i=4;i<ops.size()-1;i++){
            Instruction op=ops.get(i);
            if(op instanceof ConstantInstruction constant){Object value=constant.constantValue();if(!(value instanceof Integer||value instanceof Float))throw new IllegalArgumentException("performance constant");if(Integer.valueOf(4000).equals(value)&&i>0&&ops.get(i-1).opcode()==Opcode.ILOAD_1&&i+2<ops.size()&&ops.get(i+1).opcode()==Opcode.IDIV&&ops.get(i+2).opcode()==Opcode.ISTORE_1)altitude=true;continue;}
            if(op instanceof BranchInstruction branch){Integer to=labels.get(branch.target());if(to==null||to<=i||to>=ops.size()||(op.opcode()!=Opcode.GOTO&&op.opcode()!=Opcode.IF_ICMPGE))throw new IllegalArgumentException("performance unbounded branch");continue;}
            if(op instanceof InvokeInstruction call){if(call.opcode()!=Opcode.INVOKESTATIC||!call.owner().asInternalName().equals("java/lang/Math")||!call.name().equalsString("round")||!call.type().equalsString("(F)I"))throw new IllegalArgumentException("performance impure invocation");continue;}
            if(op instanceof FieldInstruction f){if(!f.owner().asInternalName().equals("FmcAppRef$V")||!f.type().equalsString("I")||(!f.name().equalsString("ref25")&&!f.name().equalsString("ref30"))||(f.opcode()!=Opcode.GETFIELD&&f.opcode()!=Opcode.PUTFIELD))throw new IllegalArgumentException("performance impure field");if(f.opcode()==Opcode.PUTFIELD){if(f.name().equalsString("ref25"))writes25++;else writes30++;}continue;}
            if(!numeric.contains(op.opcode()))throw new IllegalArgumentException("performance unsupported opcode "+op.opcode());
        }
        if(!altitude||writes25<2||writes30<2)throw new IllegalArgumentException("performance altitude/outputs");
        // Independent native unit evidence: direct kg setter and the real gross-weight sum.
        var weight=Prepare.instructions(method(models.get("Aerodynamics"),"setFuelWeightKgs","(F)V"));
        if(weight.size()!=3||weight.getFirst().opcode()!=Opcode.FLOAD_0||!member(weight.get(1),Opcode.PUTSTATIC,"Aerodynamics","fuelWeightReal","F")||weight.getLast().opcode()!=Opcode.RETURN)throw new IllegalArgumentException("performance kg setter");
        var dynamics=Prepare.instructions(method(models.get("Aerodynamics"),"runDelta","()V"));boolean sum=false;
        for(int i=3;i<dynamics.size();i++)if(member(dynamics.get(i),Opcode.PUTSTATIC,"Aerodynamics","grWeightReal","F")&&dynamics.get(i-1).opcode()==Opcode.FADD&&member(dynamics.get(i-2),Opcode.GETSTATIC,"Aerodynamics","fuelWeightReal","F")&&member(dynamics.get(i-3),Opcode.GETSTATIC,"Aerodynamics","zeroFuelWeightReal","F"))sum=true;
        if(!sum)throw new IllegalArgumentException("performance actual gross sum");
        var landing=Prepare.instructions(method(models.get("CabinPress"),"setLdgAlt","(I)V"));
        if(landing.stream().noneMatch(i->member(i,Opcode.PUTSTATIC,"CabinPress","ldgAltDisp","I")))throw new IllegalArgumentException("performance landing altitude writer");
        List<String> evidence=new ArrayList<>();for(var e:models.entrySet())evidence.add("class "+e.getKey()+" sha256="+Identity.hash(inputs.get(e.getKey())));
        for(var e:inputs.entrySet())for(var m:cf.parse(e.getValue()).methods())if(m.code().isPresent())for(var op:Prepare.instructions(m))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)&&Set.of("Aerodynamics.grWeightReal","CabinPress.ldgAltDisp").contains(f.owner().asInternalName()+"."+f.name().stringValue()))evidence.add("writer "+e.getKey()+"."+m.methodName()+m.methodType()+" "+f.name());
        evidence.add("getVref pure bounded fresh-object calculation: kg/1000, native landing-altitude/4000; no updateVref/manual FMC weight");return List.copyOf(evidence);
    }
    static final class Runner {
        final ClassLoader loader;final Path input,target;final long pid,birth;
        Field weight,landing,loading,ref25,ref30,up;Method getter;boolean resolved;String resolutionError,upResolutionError;long sequence;
        Runner(ClassLoader loader,Path input,Path directory,long pid,long birth){this.loader=loader;this.input=input;this.pid=pid;this.birth=birth;target=directory.resolve(pid+".json");}
        void resolve() throws Exception {
            Map<String,byte[]> inputs=Prepare.readJar(input);
            try { qualifyUp(inputs);up=accessible(owner("FmcAppRef"),"vRef00",int.class,true); }
            catch(Throwable unavailable){up=null;upResolutionError="UP marker qualification unavailable: "+unavailable.getClass().getSimpleName();}
            qualify(inputs);
            Class<?> aero=owner("Aerodynamics"),cabin=owner("CabinPress"),core=owner("Core"),fmc=owner("FmcAppRef"),result=owner("FmcAppRef$V");
            weight=accessible(aero,"grWeightReal",float.class,true);landing=accessible(cabin,"ldgAltDisp",int.class,true);loading=accessible(core,"loaderPaused",boolean.class,true);ref25=accessible(result,"ref25",int.class,false);ref30=accessible(result,"ref30",int.class,false);
            getter=fmc.getDeclaredMethod("getVref",int.class,int.class);if(!Modifier.isStatic(getter.getModifiers())||getter.getReturnType()!=result||!getter.trySetAccessible())throw new IllegalArgumentException("performance getter inaccessible");
        }
        Class<?> owner(String name)throws Exception {Class<?> c=Class.forName(name,false,loader);if(!Agent.sameInputSource(c,input))throw new IllegalArgumentException("performance source "+name);return c;}
        static Field accessible(Class<?> owner,String name,Class<?> type,boolean stat)throws Exception {Field f=owner.getDeclaredField(name);if(f.getType()!=type||Modifier.isStatic(f.getModifiers())!=stat||!f.trySetAccessible())throw new IllegalArgumentException("performance field inaccessible");return f;}
        void sample(boolean ready) {
            long sampled=System.currentTimeMillis();
            Float gross=null;Integer calculation=null,altitude=null,v25=null,v30=null,marker=null;String error="PSX initialization pending",upError=error;
            if(ready){if(!resolved){resolved=true;try{resolve();}catch(Throwable failure){resolutionError="Performance qualification unavailable: "+failure.getClass().getSimpleName();}}error=resolutionError;
                if(error==null)try{
                    boolean accepted=false;
                    for(int attempt=0;attempt<3&&!accepted;attempt++){
                        long epoch=Observer.epochForTest();if(loading.getBoolean(null))throw new IllegalStateException("PSX loading");
                        float w=weight.getFloat(null);int a=landing.getInt(null);
                        Integer before=null;upError=upResolutionError;
                        if(up!=null)try{before=up.getInt(null);}catch(Throwable unavailable){upError="UP marker read unavailable";}
                        if(!Float.isFinite(w)||w<=0||w>=Integer.MAX_VALUE||a< -2000||a>60000)throw new IllegalArgumentException("performance input range");
                        int kg=Math.round(w);Object result=getter.invoke(null,kg,a);int p=ref25.getInt(result),q=ref30.getInt(result);
                        if(p<=0||p>400||q<=0||q>400)throw new IllegalArgumentException("performance output range");
                        if(epoch==Observer.epochForTest()&&!loading.getBoolean(null)&&Float.floatToRawIntBits(w)==Float.floatToRawIntBits(weight.getFloat(null))&&a==landing.getInt(null)){
                            gross=w;calculation=kg;altitude=a;v25=p;v30=q;accepted=true;
                            if(before!=null)try {
                                if(before!=up.getInt(null))upError="UP marker changed during observation";
                                else if(before<=0||before>400)upError="UP marker is not valid";
                                else {marker=before;upError="";}
                            }catch(Throwable unavailable){upError="UP marker read unavailable";}
                            if(epoch!=Observer.epochForTest()||loading.getBoolean(null)){marker=null;upError="PSX lifecycle changed during UP observation";}
                        }
                    }
                    if(!accepted)throw new IllegalStateException("performance inputs changed");error="";
                }catch(Throwable failure){gross=null;calculation=altitude=v25=v30=marker=null;error="Performance read unavailable: "+failure.getClass().getSimpleName();upError=error;}
            }
            if(error!=null&&!error.isEmpty()){marker=null;upError=error;}
            try{publish(sampled,gross,calculation,altitude,v25,v30,marker,upError,error);}catch(Throwable failure){try{Files.deleteIfExists(target);}catch(Throwable ignored){}}
        }
        void publish(long sampled,Float gross,Integer calculation,Integer altitude,Integer v25,Integer v30,Integer marker,String upError,String error)throws Exception {
            if(sequence==Long.MAX_VALUE)throw new IllegalStateException("performance sequence exhausted");
            byte[] json=("{\"schema\":2,\"pid\":"+pid+",\"process_start_unix_ms\":"+birth+",\"sequence\":"+(++sequence)+",\"sampled_unix_ms\":"+sampled+",\"gross_weight_kg\":"+gross+",\"calculation_weight_kg\":"+calculation+",\"landing_altitude_ft\":"+altitude+",\"vref25_kt\":"+v25+",\"vref30_kt\":"+v30+",\"up_maneuver_kt\":"+marker+",\"up_error\":"+Identity.json(upError)+",\"error\":"+Identity.json(error)+"}").getBytes(StandardCharsets.UTF_8);
            if(json.length>4096)throw new IllegalArgumentException("performance metadata bound");Files.createDirectories(target.getParent());Path stage=Files.createTempFile(target.getParent(),pid+"-",".tmp");
            try{Files.write(stage,json);for(int attempt=0;;attempt++)try{Files.move(stage,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);break;}catch(AccessDeniedException busy){if(attempt==4)throw busy;Thread.sleep(10);}}finally{Files.deleteIfExists(stage);}
        }
    }
}
