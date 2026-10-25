package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;
import java.nio.file.*;

/** Conservative native pause evidence. A pause-dominated method is only one
 * part of exclusion: call targets, aliases and visibility need separate proof. */
public final class FmcExclusionAudit {
    record Entry(boolean earlyPauseReturn,String reason){}
    private FmcExclusionAudit(){}
    static Entry entry(MethodModel method) {
        if(method.code().isEmpty())return new Entry(false,"no native body");
        var code=method.code().get();var ops=new ArrayList<Instruction>();var labels=new HashMap<Label,Integer>();
        for(var element:code) {
            if(element instanceof LabelTarget target)labels.put(target.label(),ops.size());
            else if(element instanceof Instruction instruction)ops.add(instruction);
        }
        if(ops.size()<3)return new Entry(false,"no early pause branch");
        Instruction first=ops.getFirst();boolean pause=first instanceof InvokeInstruction call
            &&call.opcode()==Opcode.INVOKESTATIC&&call.owner().asInternalName().equals("Core")
            &&call.name().equalsString("isPaused")&&call.type().equalsString("()Z");
        pause|=first instanceof FieldInstruction field&&field.opcode()==Opcode.GETSTATIC
            &&field.owner().asInternalName().equals("Core")&&field.name().equalsString("paused")&&field.type().equalsString("Z");
        if(!pause||!(ops.get(1) instanceof BranchInstruction branch)||branch.opcode()!=Opcode.IFEQ
            ||ops.get(2).opcode()!=Opcode.RETURN||!Objects.equals(labels.get(branch.target()),3))
            return new Entry(false,"native work precedes or bypasses qualified pause return");
        for(var handler:code.exceptionHandlers()) {
            Integer start=labels.get(handler.tryStart()),end=labels.get(handler.tryEnd());
            if(start==null||end==null||start<3&&end>0)return new Entry(false,"pause entry exception path unqualified");
        }
        return new Entry(true,"first native predicate returns while paused; body begins only on clear branch (visibility and caller exclusion still required)");
    }

    static String report(Prepare data,String jarHash)throws Exception {
        var inventory=FmcQualification.inventory(data);
        var wanted=new LinkedHashSet<Prepare.MethodId>();
        for(String owner:List.of("FmcGlobal","AfdsHub","ControlDisplayUnit"))wanted.add(new Prepare.MethodId(owner,"runDelta","()V"));
        wanted.add(new Prepare.MethodId("CduHub","runAllegro","()V"));
        wanted.add(new Prepare.MethodId("FmcGlobal","runAllegro","()V"));
        for(String owner:List.of("FmcRte$ExecL","FmcRte$ExecR","FmcRte$ThreadGa","FmcNg$ThreadDivert","FmcNg$ThreadEraseMod"))
            wanted.add(new Prepare.MethodId(owner,"run","()V"));
        var text=new StringBuilder("{\"schema\":1,\"jarSha256\":").append(FmcQualification.quote(jarHash))
            .append(",\"captureQualified\":false,\"recallQualified\":false,\"methods\":[");
        boolean comma=false;
        for(var id:wanted) {
            var method=data.methods.get(id);if(method==null)throw new IllegalArgumentException("missing exclusion authority "+id);
            Entry pause=entry(method);int sleeps=0,reads=0,writes=0;
            for(var op:Prepare.instructions(method)) {
                if(op instanceof InvokeInstruction call&&call.name().equalsString("sleep")&&call.type().equalsString("(J)V"))sleeps++;
                if(op instanceof FieldInstruction field){if(field.opcode()==Opcode.GETSTATIC||field.opcode()==Opcode.GETFIELD)reads++;else writes++;}
            }
            if(comma)text.append(',');comma=true;
            text.append("{\"method\":").append(FmcQualification.quote(id.toString()))
                .append(",\"classSha256\":").append(FmcQualification.quote(Identity.hash(data.inputs.get(id.owner()))))
                .append(",\"earlyPauseReturn\":").append(pause.earlyPauseReturn())
                .append(",\"reason\":").append(FmcQualification.quote(pause.reason()))
                .append(",\"synchronizedMethod\":").append((method.flags().flagsMask()&ClassFile.ACC_SYNCHRONIZED)!=0)
                .append(",\"sleepCalls\":").append(sleeps).append(",\"fieldReads\":").append(reads)
                .append(",\"fieldWrites\":").append(writes).append('}');
        }
        var paused=data.models.get("Core").fields().stream().filter(f->f.fieldName().equalsString("paused")&&f.fieldType().equalsString("Z")).findFirst().orElseThrow();
        int arrays=0;for(var field:inventory.fields())if(field.descriptor().startsWith("["))arrays++;
        return text.append("],\"nativePauseVolatile\":").append((paused.flags().flagsMask()&ClassFile.ACC_VOLATILE)!=0)
            .append(",\"arrayCarriers\":").append(arrays).append(",\"referenceBoundaries\":").append(inventory.boundaries().size())
            .append(",\"proofStillRequired\":[\"owned field/root/factory classification\",\"array and object alias closure including external writers\",\"all relevant readers and asynchronous start/reservation lifetime\",\"legal bridge and final transformed member access\",\"native gate memory visibility and quiescence\",\"complete preimage/readback and preservation witnesses\"]}").toString();
    }
    public static void main(String[] args)throws Exception {
        if(args.length!=2)throw new IllegalArgumentException("FmcExclusionAudit <installed jar> <report>");
        Path jar=Path.of(args[0]);String hash=Identity.hash(jar);
        if(!FmcQualification.INSTALLED_JAR.equals(hash))throw new IllegalArgumentException("unqualified PSX JAR identity");
        Files.writeString(Path.of(args[1]),report(new Prepare(Prepare.readJar(jar)),hash),StandardOpenOption.CREATE_NEW);
        System.out.println("Native FMC pause/async evidence inspected as data; no capture/recall capability advertised");
    }
}
