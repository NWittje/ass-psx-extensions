package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Native comm cache replacement never mutates the previous generation.
 * Its file-dependent count therefore needs a pointer witness, not traversal
 * or ownership tags on all old records. Every store/copy authority check on
 * the new ordinary native generation remains in the potential audit. */
final class FmcReconstructionFreshCache {
    static final FmcQualification.Member ROOT=new FmcQualification.Member("ScanComm","cache","[LScanComm$CacheRec;");
    record Profile(Set<FmcQualification.Member> references,Set<FmcQualification.Member> recordFields,int freshStoreOrdinal){}
    static Profile inspect(Prepare data){
        var maker=new Prepare.MethodId("ScanComm","makeCache","(Ljava/lang/String;)V");var method=data.methods.get(maker);var ops=Prepare.instructions(method);int fresh=-1;
        for(int at=0;at<ops.size();at++)if(Prepare.field(ops.get(at),Opcode.PUTSTATIC,"ScanComm","cache")){
            if(fresh!=-1||at==0||!(ops.get(at-1) instanceof NewReferenceArrayInstruction array)||!array.componentType().asInternalName().equals("ScanComm$CacheRec"))throw new IllegalArgumentException("native comm cache fresh array replacement");fresh=at;
        }
        if(fresh<0)throw new IllegalArgumentException("native comm cache replacement absent");var before=without(method,fresh);
        for(int at=0;at<ops.size();at++)if(Prepare.field(ops.get(at),Opcode.GETSTATIC,"ScanComm","cache")&&before.contains(at))throw new IllegalArgumentException("native comm cache reads previous generation before replacement");
        var fields=new HashSet<FmcQualification.Member>();var others=new HashSet<Prepare.MethodId>();var arrays=new HashSet<FmcQualification.Member>();int rootStores=0;
        for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){
            if(!e.getKey().equals(maker)&&!(e.getKey().owner().equals("ScanComm$CacheRec")&&e.getKey().name().equals("<init>")))others.add(e.getKey());
            for(var op:Prepare.instructions(e.getValue()))if(op instanceof FieldInstruction f){
                if(f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("ScanComm")&&f.name().equalsString("cache")){rootStores++;if(!e.getKey().equals(maker)&&!(e.getKey().owner().equals("ScanComm")&&e.getKey().name().equals("<clinit>")))throw new IllegalArgumentException("native comm cache alternative writer");}
                if(f.owner().asInternalName().equals("ScanComm$CacheRec")&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                    if(!e.getKey().equals(maker)&&!(e.getKey().owner().equals("ScanComm$CacheRec")&&e.getKey().name().equals("<init>")))throw new IllegalArgumentException("native old comm record mutator "+e.getKey());
                    var field=new FmcQualification.Member("ScanComm$CacheRec",f.name().stringValue(),f.type().stringValue());fields.add(field);if(field.descriptor().startsWith("["))arrays.add(field);
                }
                String type=f.type().stringValue();if(e.getKey().equals(maker)&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)&&type.contains("ScanComm$CacheRec")&&!Prepare.field(op,Opcode.GETSTATIC,"ScanComm","cache"))throw new IllegalArgumentException("native comm maker old record alternative source");
            }
        }
        if(rootStores!=2||arrays.size()!=2||!FmcReconstructionArrayProof.audit(data,arrays,others).mutations().isEmpty()||!FmcReconstructionArrayProof.membership(data,Set.of(ROOT),others).mutations().isEmpty())throw new IllegalArgumentException("native old comm cache array mutation");
        return new Profile(Set.of(ROOT),Set.copyOf(fields),fresh);
    }
    private static Set<Integer> without(MethodModel method,int cut){
        var code=method.code().orElseThrow();var ops=Prepare.instructions(method);var labels=new HashMap<Label,Integer>();int n=0;for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),n);if(e instanceof Instruction)n++;}
        var seen=new HashSet<Integer>();var pending=new ArrayDeque<Integer>();pending.add(0);while(!pending.isEmpty()){int at=pending.removeFirst();if(at==cut||!seen.add(at))continue;var op=ops.get(at);for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd()))pending.add(labels.get(h.handler()));
            if(op instanceof BranchInstruction b){pending.add(labels.get(b.target()));if(op.opcode()==Opcode.GOTO||op.opcode()==Opcode.GOTO_W)continue;}
            else if(op instanceof LookupSwitchInstruction b){pending.add(labels.get(b.defaultTarget()));for(var c:b.cases())pending.add(labels.get(c.target()));continue;}
            else if(op instanceof TableSwitchInstruction b){pending.add(labels.get(b.defaultTarget()));for(var c:b.cases())pending.add(labels.get(c.target()));continue;}
            else if(op instanceof ReturnInstruction||op.opcode()==Opcode.ATHROW)continue;if(at+1<ops.size())pending.add(at+1);
        }return seen;
    }
    public static void main(String[] args)throws Exception{var data=new Prepare(Prepare.readJar(java.nio.file.Path.of(args[0])));System.out.println(inspect(data));}
}
