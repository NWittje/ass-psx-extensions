package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import java.nio.file.*;
import static java.lang.constant.ConstantDescs.*;

/** Retain complete raw native bank writes at individually reviewed call sites.
 * Only an unused native write result is eligible. Reads and used nonvoid
 * results remain native and need a different qualified frontier. Receiver
 * selection, original exception/continuation disposition, replay-thread
 * legality, ordering, lifetime and lifecycle proofs
 * belong to the complete installed profile; this layer advertises nothing. */
final class FmcInputPrepare {
    enum Kind { QI, QS }
    record Site(Prepare.MethodId caller,int instruction,Kind kind,String finalClassHash){}
    static final String FAULT="$psxExporter$fmcInputFailure";
    static final String REPLAY="$psxExporter$fmcReplayInput";
    private static final ClassDesc ADMISSION=ClassDesc.of("psx.exporter.FmcJobAdmission");
    private FmcInputPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC retained input "+why);}
    private static Prepare.MethodId id(String owner,MethodModel method){return new Prepare.MethodId(owner,method.methodName().stringValue(),method.methodType().stringValue());}
    private static String nativeOwner(Kind kind){return kind==Kind.QI?"Qi":"Qs";}
    private static MethodTypeDesc nativeType(Kind kind){return MethodTypeDesc.of(kind==Kind.QI?CD_int:CD_String,kind==Kind.QI?CD_int:CD_String,CD_char,CD_int);}
    private static MethodTypeDesc retainedType(Kind kind,ClassDesc result){return MethodTypeDesc.of(result,CD_Object,kind==Kind.QI?CD_int:CD_String,CD_char,CD_int);}
    private static void validate(Prepare data,Map<String,byte[]> lower,Site site){
        byte[] bytes=lower.getOrDefault(site.caller.owner(),data.inputs.get(site.caller.owner()));require(bytes!=null,"caller owner");
        try{require(Identity.hash(bytes).equalsIgnoreCase(site.finalClassHash),"reviewed final caller preimage");}catch(Exception failure){throw new IllegalArgumentException("FMC input preimage",failure);}
        var model=data.cf.parse(bytes);var method=model.methods().stream().filter(m->id(site.caller.owner(),m).equals(site.caller)).findFirst().orElseThrow();
        require(method.code().isPresent()&&!site.caller.name().startsWith("<"),"ordinary input caller");
        var code=(CodeAttribute)method.code().orElseThrow();var ops=Prepare.instructions(method);int at=site.instruction;
        require(at>=2&&at+1<ops.size(),"call ordinal");
        require(ops.get(at) instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKEVIRTUAL
            &&Prepare.call(call,nativeOwner(site.kind),"transfer",nativeType(site.kind).descriptorString()),"exact native write call");
        require(ops.get(at-2) instanceof ConstantInstruction mode&&Integer.valueOf('w').equals(mode.constantValue()),"literal write mode before origin");
        require(ops.get(at+1).opcode()==Opcode.POP,"original write return is used; cannot defer it");
        int popOffset=0;for(int i=0;i<=at;i++)popOffset+=ops.get(i).sizeInBytes();
        // Removing POP is legal only when it has this one fallthrough input.
        // Native branch/handler entries retain their own original stacks.
        for(var op:ops){
            if(op instanceof BranchInstruction branch)require(code.labelToBci(branch.target())!=popOffset,"POP is an independent branch target");
            if(op instanceof LookupSwitchInstruction sw){require(code.labelToBci(sw.defaultTarget())!=popOffset,"POP switch entry");for(var c:sw.cases())require(code.labelToBci(c.target())!=popOffset,"POP switch entry");}
            if(op instanceof TableSwitchInstruction sw){require(code.labelToBci(sw.defaultTarget())!=popOffset,"POP switch entry");for(var c:sw.cases())require(code.labelToBci(c.target())!=popOffset,"POP switch entry");}
        }
        for(var handler:code.exceptionHandlers())require(code.labelToBci(handler.handler())!=popOffset,"POP is a native handler entry");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Site> sites){
        require(!sites.isEmpty(),"empty reviewed call-site roster");
        var byMethod=new HashMap<Prepare.MethodId,Map<Integer,Site>>();var owners=new TreeSet<String>();
        for(var site:sites){validate(data,lower,site);owners.add(site.caller.owner());require(byMethod.computeIfAbsent(site.caller,k->new HashMap<>()).put(site.instruction,site)==null,"duplicate call-site");}
        var before=new HashMap<>(lower);var changed=new HashMap<>(lower);
        for(String owner:owners){
            byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var source=data.cf.parse(input);var caller=ClassDesc.of(owner);
            require(source.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT)),"caller fault-field collision");
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){
                    if(!(element instanceof MethodModel method)||!byMethod.containsKey(id(owner,method))){builder.with(element);return;}
                    var selected=byMethod.get(id(owner,method));
                    builder.transformMethod(method,MethodTransform.transformingCode(new CodeTransform(){
                        int ordinal;boolean omitPop;Label afterPop;List<ExceptionCatch> handlers;
                        public void atStart(CodeBuilder b){ordinal=0;omitPop=false;handlers=new ArrayList<>();}
                        public void accept(CodeBuilder b,CodeElement element){
                            if(element instanceof ExceptionCatch handler){handlers.add(handler);return;}
                            if(!(element instanceof Instruction)){b.with(element);return;}
                            Site site=selected.get(ordinal++);
                            if(omitPop){require(((Instruction)element).opcode()==Opcode.POP,"changed native POP");omitPop=false;b.labelBinding(afterPop);return;}
                            if(site==null){b.with(element);return;}
                            int origin=b.allocateLocal(TypeKind.INT),mode=b.allocateLocal(TypeKind.INT),value=b.allocateLocal(site.kind==Kind.QI?TypeKind.INT:TypeKind.REFERENCE),receiver=b.allocateLocal(TypeKind.REFERENCE),retained=b.allocateLocal(TypeKind.INT);
                            b.istore(origin).istore(mode);if(site.kind==Kind.QI)b.istore(value);else b.astore(value);b.astore(receiver).iconst_0().istore(retained);
                            var begin=b.newLabel();var end=b.newLabel();var failure=b.newLabel();var nativeCall=b.newLabel();afterPop=b.newLabel();
                            b.labelBinding(begin).aload(receiver);if(site.kind==Kind.QI)b.iload(value);else b.aload(value);
                            b.iload(mode).iload(origin).invokestatic(ADMISSION,site.kind==Kind.QI?"retainQi":"retainQs",retainedType(site.kind,CD_boolean))
                                .istore(retained).labelBinding(end).goto_(nativeCall).exceptionCatchAll(begin,end,failure).labelBinding(failure).pop();
                            failure(b,caller);b.labelBinding(nativeCall).iload(retained).ifne(afterPop).aload(receiver);
                            if(site.kind==Kind.QI)b.iload(value);else b.aload(value);b.iload(mode).iload(origin).with(element).pop().goto_(afterPop);
                            omitPop=true;
                        }
                        public void atEnd(CodeBuilder b){require(!omitPop,"missing original POP");for(var handler:handlers)b.with(handler);}
                    }));
                }
                public void atEnd(ClassBuilder b){b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);}
            });
            require(data.cf.verify(output).isEmpty(),"caller bytecode verification "+owner);changed.put(owner,output);
        }
        for(Kind kind:Kind.values())if(sites.stream().anyMatch(s->s.kind==kind)){
            String owner=nativeOwner(kind);byte[] input=changed.getOrDefault(owner,data.inputs.get(owner));require(input!=null,"replay owner");
            var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(REPLAY)),"replay collision");
            var nativeMethods=model.methods().stream().filter(m->m.methodName().equalsString("transfer")&&m.methodType().equalsString(nativeType(kind).descriptorString())).toList();
            require(nativeMethods.size()==1&&nativeMethods.getFirst().code().isPresent()&&(nativeMethods.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"ordinary native replay method");
            before.putIfAbsent(owner,lower.getOrDefault(owner,data.inputs.get(owner)));
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){b.withMethodBody(REPLAY,retainedType(kind,CD_void),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.aload(0).checkcast(ClassDesc.of(owner));if(kind==Kind.QI)c.iload(1);else c.aload(1);
                    c.iload(2).iload(3).invokevirtual(ClassDesc.of(owner),"transfer",nativeType(kind)).pop().return_();
                });}
            });
            require(data.cf.verify(output).isEmpty(),"replay bridge verification");changed.put(owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static void failure(CodeBuilder b,ClassDesc caller){
        b.iconst_1().putstatic(caller,FAULT,CD_boolean);
        var begin=b.newLabel();var end=b.newLabel();var failed=b.newLabel();var done=b.newLabel();
        b.labelBinding(begin).invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"failed",MethodTypeDesc.of(CD_void)).labelBinding(end).goto_(done)
            .exceptionCatchAll(begin,end,failed).labelBinding(failed).pop().labelBinding(done);
    }
    /** Candidate discovery is intentionally separate from installed coverage.
     * A POP and literal mode do not prove receiver ownership, exception
     * continuation or the thread on which the write may legally resume. */
    static List<Site> candidates(Prepare data)throws Exception{
        var result=new ArrayList<Site>();
        for(var entry:data.methods.entrySet()){
            if(entry.getKey().name().startsWith("<")||entry.getValue().code().isEmpty())continue;
            var ops=Prepare.instructions(entry.getValue());
            for(int at=2;at+1<ops.size();at++)for(Kind kind:Kind.values())
                if(Prepare.call(ops.get(at),nativeOwner(kind),"transfer",nativeType(kind).descriptorString())
                    &&ops.get(at-2) instanceof ConstantInstruction mode&&Integer.valueOf('w').equals(mode.constantValue())
                    &&ops.get(at+1).opcode()==Opcode.POP)
                    result.add(new Site(entry.getKey(),at,kind,Identity.hash(data.inputs.get(entry.getKey().owner()))));
        }
        result.sort(Comparator.comparing(s->s.caller.toString()+":"+s.instruction));return List.copyOf(result);
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned output required");Path jar=Path.of(args[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var sites=candidates(data);
        var out=new StringBuilder("{\"schema\":1,\"state\":\"native_input_candidates_only\",\"recallQualified\":false,\"vendorClassesDefined\":false,\"sites\":[");boolean comma=false;
        for(var site:sites){if(comma)out.append(',');comma=true;
            out.append("{\"caller\":").append(FmcQualification.quote(site.caller.toString())).append(",\"instruction\":").append(site.instruction)
                .append(",\"kind\":").append(FmcQualification.quote(site.kind.name())).append(",\"classSha256\":").append(FmcQualification.quote(site.finalClassHash)).append('}');
        }
        out.append("],\"limitation\":\"Unused-return write discovery only. Receiver relevance, all other writers/reads, synchronous exception disposition, native replay thread, lifecycle and job dependencies require installed qualification.\"}\n");
        Files.writeString(Path.of(args[1]),out,StandardOpenOption.CREATE_NEW);
        System.out.println("Native unused-result input discovery: "+sites.size()+" candidates; no recall qualification or vendor class definition");
    }
}
