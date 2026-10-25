package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** The five flap approach points are native approach outputs, separate from
 * the six ACTIVE/MOD prediction points. Only their exact published reference
 * is partitioned; shared ProfilePoint members retain route receiver guards. */
final class FmcReconstructionApproachEffects {
    static final FmcQualification.Member FLAP_POINTS=new FmcQualification.Member("FmcNg","ppF","[LFmcVnavCalc$ProfilePoint;");
    static final FmcQualification.Member BACK_COURSE=new FmcQualification.Member("Routes","bcrsRefRwy","Ljava/lang/String;");
    record Profile(Set<FmcQualification.Member> fields,Set<FmcQualification.Member> references,FmcReconstructionArrayProof.ElementResult elementAccesses){}
    static Profile inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        require(!construction.contains(FLAP_POINTS)&&!currentInputs.contains(FLAP_POINTS),"protected flap-point reference");
        require(!construction.contains(BACK_COURSE)&&!currentInputs.contains(BACK_COURSE),"protected back-course reference");qualifyBackCourse(data);
        var stores=new HashSet<Prepare.MethodId>();int count=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent())for(var op:Prepare.instructions(entry.getValue()))if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)&&FLAP_POINTS.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()))){require(f.opcode()==Opcode.PUTSTATIC,"static flap-point pointer");stores.add(entry.getKey());count++;}
        require(count==1&&stores.equals(Set.of(new Prepare.MethodId("FmcNg","<clinit>","()V"))),"sole startup flap-point pointer writer");
        var init=Prepare.instructions(data.requireMethod("FmcNg","<clinit>","()V"));int pointer=-1;for(int i=0;i<init.size();i++)if(Prepare.field(init.get(i),Opcode.PUTSTATIC,"FmcNg","ppF"))pointer=i;
        require(pointer>=2&&init.get(pointer-2) instanceof ConstantInstruction five&&five.constantValue().equals(5)&&init.get(pointer-1) instanceof NewReferenceArrayInstruction array&&array.componentType().asInternalName().equals("FmcVnavCalc$ProfilePoint"),"native five-point array allocation");
        var fillMethod=data.requireMethod("FmcNg","init","()V");var fill=Prepare.instructions(fillMethod);int start=-1;for(int i=0;i<fill.size();i++)if(Prepare.field(fill.get(i),Opcode.GETSTATIC,"FmcNg","ppF")){start=i;break;}
        require(start>=5,"native startup flap-point fill");var fragment=fill.subList(start-5,start+11);
        require(fragment.stream().map(Instruction::opcode).toList().equals(List.of(Opcode.ICONST_0,Opcode.ISTORE_0,Opcode.ILOAD_0,Opcode.ICONST_5,Opcode.IF_ICMPGE,Opcode.GETSTATIC,Opcode.ILOAD_0,Opcode.NEW,Opcode.DUP,Opcode.ICONST_0,Opcode.LDC,Opcode.INVOKESPECIAL,Opcode.AASTORE,Opcode.IINC,Opcode.GOTO,Opcode.GETSTATIC)),"exact fresh flap-point fill loop");
        require(fragment.get(7) instanceof NewObjectInstruction fresh&&fresh.className().asInternalName().equals("FmcVnavCalc$ProfilePoint")&&((ConstantInstruction)fragment.get(10)).constantValue().equals("")&&fragment.get(11) instanceof InvokeInstruction ctor&&ctor.owner().asInternalName().equals("FmcVnavCalc$ProfilePoint")&&ctor.name().equalsString("<init>")&&ctor.type().equalsString("(ILjava/lang/String;)V"),"five independently constructed non-route points");
        var code=(java.lang.classfile.attribute.CodeAttribute)fillMethod.code().orElseThrow();require(code.labelToBci(((BranchInstruction)fragment.get(4)).target())==165&&code.labelToBci(((BranchInstruction)fragment.get(14)).target())==139,"bounded native five-point loop CFG");
        var outside=new HashSet<>(data.methods.keySet());outside.remove(new Prepare.MethodId("FmcNg","init","()V"));outside.remove(new Prepare.MethodId("FmcNg","<clinit>","()V"));require(FmcReconstructionArrayProof.membership(data,Set.of(FLAP_POINTS),outside).mutations().isEmpty(),"flap-point membership has no later aliases/replacements");
        var elements=FmcReconstructionArrayProof.elementAccesses(data,Set.of(FLAP_POINTS),outside,"FmcVnavCalc$ProfilePoint");require(elements.proof().mutations().isEmpty(),"flap-point elements have no alternate publication/escape");
        var update=Prepare.instructions(data.requireMethod("FmcAppRef","updateVref","()V"));int writes=0;for(int i=4;i<update.size();i++)if(Prepare.field(update.get(i),Opcode.PUTFIELD,"FmcVnavCalc$ProfilePoint","speedLabel")){require(Prepare.field(update.get(i-4),Opcode.GETSTATIC,"FmcNg","ppF")&&update.get(i-3) instanceof ConstantInstruction index&&index.constantValue().equals(writes)&&update.get(i-2).opcode()==Opcode.AALOAD&&Prepare.field(update.get(i-1),Opcode.GETSTATIC,"FmcAppRef",List.of("vRef00","vRef01","vRef05","vRef10","vRef20").get(writes)),"native approach speed-label receiver role");writes++;}require(writes==5,"five native approach Vref outputs");
        return new Profile(Set.of(BACK_COURSE),Set.of(FLAP_POINTS),elements);
    }
    private static void qualifyBackCourse(Prepare data){
        var writers=FmcReconstructionPageEffects.writers(data,Set.of(BACK_COURSE));var body=new Prepare.MethodId("Routes$RteBody","readGlob","(Ljava/lang/String;)V");require(writers.getOrDefault(BACK_COURSE,Set.of()).equals(Set.of(body,new Prepare.MethodId("Routes","<clinit>","()V"))),"back-course sole native writers");
        var method=data.methods.get(body);var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();var sites=new ArrayList<Integer>();int bci=0;var ops=Prepare.instructions(method);for(var op:ops){if(Prepare.field(op,Opcode.PUTSTATIC,"Routes","bcrsRefRwy"))sites.add(bci);bci+=op.sizeInBytes();}require(sites.equals(List.of(13610,13641,13656,13664,13672,13684,13696)),"seven active back-course native output sites");
        var positions=new HashMap<Integer,Instruction>();bci=0;for(var op:ops){positions.put(bci,op);bci+=op.sizeInBytes();}
        require(positions.get(13404).opcode()==Opcode.ALOAD_0&&Prepare.field(positions.get(13405),Opcode.GETSTATIC,"Routes","actBody")&&positions.get(13408) instanceof BranchInstruction active&&active.opcode()==Opcode.IF_ACMPNE&&code.labelToBci(active.target())==14236,"back-course belongs only to current active body");
        require(Prepare.field(positions.get(13481),Opcode.GETFIELD,"RouteWpt","identStrVia")&&positions.get(13484) instanceof ConstantInstruction prefix&&prefix.constantValue().equals("LBC")&&positions.get(13487) instanceof InvokeInstruction starts&&starts.owner().asInternalName().equals("java/lang/String")&&starts.name().equalsString("startsWith")&&Prepare.field(positions.get(13497),Opcode.GETFIELD,"FmcRte$Rte","destRwyNameEx"),"native back-course derives selected approach/destination runway");
        require(positions.get(13588) instanceof ConstantInstruction reciprocal&&reciprocal.constantValue().equals(18)&&positions.get(13590).opcode()==Opcode.IADD&&positions.get(13591) instanceof ConstantInstruction compass&&compass.constantValue().equals(36)&&positions.get(13593).opcode()==Opcode.IREM,"native reciprocal runway number");
        var potential=FmcReconstructionExecAudit.inspect(data);require(potential.writers().getOrDefault(BACK_COURSE,Set.of()).equals(Set.of(body)),"ordinary EXEC back-course writer");
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC approach outputs "+why);}
}
