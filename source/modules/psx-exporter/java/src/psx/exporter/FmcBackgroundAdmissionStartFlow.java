package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Data-only attribution of original Thread.start receivers. A single reaching
 * local definition must hold an inline fresh native Thread, optionally wrapping
 * a fresh native Runnable. Other starts in the same method remain unrelated.
 * Unknown fields/factories/joins are explicit obligations, never proximity
 * guesses. Original Threads, priority calls and run bodies remain untouched. */
final class FmcBackgroundAdmissionStartFlow {
    record Site(Prepare.MethodId caller,int startOrdinal,int totalStarts,String runnable,Prepare.MethodId run){}
    record Result(List<Site> sites,Map<Prepare.MethodId,Set<Integer>> unresolved){}
    private FmcBackgroundAdmissionStartFlow(){}
    static Result inspect(Prepare data,Collection<Prepare.MethodId> callers){
        var sites=new ArrayList<Site>();var unresolved=new TreeMap<Prepare.MethodId,Set<Integer>>(Comparator.comparing(Object::toString));
        for(var id:callers){
            var method=data.methods.get(id);require(method!=null&&method.code().isPresent(),"original caller body");
            var ops=Prepare.instructions(method);int total=0;for(var op:ops)if(Prepare.call(op,"java/lang/Thread","start","()V"))total++;
            if(total==0)continue;var states=reaching(method);int ordinal=0;
            for(int at=0;at<ops.size();at++)if(Prepare.call(ops.get(at),"java/lang/Thread","start","()V")){
                String runnable=null;
                if(at>0&&ops.get(at-1) instanceof LoadInstruction load&&load.typeKind()==TypeKind.REFERENCE){
                    var defs=states[at]==null?Set.<Integer>of():states[at].getOrDefault(load.slot(),Set.of(-1));
                    if(defs.size()==1&&defs.iterator().next()>0){int store=defs.iterator().next();
                        if(ops.get(store-1) instanceof InvokeInstruction ctor&&ctor.opcode()==Opcode.INVOKESPECIAL&&ctor.name().equalsString("<init>")){
                            String type=ctor.owner().asInternalName();
                            if(type.equals("java/lang/Thread")&&ctor.type().equalsString("(Ljava/lang/Runnable;)V")&&store>1
                                &&ops.get(store-2) instanceof InvokeInstruction target&&target.opcode()==Opcode.INVOKESPECIAL&&target.name().equalsString("<init>")){
                                String task=target.owner().asInternalName();
                                if(fresh(ops,store-2,task)&&fresh(ops,store-1,"java/lang/Thread"))runnable=task;
                            }else if(!type.equals("java/lang/Thread")&&subtype(data,type,"java/lang/Thread")&&fresh(ops,store-1,type))runnable=type;
                        }
                    }
                }
                else if(at>0&&ops.get(at-1) instanceof FieldInstruction field&&field.opcode()==Opcode.GETSTATIC&&field.type().equalsString("Ljava/lang/Thread;"))
                    runnable=staticThread(data,field);
                var run=runnable==null?null:run(data,runnable,new HashSet<>());
                if(run==null)unresolved.computeIfAbsent(id,k->new TreeSet<>()).add(ordinal);
                else sites.add(new Site(id,ordinal,total,runnable,run));ordinal++;
            }
        }
        var frozen=new LinkedHashMap<Prepare.MethodId,Set<Integer>>();unresolved.forEach((k,v)->frozen.put(k,Set.copyOf(v)));
        return new Result(List.copyOf(sites),Map.copyOf(frozen));
    }
    private static String staticThread(Prepare data,FieldInstruction read){
        String task=null;int stores=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            var ops=Prepare.instructions(entry.getValue());
            for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction write&&write.opcode()==Opcode.PUTSTATIC
                &&write.owner().asInternalName().equals(read.owner().asInternalName())&&write.name().equalsString(read.name().stringValue())&&write.type().equalsString(read.type().stringValue())){
                stores++;if(at<2||!(ops.get(at-1) instanceof InvokeInstruction ctor)||ctor.opcode()!=Opcode.INVOKESPECIAL||!ctor.owner().asInternalName().equals("java/lang/Thread")
                    ||!ctor.name().equalsString("<init>")||!ctor.type().equalsString("(Ljava/lang/Runnable;)V"))return null;
                if(!(ops.get(at-2) instanceof InvokeInstruction target)||target.opcode()!=Opcode.INVOKESPECIAL||!target.name().equalsString("<init>"))return null;
                String candidate=target.owner().asInternalName();if(!fresh(ops,at-2,candidate)||!fresh(ops,at-1,"java/lang/Thread"))return null;task=candidate;
            }
        }
        // A field type alone is not a runnable identity. This is the complete
        // installed single-writer assignment vector, not a nearby NEW guess.
        return stores==1?task:null;
    }
    private static boolean fresh(List<Instruction> ops,int ctor,String type){
        // Constructors retain a fresh uninitialized receiver across argument
        // evaluation. No local/field load can stand in for this native NEW/DUP.
        for(int at=ctor-1;at>=0;at--){
            if(ops.get(at) instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.name().equalsString("<init>")&&call.owner().asInternalName().equals(type))return false;
            if(ops.get(at) instanceof NewObjectInstruction allocation&&allocation.className().asInternalName().equals(type))return at+1<ctor&&ops.get(at+1).opcode()==Opcode.DUP;
            if(ops.get(at) instanceof StoreInstruction||ops.get(at) instanceof BranchInstruction||ops.get(at) instanceof ReturnInstruction)return false;
        }return false;
    }
    private static Prepare.MethodId run(Prepare data,String owner,Set<String> seen){
        if(!seen.add(owner))return null;var id=new Prepare.MethodId(owner,"run","()V");
        var method=data.methods.get(id);if(method!=null&&method.code().isPresent()&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)return id;
        var model=data.models.get(owner);return model==null||model.superclass().isEmpty()?null:run(data,model.superclass().get().asInternalName(),seen);
    }
    private static boolean subtype(Prepare data,String owner,String target){
        var seen=new HashSet<String>();while(seen.add(owner)){if(owner.equals(target))return true;var model=data.models.get(owner);if(model==null||model.superclass().isEmpty())return false;owner=model.superclass().get().asInternalName();}return false;
    }
    @SuppressWarnings("unchecked")
    private static Map<Integer,Set<Integer>>[] reaching(MethodModel method){
        var code=(CodeAttribute)method.code().orElseThrow();var ops=Prepare.instructions(method);var states=(Map<Integer,Set<Integer>>[])new Map<?,?>[ops.size()];
        var positions=new ArrayList<Integer>();int bci=0;for(var op:ops){positions.add(bci);bci+=op.sizeInBytes();}
        states[0]=new HashMap<>();var pending=new ArrayDeque<Integer>();pending.add(0);
        while(!pending.isEmpty()){
            int at=pending.removeFirst();var before=states[at];var after=new HashMap<Integer,Set<Integer>>(before);var op=ops.get(at);
            if(op instanceof StoreInstruction store&&store.typeKind()==TypeKind.REFERENCE)after.put(store.slot(),Set.of(at));
            var next=new HashSet<Integer>();
            if(op instanceof BranchInstruction branch){next.add(index(code,positions,branch.target()));if(op.opcode()!=Opcode.GOTO&&op.opcode()!=Opcode.GOTO_W)next.add(at+1);}
            else if(op instanceof LookupSwitchInstruction sw){next.add(index(code,positions,sw.defaultTarget()));for(var c:sw.cases())next.add(index(code,positions,c.target()));}
            else if(op instanceof TableSwitchInstruction sw){next.add(index(code,positions,sw.defaultTarget()));for(var c:sw.cases())next.add(index(code,positions,c.target()));}
            else if(!(op instanceof ReturnInstruction)&&op.opcode()!=Opcode.ATHROW)next.add(at+1);
            for(int to:next)if(to<ops.size()&&merge(states,to,after))pending.add(to);
            for(var h:code.exceptionHandlers())if(positions.get(at)>=code.labelToBci(h.tryStart())&&positions.get(at)<code.labelToBci(h.tryEnd())){
                int to=index(code,positions,h.handler());if(merge(states,to,before))pending.add(to);
            }
        }return states;
    }
    private static boolean merge(Map<Integer,Set<Integer>>[] states,int at,Map<Integer,Set<Integer>> incoming){
        if(states[at]==null){states[at]=new HashMap<>(incoming);return true;}boolean changed=false;var slots=new HashSet<>(states[at].keySet());slots.addAll(incoming.keySet());
        for(int slot:slots){var union=new HashSet<>(states[at].getOrDefault(slot,Set.of(-1)));union.addAll(incoming.getOrDefault(slot,Set.of(-1)));
            if(!union.equals(states[at].get(slot))){states[at].put(slot,Set.copyOf(union));changed=true;}}
        return changed;
    }
    private static int index(CodeAttribute code,List<Integer> positions,Label label){int at=Collections.binarySearch(positions,code.labelToBci(label));require(at>=0,"native branch instruction boundary");return at;}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native start receiver "+why);}
}
