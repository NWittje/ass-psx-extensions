package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.attribute.CodeAttribute;import java.lang.classfile.instruction.*;import java.util.*;
/** Sole 50 ms native CDU menu consumer. Deferral precedes freshness tests,
 * all r/w/r transfers and page selection. Its original caller, networking
 * predicate, physical siblings and cadence remain unchanged. */
final class FmcBackgroundAdmissionCduPrepare {
    static final Prepare.MethodId METHOD=new Prepare.MethodId("CduHub","runAllegro","()V");
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native CDU consumer "+why);}
    static Set<Prepare.MethodId> qualify(Prepare data){
        var method=data.methods.get(METHOD);require(method!=null&&method.code().isPresent()&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"original finite static void leaf");
        var code=(CodeAttribute)method.code().orElseThrow();require(code.codeLength()==119&&code.exceptionHandlers().isEmpty(),"complete original consumer shape");
        var fields=new ArrayList<String>();var calls=new ArrayList<String>();var constants=new ArrayList<Object>();var branches=new ArrayList<Integer>();var actual=new ArrayList<Opcode>();
        for(var op:Prepare.instructions(method)){
            actual.add(op.opcode());if(op instanceof FieldInstruction f){require(f.opcode()==Opcode.GETSTATIC,"no native physical/state stores");fields.add(f.owner().asInternalName()+"."+f.name().stringValue()+":"+f.type().stringValue());}
            if(op instanceof InvokeInstruction c){require(c.opcode()==Opcode.INVOKEVIRTUAL,"only exact bank/page native invocations");calls.add(c.owner().asInternalName()+"."+c.name().stringValue()+c.type().stringValue());}
            if(op instanceof ConstantInstruction c)constants.add(c.constantValue());if(op instanceof BranchInstruction b)branches.add(code.labelToBci(b.target()));
        }
        var expected=new ArrayList<Opcode>(List.of(Opcode.GETSTATIC,Opcode.GETSTATIC,Opcode.AALOAD,Opcode.INVOKEVIRTUAL,Opcode.IFEQ));
        for(int i=0;i<3;i++){expected.addAll(List.of(Opcode.GETSTATIC,Opcode.GETSTATIC,Opcode.AALOAD,Opcode.ICONST_0,Opcode.BIPUSH,Opcode.ICONST_0,Opcode.INVOKEVIRTUAL,i==0?Opcode.ISTORE_0:Opcode.POP));}
        for(Opcode bit:List.of(Opcode.ICONST_1,Opcode.ICONST_2,Opcode.ICONST_4))expected.addAll(List.of(Opcode.ILOAD_0,bit,Opcode.IAND,Opcode.IFEQ,Opcode.GETSTATIC,Opcode.GETSTATIC,Opcode.GETSTATIC,Opcode.AALOAD,Opcode.ICONST_0,Opcode.INVOKEVIRTUAL));expected.add(Opcode.RETURN);
        require(actual.equals(expected),"exact bounded r/w/r and three page decisions; no prefix, allocation, loop or opaque call");
        var roots=new ArrayList<String>();for(int i=0;i<4;i++)roots.addAll(List.of("Glob.qi:[LQi;","Glob.qiSelCduMenu:I"));for(String side:List.of("L","R","C"))roots.addAll(List.of("Core.cdu"+side+":LControlDisplayUnit;","CduHub.pg:[LCduHub$Page;","CduHub.pgMenu"+side+":I"));
        require(fields.equals(roots)&&calls.equals(List.of("Qi.isUpdated()Z","Qi.transfer(ICI)I","Qi.transfer(ICI)I","Qi.transfer(ICI)I","ControlDisplayUnit.selectCduPage(LCduHub$Page;I)V","ControlDisplayUnit.selectCduPage(LCduHub$Page;I)V","ControlDisplayUnit.selectCduPage(LCduHub$Page;I)V")),"exact native menu bank and three initialized CDU receivers");
        require(constants.equals(List.of(0,114,0,0,119,0,0,114,0,1,0,2,0,4,0))&&branches.equals(List.of(118,78,98,118)),"native input consumption modes and decision targets");
        int sites=0;for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(Prepare.call(op,METHOD.owner(),METHOD.name(),METHOD.descriptor())){require(entry.getKey().equals(new Prepare.MethodId("Core$AllegroThread","run","()V"))&&bci==158,"sole original periodic caller");sites++;}bci+=op.sizeInBytes();}}
        require(sites==1&&data.models.get("Core$AllegroThread").superclass().orElseThrow().asInternalName().equals("java/lang/Thread"),"native Thread lifetime/caller roster");
        var cadence=FmcReconstructionTransitionEffects.positions(data.methods.get(new Prepare.MethodId("Core$AllegroThread","run","()V")));
        require(Prepare.call(cadence.get(152),"Network","notClient","()Z")&&cadence.get(155) instanceof BranchInstruction&&Prepare.call(cadence.get(194),"Core","isPaused","()Z")&&cadence.get(456) instanceof ConstantInstruction delay&&Long.valueOf(50).equals(delay.constantValue())&&Prepare.call(cadence.get(459),"Core$AllegroThread","sleep","(J)V"),"physical prefix/siblings and native retry remain original");
        return Set.of(METHOD);
    }
}