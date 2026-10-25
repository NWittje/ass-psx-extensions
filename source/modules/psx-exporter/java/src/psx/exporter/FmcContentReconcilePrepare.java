package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Preparation-only component of content recall. Qualify the installed native
 * mass calculation before exposing an equivalent legal same-owner bridge.
 * The saved input is zero-fuel weight; fuel comes from the CURRENT native FMC
 * reference, never from the artifact. Native mass units and float arithmetic
 * are retained. Freshness/units of that reference, input ownership, exclusion,
 * initialization and the complete content profile remain separate gates.
 *
 * Only the qualified ordinary-thread recall dispatcher may use this bridge.
 * No original callback, constructor, parser or initializer is changed. This
 * component does not advertise capture/recall or run installed classes. */
public final class FmcContentReconcilePrepare {
    static final String OWNER="FmcPerf", FUEL_OWNER="FmcProgress2";
    static final String BRIDGE="$psxExporter$fmcReconcileCurrentMass";
    private FmcContentReconcilePrepare(){}
    private static void require(boolean value,String why){
        if(!value)throw new IllegalArgumentException("FMC current-mass reconciliation "+why);
    }
    private static ClassModel model(Prepare data,Map<String,byte[]> lower,String name){
        byte[] bytes=lower.getOrDefault(name,data.inputs.get(name));
        require(bytes!=null,"missing owner "+name);return data.cf.parse(bytes);
    }
    private static void field(ClassModel model,String name){
        var fields=model.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
        require(fields.size()==1&&fields.getFirst().fieldType().equalsString("F"),"field descriptor "+name);
        require((fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,
            "mutable static field "+name);
    }
    private static boolean access(Instruction instruction,Opcode op,String owner,String name){
        return instruction instanceof FieldInstruction f&&f.opcode()==op
            &&f.owner().asInternalName().equals(owner)&&f.name().equalsString(name)&&f.type().equalsString("F");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var perf=model(data,lower,OWNER);var fuel=model(data,lower,FUEL_OWNER);
        field(perf,"zfWeight");field(perf,"grWeight");field(fuel,"refTotFuelQty1000");
        var methods=perf.methods().stream().filter(m->m.methodName().equalsString("calcGrWtSum")
            &&m.methodType().equalsString("()V")).toList();
        require(methods.size()==1,"native calculation descriptor");var method=methods.getFirst();
        require((method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC
            &&method.code().isPresent(),"native calculation flags/body");
        require(method.code().orElseThrow().exceptionHandlers().isEmpty(),"native calculation exception handler");
        var instructions=Prepare.instructions(method);
        require(instructions.size()==5&&access(instructions.get(0),Opcode.GETSTATIC,OWNER,"zfWeight")
            &&access(instructions.get(1),Opcode.GETSTATIC,FUEL_OWNER,"refTotFuelQty1000")
            &&instructions.get(2).opcode()==Opcode.FADD
            &&access(instructions.get(3),Opcode.PUTSTATIC,OWNER,"grWeight")
            &&instructions.get(4).opcode()==Opcode.RETURN,"native calculation instruction pattern");
        require(perf.methods().stream().noneMatch(m->m.methodName().equalsString(BRIDGE)),"bridge collision");
        ClassDesc p=ClassDesc.of(OWNER),f=ClassDesc.of(FUEL_OWNER);
        byte[] output=data.cf.transformClass(perf,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(BRIDGE,MethodTypeDesc.of(CD_void),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->
                    c.getstatic(p,"zfWeight",CD_float).getstatic(f,"refTotFuelQty1000",CD_float)
                        .fadd().putstatic(p,"grWeight",CD_float).return_());
            }
        });
        require(data.cf.verify(output).isEmpty(),"generated verifier");
        var before=new HashMap<>(lower);before.put(OWNER,lower.getOrDefault(OWNER,data.inputs.get(OWNER)));
        var changed=new HashMap<>(lower);changed.put(OWNER,output);
        // Check the generated field references from their actual caller against
        // final transformed owners, including non-public default-package types.
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        return Map.copyOf(changed);
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report required");Path jar=Path.of(args[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"current_mass_reconciliation_component_only\",\"captureQualified\":false,\"recallQualified\":false,"
            +"\"inputOwner\":\"FmcPerf.zfWeight:F\",\"currentReference\":\"FmcProgress2.refTotFuelQty1000:F\",\"output\":\"FmcPerf.grWeight:F\","
            +"\"arithmetic\":\"native float addition, native mass units unchanged\",\"perfInputSha256\":\""+Identity.hash(data.inputs.get(OWNER))
            +"\",\"fuelInputSha256\":\""+Identity.hash(data.inputs.get(FUEL_OWNER))+"\",\"preparedSha256\":\""+Identity.hash(changed.get(OWNER))
            +"\",\"limits\":\"No installed class definition/init/invocation. Current fuel-reference freshness/units, profile ownership, coherent exclusion, initialization, remaining geometry/prediction reconciliation, JNI/service and loaded acceptance remain unqualified.\"}\n";
        Files.writeString(Path.of(args[1]),report,StandardOpenOption.CREATE_NEW);
        System.out.println("Current-mass reconciliation qualifies as class data; no native custom FMC capture/recall capability");
    }
}
