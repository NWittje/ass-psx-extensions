package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Receiver-filtered bank mutation witnesses. No input is inhibited or queued.
 * A concurrently edited bank invalidates a copy/commit witness; this layer does
 * not establish recall exclusion. Installed banks are bound only after normal
 * initialization by the independently qualified native profile. */
public final class FmcBankWriterPrepare {
    static final Set<Prepare.MethodId> WRITERS=Set.of(
        method("Q","isUpdated","()Z"),method("Qi","setMinMax","(II)V"),
        method("Qi","transfer","(ICI)I"),method("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"));
    static final Set<Prepare.MethodId> CONSTRUCTORS=Set.of(
        method("Q","<init>","(ILjava/lang/String;B)V"),method("Qi","<init>","(ILjava/lang/String;IIB)V"),method("Qs","<init>","(ILjava/lang/String;IIB)V"));
    static final Set<Prepare.MethodId> EXCLUDED=Set.of(method("Qh","transfer","(ICI)I"),method("NavRadios","situLoadInit","()V"));
    static final String NAV_BANK="$psxExporter$fmcUnownedNavBank";
    static final Set<FmcQualification.Member> FIELDS=Set.of(
        field("Q","fresh","Z"),field("Q","updated","Z"),field("Qi","iVar","I"),
        field("Qs","sVar","Ljava/lang/String;"),field("Qs","lastComeFrom","I"));
    static final Map<FmcQualification.Member,Set<Prepare.MethodId>> COVERAGE=Map.of(
        field("Q","fresh","Z"),Set.of(method("Q","<init>","(ILjava/lang/String;B)V"),method("Qi","transfer","(ICI)I"),method("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"),method("Qh","transfer","(ICI)I")),
        field("Q","updated","Z"),Set.of(method("Q","<init>","(ILjava/lang/String;B)V"),method("Q","isUpdated","()Z"),method("Qi","transfer","(ICI)I"),method("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;"),method("Qh","transfer","(ICI)I"),method("NavRadios","situLoadInit","()V")),
        field("Qi","iVar","I"),Set.of(method("Qi","<init>","(ILjava/lang/String;IIB)V"),method("Qi","setMinMax","(II)V"),method("Qi","transfer","(ICI)I")),
        field("Qs","sVar","Ljava/lang/String;"),Set.of(method("Qs","<init>","(ILjava/lang/String;IIB)V"),method("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")),
        field("Qs","lastComeFrom","I"),Set.of(method("Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")));
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBankCoherence");
    private static Prepare.MethodId method(String owner,String name,String descriptor){return new Prepare.MethodId(owner,name,descriptor);}
    private static FmcQualification.Member field(String owner,String name,String descriptor){return new FmcQualification.Member(owner,name,descriptor);}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException("FMC bank witness "+why);}
    private FmcBankWriterPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);
        // Qh writes inherited metadata, but is disjoint from exact Qi/Qs
        // receiver bindings. A changed hierarchy invalidates that exclusion.
        for(String owner:List.of("Qi","Qs","Qh"))require(models.containsKey(owner)&&models.get(owner).superclass().orElseThrow().asInternalName().equals("Q"),"native bank hierarchy "+owner);
        var nav=models.get("NavRadios");require(nav!=null,"native radio initializer");
        require(!lower.containsKey("NavRadios")||Arrays.equals(lower.get("NavRadios"),data.inputs.get("NavRadios")),"unqualified radio initializer successor");
        var navMethods=nav.methods().stream().filter(m->m.methodName().equalsString("situLoadInit")&&m.methodType().equalsString("()V")).toList();
        require(navMethods.size()==1&&navMethods.getFirst().code().isPresent(),"radio initialization method");
        var navCode=Prepare.instructions(navMethods.getFirst());int navWrites=0;
        for(int i=0;i<navCode.size();i++)if(navCode.get(i) instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.name().equalsString("updated")&&f.type().equalsString("Z")){
            require(i>=4&&navCode.get(i-4) instanceof FieldInstruction array&&array.opcode()==Opcode.GETSTATIC&&array.owner().asInternalName().equals("Glob")&&array.name().equalsString("qs")&&array.type().equalsString("[LQs;")
                &&navCode.get(i-3) instanceof FieldInstruction index&&index.opcode()==Opcode.GETSTATIC&&index.owner().asInternalName().equals("Glob")&&index.name().equalsString("qsNavRadSet")&&index.type().equalsString("I")
                &&navCode.get(i-2).opcode()==Opcode.AALOAD&&navCode.get(i-1).opcode()==Opcode.ICONST_1&&f.owner().asInternalName().equals("Qs"),"radio metadata receiver");navWrites++;
        }
        require(navWrites==1,"single excluded radio bank receiver");
        var globals=Set.of(field("Glob","qs","[LQs;"),field("Glob","qsNavRadSet","I"));var globalWriters=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();globals.forEach(g->globalWriters.put(g,new HashSet<>()));
        require(models.containsKey("Glob"),"global bank owner");
        for(var g:globals)require(models.get("Glob").fields().stream().anyMatch(f->f.fieldName().equalsString(g.name())&&f.fieldType().equalsString(g.descriptor())&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0),"global binding declaration");
        for(var field:FIELDS){var owner=models.get(field.owner());require(owner!=null,"owner");
            var match=owner.fields().stream().filter(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.descriptor())).toList();
            require(match.size()==1&&(match.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0,"mutable instance field "+field);
        }
        var seen=new HashSet<Prepare.MethodId>();
        var coverage=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();FIELDS.forEach(f->coverage.put(f,new HashSet<>()));
        for(var entry:models.entrySet())for(var method:entry.getValue().methods()){
            if(method.code().isEmpty())continue;
            for(var i:Prepare.instructions(method))
            if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                var field=FmcQualification.resolve(models,FIELDS,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                var id=method(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue());
                var global=FmcQualification.resolve(models,globals,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(global!=null){require(f.opcode()==Opcode.PUTSTATIC,"global static write");globalWriters.get(global).add(id);}
                if(field!=null){
                    require(f.opcode()==Opcode.PUTFIELD,"native instance write opcode");require(WRITERS.contains(id)||CONSTRUCTORS.contains(id)||EXCLUDED.contains(id),"uncovered writer "+id);seen.add(id);coverage.get(field).add(id);
                }
            }
        }
        require(seen.containsAll(WRITERS)&&seen.containsAll(CONSTRUCTORS),"complete native writer/constructor coverage");
        require(coverage.equals(COVERAGE),"per-field resolved native writer coverage");
        require(globalWriters.values().stream().allMatch(w->w.equals(Set.of(method("Glob","initAll","()V")))),"global bank array/index writers");
        var before=new HashMap<>(lower);var changed=new HashMap<>(lower);
        for(String owner:List.of("Q","Qi","Qs")){
            byte[] bytes=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,bytes);
            var model=models.get(owner);
            for(var id:WRITERS)if(id.owner().equals(owner)){
                var methods=model.methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).toList();
                require(methods.size()==1&&methods.getFirst().code().isPresent()&&(methods.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==0,"ordinary receiver writer "+id);
            }
            byte[] output=data.cf.transformClass(model,ClassTransform.transformingMethodBodies(
                m->WRITERS.contains(method(owner,m.methodName().stringValue(),m.methodType().stringValue())),new CodeTransform(){
                    Label begin,end,handler;int entered,throwable;List<ExceptionCatch> nativeHandlers;
                    public void atStart(CodeBuilder c){
                        entered=c.allocateLocal(TypeKind.INT);throwable=c.allocateLocal(TypeKind.REFERENCE);nativeHandlers=new ArrayList<>();
                        c.iconst_0().istore(entered);guardEnter(c,entered);begin=c.newLabel();end=c.newLabel();handler=c.newLabel();c.labelBinding(begin);
                    }
                    public void accept(CodeBuilder c,CodeElement element){
                        if(element instanceof ExceptionCatch nativeHandler){nativeHandlers.add(nativeHandler);return;}
                        if(element instanceof ReturnInstruction result){
                            TypeKind kind=result.typeKind();int value=-1;if(kind!=TypeKind.VOID){value=c.allocateLocal(kind);c.storeLocal(kind,value);}
                            guardLeave(c,entered);if(value>=0)c.loadLocal(kind,value);
                        }
                        c.with(element);
                    }
                    public void atEnd(CodeBuilder c){for(var nativeHandler:nativeHandlers)c.with(nativeHandler);
                        c.labelBinding(end).exceptionCatchAll(begin,end,handler).labelBinding(handler).astore(throwable);guardLeave(c,entered);c.aload(throwable).athrow();
                    }
                }));
            require(data.cf.verify(output).isEmpty(),"generated wrapper verification "+owner);changed.put(owner,output);
        }
        byte[] globalBytes=lower.getOrDefault("Glob",data.inputs.get("Glob"));before.put("Glob",globalBytes);var globalModel=models.get("Glob");
        require(globalModel.methods().stream().noneMatch(m->m.methodName().equalsString(NAV_BANK)),"excluded bank bridge collision");
        byte[] globalOutput=data.cf.transformClass(globalModel,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
            b.withMethodBody(NAV_BANK,MethodTypeDesc.of(CD_Object),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.getstatic(ClassDesc.of("Glob"),"qs",ClassDesc.of("Qs").arrayType()).getstatic(ClassDesc.of("Glob"),"qsNavRadSet",CD_int).aaload().areturn());
        }});require(data.cf.verify(globalOutput).isEmpty(),"excluded bank bridge verification");changed.put("Glob",globalOutput);
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static void guardEnter(CodeBuilder c,int entered){
        var begin=c.newLabel();var end=c.newLabel();var fail=c.newLabel();var done=c.newLabel();
        c.labelBinding(begin).aload(0).invokestatic(HELPER,"enter",MethodTypeDesc.of(CD_int,CD_Object)).istore(entered).labelBinding(end).goto_(done)
            .exceptionCatchAll(begin,end,fail).labelBinding(fail).pop();report(c);c.labelBinding(done);
    }
    private static void guardLeave(CodeBuilder c,int entered){
        var begin=c.newLabel();var end=c.newLabel();var fail=c.newLabel();var done=c.newLabel();
        c.labelBinding(begin).iload(entered).invokestatic(HELPER,"leave",MethodTypeDesc.of(CD_void,CD_int)).labelBinding(end).goto_(done)
            .exceptionCatchAll(begin,end,fail).labelBinding(fail).pop();report(c);c.labelBinding(done);
    }
    private static void report(CodeBuilder c){
        var begin=c.newLabel();var end=c.newLabel();var fail=c.newLabel();var done=c.newLabel();
        c.labelBinding(begin).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(end).goto_(done)
            .exceptionCatchAll(begin,end,fail).labelBinding(fail).pop().labelBinding(done);
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report");Path jar=Path.of(arguments[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var output=transform(data,Map.of());var text=new StringBuilder("{\"schema\":1,\"state\":\"filtered_bank_writer_component\",\"captureQualified\":false,\"recallQualified\":false,\"classes\":[");boolean comma=false;
        for(String owner:List.of("Q","Qi","Qs","Glob")){if(comma)text.append(',');comma=true;text.append("{\"name\":").append(FmcQualification.quote(owner)).append(",\"inputSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(owner))))
            .append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(output.get(owner)))).append('}');}
        text.append("],\"limit\":\"Receiver selection/lifecycle binding/native profile and complete memory writer/exclusion coverage remain separate; no native classes defined or initialized.\"}\n");
        Files.writeString(Path.of(arguments[1]),text,StandardOpenOption.CREATE_NEW);System.out.println("Four receiver-filtered bank writer bodies qualify as data; native profile unavailable");
    }
}
