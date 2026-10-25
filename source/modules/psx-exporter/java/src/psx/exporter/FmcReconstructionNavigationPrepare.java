package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.lang.constant.*;import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Current native navigation directory, never imported route content. Native
 * navFold(null) only observes it; changing the directory is an Instructor
 * action that also replaces ScanComm's cache. No restoration writer exists. */
final class FmcReconstructionNavigationPrepare {
    static final String VALUE="$psxExporter$fmcCurrentNavigationDirectory";
    static final FmcQualification.Member DIRECTORY=new FmcQualification.Member("Core","navDir","Ljava/lang/String;");
    static Set<FmcQualification.Member> inputs(){return Set.of(DIRECTORY);}
    static void qualify(Prepare data,FmcReconstructionExecScope.Profile scope){
        var reader=new Prepare.MethodId("Core","navFold","(Ljava/lang/String;)Ljava/lang/String;");var startup=new Prepare.MethodId("Core","<clinit>","()V");
        require(FmcReconstructionPageEffects.writers(data,inputs()).getOrDefault(DIRECTORY,Set.of()).equals(Set.of(reader,startup)),"sole current directory writers");
        var method=data.methods.get(reader);var ops=Prepare.instructions(method);require(ops.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.ALOAD_0,Opcode.IFNULL,Opcode.ALOAD_0,Opcode.PUTSTATIC,Opcode.GETSTATIC,Opcode.INVOKESTATIC,Opcode.GETSTATIC,Opcode.ARETURN)),"native null/non-null directory mode");
        var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();require(code.labelToBci(((BranchInstruction)ops.get(1)).target())==14&&Prepare.field(ops.get(3),Opcode.PUTSTATIC,"Core","navDir")&&Prepare.field(ops.get(4),Opcode.GETSTATIC,"Core","navDir")&&ops.get(5) instanceof InvokeInstruction make&&make.owner().asInternalName().equals("ScanComm")&&make.name().equalsString("makeCache")&&make.type().equalsString("(Ljava/lang/String;)V")&&Prepare.field(ops.get(6),Opcode.GETSTATIC,"Core","navDir"),"directory replacement/current cache coupling");
        int nonNull=0,selected=0;var expected=Set.of(new Prepare.MethodId("Instructor","runUpdateCombo","()V"),new Prepare.MethodId("Instructor","readSelNavDb","()V"),new Prepare.MethodId("Instructor$ThreadUpdateDb","run","()V"));var setters=new HashSet<Prepare.MethodId>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var calls=Prepare.instructions(entry.getValue());for(int i=0;i<calls.size();i++)if(calls.get(i) instanceof InvokeInstruction call&&call.owner().asInternalName().equals("Core")&&call.name().equalsString("navFold")&&call.type().equalsString(reader.descriptor())){
            boolean readOnly=i>0&&calls.get(i-1).opcode()==Opcode.ACONST_NULL;if(!readOnly){nonNull++;setters.add(entry.getKey());}
            if(scope.instructions().getOrDefault(entry.getKey(),Set.of()).contains(i)){selected++;require(readOnly,"ordinary EXEC directory caller must supply native null: "+entry.getKey());}
        }}require(nonNull==4&&setters.equals(expected)&&selected>0,"exact Instructor directory setters and ordinary read-only consumers");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,FmcReconstructionExecScope.Profile scope){
        qualify(data,scope);byte[] input=lower.getOrDefault("Core",data.inputs.get("Core"));var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(VALUE)),"getter collision");var changed=new HashMap<>(lower);byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){b.withMethodBody(VALUE,MethodTypeDesc.of(CD_String),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.getstatic(ClassDesc.of("Core"),"navDir",CD_String).areturn());}});require(data.cf.verify(output).isEmpty(),"readonly getter verifier");changed.put("Core",output);var legalBefore=new HashMap<>(data.inputs);legalBefore.putAll(lower);legalBefore.put("Core",input);var legalAfter=new HashMap<>(legalBefore);legalAfter.putAll(changed);TerrainAccess.verifyAdded(legalBefore,legalAfter,data.inputs);return Map.copyOf(changed);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC current navigation directory "+why);}
}
