package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Inline admission around narrow recurrent FMC-dependent calculations. Fuel
 * quantity/temperature parsing, instructor refresh and cabin equipment/control
 * outside the selected regions retain their complete native path. Branches,
 * native exception handlers and return values are never reconstructed. */
final class FmcBackgroundAdmissionRegionPrepare {
    private static final ClassDesc GATE=ClassDesc.of("psx.exporter.FmcExclusion");
    static final String FAULT="$psxExporter$fmcBackgroundRegionFailure";
    record Region(Prepare.MethodId method,int beginBci,int endBci,String field,int expectedReads,Set<Integer> prefixInputs,Set<Integer> outgoingExits,int expectedWrites,boolean instance){
        Region(Prepare.MethodId method,int beginBci,int endBci,String field,int expectedReads){this(method,beginBci,endBci,field,expectedReads,Set.of(),Set.of(),0,false);}
        Region(Prepare.MethodId method,int beginBci,int endBci,String field,int expectedReads,Set<Integer> prefixInputs,Set<Integer> outgoingExits){this(method,beginBci,endBci,field,expectedReads,prefixInputs,outgoingExits,0,false);}
        Region{prefixInputs=Set.copyOf(prefixInputs);outgoingExits=Set.copyOf(outgoingExits);}
    }
    static final List<Region> INSTALLED=List.of(
        new Region(new Prepare.MethodId("Fuel","readGlobQty","()V"),514,564,"minFuelTemp",1),
        new Region(new Prepare.MethodId("CabinPress","runLento","()V"),395,731,"crzAlt",5),
        new Region(new Prepare.MethodId("AfdsHub","runModerato","()V"),0,205,"RouteWpt.vertAngle",1),
        new Region(new Prepare.MethodId("FmcMaintenance","runLento","()V"),32,48,"Glob.qsIrsMonRef",1),
        new Region(new Prepare.MethodId("FmcGlobal","runLargo","()V"),0,270,"FmcRte$Rte.origLat",1),
        new Region(new Prepare.MethodId("Autothrottle","runAllegro","()V"),20,146,"FmcVnav.belowFlapAccel",2,Set.of(1),Set.of(3332)),
        new Region(new Prepare.MethodId("AfdsFcc","runAllegro","()V"),585,651,"FmcVnav.acftHeight",1,Set.of(0,4),Set.of()),
        new Region(new Prepare.MethodId("FmcAppRef","calcBuffets","()V"),70,186,"FmcVnav$Pred.clbSpdMode",1));
    private FmcBackgroundAdmissionRegionPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower)throws Exception{
        return transform(data,lower,INSTALLED);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Region> regions)throws Exception{
        var changed=new HashMap<String,byte[]>();var before=new HashMap<>(lower);var methods=new HashSet<Prepare.MethodId>();
        for(var region:regions){
            String owner=region.method.owner();require(methods.add(region.method),"one exact region per method");boolean first=!changed.containsKey(owner);
            byte[] input=changed.getOrDefault(owner,lower.getOrDefault(owner,data.inputs.get(owner)));before.putIfAbsent(owner,input);var model=data.cf.parse(input);
            require(!first||model.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT)),"fault collision");
            var method=model.methods().stream().filter(m->m.methodName().equalsString(region.method.name())&&m.methodType().equalsString(region.method.descriptor())).findFirst().orElseThrow();
            require(method.code().orElseThrow() instanceof CodeAttribute&&((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)==region.instance,"exact original region dispatch");
            var code=(CodeAttribute)method.code().orElseThrow();var ops=Prepare.instructions(method);var positions=new ArrayList<Integer>();int bci=0,reads=0,writes=0;
            for(var op:ops){positions.add(bci);bci+=op.sizeInBytes();}
            int begin=positions.indexOf(region.beginBci),end=positions.indexOf(region.endBci);require(begin>=0&&end>begin,"exact region instruction boundaries");
            var defined=new HashSet<Integer>();var prefixUsed=new HashSet<Integer>();var exitsUsed=new HashSet<Integer>();
            for(int slot:region.prefixInputs)require(definedBefore(code,ops,positions,begin,slot),"prefix local does not dominate region "+slot);
            for(int i=0;i<ops.size();i++){
                var op=ops.get(i);var targets=new ArrayList<Label>();
                if(op instanceof BranchInstruction branch)targets.add(branch.target());
                if(op instanceof LookupSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}
                if(op instanceof TableSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}
                for(var target:targets){int to=code.labelToBci(target);boolean inside=i>=begin&&i<end;if(inside&&region.outgoingExits.contains(to)){require(to>region.endBci&&positions.contains(to),"exact native region exit");exitsUsed.add(to);}else require(inside?to>=region.beginBci&&to<=region.endBci:to<=region.beginBci||to>=region.endBci,"partial/crossing region branch");}
                String fieldOwner=region.field.contains(".")?region.field.substring(0,region.field.indexOf('.')):"FmcPerf";
                String fieldName=region.field.contains(".")?region.field.substring(region.field.indexOf('.')+1):region.field;
                if(op instanceof FieldInstruction f&&f.owner().asInternalName().equals(fieldOwner)&&f.name().equalsString(fieldName)){
                    var declaration=data.models.get(fieldOwner).fields().stream().filter(v->v.fieldName().equalsString(fieldName)&&v.fieldType().equalsString(f.type().stringValue())).findFirst().orElseThrow();
                    boolean stat=(declaration.flags().flagsMask()&ClassFile.ACC_STATIC)!=0;
                    require(i>=begin&&i<end,"complete selected content region");if(f.opcode()==(stat?Opcode.GETSTATIC:Opcode.GETFIELD))reads++;else {require(f.opcode()==(stat?Opcode.PUTSTATIC:Opcode.PUTFIELD),"selected region field opcode");writes++;}
                }
                if(i>=begin&&i<end){
                    require(!(op instanceof ReturnInstruction)&&!(op instanceof InvokeDynamicInstruction)&&op.opcode()!=Opcode.ATHROW,"region terminating/dynamic effect");
                    if(op instanceof StoreInstruction s)defined.add(s.slot());
                    if(op instanceof LoadInstruction l&&!defined.contains(l.slot())){require(region.prefixInputs.contains(l.slot()),"region consumes prefix local");prefixUsed.add(l.slot());}
                    if(op instanceof IncrementInstruction n)require(defined.contains(n.slot()),"region increments prefix local");
                }
            }
            require(reads==region.expectedReads&&writes==region.expectedWrites,"selected reads/writes");
            require(prefixUsed.equals(region.prefixInputs)&&exitsUsed.equals(region.outgoingExits),"exact prefix/exit vector");
            for(int slot:defined)require(!usesBeforeDefinition(code,ops,positions,end,slot),"local continuation depends on deferred region");
            for(var handler:code.exceptionHandlers()){
                int start=code.labelToBci(handler.tryStart()),finish=code.labelToBci(handler.tryEnd()),target=code.labelToBci(handler.handler());
                boolean inside=start>=region.beginBci&&finish<=region.endBci&&target>=region.beginBci&&target<region.endBci;
                require(inside||((start<=region.beginBci&&finish>=region.endBci||finish<=region.beginBci||start>=region.endBci)&&!(target>=region.beginBci&&target<region.endBci)),"partial native exception span");
            }
            ClassDesc nativeOwner=ClassDesc.of(owner);byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                    if(!(element instanceof MethodModel m)||!m.methodName().equalsString(region.method.name())||!m.methodType().equalsString(region.method.descriptor())){builder.with(element);return;}
                    builder.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                        int ordinal,ticket,throwable;Label wholeBegin,wholeEnd,wholeHandler,regionEnd;List<ExceptionCatch> nativeHandlers;Set<Label> handlerEntries;
                        public void atStart(CodeBuilder b){
                            ticket=b.allocateLocal(TypeKind.INT);throwable=b.allocateLocal(TypeKind.REFERENCE);b.iconst_m1().istore(ticket);
                            wholeBegin=b.newLabel();wholeEnd=b.newLabel();wholeHandler=b.newLabel();regionEnd=b.newLabel();nativeHandlers=new ArrayList<>();handlerEntries=new HashSet<>();
                            for(var h:code.exceptionHandlers())handlerEntries.add(h.handler());b.labelBinding(wholeBegin);
                        }
                        public void accept(CodeBuilder b,CodeElement e){
                            if(e instanceof ExceptionCatch caught){nativeHandlers.add(caught);return;}
                            if(e instanceof LabelTarget label&&handlerEntries.contains(label.label())){
                                b.with(e);b.astore(throwable);release(b,nativeOwner,ticket);b.aload(throwable);return;
                            }
                            if(e instanceof Instruction){
                                if(ordinal==begin){
                                    guard(b,nativeOwner,()->b.invokestatic(GATE,"enter",MethodTypeDesc.of(CD_int)).istore(ticket));
                                    b.iload(ticket).ifeq(regionEnd);
                                }
                                if(ordinal==end){b.labelBinding(regionEnd);release(b,nativeOwner,ticket);}
                                if(e instanceof ReturnInstruction)release(b,nativeOwner,ticket);
                                ordinal++;
                            }
                            b.with(e);
                        }
                        public void atEnd(CodeBuilder b){
                            for(var handler:nativeHandlers)b.with(handler);
                            b.labelBinding(wholeEnd).exceptionCatchAll(wholeBegin,wholeEnd,wholeHandler).labelBinding(wholeHandler).astore(throwable);
                            release(b,nativeOwner,ticket);b.aload(throwable).athrow();
                        }
                    }));
                }
                public void atEnd(ClassBuilder builder){if(first)builder.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);}
            });
            var errors=data.cf.verify(output);require(errors.isEmpty(),"final verifier "+errors);changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static boolean definedBefore(CodeAttribute code,List<Instruction> ops,List<Integer> positions,int begin,int slot){
        var previous=new HashMap<Integer,Set<Integer>>();
        for(int at=0;at<ops.size();at++){
            var op=ops.get(at);var next=new HashSet<Integer>();int bci=positions.get(at);
            for(var h:code.exceptionHandlers())if(bci>=code.labelToBci(h.tryStart())&&bci<code.labelToBci(h.tryEnd()))next.add(positions.indexOf(code.labelToBci(h.handler())));
            if(!(op instanceof ReturnInstruction)&&op.opcode()!=Opcode.ATHROW){
                if(op instanceof BranchInstruction b){next.add(positions.indexOf(code.labelToBci(b.target())));if(op.opcode()!=Opcode.GOTO&&op.opcode()!=Opcode.GOTO_W)next.add(at+1);}
                else if(op instanceof LookupSwitchInstruction s){next.add(positions.indexOf(code.labelToBci(s.defaultTarget())));for(var c:s.cases())next.add(positions.indexOf(code.labelToBci(c.target())));}
                else if(op instanceof TableSwitchInstruction s){next.add(positions.indexOf(code.labelToBci(s.defaultTarget())));for(var c:s.cases())next.add(positions.indexOf(code.labelToBci(c.target())));}
                else next.add(at+1);
            }
            for(int to:next)if(to>=0&&to<ops.size())previous.computeIfAbsent(to,k->new HashSet<>()).add(at);
        }
        var pending=new ArrayDeque<Integer>();pending.add(begin);var seen=new HashSet<Integer>();boolean definition=false;
        while(!pending.isEmpty()){int at=pending.removeFirst();if(!seen.add(at))continue;if(at!=begin&&ops.get(at) instanceof StoreInstruction s&&s.slot()==slot){definition=true;continue;}if(at==0)return false;var predecessors=previous.getOrDefault(at,Set.of());if(predecessors.isEmpty())return false;pending.addAll(predecessors);}
        return definition;
    }
    /** Follow every suffix branch/handler until this local is overwritten.
     * A linear scan would wrongly accept a store bypassed by a native branch. */
    private static boolean usesBeforeDefinition(CodeAttribute code,List<Instruction> ops,List<Integer> positions,int start,int slot){
        var seen=new HashSet<Integer>();var pending=new ArrayDeque<Integer>();pending.add(start);
        while(!pending.isEmpty()){
            int at=pending.removeFirst();if(at<0||at>=ops.size()||!seen.add(at))continue;var op=ops.get(at);
            if(op instanceof LoadInstruction l&&l.slot()==slot||op instanceof IncrementInstruction n&&n.slot()==slot)return true;
            if(op instanceof StoreInstruction s&&s.slot()==slot)continue;
            int bci=positions.get(at);
            for(var h:code.exceptionHandlers())if(bci>=code.labelToBci(h.tryStart())&&bci<code.labelToBci(h.tryEnd()))pending.add(positions.indexOf(code.labelToBci(h.handler())));
            if(op instanceof ReturnInstruction||op.opcode()==Opcode.ATHROW)continue;
            if(op instanceof BranchInstruction branch){pending.add(positions.indexOf(code.labelToBci(branch.target())));if(op.opcode()==Opcode.GOTO||op.opcode()==Opcode.GOTO_W)continue;}
            if(op instanceof LookupSwitchInstruction sw){pending.add(positions.indexOf(code.labelToBci(sw.defaultTarget())));for(var c:sw.cases())pending.add(positions.indexOf(code.labelToBci(c.target())));continue;}
            if(op instanceof TableSwitchInstruction sw){pending.add(positions.indexOf(code.labelToBci(sw.defaultTarget())));for(var c:sw.cases())pending.add(positions.indexOf(code.labelToBci(c.target())));continue;}
            pending.add(at+1);
        }
        return false;
    }
    private static void release(CodeBuilder b,ClassDesc owner,int ticket){
        var done=b.newLabel();b.iload(ticket).ifle(done);guard(b,owner,()->b.iload(ticket).invokestatic(GATE,"leave",MethodTypeDesc.of(CD_void,CD_int)));b.iconst_m1().istore(ticket).labelBinding(done);
    }
    private static void guard(CodeBuilder b,ClassDesc owner,Runnable work){
        var start=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var done=b.newLabel();b.labelBinding(start);work.run();
        b.labelBinding(end).goto_(done).exceptionCatchAll(start,end,failure).labelBinding(failure).pop().iconst_1().putstatic(owner,FAULT,CD_boolean);
        var report=b.newLabel();var reported=b.newLabel();var absent=b.newLabel();b.labelBinding(report).invokestatic(GATE,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reported).goto_(done)
            .exceptionCatchAll(report,reported,absent).labelBinding(absent).pop().labelBinding(done);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC recurrent region "+why);}
}
