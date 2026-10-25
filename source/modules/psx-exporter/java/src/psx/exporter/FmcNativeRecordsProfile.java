package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;

/** Installed native serialization contract, inspected without defining vendor classes. */
public final class FmcNativeRecordsProfile {
    public enum Bank { QI, QH, QS }
    public record Slot(Bank bank, int index, String selector, String key) {
        public Slot { Objects.requireNonNull(bank); Objects.requireNonNull(selector); Objects.requireNonNull(key); if(index<0)throw new IllegalArgumentException("negative native bank index"); }
    }
    public static final String JAR_SHA256="96B7504A58920794394FF0B7B6A4E33189A37965E836119E845A34164EDE8285";
    private static final String WORLD_SHA256="3F9B8D36FDE9709C3CB224013E13747807C57495E91BD65DE3318E9E17F339DF";
    /* These are aircraft configuration, physical controls or printer state read
       by an FMC callback, rather than FMC/CDU serialized state. Native reload
       still observes their current values and keeps every original side effect. */
    private static final Set<String> CONTEXT=Set.of("qiWxAutoSet","qiMcpWdoSpd","qiMcpWdoAlt","qiTimeAccel","qsMfSelfInduced","qsPrinterText","qsNavRadSet");
    public static final class Profile {
        private final List<Slot> slots; private final byte[] identity;
        private Profile(List<Slot> slots,byte[] identity){this.slots=List.copyOf(slots);this.identity=identity.clone();}
        public List<Slot> slots(){return slots;}
        public List<Slot> slots(Bank bank){return slots.stream().filter(s->s.bank()==bank).toList();}
        public byte[] identity(){return identity.clone();}
        public Set<String> currentContextSelectors(){return CONTEXT;}
        /** Bridge entry/record receiver prerequisites; descendants execute only
            through their unchanged ordinary native initialization calls. */
        public String[] initializedOwners(){return new String[]{"Glob","Qi","Qh","Qs","Core","CduHub","Option","RoutesStby","RoutesStbyProg","SituationFiler"};}
    }
    private record NativeSlot(Slot slot,int netMode){}
    private FmcNativeRecordsProfile(){}

    /** Consume offline-qualified slot metadata without repeating native class
     * analysis in the running simulator. The pinned digest binds every tuple. */
    static Profile fromPrepared(List<Slot> slots){
        if(slots.size()!=201)throw new IllegalArgumentException("prepared native record count");
        MessageDigest hash=digest();feed(hash,JAR_SHA256);feed(hash,WORLD_SHA256);
        feed(hash,"native-records-v1;required-current-native;Qi/Qh/Qs:w:1;Option.readGlobAirlinePol;CduHub.situLoadInit-once;RoutesStby.situLoadInit;Core.setPaused(1);Core.isPaused");
        for(Slot slot:slots){feed(hash,slot.bank().name());integer(hash,slot.index());feed(hash,slot.selector());feed(hash,slot.key());}
        byte[] identity=hash.digest();
        if(!HexFormat.of().formatHex(identity).equals("b70621d6333f63812d56bc66022fdfa217cd709888c3399dcd5ef99fc94d3bf5"))
            throw new IllegalArgumentException("prepared native profile identity");
        return new Profile(slots,identity);
    }

    static Profile inspect(Prepare data){
        verifyWorld(data);
        Map<String,NativeSlot> all=decodeGlob(data);
        Set<String> read=new TreeSet<>();
        for(var entry:data.methods.entrySet())if(domain(entry.getKey().owner())&&entry.getValue().code().isPresent())
            for(Instruction op:entry.getValue().code().orElseThrow().elementStream().filter(Instruction.class::isInstance).map(Instruction.class::cast).toList())
                if(op instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("Glob")&&f.type().equalsString("I")&&all.containsKey(f.name().stringValue()))read.add(f.name().stringValue());
        // Option.monitor owns the native airline-policy parser. Its saved FMC
        // policy record is part of the snapshot even though Option is shared.
        if(!hasRead(data,"Option","readGlobAirlinePol","()V","qsFmcAirlPol")||!hasRead(data,"Option","monitor","()V","qsFmcAirlPol"))throw new IllegalArgumentException("native FMC airline-policy reader");
        read.add("qsFmcAirlPol");
        if(!read.containsAll(CONTEXT))throw new IllegalArgumentException("native aircraft-context census");
        read.removeAll(CONTEXT);
        List<Slot> slots=read.stream().map(all::get).filter(s->s.netMode()<11).map(NativeSlot::slot)
            .sorted(Comparator.comparing(Slot::bank).thenComparingInt(Slot::index)).toList();
        if(slots.size()!=201||new HashSet<>(slots.stream().map(Slot::key).toList()).size()!=slots.size())throw new IllegalArgumentException("complete native FMC/CDU record census "+slots.size());
        MethodModel init=data.requireStatic("CduHub","situLoadInit");
        List<Instruction> code=instructions(init);
        if(code.stream().filter(i->Prepare.call(i,"FmcGlobal","situLoadInit","()V")).count()!=1||
            !(code.get(code.size()-2) instanceof InvokeInstruction end)||!Prepare.call(end,"FmcGlobal","situLoadInit","()V")||code.getLast().opcode()!=Opcode.RETURN)
            throw new IllegalArgumentException("complete native CDU initialization tail");
        requireTransfer(data,"Qi","(ICI)I");requireTransfer(data,"Qh","(ICI)I");requireTransfer(data,"Qs","(Ljava/lang/String;CI)Ljava/lang/String;");
        data.requireMethod("Core","setPaused","(I)V");data.requireMethod("Core","isPaused","()Z");
        data.requireStatic("Core","setLoaderPaused");
        MessageDigest hash=digest();feed(hash,JAR_SHA256);feed(hash,WORLD_SHA256);
        data.requireStatic("Option","readGlobAirlinePol");data.requireStatic("RoutesStby","situLoadInit");
        feed(hash,"native-records-v1;required-current-native;Qi/Qh/Qs:w:1;Option.readGlobAirlinePol;CduHub.situLoadInit-once;RoutesStby.situLoadInit;Core.setPaused(1);Core.isPaused");
        for(Slot slot:slots){feed(hash,slot.bank().name());integer(hash,slot.index());feed(hash,slot.selector());feed(hash,slot.key());}
        return new Profile(slots,hash.digest());
    }
    private static boolean domain(String owner){return owner.startsWith("Fmc")||owner.equals("CduHub")||owner.startsWith("ControlDisplayUnit")||owner.startsWith("Routes")||owner.startsWith("RouteTools")||owner.startsWith("Fans")||owner.startsWith("AltnMessages")||owner.startsWith("AltnEfisMessages")||owner.startsWith("AcarsMessages");}
    private static boolean hasRead(Prepare d,String owner,String name,String desc,String field){return instructions(d.requireMethod(owner,name,desc)).stream().anyMatch(i->Prepare.field(i,Opcode.GETSTATIC,"Glob",field));}
    private static void requireTransfer(Prepare d,String owner,String desc){MethodModel m=d.requireMethod(owner,"transfer",desc);if((m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE))!=0)throw new IllegalArgumentException("native same-package transfer visibility "+owner);}
    private static List<Instruction> instructions(MethodModel m){return m.code().orElseThrow().elementStream().filter(Instruction.class::isInstance).map(Instruction.class::cast).toList();}
    private static Map<String,NativeSlot> decodeGlob(Prepare d){
        List<Instruction> ops=instructions(d.requireStatic("Glob","initAll"));
        Map<String,Integer> index=new HashMap<>();Map<String,Bank> bank=new HashMap<>();EnumMap<Bank,Integer> counts=new EnumMap<>(Bank.class);
        for(int n=0;n+1<ops.size();n++)if(ops.get(n) instanceof InvokeInstruction v&&v.owner().asInternalName().equals("Glob")&&Set.of("iterateQi","iterateQh","iterateQs").contains(v.name().stringValue())){
            Bank kind=kind(v.name().stringValue().substring(7));
            if(!(ops.get(n+1) instanceof FieldInstruction f)||f.opcode()!=Opcode.PUTSTATIC||!f.owner().asInternalName().equals("Glob")||!f.type().equalsString("I"))throw new IllegalArgumentException("native index producer");
            String field=f.name().stringValue();int at=counts.getOrDefault(kind,0);counts.put(kind,at+1);if(index.put(field,at)!=null)throw new IllegalArgumentException("duplicate native selector");bank.put(field,kind);
        }
        if(!counts.equals(Map.of(Bank.QI,278,Bank.QH,431,Bank.QS,575)))throw new IllegalArgumentException("native bank lengths");
        Map<String,NativeSlot> result=new HashMap<>();
        for(int n=0;n<ops.size();n++)if(ops.get(n) instanceof NewObjectInstruction made&&Set.of("Qi","Qh","Qs").contains(made.className().asInternalName())){
            String owner=made.className().asInternalName();Bank kind=kind(owner);int call=n+1;while(call<ops.size()&&!(ops.get(call) instanceof InvokeInstruction))call++;
            if(n<2||call>=ops.size()||!(ops.get(call) instanceof InvokeInstruction ctor)||ctor.opcode()!=Opcode.INVOKESPECIAL||!ctor.owner().asInternalName().equals(owner)||!ctor.name().equalsString("<init>")||ops.get(call+1).opcode()!=Opcode.AASTORE||ops.get(n+1).opcode()!=Opcode.DUP)
                throw new IllegalArgumentException("native bank construction");
            FieldInstruction selector=field(ops.get(n-1),Opcode.GETSTATIC,"Glob","I");String name=selector.name().stringValue();
            FieldInstruction array=field(ops.get(n-2),Opcode.GETSTATIC,"Glob","[L"+owner+";");
            if(!array.name().equalsString(owner.toLowerCase(Locale.ROOT))||!field(ops.get(n+2),Opcode.GETSTATIC,"Glob","I").name().equalsString(name)||bank.get(name)!=kind)throw new IllegalArgumentException("native bank selector");
            if(!(ops.get(n+3) instanceof ConstantInstruction key)||!(key.constantValue() instanceof String text)||!(ops.get(n+6) instanceof ConstantInstruction mode)||!(mode.constantValue() instanceof Integer value))throw new IllegalArgumentException("native record key/mode");
            if(result.put(name,new NativeSlot(new Slot(kind,index.get(name),name,text),value))!=null)throw new IllegalArgumentException("duplicate native bank construction");
        }
        if(result.size()!=1284||!result.keySet().equals(index.keySet()))throw new IllegalArgumentException("native bank construction completeness");return result;
    }
    private static FieldInstruction field(Instruction op,Opcode opcode,String owner,String type){if(!(op instanceof FieldInstruction f)||f.opcode()!=opcode||!f.owner().asInternalName().equals(owner)||!f.type().equalsString(type))throw new IllegalArgumentException("native bank operand");return f;}
    private static Bank kind(String name){return switch(name){case "Qi"->Bank.QI;case "Qh"->Bank.QH;case "Qs"->Bank.QS;default->throw new IllegalArgumentException("native bank kind");};}
    private static void verifyWorld(Prepare d){
        if(d.inputs.size()!=4206||!d.models.keySet().equals(d.inputs.keySet()))throw new IllegalArgumentException("native source world");
        MessageDigest hash=digest();int methods=0;
        for(var item:new TreeMap<>(d.inputs).entrySet()){
            String owner=item.getKey();byte[] bytes=item.getValue();feed(hash,owner);integer(hash,bytes.length);hash.update(bytes);
            ClassModel model=d.models.get(owner);
            if(!(model.constantPool() instanceof ClassReader reader)||!Arrays.equals(bytes,reader.readBytes(0,reader.classfileLength()))||!model.thisClass().asInternalName().equals(owner))throw new IllegalArgumentException("native model backing "+owner);
            for(MethodModel m:model.methods()){
                if(d.methods.get(new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue()))!=m)throw new IllegalArgumentException("native method index "+owner);methods++;
            }
        }
        if(methods!=25411||d.methods.size()!=methods||!HexFormat.of().withUpperCase().formatHex(hash.digest()).equals(WORLD_SHA256))throw new IllegalArgumentException("installed native source identity");
    }
    private static MessageDigest digest(){try{return MessageDigest.getInstance("SHA-256");}catch(NoSuchAlgorithmException e){throw new AssertionError(e);}}
    private static void feed(MessageDigest hash,String text){byte[] bytes=text.getBytes(StandardCharsets.UTF_8);integer(hash,bytes.length);hash.update(bytes);}
    private static void integer(MessageDigest h,int x){h.update((byte)(x>>>24));h.update((byte)(x>>>16));h.update((byte)(x>>>8));h.update((byte)x);}
}
