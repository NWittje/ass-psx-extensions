package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Preparation-only content derivative. The native cost-index cache depends
 * only on its restored input. Qualify the complete calculation and its native
 * assignment before adding an unused legal same-owner bridge. Broader native
 * performance routines also affect live systems and are deliberately absent.
 * Complete content ownership, exclusion, initialization and recall dispatch
 * are separate gates; this component advertises no runtime capability. */
public final class FmcCostIndexReconcilePrepare {
    static final String OWNER="FmcPerf", CALCULATOR="FmcVnavCalc";
    static final String FUNCTION="getCostIndexFacClbDes";
    static final String BRIDGE="$psxExporter$fmcReconcileCostIndex";
    private FmcCostIndexReconcilePrepare(){}
    private static void require(boolean value,String why){
        if(!value)throw new IllegalArgumentException("FMC cost-index reconciliation "+why);
    }
    private static ClassModel model(Prepare data,Map<String,byte[]> lower,String owner){
        byte[] bytes=lower.getOrDefault(owner,data.inputs.get(owner));
        require(bytes!=null,"missing owner "+owner);return data.cf.parse(bytes);
    }
    private static void field(ClassModel model,String name,String type){
        var fields=model.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
        require(fields.size()==1&&fields.getFirst().fieldType().equalsString(type),"field descriptor "+name);
        require((fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,
            "mutable static field "+name);
    }
    private static MethodModel method(ClassModel owner,String name,String descriptor){
        var methods=owner.methods().stream().filter(m->m.methodName().equalsString(name)
            &&m.methodType().equalsString(descriptor)).toList();
        require(methods.size()==1,"method descriptor "+name+descriptor);return methods.getFirst();
    }
    private static boolean access(Instruction instruction,Opcode opcode,String name,String descriptor){
        return instruction instanceof FieldInstruction f&&f.opcode()==opcode
            &&f.owner().asInternalName().equals(OWNER)&&f.name().equalsString(name)&&f.type().equalsString(descriptor);
    }
    private static boolean call(Instruction instruction){
        return instruction instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKESTATIC
            &&c.owner().asInternalName().equals(CALCULATOR)&&c.name().equalsString(FUNCTION)&&c.type().equalsString("(F)F");
    }
    private static void constant(List<Instruction> code,int index,float expected){
        require(code.get(index) instanceof ConstantInstruction c&&c.constantValue() instanceof Float f
            &&Float.floatToRawIntBits(f)==Float.floatToRawIntBits(expected),"constant at "+index);
    }
    private static void pureCalculation(MethodModel method){
        require((method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC
            &&method.code().isPresent(),"calculator flags/body");
        var body=method.code().orElseThrow();require(body.exceptionHandlers().isEmpty(),"calculator exception handler");
        require(body instanceof CodeAttribute,"parsed calculator code attribute");var attribute=(CodeAttribute)body;
        var code=Prepare.instructions(method);require(code.size()==28,"calculator instruction count");
        for(int i:new int[]{0,6,12,16,21})require(code.get(i) instanceof LoadInstruction l&&l.slot()==0
            &&(l.opcode()==Opcode.FLOAD_0||l.opcode()==Opcode.FLOAD),"input load at "+i);
        int[] indices={1,4,7,10,13,17,20,22,24};
        float[] values={0f,0f,9999f,1f,400f,460f,0.8696f,400f,1.35847E-5f};
        for(int i=0;i<indices.length;i++)constant(code,indices[i],values[i]);
        int[] operations={2,8,14,18,23,25,26,5,11,19,27};
        Opcode[] opcodes={Opcode.FCMPG,Opcode.FCMPL,Opcode.FCMPG,Opcode.FDIV,Opcode.FSUB,Opcode.FMUL,Opcode.FADD,
            Opcode.FRETURN,Opcode.FRETURN,Opcode.FRETURN,Opcode.FRETURN};
        for(int i=0;i<operations.length;i++)require(code.get(operations[i]).opcode()==opcodes[i],"calculator operation at "+operations[i]);
        // Resolve each branch to an instruction, not a historical byte offset.
        // This preserves comparison/NaN behavior and accepts constant-pool width
        // changes without weakening the control-flow qualification.
        int[] bci=new int[code.size()+1];for(int i=0;i<code.size();i++)bci[i+1]=bci[i]+code.get(i).sizeInBytes();
        int[] branches={3,9,15},targets={6,12,20};Opcode[] conditions={Opcode.IFGT,Opcode.IFLE,Opcode.IFGE};
        for(int i=0;i<branches.length;i++)require(code.get(branches[i]) instanceof BranchInstruction b
            &&b.opcode()==conditions[i]&&attribute.labelToBci(b.target())==bci[targets[i]],"calculator branch at "+branches[i]);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var perf=model(data,lower,OWNER);var calculator=model(data,lower,CALCULATOR);
        field(perf,"costIndex","I");field(perf,"costIndexFac","F");
        pureCalculation(method(calculator,FUNCTION,"(F)F"));
        var reader=method(perf,"readGlobAll","()V");
        require((reader.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==ClassFile.ACC_STATIC
            &&reader.code().isPresent(),"native input-reader flags/body");
        var instructions=Prepare.instructions(reader);int assignments=0;
        for(int i=0;i+3<instructions.size();i++)if(access(instructions.get(i),Opcode.GETSTATIC,"costIndex","I")
            &&instructions.get(i+1).opcode()==Opcode.I2F&&call(instructions.get(i+2))
            &&access(instructions.get(i+3),Opcode.PUTSTATIC,"costIndexFac","F"))assignments++;
        require(assignments==1,"unique native input/derivative relationship");
        require(perf.methods().stream().noneMatch(m->m.methodName().equalsString(BRIDGE)),"bridge collision");
        ClassDesc p=ClassDesc.of(OWNER),c=ClassDesc.of(CALCULATOR);
        byte[] output=data.cf.transformClass(perf,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(BRIDGE,MethodTypeDesc.of(CD_void),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,
                    code->code.getstatic(p,"costIndex",CD_int).i2f().invokestatic(c,FUNCTION,MethodTypeDesc.of(CD_float,CD_float))
                        .putstatic(p,"costIndexFac",CD_float).return_());
            }
        });
        require(data.cf.verify(output).isEmpty(),"generated verifier");
        var before=new HashMap<>(lower);before.put(OWNER,lower.getOrDefault(OWNER,data.inputs.get(OWNER)));
        var changed=new HashMap<>(lower);changed.put(OWNER,output);
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        return Map.copyOf(changed);
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report required");Path jar=Path.of(args[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"cost_index_reconciliation_component_only\",\"captureQualified\":false,\"recallQualified\":false,"
            +"\"input\":\"FmcPerf.costIndex:I\",\"output\":\"FmcPerf.costIndexFac:F\",\"calculator\":\"FmcVnavCalc.getCostIndexFacClbDes(F)F\","
            +"\"qualifiedInstructions\":28,\"perfInputSha256\":\""+Identity.hash(data.inputs.get(OWNER))
            +"\",\"calculatorInputSha256\":\""+Identity.hash(data.inputs.get(CALCULATOR))+"\",\"preparedSha256\":\""+Identity.hash(changed.get(OWNER))
            +"\",\"limits\":\"Installed classes inspected as data; helper unused. Complete native profile, initialization, exclusion, cache reconciliation, adapter/JNI/service and loaded acceptance remain unqualified.\"}\n";
        Files.writeString(Path.of(args[1]),report,StandardOpenOption.CREATE_NEW);
        System.out.println("Cost-index reconciliation qualifies as class data; no native custom FMC capture/recall capability");
    }
}
