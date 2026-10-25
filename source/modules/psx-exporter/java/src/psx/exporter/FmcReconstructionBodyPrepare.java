package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Exact native route serialization and the arithmetic prefix of readGlob.
 * The prefix ends before selected-header clearing, navigation-radio tuning,
 * progress/history, performance input parsing and CDU actions. Its normal
 * flow rejoins only the native destination-cache section. Forecast load/blank
 * calls are removed; current forecast objects and inputs survive in place.
 * Installed classes are byte data during qualification. */
final class FmcReconstructionBodyPrepare {
    static final String READ="$psxExporter$fmcReconstructBody",SEND="$psxExporter$fmcSerializeBody",ND="$psxExporter$fmcRefreshBodyNdData";
    static final String SAVE="$psxExporter$fmcSaveCurrentForecast";
    static final String BANK="$psxExporter$fmcBodyBank";
    static final String FORECAST="$psxExporter$fmcCopyCurrentForecast";
    static final String ALIASES="$psxExporter$fmcBodyAliases",SAFETY="$psxExporter$fmcSafetyBody";
    static final String WIND_ALIASES="$psxExporter$fmcCurrentForecastAliases",WPT_FORECAST="$psxExporter$fmcWaypointForecast";
    static final String OPTIONS="$psxExporter$fmcCurrentParserOptions";
    static final int CORE_END=12814,DEST_START=14608,DEST_END=14842;
    private static Prepare.MethodId id(String owner,String name,String type){return new Prepare.MethodId(owner,name,type);}
    private static Prepare.MethodId id(InvokeInstruction call){return id(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());}
    private static final Prepare.MethodId READ_NATIVE=id("Routes$RteBody","readGlob","(Ljava/lang/String;)V"),SEND_NATIVE=id("Routes$RteBody","sendGlob","(I)V");
    private static final Map<Prepare.MethodId,Integer> OMIT=Map.ofEntries(
        Map.entry(id("FmcWindOat","loadWptWindsOat","(Ljava/lang/String;)V"),2),Map.entry(id("FmcWindOat","blank","()V"),1),
        Map.entry(id("FmcGlobal","addHiMsg","(I)V"),1),Map.entry(id("FmcGlobal$MemBits","trans","(ZZI)V"),4),
        Map.entry(id("Routes","checkCduOffsetLight","()V"),0),
        Map.entry(id("Routes","checkStepperL","()V"),0),Map.entry(id("Routes","checkStepperR","()V"),0),Map.entry(id("Routes","sendGlobStep","(I)V"),1));
    private static final Set<String> OWNERS=Set.of("Routes$RteBody","Routes$Via","RouteWpt","FmcWindOat","FmcWindOat$WindOatAlt","Calc$PlaceBearingDistance","Calc$PlaceBearingPlaceBearing","Expression$DegMinTenth");
    record Qualified(Map<String,byte[]> changed,Set<FmcQualification.Member> writes,Set<FmcQualification.Member> inputs,Set<Prepare.MethodId> methods){}
    static Set<Prepare.MethodId> roots(){return Set.of(id("Routes$RteBody",READ,"(Ljava/lang/String;)V"),id("Routes$RteBody",SEND,"(I)V"),id("Routes$RteBody",FORECAST,"(Ljava/lang/Object;Ljava/lang/Object;)V"),id("Routes$RteBody",ND,"(Ljava/lang/Object;)V"));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction body "+why);}
    private static MethodModel method(Map<String,ClassModel> models,Prepare.MethodId id){var m=models.get(id.owner());require(m!=null,"owner "+id);
        return m.methods().stream().filter(x->x.methodName().equalsString(id.name())&&x.methodType().equalsString(id.descriptor())).findFirst().orElseThrow(()->new IllegalArgumentException("FMC reconstruction body method "+id));}
    private static boolean kept(int bci){return bci<CORE_END||bci>=DEST_START&&bci<DEST_END;}
    /** Formatting scratch is allocated per invocation. Geometry scratch is
     * either an existing owned body helper or an unpublished local allocation;
     * the common class name does not imply ownership of another user's object. */
    private static void qualifyScratch(Map<String,ClassModel> models,Set<Prepare.MethodId> reached){
        var scratch=Set.of("Calc$PlaceBearingDistance","Calc$PlaceBearingPlaceBearing","Expression$DegMinTenth");
        for(String owner:List.of("Calc","Expression"))for(var field:models.get(owner).fields())
            require(!scratch.stream().anyMatch(type->field.fieldType().stringValue().contains("L"+type+";")),"shared scratch declaration "+owner+"."+field.fieldName());
        for(var methodId:reached){var instructions=Prepare.instructions(method(models,methodId));
            for(var instruction:instructions)if(instruction instanceof FieldInstruction field&&scratch.stream().anyMatch(type->field.type().stringValue().contains("L"+type+";")))
                require(instruction.opcode()==Opcode.GETFIELD&&field.owner().asInternalName().equals("Routes$RteBody"),"scratch reference publication/shared read in "+methodId);
            boolean formatter=instructions.stream().anyMatch(i->i instanceof InvokeInstruction call&&call.owner().asInternalName().equals("Expression$DegMinTenth"));
            if(formatter){require(methodId.owner().equals("Expression"),"foreign formatter scratch receiver");
                require(instructions.stream().filter(i->i instanceof NewObjectInstruction n&&n.className().asInternalName().equals("Expression$DegMinTenth")).count()==1,"one private formatter scratch allocation "+methodId);
                require(instructions.stream().noneMatch(i->i.opcode()==Opcode.AASTORE||i.opcode()==Opcode.PUTFIELD||i.opcode()==Opcode.PUTSTATIC||i.opcode()==Opcode.ARETURN&&methodId.descriptor().endsWith("LExpression$DegMinTenth;")),"formatter scratch escapes "+methodId);
            }
        }
    }
    private static final FmcQualification.Member MEMORY_RECEIVER=new FmcQualification.Member("FmcGlobal","mem1","LFmcGlobal$MemBits;");
    /** This receiver belongs solely to the omitted native sender notification.
     * No memory mask is read by the clone: its four operands are discarded.
     * Keep every genuine current input; fail qualification if this exact
     * receiver/argument use changes or another retained use appears. */
    private static void qualifyDiscardedMemoryReceiver(Prepare.MethodId method,List<Instruction> code,Instruction receiver){
        int at=code.indexOf(receiver);require(method.equals(SEND_NATIVE)&&at>=0&&at+4<code.size(),"discarded memory receiver source");
        for(int n=1;n<=3;n++)require(code.get(at+n) instanceof ConstantInstruction c&&c.constantValue() instanceof Integer v&&v==(n==3?16:1),"discarded memory sender arguments");
        require(code.get(at+4) instanceof InvokeInstruction call&&id(call).equals(id("FmcGlobal$MemBits","trans","(ZZI)V"))&&OMIT.get(id(call))==4,"discarded memory receiver only omitted sender notification");
    }
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var parser=method(data.models,READ_NATIVE);var attribute=(CodeAttribute)parser.code().orElseThrow();
        int bci=0;var retained=new ArrayList<Instruction>();var boundaries=new HashSet<Integer>();
        for(var instruction:Prepare.instructions(parser)){
            boundaries.add(bci);if(kept(bci)){
                if(instruction instanceof BranchInstruction branch)require(kept(attribute.labelToBci(branch.target()))||attribute.labelToBci(branch.target())==CORE_END||attribute.labelToBci(branch.target())==DEST_END,"retained branch crosses foreign section at "+bci);
                retained.add(instruction);
            }bci+=instruction.sizeInBytes();
        }
        require(boundaries.containsAll(Set.of(CORE_END,DEST_START,DEST_END)),"exact native arithmetic/destination boundaries");
        require(retained.stream().anyMatch(i->i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.name().equalsString("destRwyIdx")),"native geometry cache completion");
        var pending=new ArrayDeque<Prepare.MethodId>();var roots=Map.of(READ_NATIVE,READ,SEND_NATIVE,SEND);pending.addAll(roots.keySet());
        var seen=new HashSet<Prepare.MethodId>();var writes=new HashSet<FmcQualification.Member>();var inputs=new HashSet<FmcQualification.Member>();int discardedMemoryReceivers=0;
        while(!pending.isEmpty()){
            var next=pending.removeFirst();if(!seen.add(next))continue;require(seen.size()<=200,"finite native arithmetic/format closure");var m=method(models,next);require(m.code().isPresent(),"opaque callee "+next);
            var instructions=next.equals(READ_NATIVE)?retained:Prepare.instructions(m);
            for(var i:instructions){
                require(!(i instanceof InvokeDynamicInstruction),"opaque formatter call "+next);
                if(i instanceof FieldInstruction f){
                    if(f.owner().asInternalName().startsWith("java/")){require(f.opcode()==Opcode.GETSTATIC||f.opcode()==Opcode.GETFIELD,"foreign JDK store");continue;}
                    var field=FmcQualification.resolveField(models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());require(field!=null,"field binding "+f.owner()+"."+f.name());
                    if(i.opcode()==Opcode.PUTFIELD||i.opcode()==Opcode.PUTSTATIC){require(i.opcode()==Opcode.PUTFIELD&&(OWNERS.contains(field.owner())||field.equals(new FmcQualification.Member("FmcRte$Rte","ndDestOn","Z")))||i.opcode()==Opcode.PUTSTATIC&&field.equals(new FmcQualification.Member("Routes","actWptEtaUpdCode","I")),"foreign write "+field+" in "+next);writes.add(field);}
                    if(i.opcode()==Opcode.GETSTATIC||i.opcode()==Opcode.GETFIELD){if(field.equals(MEMORY_RECEIVER)){qualifyDiscardedMemoryReceiver(next,instructions,i);discardedMemoryReceivers++;}else inputs.add(field);}
                }
                if(!(i instanceof InvokeInstruction call))continue;var target=id(call);
                if(OMIT.containsKey(target)){require(roots.containsKey(next),"indirect removed effect "+target);continue;}
                if(target.equals(id("java/lang/Exception","printStackTrace","()V"))){require(roots.containsKey(next),"indirect swallowed error");continue;}
                if(target.equals(id("java/io/PrintStream","println","(Ljava/lang/String;)V"))){require(next.equals(id("FmcWindOat","saveAllAltToFile","(Ljava/lang/StringBuilder;)V")),"unqualified native debug output");continue;}
                if(target.equals(id("java/lang/ArrayIndexOutOfBoundsException","printStackTrace","()V"))){require(next.equals(id("Calc","getMagvarPoint","(II)S")),"unqualified native magnetic-grid bounds fallback");continue;}
                if(target.equals(id("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"))){require(next.equals(SEND_NATIVE),"foreign bank effect");writes.addAll(FmcBankWriterPrepare.FIELDS);continue;}
                require(!target.equals(id("Qi","transfer","(ICI)I")),"native Qi transfer frontier in "+next);
                if(target.equals(id("RouteWpt","blank","()V"))){pending.add(id("RouteWpt",FmcReconstructionEffects.CLONES.stream().filter(x->x.original().owner().equals("RouteWpt")).findFirst().orElseThrow().generated(),"()V"));continue;}
                if(target.owner().startsWith("java/")){require(Set.of("java/lang/String","java/lang/StringBuilder","java/lang/Math","java/lang/Float","java/lang/Integer","java/lang/Character","java/lang/Object").contains(target.owner()),"external native arithmetic/format effect "+target+" in "+next);continue;}
                pending.add(target);
            }
        }
        require(discardedMemoryReceivers==1&&!inputs.contains(MEMORY_RECEIVER),"exact sole omitted memory sender receiver");
        qualifyScratch(models,seen);
        require(seen.contains(id("Routes$RteBody","makeNdDataStrings","(I)V")),"ND refresh already in exact qualified body closure");
        var ndData=Prepare.instructions(method(data.models,id("Routes$RteBody","makeNdDataStrings","(I)V")));var ndFields=new HashSet<FmcQualification.Member>();for(var op:ndData)if(op instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC)ndFields.add(new FmcQualification.Member(f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue()));
        require(ndFields.containsAll(Set.of(new FmcQualification.Member("FmcVnav","transAlt","I"),new FmcQualification.Member("FmcVnav","transLvl","I"),new FmcQualification.Member("FmcVnav","phase","I"))),"current ND transition/phase input footprint");
        var ndOutputs=new HashSet<FmcQualification.Member>();for(var op:ndData)if(op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD)ndOutputs.add(new FmcQualification.Member(f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue()));
        require(ndOutputs.equals(Set.of(new FmcQualification.Member("RouteWpt","altConstraintDisplay","Ljava/lang/String;"),new FmcQualification.Member("RouteWpt","altConstrNdX","F"),new FmcQualification.Member("RouteWpt","etaDisplay","Ljava/lang/String;")))&&writes.containsAll(ndOutputs),"complete already-owned native ND cache outputs");
        var producerPending=new ArrayDeque<Prepare.MethodId>();var producerSeen=new HashSet<Prepare.MethodId>();producerPending.add(SEND_NATIVE);while(!producerPending.isEmpty()){var next=producerPending.removeFirst();if(!producerSeen.add(next))continue;require(producerSeen.size()<=200,"finite native bank cache-read proof");for(var op:Prepare.instructions(method(data.models,next))){if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC))require(!ndOutputs.contains(new FmcQualification.Member(f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue()))&&!Prepare.field(op,Opcode.GETFIELD,"Routes$RteBody","sbAcd"),"native bank producer must not serialize derived ND caches");if(op instanceof InvokeInstruction call&&data.models.containsKey(call.owner().asInternalName())&&!OMIT.containsKey(id(call))&&!id(call).equals(id("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")))producerPending.add(id(call));}}
        byte[] original=lower.getOrDefault("Routes$RteBody",data.inputs.get("Routes$RteBody"));var model=data.cf.parse(original);
        require(model.methods().stream().noneMatch(m->Set.of(READ,SEND,ND).contains(m.methodName().stringValue())),"entry collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(ND,MethodTypeDesc.of(CD_void,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var body=ClassDesc.of("Routes$RteBody");var bad=c.newLabel();var loop=c.newLabel();var done=c.newLabel();c.aload(0).checkcast(body).getfield(body,"legsAmo",CD_int).istore(1).iload(1).iconst_2().if_icmplt(bad).iload(1).loadConstant(253).if_icmpgt(bad).aload(0).checkcast(body).getfield(body,"leg",ClassDesc.of("RouteWpt").arrayType()).arraylength().loadConstant(253).if_icmpne(bad).iconst_2().istore(2).labelBinding(loop).iload(2).iload(1).if_icmpge(done).aload(0).checkcast(body).iload(2).invokevirtual(body,"makeNdDataStrings",MethodTypeDesc.of(CD_void,CD_int)).iinc(2,1).goto_(loop).labelBinding(done).return_().labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC ND refresh native NEW body span changed").invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();});
                b.withMethodBody(OPTIONS,MethodTypeDesc.of(CD_int.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.iconst_2().newarray(TypeKind.INT).astore(0);c.aload(0).iconst_0().getstatic(ClassDesc.of("Option"),"ngFmc",CD_boolean).iastore();c.aload(0).iconst_1().getstatic(ClassDesc.of("Core"),"situLoadedTime",CD_int).iastore();c.aload(0).areturn();});
                var aliasFields=data.models.get("Routes$RteBody").fields().stream().filter(field->(field.flags().flagsMask()&ClassFile.ACC_STATIC)==0&&(field.fieldType().stringValue().startsWith("L")||field.fieldType().stringValue().startsWith("["))&&!field.fieldType().equalsString("Ljava/lang/String;")).toList();
                b.withMethodBody(ALIASES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.loadConstant(aliasFields.size()).anewarray(CD_Object).astore(1);for(int i=0;i<aliasFields.size();i++){var field=aliasFields.get(i);c.aload(1).loadConstant(i).aload(0).checkcast(ClassDesc.of("Routes$RteBody")).getfield(ClassDesc.of("Routes$RteBody"),field.fieldName().stringValue(),field.fieldTypeSymbol()).aastore();}c.aload(1).areturn();});
                b.withMethodBody(SAFETY,MethodTypeDesc.of(CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.getstatic(ClassDesc.of("Routes"),"rteBodySafety",ClassDesc.of("Routes$RteBody")).areturn());
                b.withMethodBody(FORECAST,MethodTypeDesc.of(CD_void,CD_Object,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.iconst_0().istore(2);var loop=c.newLabel();var done=c.newLabel();c.labelBinding(loop).iload(2).loadConstant(253).if_icmpge(done);
                    c.aload(0).checkcast(ClassDesc.of("Routes$RteBody")).getfield(ClassDesc.of("Routes$RteBody"),"leg",ClassDesc.of("RouteWpt").arrayType()).iload(2).aaload().getfield(ClassDesc.of("RouteWpt"),"winds",ClassDesc.of("FmcWindOat"))
                        .aload(1).checkcast(ClassDesc.of("Routes$RteBody")).getfield(ClassDesc.of("Routes$RteBody"),"leg",ClassDesc.of("RouteWpt").arrayType()).iload(2).aaload().getfield(ClassDesc.of("RouteWpt"),"winds",ClassDesc.of("FmcWindOat"))
                        .invokevirtual(ClassDesc.of("FmcWindOat"),"copy",MethodTypeDesc.of(CD_void,ClassDesc.of("FmcWindOat"))).iinc(2,1).goto_(loop).labelBinding(done).return_();
                });
                b.withMethodBody(BANK,MethodTypeDesc.of(CD_Object,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.getstatic(ClassDesc.of("Glob"),"qs",ClassDesc.of("Qs").arrayType()).aload(0).checkcast(ClassDesc.of("Routes$RteBody")).getfield(ClassDesc.of("Routes$RteBody"),"globIdx",CD_int).aaload().areturn());
                for(var root:roots.entrySet()){
                var nativeMethod=method(data.models,root.getKey());var code=(CodeAttribute)nativeMethod.code().orElseThrow();
                Label destination=root.getKey().equals(READ_NATIVE)?code.elementStream().filter(e->e instanceof LabelTarget target&&code.labelToBci(target.label())==DEST_START).map(e->((LabelTarget)e).label()).findFirst().orElseThrow():null;
                b.withMethod(root.getValue(),nativeMethod.methodTypeSymbol(),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC|ClassFile.ACC_SYNCHRONIZED,m->m.transformCode(code,new CodeTransform(){
                    int offset;boolean finished;
                    public void accept(CodeBuilder c,CodeElement e){
                        if(e instanceof LabelTarget){c.with(e);return;}
                        if(e instanceof ExceptionCatch caught){
                            if(!root.getKey().equals(READ_NATIVE)||code.labelToBci(caught.tryStart())<CORE_END){require(!root.getKey().equals(READ_NATIVE)||code.labelToBci(caught.tryEnd())<=CORE_END&&code.labelToBci(caught.handler())<CORE_END,"exception range crosses foreign section");c.with(e);}return;
                        }
                        if(!(e instanceof Instruction instruction))return;
                        int position=offset;offset+=instruction.sizeInBytes();
                        if(root.getKey().equals(READ_NATIVE)){
                            if(position==CORE_END){
                                c.aload(0).iload(29).putfield(ClassDesc.of("Routes$RteBody"),"firstApproachIdx",CD_int).aload(0).iload(4).putfield(ClassDesc.of("Routes$RteBody"),"highestAltConstr",CD_int);
                                c.aload(0).aload(0).iconst_2().invokevirtual(ClassDesc.of("Routes$RteBody"),"hasAnyCrsFrom",MethodTypeDesc.of(CD_boolean,CD_int));var ordinary=c.newLabel();var done=c.newLabel();c.ifeq(ordinary).iconst_1().goto_(done).labelBinding(ordinary).iconst_2().labelBinding(done).putfield(ClassDesc.of("Routes$RteBody"),"rte0FirstIdxNd",CD_int);
                                c.aload(0).getfield(ClassDesc.of("Routes$RteBody"),"rteLink",ClassDesc.of("FmcRte$Rte"))
                                .getfield(ClassDesc.of("FmcRte$Rte"),"destRwy",ClassDesc.of("FmcRte$Rwy")).getfield(ClassDesc.of("FmcRte$Rwy"),"name",CD_String)
                                .invokevirtual(CD_String,"isEmpty",MethodTypeDesc.of(CD_boolean)).istore(39).goto_(destination);
                            }
                            if(position==DEST_END){
                                var origin=c.newLabel();var originDone=c.newLabel();c.aload(0);for(int index=0;index<2;index++)c.aload(0).getfield(ClassDesc.of("Routes$RteBody"),"leg",ClassDesc.of("RouteWpt").arrayType()).loadConstant(index).aaload().getfield(ClassDesc.of("RouteWpt"),"reserved",CD_char).loadConstant((int)'O').if_icmpeq(origin);
                                c.iconst_0().goto_(originDone).labelBinding(origin).iconst_1().labelBinding(originDone).putfield(ClassDesc.of("Routes$RteBody"),"origRwyInLegs",CD_boolean);
                                c.aload(0).aload(0).getfield(ClassDesc.of("Routes$RteBody"),"legsAmo",CD_int).putfield(ClassDesc.of("Routes$RteBody"),"legsAmoNew",CD_int).aload(0).iconst_0().putfield(ClassDesc.of("Routes$RteBody"),"busy",CD_boolean).return_();finished=true;
                            }
                            if(!kept(position))return;
                        }
                        if(root.getKey().equals(READ_NATIVE)&&instruction instanceof FieldInstruction field&&instruction.opcode()==Opcode.PUTFIELD&&field.owner().asInternalName().equals("FmcWindOat$WindOatAlt")){c.pop().pop();return;}
                        if(instruction instanceof InvokeInstruction call){var target=id(call);
                            if(target.equals(id("FmcWindOat","saveAllAltToFile","(Ljava/lang/StringBuilder;)V"))){c.invokevirtual(ClassDesc.of("FmcWindOat"),SAVE,MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.StringBuilder")));return;}
                            if(OMIT.containsKey(target)){for(int i=0;i<OMIT.get(target);i++)c.pop();return;}
                            if(target.equals(id("java/lang/Exception","printStackTrace","()V"))){c.athrow();return;}
                            if(target.equals(id("RouteWpt","blank","()V"))){c.invokevirtual(ClassDesc.of("RouteWpt"),"$psxExporter$fmcBlankPreserveForecast",MethodTypeDesc.of(CD_void));return;}
                            if(root.getKey().equals(SEND_NATIVE)&&target.equals(id("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"))&&position==170){c.pop().pop().pop().invokestatic(ClassDesc.of("Qs"),FmcReconstructionBanks.VALUE,MethodTypeDesc.of(CD_String,CD_Object));return;}
                        }
                        c.with(e);
                    }
                    public void atEnd(CodeBuilder c){require(!root.getKey().equals(READ_NATIVE)||finished,"native destination completion emitted");}
                }));
            }}
        });require(data.cf.verify(output).isEmpty(),"generated verifier "+data.cf.verify(output));var changed=new HashMap<>(lower);changed.put("Routes$RteBody",output);var before=new HashMap<>(lower);before.put("Routes$RteBody",original);
        byte[] windInput=lower.getOrDefault("FmcWindOat",data.inputs.get("FmcWindOat"));before.put("FmcWindOat",windInput);
        byte[] windOutput=data.cf.transformClass(data.cf.parse(windInput),new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
            b.withMethodBody(WIND_ALIASES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                c.loadConstant(5).anewarray(CD_Object).astore(1);c.aload(0).checkcast(ClassDesc.of("FmcWindOat")).getfield(ClassDesc.of("FmcWindOat"),"windOatAlt",ClassDesc.of("FmcWindOat$WindOatAlt").arrayType()).astore(2);
                c.aload(1).iconst_0().aload(2).aastore();for(int i=0;i<4;i++)c.aload(1).loadConstant(i+1).aload(2).loadConstant(i).aaload().aastore();c.aload(1).areturn();
            });
            var nativeSave=method(data.models,id("FmcWindOat","saveAllAltToFile","(Ljava/lang/StringBuilder;)V"));
            b.withMethod(SAVE,nativeSave.methodTypeSymbol(),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC,m->m.transformCode(nativeSave.code().orElseThrow(),(c,e)->{
                if(e instanceof InvokeInstruction call&&id(call).equals(id("java/io/PrintStream","println","(Ljava/lang/String;)V"))){c.pop().pop().new_(ClassDesc.of("java.lang.IllegalStateException")).dup().loadConstant("FMC current forecast prefix is malformed").invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();return;}c.with(e);
            }));
        }});require(data.cf.verify(windOutput).isEmpty(),"current forecast formatter verifier");changed.put("FmcWindOat",windOutput);TerrainAccess.verifyAdded(before,changed,data.inputs);
        byte[] wptInput=lower.getOrDefault("RouteWpt",data.inputs.get("RouteWpt"));before.put("RouteWpt",wptInput);byte[] wptOutput=data.cf.transformClass(data.cf.parse(wptInput),new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){b.withMethodBody(WPT_FORECAST,MethodTypeDesc.of(CD_Object,CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.aload(0).checkcast(ClassDesc.of("RouteWpt")).getfield(ClassDesc.of("RouteWpt"),"winds",ClassDesc.of("FmcWindOat")).areturn());}});
        require(data.cf.verify(wptOutput).isEmpty(),"current waypoint forecast getter verifier");changed.put("RouteWpt",wptOutput);TerrainAccess.verifyAdded(before,changed,data.inputs);
        writes.add(new FmcQualification.Member("Routes$RteBody","legsAmoNew","I"));writes.add(new FmcQualification.Member("Routes$RteBody","busy","Z"));
        for(String field:List.of("firstApproachIdx","highestAltConstr","rte0FirstIdxNd"))writes.add(new FmcQualification.Member("Routes$RteBody",field,"I"));
        writes.add(new FmcQualification.Member("Routes$RteBody","origRwyInLegs","Z"));
        return new Qualified(Map.copyOf(changed),Set.copyOf(writes),Set.copyOf(inputs),Set.copyOf(seen));
    }
}
