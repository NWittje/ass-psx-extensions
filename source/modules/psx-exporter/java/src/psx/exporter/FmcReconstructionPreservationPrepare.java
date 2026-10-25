package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Non-consuming same-owner views of current global flight number and manual
 * FMC fuel/history. Fuel entries retain raw float bits; no aircraft fuel or
 * saved flight-number value is published by these read-only methods. */
final class FmcReconstructionPreservationPrepare {
    static final String FLIGHT="$psxExporter$fmcCurrentFlightNumber",FUEL="$psxExporter$fmcCurrentManualFuel";
    static final FmcQualification.Member FLIGHT_FIELD=new FmcQualification.Member("FmcRte","fltNumberStr","Ljava/lang/String;");
    static final List<FmcQualification.Member> FUEL_FIELDS=List.of(new FmcQualification.Member("FmcPerf","ballast","I"),new FmcQualification.Member("FmcPerf","fuelWtMode","I"),new FmcQualification.Member("FmcPerf","fuelWeightEntry","F"),new FmcQualification.Member("FmcPerf","fuelWeightSens","F"));
    static Set<FmcQualification.Member> fields(){var fields=new HashSet<>(FUEL_FIELDS);fields.add(FLIGHT_FIELD);fields.add(new FmcQualification.Member("Glob","qsFmcFltNo","I"));return Set.copyOf(fields);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        for(var f:fields())if(!f.equals(FmcQualification.resolveField(data.models,f.owner(),f.name(),f.descriptor(),new HashSet<>()))||data.models.get(f.owner()).fields().stream().noneMatch(m->m.fieldName().equalsString(f.name())&&(m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0))throw new IllegalArgumentException("FMC exact current preservation field "+f);
        var nativeReader=data.requireMethod("FmcRte","readGlobFltNo","()V");var reader=Prepare.instructions(nativeReader);var labels=new HashMap<Label,Integer>();int at=0;for(var e:nativeReader.code().orElseThrow()){if(e instanceof LabelTarget label)labels.put(label.label(),at);if(e instanceof java.lang.classfile.Instruction)at++;}
        if(reader.size()<17||!Prepare.field(reader.get(0),Opcode.GETSTATIC,"Glob","qs")||!Prepare.field(reader.get(1),Opcode.GETSTATIC,"Glob","qsFmcFltNo")||reader.get(2).opcode()!=Opcode.AALOAD||reader.get(3).opcode()!=Opcode.ACONST_NULL||!(reader.get(4) instanceof ConstantInstruction mode)||!Objects.equals(mode.constantValue(),(int)'r')||!(reader.get(5) instanceof ConstantInstruction origin)||!Objects.equals(origin.constantValue(),0)||!(reader.get(6) instanceof InvokeInstruction transfer)||!transfer.owner().asInternalName().equals("Qs")||!transfer.name().equalsString("transfer")||!transfer.type().equalsString("(Ljava/lang/String;CI)Ljava/lang/String;")||!(reader.get(7) instanceof InvokeInstruction text)||!text.owner().asInternalName().equals("java/lang/String")||!text.name().equalsString("toString")||!(reader.get(8) instanceof StoreInstruction save)||save.slot()!=0||!(reader.get(9) instanceof LoadInstruction read)||read.slot()!=0||!(reader.get(10) instanceof ConstantInstruction dash)||!Objects.equals(dash.constantValue(),"-")||!(reader.get(11) instanceof InvokeInstruction equal)||!equal.owner().asInternalName().equals("java/lang/String")||!equal.name().equalsString("equals")||!(reader.get(12) instanceof BranchInstruction other)||other.opcode()!=Opcode.IFEQ||!Objects.equals(labels.get(other.target()),15)||!(reader.get(13) instanceof ConstantInstruction blank)||!Objects.equals(blank.constantValue(),"")||!(reader.get(14) instanceof BranchInstruction join)||join.opcode()!=Opcode.GOTO||!Objects.equals(labels.get(join.target()),16)||!(reader.get(15) instanceof LoadInstruction unchanged)||unchanged.slot()!=0||!Prepare.field(reader.get(16),Opcode.PUTSTATIC,"FmcRte","fltNumberStr"))throw new IllegalArgumentException("FMC exact current flight-number bank/dash normalization publisher");
        var output=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String name:List.of("FmcRte","FmcPerf")){byte[] input=lower.getOrDefault(name,data.inputs.get(name));before.put(name,input);var model=data.cf.parse(input);if(model.methods().stream().anyMatch(m->m.methodName().equalsString(FLIGHT)||m.methodName().equalsString(FUEL)))throw new IllegalArgumentException("FMC preservation view collision");var self=ClassDesc.of(name);
            output.put(name,data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
                if(name.equals("FmcRte"))b.withMethodBody(FLIGHT,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->c.iconst_2().anewarray(CD_Object).dup().iconst_0().getstatic(self,FLIGHT_FIELD.name(),CD_String).aastore().dup().iconst_1().getstatic(ClassDesc.of("Glob"),"qs",ClassDesc.of("Qs").arrayType()).getstatic(ClassDesc.of("Glob"),"qsFmcFltNo",CD_int).aaload().aastore().areturn());
                else b.withMethodBody(FUEL,MethodTypeDesc.of(CD_int.arrayType()),flags,c->{c.iconst_4().newarray(TypeKind.INT).astore(0);for(int i=0;i<4;i++){var f=FUEL_FIELDS.get(i);c.aload(0).loadConstant(i).getstatic(self,f.name(),ClassDesc.ofDescriptor(f.descriptor()));if(f.descriptor().equals("F"))c.invokestatic(ClassDesc.of("java.lang.Float"),"floatToRawIntBits",MethodTypeDesc.of(CD_int,CD_float));c.iastore();}c.aload(0).areturn();});
            }}));if(!data.cf.verify(output.get(name)).isEmpty())throw new IllegalArgumentException("FMC preservation view verifier");
        }TerrainAccess.verifyAdded(before,output,data.inputs);return Map.copyOf(output);
    }
}
