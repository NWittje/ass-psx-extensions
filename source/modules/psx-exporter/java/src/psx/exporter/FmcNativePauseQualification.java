package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Qualification of the installed ordinary-Pause component, as class data.
 * No installed class is defined or initialized here. This proves neither FMC
 * ownership nor recall exclusion; those are separate activation conditions. */
public final class FmcNativePauseQualification {
    static final Map<String,String> ORIGINAL=Map.of(
        "Core","d522384326e98914fb2d2028588a6c2c8d1aa8dc7ffdfb7582aee5be49c7e037",
        "Qi","c3a7b8ae1a67ee4ba0e9c08256b2c5c48f8a9d9024e0f939dd0e862820a666dc",
        "Glob","d8c552ccaaabf80f92358af7edb4abdd4ba9e77ad6d432fe38889ce655b3dec1");
    static final List<Prepare.MethodId> NATIVE=List.of(
        new Prepare.MethodId("Core","isPaused","()Z"),new Prepare.MethodId("Core","setPaused","(I)V"),
        new Prepare.MethodId("Core","setGlobPauseMode","(II)V"),new Prepare.MethodId("Qi","transfer","(ICI)I"));
    record Result(Map<String,byte[]> changed,Map<String,String> originalHashes,Map<String,String> preparedHashes,Map<String,String> nativeBodies){}
    private FmcNativePauseQualification(){}
    static void require(boolean value,String reason){if(!value)throw new IllegalArgumentException("installed FMC Pause "+reason);}
    // Prepare accepts only pristine input classes. Inspect composed output as
    // class data directly, keeping its reinstrumentation rejection intact.
    static MethodModel method(Map<String,byte[]> classes,Prepare.MethodId id){
        byte[] bytes=classes.get(id.owner());require(bytes!=null,"final class missing: "+id.owner());
        return ClassFile.of().parse(bytes).methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor()))
            .findFirst().orElseThrow(()->new IllegalArgumentException("installed FMC Pause final method missing: "+id));
    }

    // Canonical instruction ordinals preserve branch and exception topology,
    // independently of constant-pool indices or rewritten stack-map encoding.
    static List<String> body(MethodModel method){
        var code=method.code().orElseThrow();var labels=new HashMap<Label,Integer>();int ordinal=0;
        for(var element:code){if(element instanceof LabelTarget label)labels.put(label.label(),ordinal);if(element instanceof Instruction)ordinal++;}
        var tokens=new ArrayList<String>();
        for(var element:code){
            if(element instanceof Instruction instruction){
                String value=instruction.opcode().name();
                if(instruction instanceof FieldInstruction field)value+=" "+field.owner().asInternalName()+"."+field.name().stringValue()+field.type().stringValue();
                else if(instruction instanceof InvokeInstruction call)value+=" "+call.owner().asInternalName()+"."+call.name().stringValue()+call.type().stringValue();
                else if(instruction instanceof BranchInstruction branch)value+=" "+labels.get(branch.target());
                else if(instruction instanceof LoadInstruction load)value+=" "+load.slot();
                else if(instruction instanceof StoreInstruction store)value+=" "+store.slot();
                else if(instruction instanceof IncrementInstruction increment)value+=" "+increment.slot()+" "+increment.constant();
                else if(instruction instanceof ConstantInstruction constant)value+=" "+constant.constantValue();
                // These four pinned methods contain only the operand kinds
                // above and intrinsic arithmetic/array/monitor/return opcodes.
                else require(Set.of(Opcode.Kind.ARRAY_LOAD,Opcode.Kind.ARRAY_STORE,Opcode.Kind.STACK,Opcode.Kind.CONVERT,
                    Opcode.Kind.OPERATOR,Opcode.Kind.RETURN,Opcode.Kind.THROW_EXCEPTION,Opcode.Kind.MONITOR,Opcode.Kind.NOP)
                    .contains(instruction.opcode().kind()),"unexpected native operand kind");
                tokens.add(value);
            }else if(element instanceof ExceptionCatch caught)tokens.add("CATCH "+labels.get(caught.tryStart())+" "+labels.get(caught.tryEnd())+" "+labels.get(caught.handler())+" "+caught.catchType().map(c->c.asInternalName()).orElse("any"));
        }
        return List.copyOf(tokens);
    }
    static Result prepare(Prepare data,Map<String,byte[]> lower)throws Exception{
        var original=new TreeMap<String,String>();
        for(var pin:ORIGINAL.entrySet()){
            byte[] bytes=data.inputs.get(pin.getKey());require(bytes!=null&&Identity.hash(bytes).equalsIgnoreCase(pin.getValue()),"class identity changed: "+pin.getKey());original.put(pin.getKey(),Identity.hash(bytes));
        }
        var changed=FmcPausePrepare.transform(data,lower);var combined=new HashMap<>(data.inputs);combined.putAll(lower);combined.putAll(changed);
        var bodies=new TreeMap<String,String>();
        for(var id:NATIVE){
            MethodModel before=data.methods.get(id),after=method(combined,id);require(before!=null,"native method missing");
            require(before.flags().flagsMask()==after.flags().flagsMask()&&body(before).equals(body(after)),"native body/topology changed: "+id);
            bodies.put(id.toString(),Identity.hash(String.join("\n",body(before)).getBytes(java.nio.charset.StandardCharsets.UTF_8)));
        }
        var requested=method(combined,new Prepare.MethodId("Qi",FmcPausePrepare.PREFIX+"Requested","()I"));
        int requestedFlags=ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_SYNTHETIC;
        require(requested.flags().flagsMask()==requestedFlags,"peek monitor/access flags");
        var ops=Prepare.instructions(requested);require(ops.size()==3&&ops.get(0).opcode()==Opcode.ALOAD_0&&ops.get(1) instanceof FieldInstruction field&&field.opcode()==Opcode.GETFIELD&&field.owner().asInternalName().equals("Qi")&&field.name().equalsString("iVar")&&field.type().equalsString("I")&&ops.get(2).opcode()==Opcode.IRETURN,"peek consumes freshness or writes native state");
        // Access checks use the final vendor classes and exact helpers already
        // resolved by FmcPausePrepare. Preserve lower-layer additions as well.
        for(var entry:lower.entrySet())if(!changed.containsKey(entry.getKey()))require(Arrays.equals(entry.getValue(),combined.get(entry.getKey())),"lower layer changed");
        var hashes=new TreeMap<String,String>();for(var entry:changed.entrySet())hashes.put(entry.getKey(),Identity.hash(entry.getValue()));
        return new Result(Map.copyOf(changed),Map.copyOf(original),Map.copyOf(hashes),Map.copyOf(bodies));
    }
    static String report(Result result){
        var b=new StringBuilder("{\"schema\":1,\"state\":\"ordinary_pause_component_only\",\"captureQualified\":false,\"recallQualified\":false");
        for(var item:List.of(Map.entry("originalClasses",result.originalHashes),Map.entry("preparedClasses",result.preparedHashes),Map.entry("nativeBodies",result.nativeBodies))){
            b.append(',').append(FmcQualification.quote(item.getKey())).append(":{");boolean comma=false;
            for(var entry:new TreeMap<>(item.getValue()).entrySet()){if(comma)b.append(',');comma=true;b.append(FmcQualification.quote(entry.getKey())).append(':').append(FmcQualification.quote(entry.getValue()));}b.append('}');
        }
        return b.append(",\"limits\":\"Native thread/binding, partial-Pause receipt, complete FMC memory, aliases and exclusion remain separate obligations. No installed classes defined or initialized.\"}").toString();
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=2)throw new IllegalArgumentException("installed JAR and owned report required");Path jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"JAR identity");
        var result=prepare(new Prepare(Prepare.readJar(jar)),Map.of());Files.writeString(Path.of(args[1]),report(result),StandardOpenOption.CREATE_NEW);
        System.out.println("Installed ordinary FMC Pause component: three exact class inputs, four unchanged native bodies, legal generated synchronized pure peek and final class verification passed; FMC capture/recall remain unqualified");
    }
}
