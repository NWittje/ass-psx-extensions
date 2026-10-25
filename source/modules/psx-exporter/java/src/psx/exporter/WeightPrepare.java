package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Installed method/child identities and legal bridges. Inspection never loads PSX. */
final class WeightPrepare {
    static final ClassDesc HOOK=ClassDesc.of("psx.exporter.WeightHooks");
    static final ClassDesc THREAD=ClassDesc.of("java.lang.Thread");
    static final String PREFIX="$psxWeight$";
    static final Set<String> OWNERS=Set.of("Core","Fuel","FmcProgress2","InertialReference","EngineBasics","EngineBasics$ThreadQuickEngStart","InertialReference$ThreadAlign","Gear");
    record Qualified(int capabilities,Map<String,byte[]> overlay,List<String> evidence){}
    private WeightPrepare(){}
    static void expectCalls(Prepare data,String owner,String name,List<String> expected){
        MethodModel method=data.requireMethod(owner,name,"()V");
        List<String> calls=new ArrayList<>();
        for(Instruction i:Prepare.instructions(method))if(i instanceof InvokeInstruction call)calls.add(call.owner().asInternalName()+"."+call.name().stringValue()+call.type().stringValue());
        if(!calls.equals(expected))throw new IllegalArgumentException("Native service launcher changed "+owner+"."+name);
    }
    static void launcher(Prepare data,String owner,String name,String child){
        expectCalls(data,owner,name,List.of(child+".<init>()V","java/lang/Thread.<init>(Ljava/lang/Runnable;)V","java/lang/Thread.setPriority(I)V","java/lang/Thread.start()V"));
        data.requireMethod(child,"run","()V");
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower,String jar){
        if(!InstructorMetadata.INSTALLED_JAR.equalsIgnoreCase(jar))return new Qualified(0,Map.of(),List.of("Weight actions unavailable: installed identity"));
        try{
            launcher(data,"EngineBasics","quickEngStart","EngineBasics$ThreadQuickEngStart");
            launcher(data,"InertialReference","instructorAlignment","InertialReference$ThreadAlign");
            for(String name:List.of("instructorEvent","checkFuelEditQi","checkInstructorProcess","resetAllFuelUsed"))data.requireMethod("FmcProgress2",name,"()V");
            data.requireMethod("Gear","coolAllBrakes","()V");data.requireMethod("Fuel","distribute","(F)V");data.requireMethod("Fuel","resetAllLag","()V");
            data.requireMethod("StandbyAttitude","instructorAlignment","()V");data.requireMethod("StandbyIsfd","instructorAlignment","()V");
            if(data.models.get("FmcProgress2").fields().stream().noneMatch(f->f.fieldName().equalsString("instructorProcessTask")&&f.fieldType().equalsString("I")&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0))throw new IllegalArgumentException("Native fuel process field changed");
            Map<String,byte[]> output=new TreeMap<>(lower);
            for(String owner:OWNERS){
                ClassDesc caller=ClassDesc.of(owner);var model=data.cf.parse(output.getOrDefault(owner,data.inputs.get(owner)));
                byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
                    public void accept(ClassBuilder b,ClassElement element){
                        if(!(element instanceof MethodModel method)||method.code().isEmpty()){b.with(element);return;}
                        String name=method.methodName().stringValue();
                        boolean launch=owner.equals("EngineBasics")&&name.equals("quickEngStart")||owner.equals("InertialReference")&&name.equals("instructorAlignment");
                        boolean child=owner.equals("EngineBasics$ThreadQuickEngStart")&&name.equals("run")||owner.equals("InertialReference$ThreadAlign")&&name.equals("run");
                        boolean fuel=owner.equals("FmcProgress2")&&Set.of("instructorEvent","checkFuelEditQi","checkInstructorProcess").contains(name);
                        if(!launch&&!child&&!fuel){b.with(element);return;}
                        if(!method.methodType().equalsString("()V"))throw new IllegalArgumentException("Native service method descriptor drift");
                        int action=owner.startsWith("EngineBasics")?4:2;
                        b.transformMethod(method,(mb,me)->{
                            if(!(me instanceof CodeModel code)){mb.with(me);return;}
                            Set<Label> nativeHandlers=Collections.newSetFromMap(new IdentityHashMap<>());
                            if(child)for(CodeElement handlerElement:code)if(handlerElement instanceof ExceptionCatch caught)nativeHandlers.add(caught.handler());
                            mb.transformCode(code,new CodeTransform(){
                                Label begin,end,handler;
                                public void atStart(CodeBuilder cb){
                                    if(child)cb.invokestatic(caller,PREFIX+"childBegin",MethodTypeDesc.of(CD_void));
                                    begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);
                                }
                                public void accept(CodeBuilder cb,CodeElement ce){
                                    if(child&&ce instanceof LabelTarget label&&nativeHandlers.contains(label.label())){
                                        cb.with(ce).invokestatic(caller,PREFIX+"childFailure",MethodTypeDesc.of(CD_void));return;
                                    }
                                    if(launch&&ce instanceof InvokeInstruction invoke&&invoke.owner().asInternalName().equals("java/lang/Thread")&&invoke.name().equalsString("start")&&invoke.type().equalsString("()V"))cb.dup().invokestatic(caller,PREFIX+"child",MethodTypeDesc.of(CD_void,THREAD));
                                    if(ce instanceof ReturnInstruction){
                                        if(child)cb.iconst_1().invokestatic(caller,PREFIX+"childEnd",MethodTypeDesc.of(CD_void,CD_boolean));
                                        if(fuel)cb.invokestatic(caller,PREFIX+name,MethodTypeDesc.of(CD_void));
                                    }cb.with(ce);
                                }
                                public void atEnd(CodeBuilder cb){if(child){cb.labelBinding(end).labelBinding(handler).iconst_0().invokestatic(caller,PREFIX+"childEnd",MethodTypeDesc.of(CD_void,CD_boolean)).athrow();cb.exceptionCatchAll(begin,end,handler);}}
                            });
                        });
                    }
                    public void atEnd(ClassBuilder b){
                        if(owner.equals("EngineBasics")||owner.equals("InertialReference"))safe(b,"child",MethodTypeDesc.of(CD_void,THREAD),cb->cb.aload(0).loadConstant(owner.equals("EngineBasics")?4:2).invokestatic(HOOK,"child",MethodTypeDesc.of(CD_void,THREAD,CD_int)));
                        if(owner.endsWith("$ThreadQuickEngStart")||owner.endsWith("$ThreadAlign")){
                            int action=owner.endsWith("$ThreadQuickEngStart")?4:2;
                            safe(b,"childBegin",MethodTypeDesc.of(CD_void),cb->cb.loadConstant(action).invokestatic(HOOK,"childBegin",MethodTypeDesc.of(CD_void,CD_int)));
                            safe(b,"childEnd",MethodTypeDesc.of(CD_void,CD_boolean),cb->cb.loadConstant(action).iload(0).invokestatic(HOOK,"childEnd",MethodTypeDesc.of(CD_void,CD_int,CD_boolean)));
                            safe(b,"childFailure",MethodTypeDesc.of(CD_void),cb->cb.loadConstant(action).invokestatic(HOOK,"childFailure",MethodTypeDesc.of(CD_void,CD_int)));
                        }
                        if(owner.equals("FmcProgress2")){
                            safe(b,"instructorEvent",MethodTypeDesc.of(CD_void),cb->cb.invokestatic(HOOK,"fuelEvent",MethodTypeDesc.of(CD_void)));
                            safe(b,"checkFuelEditQi",MethodTypeDesc.of(CD_void),cb->cb.getstatic(caller,"instructorProcessTask",CD_int).invokestatic(HOOK,"fuelConsumed",MethodTypeDesc.of(CD_void,CD_int)));
                            safe(b,"checkInstructorProcess",MethodTypeDesc.of(CD_void),cb->cb.getstatic(caller,"instructorProcessTask",CD_int).invokestatic(HOOK,"fuelProcessed",MethodTypeDesc.of(CD_void,CD_int)));
                        }
                        if(owner.equals("Fuel")){bridge(b,caller,"distribute",MethodTypeDesc.of(CD_void,CD_float));bridge(b,caller,"resetAllLag",MethodTypeDesc.of(CD_void));}
                        if(owner.equals("Gear"))bridge(b,caller,"coolAllBrakes",MethodTypeDesc.of(CD_void));
                        if(owner.equals("FmcProgress2"))bridge(b,caller,"instructorEvent",MethodTypeDesc.of(CD_void));
                        if(owner.equals("EngineBasics"))bridge(b,caller,"quickEngStart",MethodTypeDesc.of(CD_void));
                        if(owner.equals("InertialReference"))bridge(b,caller,"instructorAlignment",MethodTypeDesc.of(CD_void));
                        if(owner.equals("Core"))b.withMethodBody(PREFIX+"call$alignStandby",MethodTypeDesc.of(CD_void),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->cb.getstatic(caller,"standbyAttitude",ClassDesc.of("StandbyAttitude")).invokevirtual(ClassDesc.of("StandbyAttitude"),"instructorAlignment",MethodTypeDesc.of(CD_void)).invokestatic(ClassDesc.of("StandbyIsfd"),"instructorAlignment",MethodTypeDesc.of(CD_void)).return_());
                    }
                });
                if(!data.cf.verify(bytes).isEmpty())throw new IllegalArgumentException("Weight source verification "+owner);output.put(owner,bytes);
            }
            var before=new HashMap<>(data.inputs);before.putAll(lower);TerrainAccess.verifyAdded(before,output,data.inputs);
            Map<String,byte[]> overlay=new TreeMap<>();for(String name:OWNERS)overlay.put(name,output.get(name));
            return new Qualified(15,Map.copyOf(overlay),List.of("Hash-qualified ordinary Fuel/IRS/Cool/Quickstart entrypoints; actual original outer Thread identity; post-body child and delayed fuel consumers; no recall activation."));
        }catch(Throwable failure){return new Qualified(0,Map.of(),List.of("Weight action qualification unavailable: "+failure));}
    }
    static void safe(ClassBuilder b,String name,MethodTypeDesc descriptor,java.util.function.Consumer<CodeBuilder> body){
        b.withMethodBody(PREFIX+name,descriptor,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            cb.trying(block->body.accept(block),catches->catches.catching(CD_Throwable,failure->{failure.pop();failure.trying(c->c.iconst_0().invokestatic(HOOK,"failed",MethodTypeDesc.of(CD_void,CD_int)),catches2->catches2.catching(CD_Throwable,c->c.pop()));}));cb.return_();
        });
    }
    static void bridge(ClassBuilder b,ClassDesc owner,String name,MethodTypeDesc descriptor){
        b.withMethodBody(PREFIX+"call$"+name,descriptor,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{if(descriptor.parameterCount()==1)cb.fload(0);cb.invokestatic(owner,name,descriptor).return_();});
    }
}
