package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.lang.invoke.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only published receiver discovery for the trusted ordinary EXEC
 * footprint. Static bank arrays and the all-pages registry are never expanded:
 * bank receivers use exact native array/index pairs, and CDU receivers come
 * from actual current ControlDisplayUnit references. Local NEW scratch is not
 * a root. Returned references grant no mutation authority. */
final class FmcReconstructionExecRoots {
    static final String ROOTS="$psxExporter$fmcExecPublishedRoots",CHILDREN="$psxExporter$fmcExecPublishedChildren";
    static final String PROPAGATE="$psxExporter$fmcPropagateExecScope",INHERIT="$psxExporter$fmcInheritExecJobScope";
    private static final Map<Prepare,Map<Prepare.MethodId,byte[]>> METADATA_BODIES=Collections.synchronizedMap(new WeakHashMap<>());
    static final Set<String> CREATED=Set.of("Acars$ThreadTelexDn","CduHub$ThreadTimeoutMsg","ControlDisplayUnit$ThreadPageDelay","FmcDepArr$ThreadArrList","FmcDepArr$ThreadSidList");
    record Bank(FmcQualification.Member array,FmcQualification.Member index){}
    record Profile(Map<String,byte[]> changed,Map<String,List<FmcQualification.Member>> statics,
                   Map<String,List<FmcQualification.Member>> children,List<Bank> banks,
                   Set<FmcQualification.Member> references,Set<String> unclosedReceivers,Set<FmcQualification.Member> unclosedReferences,Set<String> operationCreated,Set<String> propagationOwners,Set<FmcQualification.Member> immutableArrays){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC EXEC roots "+why);}
    private static boolean reference(String descriptor){return descriptor.startsWith("[")||descriptor.startsWith("L")&&!descriptor.equals("Ljava/lang/String;");}
    private static String leaf(String descriptor){while(descriptor.startsWith("["))descriptor=descriptor.substring(1);return descriptor.startsWith("L")?descriptor.substring(1,descriptor.length()-1):descriptor;}
    private static boolean isStatic(Prepare data,FmcQualification.Member field){return(data.models.get(field.owner()).fields().stream().filter(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.descriptor())).findFirst().orElseThrow().flags().flagsMask()&ClassFile.ACC_STATIC)!=0;}
    private static boolean allPages(FmcQualification.Member f){return f.owner().equals("CduHub")&&f.name().equals("pg");}
    private static boolean bankArray(FmcQualification.Member f){return f.owner().equals("Glob")&&Set.of("qi","qs","qh").contains(f.name());}
    private static boolean page(Prepare data,String type){var seen=new HashSet<String>();while(data.models.containsKey(type)&&seen.add(type)){if(type.equals("CduHub$Page"))return true;var model=data.models.get(type);if(model.superclass().isEmpty())return false;type=model.superclass().orElseThrow().asInternalName();}return false;}
    static Profile transform(Prepare data,Map<String,byte[]> lower){
        return transform(data,lower,null);
    }
    static Profile transform(Prepare data,Map<String,byte[]> lower,FmcReconstructionNormalExec.Profile normal){
        return transform(data,lower,normal,FmcReconstructionExecScope.inspect(data));
    }
    static Profile transform(Prepare data,Map<String,byte[]> lower,FmcReconstructionNormalExec.Profile normal,FmcReconstructionExecScope.Profile scope){
        var classified=FmcReconstructionExecAudit.classify(data);var freshCache=FmcReconstructionFreshCache.inspect(data);var effects=classified.potential();var methods=scope.methods();var read=new TreeSet<FmcQualification.Member>();var banks=new LinkedHashSet<Bank>();var arrayTouched=new HashSet<FmcQualification.Member>();
        require(FmcExecMutationPrepare.profile(data).methods().containsAll(methods)&&effects.writes().containsAll(scope.writes()),"qualified scope subset of potential authority");
        for(var method:methods){var instructions=Prepare.instructions(data.methods.get(method));for(int i:scope.instructions().get(method))if(instructions.get(i) instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)){
            if(!data.models.containsKey(f.owner().asInternalName()))continue;
            var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());require(member!=null,"resolved original reference");if(reference(member.descriptor()))read.add(member);
            if(member.descriptor().startsWith("[")&&classified.liveArrayMethods().contains(method))arrayTouched.add(member);
            if(bankArray(member)&&i+2<instructions.size()&&instructions.get(i+1) instanceof FieldInstruction index&&index.opcode()==Opcode.GETSTATIC&&index.owner().asInternalName().equals("Glob")&&index.type().equalsString("I")&&instructions.get(i+2).opcode()==Opcode.AALOAD){
                var bound=FmcQualification.resolveField(data.models,"Glob",index.name().stringValue(),"I",new HashSet<>());require(bound!=null,"exact original bank index");banks.add(new Bank(member,bound));
            }
        }}
        for(String owner:CREATED)for(var field:data.models.get(owner).fields())if((field.flags().flagsMask()&ClassFile.ACC_STATIC)==0&&reference(field.fieldType().stringValue()))read.add(new FmcQualification.Member(owner,field.fieldName().stringValue(),field.fieldType().stringValue()));
        if(normal!=null){read.removeAll(normal.references());banks.removeAll(normal.banks());}
        var instanceOwners=new HashSet<String>();for(var f:scope.writes())if((normal==null||!normal.fields().contains(f))&&!freshCache.recordFields().contains(f)&&!classified.transport().fields().contains(f)&&!isStatic(data,f))instanceOwners.add(f.owner());instanceOwners.addAll(CREATED);
        // Native current/future Page identity remains receiver ownership even
        // when its pagination is an accepted presentation output. Discovery
        // still uses the three actual CDU.page edges, never allCduHub.pg.
        instanceOwners.add("CduHub$Page");
        var relevant=new HashSet<>(instanceOwners);relevant.add("java/lang/StringBuilder");relevant.add("java/lang/StringBuffer");
        // Published wrappers are admitted only along original reference edges.
        boolean more;do{more=false;for(var f:read)if(!isStatic(data,f)&&relevant.contains(leaf(f.descriptor())))more|=relevant.add(f.owner());
            for(var e:data.models.entrySet())if(e.getValue().superclass().isPresent()&&relevant.contains(e.getKey()))more|=relevant.add(e.getValue().superclass().orElseThrow().asInternalName());
        }while(more);
        var roots=new TreeMap<String,List<FmcQualification.Member>>();var children=new TreeMap<String,List<FmcQualification.Member>>();var references=new HashSet<FmcQualification.Member>();var unclosedReferences=new HashSet<FmcQualification.Member>();
        var immutableArrays=new HashSet<FmcQualification.Member>();for(var field:read)if(field.descriptor().startsWith("[")&&leaf(field.descriptor()).length()==1&&!arrayTouched.contains(field))try{var proof=FmcReconstructionArrayProof.audit(data,Set.of(field),methods);if(proof.mutations().isEmpty())immutableArrays.add(field);}catch(IllegalArgumentException unresolved){/* Opaque alias stays owned; absence of a proof is not immutability. */}
        for(var field:read)if(field.descriptor().equals("[Ljava/lang/String;")&&isStatic(data,field)){
            var outside=new HashSet<Prepare.MethodId>();int stores=0;boolean startupOnly=true;
            for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){
                if(!e.getKey().owner().equals(field.owner())||!e.getKey().name().equals("<clinit>"))outside.add(e.getKey());
                for(var op:Prepare.instructions(e.getValue()))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)&&field.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()))){stores++;startupOnly&=e.getKey().owner().equals(field.owner())&&e.getKey().name().equals("<clinit>");}
            }
            if(stores==1&&startupOnly)try{if(FmcReconstructionArrayProof.membership(data,Set.of(field),outside).mutations().isEmpty())immutableArrays.add(field);}catch(IllegalArgumentException unresolved){/* Original opaque array argument remains an explicit obligation. */}
        }
        for(var f:read){if(allPages(f)||bankArray(f))continue;boolean stat=isStatic(data,f);String type=leaf(f.descriptor());
            if(freshCache.references().contains(f)){references.add(f);continue;}
            if(classified.transport().references().contains(f))continue;
            boolean array=f.descriptor().startsWith("[");if(!relevant.contains(type)&&!array)continue;if(immutableArrays.contains(f)){references.add(f);continue;}
            if(array&&data.models.containsKey(type)&&!relevantType(data,type,relevant)){references.add(f);continue;}
            // Global Page definitions are not current CDU receivers. Current
            // pages are reached from ControlDisplayUnit.page below.
            if(stat&&page(data,type))continue;
            if(stat&&f.owner().startsWith("CduPg")){unclosedReferences.add(f);continue;}
            if(!stat&&page(data,type)&&!f.owner().equals("ControlDisplayUnit")&&!f.owner().equals("ControlDisplayUnit$ThreadPageDelay")&&!f.owner().equals("CduPgMenu$Prompt")){unclosedReferences.add(f);continue;}
            if(!stat&&f.owner().equals("ControlDisplayUnit")&&page(data,type)&&!f.name().equals("page")){unclosedReferences.add(f);continue;}
            (stat?roots:children).computeIfAbsent(f.owner(),k->new ArrayList<>()).add(f);references.add(f);
        }
        require(roots.values().stream().flatMap(Collection::stream).anyMatch(f->f.owner().equals("Core")&&f.descriptor().equals("LControlDisplayUnit;")),"current CDU static receiver roots");
        require(children.getOrDefault("ControlDisplayUnit",List.of()).stream().anyMatch(f->f.name().equals("page")&&f.descriptor().equals("LCduHub$Page;")),"actual current CDU page edge");
        var reachable=new HashSet<String>();roots.values().forEach(rows->rows.forEach(f->reachable.add(leaf(f.descriptor()))));banks.forEach(b->reachable.add(leaf(b.array.descriptor())));
        do{more=false;for(var e:children.entrySet())if(reachable.contains(e.getKey()))for(var f:e.getValue())more|=reachable.add(leaf(f.descriptor()));
            for(var e:data.models.entrySet())if(reachable.contains(e.getKey())&&e.getValue().superclass().isPresent())more|=reachable.add(e.getValue().superclass().orElseThrow().asInternalName());
            for(var e:data.models.entrySet())if(e.getValue().superclass().isPresent()&&reachable.contains(e.getValue().superclass().orElseThrow().asInternalName())&&instanceOwners.contains(e.getKey()))more|=reachable.add(e.getKey());
        }while(more);
        var unclosed=new TreeSet<>(instanceOwners);unclosed.removeAll(reachable);require(unclosed.containsAll(CREATED),"exact operation-created unpublished receiver frontier "+unclosed);unclosed.removeAll(CREATED);
        var propagation=new TreeSet<>(instanceOwners);propagation.addAll(children.keySet());
        var changed=new HashMap<>(lower);var before=new HashMap<>(lower);var owners=new TreeSet<String>();owners.addAll(roots.keySet());owners.addAll(children.keySet());owners.add("Glob");

        for(String owner:owners){byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(ROOTS)||m.methodName().equalsString(CHILDREN)),"generated getter collision");
            byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
                if(roots.containsKey(owner)||owner.equals("Glob")){var fields=roots.getOrDefault(owner,List.of());var bankRows=owner.equals("Glob")?List.copyOf(banks):List.<Bank>of();
                    b.withMethodBody(ROOTS,MethodTypeDesc.of(CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.loadConstant(fields.size()+bankRows.size()).anewarray(CD_Object).astore(0);for(int i=0;i<fields.size();i++){var f=fields.get(i);c.aload(0).loadConstant(i).getstatic(ClassDesc.of(owner),f.name(),ClassDesc.ofDescriptor(f.descriptor())).aastore();}for(int i=0;i<bankRows.size();i++){var bank=bankRows.get(i);c.aload(0).loadConstant(fields.size()+i).getstatic(ClassDesc.of("Glob"),bank.array.name(),ClassDesc.ofDescriptor(bank.array.descriptor())).getstatic(ClassDesc.of("Glob"),bank.index.name(),CD_int).aaload().aastore();}c.aload(0).areturn();});
                }
                if(children.containsKey(owner)){var fields=children.get(owner);b.withMethodBody(CHILDREN,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.loadConstant(fields.size()).anewarray(CD_Object).astore(1);for(int i=0;i<fields.size();i++){var f=fields.get(i);c.aload(1).loadConstant(i).aload(0).checkcast(ClassDesc.of(owner)).getfield(ClassDesc.of(owner),f.name(),ClassDesc.ofDescriptor(f.descriptor())).aastore();}c.aload(1).areturn();});}
            }});require(data.cf.verify(output).isEmpty(),"getter verifier "+owner);changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);var rootCopy=new TreeMap<String,List<FmcQualification.Member>>();roots.forEach((k,v)->rootCopy.put(k,List.copyOf(v)));var childCopy=new TreeMap<String,List<FmcQualification.Member>>();children.forEach((k,v)->childCopy.put(k,List.copyOf(v)));
        banks.forEach(b->{references.add(b.array);references.add(b.index);});references.add(new FmcQualification.Member("CduHub","pg","[LCduHub$Page;"));for(String name:List.of("pgMenuL","pgMenuR","pgMenuC"))references.add(new FmcQualification.Member("CduHub",name,"I"));references.add(new FmcQualification.Member("ControlDisplayUnit","cduId","C"));var result=new Profile(Map.copyOf(changed),Map.copyOf(rootCopy),Map.copyOf(childCopy),List.copyOf(banks),Set.copyOf(references),Set.copyOf(unclosed),Set.copyOf(unclosedReferences),CREATED,Set.copyOf(propagation),Set.copyOf(immutableArrays));if(normal!=null){FmcReconstructionCapacity.qualifyArrays(data,result);FmcReconstructionCapacity.checkedBudget(result);}return result;
    }
    static Map<String,byte[]> propagate(Prepare data,Map<String,byte[]> tagged,Profile profile){
        var changed=new HashMap<>(tagged);var before=new HashMap<>(tagged);var owners=new TreeSet<>(profile.propagationOwners);owners.add("FmcRte");
        for(String owner:profile.propagationOwners){var model=FmcQualification.finalModels(data,tagged).get(owner);FmcBackgroundAdmissionOwnership.qualify(data,owner,model);}
        for(String owner:owners){byte[] input=tagged.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);
            require(model.methods().stream().noneMatch(m->m.methodName().equalsString(PROPAGATE)||m.methodName().equalsString(INHERIT)),"propagation collision");
            byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
                if(profile.propagationOwners.contains(owner))b.withMethodBody(PROPAGATE,MethodTypeDesc.of(CD_void,CD_Object,CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var done=c.newLabel();c.aload(0).ifnull(done).lload(1).lconst_0().lcmp().ifeq(done);var type=ClassDesc.of(owner);
                    c.aload(0).checkcast(type).getfield(type,FmcBackgroundAdmissionOwnership.TAG,CD_long).lload(1).lcmp().ifeq(done);
                    c.aload(0).checkcast(type).lload(1).putfield(type,FmcBackgroundAdmissionOwnership.TAG,CD_long);
                    for(var field:profile.children.getOrDefault(owner,List.of())){String target=propagationTarget(data,leaf(field.descriptor()),profile.propagationOwners);if(target==null)continue;c.aload(0).checkcast(type).getfield(type,field.name(),ClassDesc.ofDescriptor(field.descriptor()));propagateReference(c,field.descriptor(),target);}
                    if(owner.equals("ControlDisplayUnit$ThreadPageDelay")){
                        currentMenu(c);c.lload(1).invokestatic(ClassDesc.of("CduHub$Page"),PROPAGATE,MethodTypeDesc.of(CD_void,CD_Object,CD_long));
                    }
                    // Tag each relevant concrete native subtype by actual
                    // receiver identity. No page registry is traversed.
                    for(String subtype:profile.propagationOwners)if(!subtype.equals(owner)&&subtype(data,subtype,owner)){var next=c.newLabel();c.aload(0).instanceOf(ClassDesc.of(subtype)).ifeq(next).aload(0).lload(1).invokestatic(ClassDesc.of(subtype),PROPAGATE,MethodTypeDesc.of(CD_void,CD_Object,CD_long)).labelBinding(next);}
                    c.labelBinding(done).return_();
                });
                if(owner.equals("FmcRte"))b.withMethodBody(INHERIT,MethodTypeDesc.of(CD_void,ClassDesc.of("java.lang.Thread"),CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var done=c.newLabel();c.lload(1).lconst_0().lcmp().ifeq(done);for(String job:new TreeSet<>(CREATED)){var next=c.newLabel();c.aload(0).instanceOf(ClassDesc.of(job)).ifeq(next).aload(0).lload(1).invokestatic(ClassDesc.of(job),PROPAGATE,MethodTypeDesc.of(CD_void,CD_Object,CD_long)).goto_(done).labelBinding(next);}c.labelBinding(done).return_();});
            }});require(data.cf.verify(output).isEmpty(),"propagation verifier "+owner);changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        for(String owner:owners)for(var method:data.cf.parse(changed.get(owner)).methods())if(Set.of(PROPAGATE,INHERIT).contains(method.methodName().stringValue()))for(var instruction:Prepare.instructions(method)){
            require(!Set.of(Opcode.NEW,Opcode.ANEWARRAY,Opcode.NEWARRAY,Opcode.MULTIANEWARRAY).contains(instruction.opcode())&&!(instruction instanceof InvokeDynamicInstruction),"allocation/dynamic propagation");
            if(instruction instanceof FieldInstruction field&&(instruction.opcode()==Opcode.PUTFIELD||instruction.opcode()==Opcode.PUTSTATIC))require(instruction.opcode()==Opcode.PUTFIELD&&field.name().equalsString(FmcBackgroundAdmissionOwnership.TAG)&&field.type().equalsString("J")&&profile.propagationOwners.contains(field.owner().asInternalName()),"native content propagation store");
            if(instruction instanceof InvokeInstruction call)require(call.opcode()==Opcode.INVOKESTATIC&&call.name().equalsString(PROPAGATE)&&profile.propagationOwners.contains(call.owner().asInternalName()),"foreign propagation call");
        }
        var bodies=new HashMap<Prepare.MethodId,byte[]>();for(String owner:owners)for(var method:data.cf.parse(changed.get(owner)).methods())if(Set.of(PROPAGATE,INHERIT).contains(method.methodName().stringValue()))bodies.put(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue()),normalized(data,owner,method));METADATA_BODIES.put(data,Map.copyOf(bodies));
        return Map.copyOf(changed);
    }
    private static boolean subtype(Prepare data,String type,String parent){var seen=new HashSet<String>();while(data.models.containsKey(type)&&seen.add(type)){if(type.equals(parent))return true;var model=data.models.get(type);if(model.superclass().isEmpty())return false;type=model.superclass().orElseThrow().asInternalName();}return false;}
    private static boolean relevantType(Prepare data,String type,Set<String> relevant){var seen=new HashSet<String>();while(seen.add(type)){if(type.equals("java/lang/Object"))return false;if(relevant.contains(type))return true;var model=data.models.get(type);if(model==null||model.superclass().isEmpty())return false;type=model.superclass().orElseThrow().asInternalName();}return false;}
    private static void currentMenu(CodeBuilder c){var right=c.newLabel();var centre=c.newLabel();var selected=c.newLabel();c.getstatic(ClassDesc.of("CduHub"),"pg",ClassDesc.of("CduHub$Page").arrayType());c.aload(0).checkcast(ClassDesc.of("ControlDisplayUnit$ThreadPageDelay")).getfield(ClassDesc.of("ControlDisplayUnit$ThreadPageDelay"),"this$0",ClassDesc.of("ControlDisplayUnit")).getfield(ClassDesc.of("ControlDisplayUnit"),"cduId",CD_char).loadConstant((int)'L').if_icmpne(right).getstatic(ClassDesc.of("CduHub"),"pgMenuL",CD_int).goto_(selected).labelBinding(right);c.aload(0).checkcast(ClassDesc.of("ControlDisplayUnit$ThreadPageDelay")).getfield(ClassDesc.of("ControlDisplayUnit$ThreadPageDelay"),"this$0",ClassDesc.of("ControlDisplayUnit")).getfield(ClassDesc.of("ControlDisplayUnit"),"cduId",CD_char).loadConstant((int)'R').if_icmpne(centre).getstatic(ClassDesc.of("CduHub"),"pgMenuR",CD_int).goto_(selected).labelBinding(centre).getstatic(ClassDesc.of("CduHub"),"pgMenuC",CD_int).labelBinding(selected).aaload();}
    private static String propagationTarget(Prepare data,String type,Set<String> owners){var seen=new HashSet<String>();while(data.models.containsKey(type)&&seen.add(type)){if(owners.contains(type))return type;var model=data.models.get(type);if(model.superclass().isEmpty())return null;type=model.superclass().orElseThrow().asInternalName();}return null;}
    private static void propagateReference(CodeBuilder c,String descriptor,String target){
        if(!descriptor.startsWith("[")){c.lload(1).invokestatic(ClassDesc.of(target),PROPAGATE,MethodTypeDesc.of(CD_void,CD_Object,CD_long));return;}
        int array=c.allocateLocal(TypeKind.REFERENCE),index=c.allocateLocal(TypeKind.INT);c.astore(array);var done=c.newLabel();var loop=c.newLabel();c.aload(array).ifnull(done).iconst_0().istore(index).labelBinding(loop).iload(index).aload(array).arraylength().if_icmpge(done).aload(array).iload(index).aaload();propagateReference(c,descriptor.substring(1),target);c.iinc(index,1).goto_(loop).labelBinding(done);
    }
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("FmcRte",INHERIT,"(Ljava/lang/Thread;J)V"));}
    /** Exact source-proved metadata helper only. A prefix/field-name resemblance
     * never exempts a method from native mutation/read qualification. */
    static boolean metadataOnly(Prepare data,String owner,MethodModel method){var bodies=METADATA_BODIES.get(data);if(bodies==null||!Set.of(PROPAGATE,INHERIT).contains(method.methodName().stringValue()))return false;var expected=bodies.get(new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue()));return expected!=null&&Arrays.equals(expected,normalized(data,owner,method));}
    private static byte[] normalized(Prepare data,String owner,MethodModel method){return data.cf.build(ClassDesc.of(owner),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));}
    static Object[] read(FmcInitialization.Context context,Profile profile)throws Throwable{return read(context,profile,List.of());}
    static Object[] read(FmcInitialization.Context context,Profile profile,Collection<?> replacedHeaders)throws Throwable{
        require(context.current(),"worker lifecycle");var child=new HashMap<Class<?>,List<MethodHandle>>();var handles=new HashMap<String,MethodHandle>();var replaced=new IdentityHashMap<Object,Boolean>();replacedHeaders.forEach(o->replaced.put(o,Boolean.TRUE));
        int budget=FmcReconstructionCapacity.checkedBudget(profile);var parentCounts=new HashMap<String,Integer>();var handleFields=new HashMap<MethodHandle,List<FmcQualification.Member>>();
        var queue=new ArrayDeque<Object>();var objects=new ArrayList<Object>();var seen=new IdentityHashMap<Object,Boolean>();var roots=new TreeSet<>(profile.statics.keySet());roots.add("Glob");
        for(String owner:roots){Object[] values=(Object[])getter(context.owner(owner),ROOTS,MethodType.methodType(Object[].class)).invokeExact();var fields=profile.statics.getOrDefault(owner,List.of());for(int i=0;i<values.length;i++){Object value=values[i];if(i<fields.size())FmcReconstructionCapacity.checkArray(fields.get(i),value);if(value!=null)queue.add(value);}}
        while(!queue.isEmpty()){Object value=queue.removeFirst();if(value instanceof String||seen.put(value,Boolean.TRUE)!=null)continue;
            // Array containers only lead to their selected native elements.
            // Primitive tables and immutable String elements have no owner
            // metadata or callback identity-table consumer.
            if(value instanceof Object[] array){for(Object row:array)if(row!=null&&!(row instanceof String))queue.add(row);continue;}
            if(value.getClass().isArray())continue;objects.add(value);require(objects.size()<=budget,"published native receiver capacity");
            for(Class<?> type=value.getClass();type!=null;type=type.getSuperclass()){String owner=type.getName().replace('.','/');if(profile.children.containsKey(owner)){int count=parentCounts.merge(owner,1,Integer::sum);require(count<=FmcReconstructionCapacity.parentLimit(owner),"current native parent count outside qualified schema "+owner);}}
            var getters=child.get(value.getClass());if(getters==null){getters=new ArrayList<>();for(Class<?> type=value.getClass();type!=null;type=type.getSuperclass()){String owner=type.getName().replace('.','/');if(!profile.children.containsKey(owner))continue;var handle=handles.get(owner);if(handle==null){handle=getter(context.owner(owner),CHILDREN,MethodType.methodType(Object[].class,Object.class));handles.put(owner,handle);handleFields.put(handle,profile.children.get(owner));}getters.add(handle);}child.put(value.getClass(),getters);}
            for(var handle:getters){Object[] values=(Object[])handle.invokeExact(value);List<FmcQualification.Member> fields=null;
                if(replaced.containsKey(value))for(Class<?> type=value.getClass();type!=null;type=type.getSuperclass())if(type.getName().equals("FmcRte$Rte")){fields=profile.children.get("FmcRte$Rte");break;}
                for(int i=0;i<values.length;i++){if(fields!=null&&i<fields.size()&&Set.of("sidPack","arrPkOr","appPkOr","arrPkDe","appPkDe").contains(fields.get(i).name()))continue;Object row=values[i];FmcReconstructionCapacity.checkArray(handleFields.get(handle).get(i),row);if(row!=null)queue.add(row);}
            }
        }
        require(context.current(),"worker lifecycle changed");return objects.toArray();
    }
    private static MethodHandle getter(Class<?> owner,String name,MethodType type)throws Exception{var method=owner.getDeclaredMethod(name,type.parameterArray());require(method.isSynthetic()&&method.getModifiers()==(ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC)&&method.getReturnType()==Object[].class,"same-owner getter shape");return MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,name,type);}
    static Set<String> potentialWorkerOwners(Profile profile){var result=new TreeSet<>(profile.statics.keySet());result.add("Glob");result.addAll(profile.children.keySet());result.removeAll(CREATED);return Set.copyOf(result);}
    public static void main(String[] args)throws Exception{Path jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"native identity");var data=new Prepare(Prepare.readJar(jar));var profile=transform(data,Map.of());System.out.println("EXEC published roots "+profile.statics.size()+" static-owner getters; "+profile.children.size()+" child-owner getters; "+profile.banks.size()+" exact banks; unclosed receivers "+new TreeSet<>(profile.unclosedReceivers));for(var e:new TreeMap<>(profile.statics).entrySet())System.out.println("ROOT "+e);for(var e:new TreeMap<>(profile.children).entrySet())System.out.println("CHILD "+e);for(var bank:profile.banks)System.out.println("BANK "+bank);for(var ref:new TreeSet<>(profile.unclosedReferences))System.out.println("UNCLOSED_REF "+ref);}
}


