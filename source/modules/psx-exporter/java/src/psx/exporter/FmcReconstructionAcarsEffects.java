package psx.exporter;

import java.lang.classfile.Opcode;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Native route/OOOI status outputs of ordinary EXEC. The current global
 * flight number and actual/manual FMC fuel remain preservation inputs.
 * ACARS may start a new completed-flight timing record for a changed route;
 * its physical event cadence continues with the original clocks and inputs.
 * No ACARS input array, message buffer or arbitrary owner field is exempt. */
final class FmcReconstructionAcarsEffects {
    private static final Prepare.MethodId RESET=new Prepare.MethodId("Acars","checkReset","()V"),TELEX=new Prepare.MethodId("Acars$ThreadTelexDn","run","()V");
    static FmcReconstructionNormalExec.Profile apply(Prepare data,FmcReconstructionNormalExec.Profile normal,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs,Set<FmcReconstructionExecRoots.Bank> protectedBanks){
        var fields=new HashSet<>(normal.fields());fields.addAll(inspect(data,construction,currentInputs));
        var bank=new FmcReconstructionExecRoots.Bank(new FmcQualification.Member("Glob","qs","[LQs;"),new FmcQualification.Member("Glob","qsAcarsData","I"));require(!protectedBanks.contains(bank),"ACARS output cannot replace a protected bank");
        var send=Prepare.instructions(data.requireMethod("Acars","sendGlob","()V"));int publications=0;
        for(int at=0;at+7<send.size();at++)if(Prepare.field(send.get(at),Opcode.GETSTATIC,"Glob","qs")&&Prepare.field(send.get(at+1),Opcode.GETSTATIC,"Glob","qsAcarsData")&&send.get(at+2).opcode()==Opcode.AALOAD){
            require(send.get(at+3) instanceof LoadInstruction&&send.get(at+4) instanceof InvokeInstruction text&&text.owner().asInternalName().equals("java/lang/StringBuilder")&&text.name().equalsString("toString")&&send.get(at+5) instanceof ConstantInstruction mode&&Objects.equals(mode.constantValue(),(int)'w')&&send.get(at+6) instanceof ConstantInstruction origin&&Objects.equals(origin.constantValue(),1)&&send.get(at+7) instanceof InvokeInstruction call&&call.owner().asInternalName().equals("Qs")&&call.name().equalsString("transfer")&&call.type().equalsString("(Ljava/lang/String;CI)Ljava/lang/String;"),"exact native ACARS output bank publication");publications++;
        }
        require(publications==1,"unique native ACARS data publication");var banks=new HashSet<>(normal.banks());banks.add(bank);
        return new FmcReconstructionNormalExec.Profile(Set.copyOf(fields),Set.copyOf(banks),normal.retainedInputs(),normal.bankReaders(),normal.references(),normal.dynamicScratchpads(),normal.atomicCurrentOutputs());
    }
    static Set<FmcQualification.Member> inspect(Prepare data,Set<FmcQualification.Member> construction,Set<FmcQualification.Member> currentInputs){
        var fields=new HashSet<FmcQualification.Member>();for(String name:List.of("phase","minsOut","minsOff","minsOn","minsIn","minsBlock","minsFlight"))fields.add(new FmcQualification.Member("Acars",name,"I"));
        for(String name:List.of("aptOrig","aptDest","fltNo","oooiOut","oooiOff","oooiOn","oooiIn"))fields.add(new FmcQualification.Member("Acars",name,"Ljava/lang/String;"));
        var sequence=new FmcQualification.Member("Acars","posRepSendSeq","I");fields.add(sequence);
        var potential=FmcReconstructionExecAudit.inspect(data);var actual=new HashSet<FmcQualification.Member>();for(var field:potential.writes())if(field.owner().equals("Acars"))actual.add(field);
        require(actual.equals(fields),"ordinary native ACARS output field drift");
        for(var field:fields)require(potential.writers().get(field).equals(Set.of(field.equals(sequence)?TELEX:RESET)),"exact native ACARS output publisher");
        var stores=new HashSet<FmcQualification.Member>();boolean currentFlight=false,newRoute=false;
        for(var op:Prepare.instructions(data.requireMethod(RESET.owner(),RESET.name(),RESET.descriptor())))if(op instanceof FieldInstruction f){
            if(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD){var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());require(field!=null&&fields.contains(field)&&!field.equals(sequence),"reset cannot write physical/manual input");stores.add(field);}
            currentFlight|=Prepare.field(op,Opcode.GETSTATIC,"FmcRte","fltNumberStr");newRoute|=Prepare.field(op,Opcode.GETSTATIC,"Routes","actBody");
        }
        var expected=new HashSet<>(fields);expected.remove(sequence);require(stores.equals(expected)&&currentFlight&&newRoute,"native reset uses current flight number and NEW route");
        require(Collections.disjoint(fields,construction)&&Collections.disjoint(fields,currentInputs),"ACARS outputs overlap reconstruction/preservation");return Set.copyOf(fields);
    }
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException("FMC ACARS effects "+why);}
}
