package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Isolated native reserve/CG arithmetic, without the ordinary input reader's
 * fuel, caution, VNAV or display actions. Raw native units are unchanged. This
 * unused component grants no recall authority or input-bank coherence. */
public final class FmcPerfDerivedPrepare {
    static final String BRIDGE="$psxExporter$fmcReconcilePerfDerived";
    private FmcPerfDerivedPrepare(){}
    private static void require(boolean yes,String reason){if(!yes)throw new IllegalArgumentException("FMC PERF derivative "+reason);}
    private static boolean constant(Instruction i,int value){return i instanceof ConstantInstruction c&&c.constantValue() instanceof Integer v&&v==value;}
    private static boolean load(Instruction i,int slot){return i instanceof LoadInstruction l&&l.slot()==slot&&(l.opcode()==Opcode.ILOAD||l.opcode()==Opcode.ILOAD_0||l.opcode()==Opcode.ILOAD_1||l.opcode()==Opcode.ILOAD_2||l.opcode()==Opcode.ILOAD_3);}
    private static boolean store(Instruction i,String name){return i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("FmcPerf")&&f.name().equalsString(name)&&f.type().equalsString("I");}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        byte[] input=lower.getOrDefault("FmcPerf",data.inputs.get("FmcPerf"));require(input!=null,"owner");var model=data.cf.parse(input);
        var finalModels=FmcQualification.finalModels(data,lower);
        var caches=Set.of(new FmcQualification.Member("FmcPerf","fuelReservesInUse","I"),new FmcQualification.Member("FmcPerf","crzCgAltAdd","I"));
        var writers=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();caches.forEach(f->writers.put(f,new HashSet<>()));
        for(var entry:finalModels.entrySet())for(var method:entry.getValue().methods()){
            if(method.code().isEmpty())continue;
            for(var i:Prepare.instructions(method))if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){
                var owner=FmcQualification.resolve(finalModels,caches,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(owner!=null)writers.get(owner).add(new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()));
            }
        }
        for(var cache:caches)require(writers.get(cache).equals(Set.of(new Prepare.MethodId("FmcPerf","readGlobAll","()V"))),"final cache writer coverage "+cache);
        for(String name:List.of("fuelReserves","fuelReservesInUse","crzCg","crzCgAltAdd")){
            var fields=model.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
            require(fields.size()==1&&fields.getFirst().fieldType().equalsString("I")&&(fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,"mutable static descriptor "+name);
        }
        var readers=model.methods().stream().filter(m->m.methodName().equalsString("readGlobAll")&&m.methodType().equalsString("()V")).toList();
        require(readers.size()==1&&readers.getFirst().code().isPresent()&&(readers.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT))==ClassFile.ACC_STATIC,"reader");
        var reader=readers.getFirst();var body=reader.code().orElseThrow();require(body instanceof CodeAttribute,"parsed native code");var attribute=(CodeAttribute)body;
        var instructions=Prepare.instructions(reader);int[] offsets=new int[instructions.size()+1];for(int i=0;i<instructions.size();i++)offsets[i+1]=offsets[i]+instructions.get(i).sizeInBytes();
        int reserves=0,cg=0;
        for(int i=0;i<instructions.size();i++){
            if(store(instructions.get(i),"fuelReservesInUse")){
                require(i>=7&&instructions.get(i-4) instanceof BranchInstruction branch&&branch.opcode()==Opcode.IFGE,"reserve conditional");
                var branch=(BranchInstruction)instructions.get(i-4);require(instructions.get(i-5) instanceof LoadInstruction,"reserve input local");
                int slot=((LoadInstruction)instructions.get(i-5)).slot();
                require(load(instructions.get(i-7),slot)&&store(instructions.get(i-6),"fuelReserves"),"reserve input assignment");
                require(load(instructions.get(i-5),slot)&&constant(instructions.get(i-3),4000)&&instructions.get(i-2) instanceof BranchInstruction jump&&jump.opcode()==Opcode.GOTO
                    &&attribute.labelToBci(branch.target())==offsets[i-1]&&attribute.labelToBci(((BranchInstruction)instructions.get(i-2)).target())==offsets[i]
                    &&load(instructions.get(i-1),slot),"reserve default/input relationship");
                reserves++;
            }
            if(store(instructions.get(i),"crzCgAltAdd")){
                require(i>=7&&constant(instructions.get(i-7),1600)&&instructions.get(i-6) instanceof LoadInstruction
                    &&load(instructions.get(i-6),((LoadInstruction)instructions.get(i-6)).slot())&&constant(instructions.get(i-5),85)
                    &&instructions.get(i-4).opcode()==Opcode.ISUB&&instructions.get(i-3).opcode()==Opcode.IMUL
                    &&constant(instructions.get(i-2),245)&&instructions.get(i-1).opcode()==Opcode.IDIV,"native CG integer formula");
                int slot=((LoadInstruction)instructions.get(i-6)).slot();
                require(i+9<instructions.size()&&instructions.get(i+1) instanceof FieldInstruction field&&field.opcode()==Opcode.GETSTATIC&&field.owner().asInternalName().equals("FmcPerf")&&field.name().equalsString("crzCg")&&field.type().equalsString("I")
                    &&load(instructions.get(i+2),slot)&&instructions.get(i+3).opcode()==Opcode.IF_ICMPEQ&&load(instructions.get(i+4),slot)&&store(instructions.get(i+5),"crzCg"),"CG input assignment");cg++;
                require(instructions.get(i+3) instanceof BranchInstruction comparison&&attribute.labelToBci(comparison.target())==offsets[i+9]
                    &&constant(instructions.get(i+6),1)&&constant(instructions.get(i+7),100131)
                    &&Prepare.call(instructions.get(i+8),"FmcVnav","startWptDataCalcTimer","(ZI)V")
                    &&Prepare.call(instructions.get(i+9),"FmcPerf","makeDisplaysRest","()V"),"CG changed-input timer/control-flow boundary");
            }
        }
        require(reserves==1&&cg==1,"unique derivative assignments");require(model.methods().stream().noneMatch(m->m.methodName().equalsString(BRIDGE)),"bridge collision");
        ClassDesc p=ClassDesc.of("FmcPerf");byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){b.withMethodBody(BRIDGE,MethodTypeDesc.of(CD_void),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                var entered=c.newLabel();var assigned=c.newLabel();
                c.getstatic(p,"fuelReserves",CD_int).dup().ifge(entered).pop().loadConstant(4000).goto_(assigned);
                c.labelBinding(entered).labelBinding(assigned).putstatic(p,"fuelReservesInUse",CD_int);
                c.loadConstant(1600).getstatic(p,"crzCg",CD_int).loadConstant(85).isub().imul().loadConstant(245).idiv().putstatic(p,"crzCgAltAdd",CD_int).return_();
            });}
        });
        require(data.cf.verify(output).isEmpty(),"generated verification");var before=new HashMap<>(lower);before.put("FmcPerf",input);var changed=new HashMap<>(lower);changed.put("FmcPerf",output);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report");Path jar=Path.of(arguments[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"perf_reserve_cg_component\",\"captureQualified\":false,\"recallQualified\":false,\"inputSha256\":\""+Identity.hash(data.inputs.get("FmcPerf"))+"\",\"preparedSha256\":\""+Identity.hash(changed.get("FmcPerf"))+"\",\"limit\":\"Unused preparation component; entered-input validity, bank coherence, complete adapter/exclusion/JNI/service unqualified.\"}\n";
        Files.writeString(Path.of(arguments[1]),report,StandardOpenOption.CREATE_NEW);System.out.println("Native reserve/CG arithmetic qualifies as data; no FMC capability");
    }
}
