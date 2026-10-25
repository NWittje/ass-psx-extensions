package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Negative dispatch proof. Unknown receivers keep every conservative edge;
 * only a complete original writer proof permits removing an override. */
final class FmcReconstructionExecScope {
    record Profile(Set<Prepare.MethodId> methods,Set<FmcQualification.Member> writes,
                   Map<String,String> pages,int excludedCallbackEdges,int excludedPageEdges,
                   Set<Prepare.MethodId> symbolicPageCalls,Map<Prepare.MethodId,Set<Integer>> instructions){}
    private record Context(Prepare.MethodId method,boolean memWrite){}
    static Profile inspect(Prepare data){
        var potential=FmcExecMutationPrepare.profile(data).methods();var dispatch=new FmcBackgroundAdmissionDispatch(data);
        var pages=pages(data);var callers=FmcRecallBoundary.inspect(data,Set.of()).callers();var calls=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        callers.forEach((callee,parents)->{if(potential.contains(callee))for(var parent:parents)if(potential.contains(parent))calls.computeIfAbsent(parent,k->new HashSet<>()).add(callee);});
        var pending=new ArrayDeque<Context>();for(String owner:List.of("FmcRte$ExecL","FmcRte$ExecR"))pending.add(new Context(new Prepare.MethodId(owner,"run","()V"),false));
        var reached=new HashSet<Prepare.MethodId>();var contexts=new HashSet<Context>();var admittedOps=new HashMap<Prepare.MethodId,Set<Integer>>();var symbolic=new HashSet<Prepare.MethodId>();int callbacks=0,pageEdges=0;
        var arguments=new HashMap<Prepare.MethodId,Map<Integer,List<Integer>>>();
        while(!pending.isEmpty()){
            var context=pending.removeFirst();if(!contexts.add(context))continue;var id=context.method;reached.add(id);var ops=Prepare.instructions(data.methods.get(id));var ordinals=reachable(data,id,context.memWrite);admittedOps.computeIfAbsent(id,k->new HashSet<>()).addAll(ordinals);
            for(var target:calls.getOrDefault(id,Set.of())){
                if(!dispatch.mayCall(id,target,ordinals)){callbacks++;continue;}
                if(!pageMayCall(data,id,target,ops,ordinals,pages,symbolic)){pageEdges++;continue;}
                if(target.owner().equals("FmcGlobal$MemBits")&&target.name().equals("trans")&&target.descriptor().equals("(ZZI)V")){
                    var values=arguments.computeIfAbsent(id,k->FmcReconstructionArrayProof.arguments(data,k));for(int at:ordinals)if(ops.get(at) instanceof InvokeInstruction call&&call.owner().asInternalName().equals("FmcGlobal$MemBits")&&call.name().equalsString("trans")&&call.type().equalsString("(ZZI)V"))pending.add(new Context(target,values.get(at).getFirst()==1));
                }else pending.add(new Context(target,false));
            }
            // Same conservative NEW-to-run rule as the potential authority
            // audit, with exact allocated native job identity.
            for(int at:ordinals)if(ops.get(at) instanceof NewObjectInstruction n){String owner=n.className().asInternalName();if(subtype(data,owner,"java/lang/Thread")||subtype(data,owner,"java/lang/Runnable")){var run=resolve(data,owner,"run","()V");if(run!=null&&potential.contains(run))pending.add(new Context(run,false));}}
        }
        var writes=new HashSet<FmcQualification.Member>();for(var id:reached){var ops=Prepare.instructions(data.methods.get(id));for(int at:admittedOps.get(id))if(ops.get(at) instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field==null)throw new IllegalArgumentException("native EXEC dispatch unresolved field");writes.add(field);}}
        var frozen=new HashMap<Prepare.MethodId,Set<Integer>>();admittedOps.forEach((k,v)->frozen.put(k,Set.copyOf(v)));return new Profile(Set.copyOf(reached),Set.copyOf(writes),Map.copyOf(pages),callbacks,pageEdges,Set.copyOf(symbolic),Map.copyOf(frozen));
    }
    static Map<String,String> pages(Prepare data){
        var pg=new FmcQualification.Member("CduHub","pg","[LCduHub$Page;");var methods=new HashSet<Prepare.MethodId>();data.methods.forEach((id,m)->{if(m.code().isPresent())methods.add(id);});
        var membership=FmcReconstructionArrayProof.membership(data,Set.of(pg),methods);var result=new HashMap<String,String>();Prepare.MethodId initializer=null;
        for(var site:membership.mutations()){
            if(!site.name().equals("AASTORE"))throw new IllegalArgumentException("opaque native page membership mutation "+site);
            var ops=Prepare.instructions(data.methods.get(site.method()));int at=site.ordinal();if(at<1||!(ops.get(at-1) instanceof InvokeInstruction ctor)||ctor.opcode()!=Opcode.INVOKESPECIAL||!ctor.name().equalsString("<init>"))throw new IllegalArgumentException("native page store lacks exact constructor");
            int start=at-ctor.typeSymbol().parameterCount()-5;
            if(start<0||!Prepare.field(ops.get(start),Opcode.GETSTATIC,"CduHub","pg")||!(ops.get(start+1) instanceof FieldInstruction index)||index.opcode()!=Opcode.GETSTATIC||!index.owner().asInternalName().equals("CduHub")||!index.type().equalsString("I")||!(ops.get(start+2) instanceof NewObjectInstruction made)||!made.className().equals(ctor.owner())||ops.get(start+3).opcode()!=Opcode.DUP)throw new IllegalArgumentException("native page constructor/store operands");
            for(int i=start+4;i<at-1;i++)if(!(ops.get(i) instanceof ConstantInstruction)&&!(ops.get(i) instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC))throw new IllegalArgumentException("native page constructor argument provenance "+site+" "+ops.subList(start,at+1));
            if(!subtype(data,made.className().asInternalName(),"CduHub$Page")||result.put(index.name().stringValue(),made.className().asInternalName())!=null)throw new IllegalArgumentException("native page registry duplicate/non-page");
            if(initializer==null)initializer=site.method();else if(!initializer.equals(site.method()))throw new IllegalArgumentException("native page registry multiple writer bodies");
        }
        if(initializer==null||result.isEmpty())throw new IllegalArgumentException("native page registry unproved");int pointers=0;
        for(var id:methods)for(var op:Prepare.instructions(data.methods.get(id)))if(op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("CduHub")&&(f.name().equalsString("pg")||result.containsKey(f.name().stringValue()))){if(!id.equals(initializer))throw new IllegalArgumentException("native page index/reference later writer "+id);if(f.name().equalsString("pg"))pointers++;}
        if(pointers!=1)throw new IllegalArgumentException("native page registry root assignment");return result;
    }
    private static boolean pageMayCall(Prepare data,Prepare.MethodId caller,Prepare.MethodId target,List<Instruction> ops,Set<Integer> ordinals,Map<String,String> pages,Set<Prepare.MethodId> symbolic){
        boolean matching=false;
        for(int at:ordinals)if(ops.get(at) instanceof InvokeInstruction call&&call.name().equalsString(target.name())&&call.type().equalsString(target.descriptor())&&!subtype(data,call.owner().asInternalName(),"CduHub$Page"))return true;
        for(int at:ordinals)if(ops.get(at) instanceof InvokeInstruction call&&(call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE)&&call.name().equalsString(target.name())&&call.type().equalsString(target.descriptor())&&subtype(data,call.owner().asInternalName(),"CduHub$Page")){
            matching=true;int receiver=at-call.typeSymbol().parameterCount()-3;boolean plain=true;for(int i=at-call.typeSymbol().parameterCount();i<at;i++)plain&=ops.get(i) instanceof ConstantInstruction||ops.get(i) instanceof LoadInstruction;
            if(!plain||receiver<0||!Prepare.field(ops.get(receiver),Opcode.GETSTATIC,"CduHub","pg")||!(ops.get(receiver+1) instanceof FieldInstruction index)||index.opcode()!=Opcode.GETSTATIC||!index.owner().asInternalName().equals("CduHub")||ops.get(receiver+2).opcode()!=Opcode.AALOAD||!pages.containsKey(index.name().stringValue())){symbolic.add(caller);return true;}
            if(target.equals(resolve(data,pages.get(index.name().stringValue()),call.name().stringValue(),call.type().stringValue())))return true;
        }
        return !matching;
    }
    private static Set<Integer> reachable(Prepare data,Prepare.MethodId id,boolean memWrite){
        var method=data.methods.get(id);var code=method.code().orElseThrow();var ops=Prepare.instructions(method);var labels=new HashMap<Label,Integer>();int ordinal=0;for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ordinal);if(e instanceof Instruction)ordinal++;}
        if(memWrite&&(!id.owner().equals("FmcGlobal$MemBits")||!id.name().equals("trans")||!id.descriptor().equals("(ZZI)V")||!(ops.get(0) instanceof LoadInstruction load)||load.slot()!=1||load.typeKind()!=TypeKind.INT||!(ops.get(1) instanceof BranchInstruction branch)||branch.opcode()!=Opcode.IFEQ))throw new IllegalArgumentException("native MemBits first-argument branch proof");
        var pending=new ArrayDeque<Integer>();pending.add(0);var reached=new HashSet<Integer>();while(!pending.isEmpty()){int at=pending.removeFirst();if(!reached.add(at))continue;var op=ops.get(at);for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd()))pending.add(labels.get(h.handler()));
            if(op instanceof BranchInstruction b){if(!(memWrite&&at==1))pending.add(labels.get(b.target()));if(op.opcode()==Opcode.GOTO||op.opcode()==Opcode.GOTO_W)continue;}
            else if(op instanceof LookupSwitchInstruction b){pending.add(labels.get(b.defaultTarget()));for(var c:b.cases())pending.add(labels.get(c.target()));continue;}
            else if(op instanceof TableSwitchInstruction b){pending.add(labels.get(b.defaultTarget()));for(var c:b.cases())pending.add(labels.get(c.target()));continue;}
            else if(op instanceof ReturnInstruction||op.opcode()==Opcode.ATHROW)continue;
            if(at+1>=ops.size())throw new IllegalArgumentException("native EXEC CFG falls through");pending.add(at+1);
        }return Set.copyOf(reached);
    }
    private static boolean subtype(Prepare data,String owner,String parent){var seen=new HashSet<String>();while(seen.add(owner)){if(owner.equals(parent))return true;var model=data.models.get(owner);if(model==null)return false;for(var i:model.interfaces())if(i.asInternalName().equals(parent))return true;if(model.superclass().isEmpty())return false;owner=model.superclass().orElseThrow().asInternalName();}return false;}
    private static Prepare.MethodId resolve(Prepare data,String owner,String name,String descriptor){var seen=new HashSet<String>();while(seen.add(owner)){var id=new Prepare.MethodId(owner,name,descriptor);if(data.methods.containsKey(id))return id;var model=data.models.get(owner);if(model==null||model.superclass().isEmpty())return null;owner=model.superclass().orElseThrow().asInternalName();}return null;}
    public static void main(String[] args)throws Exception{var data=new Prepare(Prepare.readJar(java.nio.file.Path.of(args[0])));var p=inspect(data);System.out.println("Qualified dispatch methods="+p.methods.size()+" fields="+p.writes.size()+" pages="+p.pages.size()+" callback-exclusions="+p.excludedCallbackEdges+" page-exclusions="+p.excludedPageEdges+" symbolic-page-callers="+p.symbolicPageCalls.size());for(var id:p.methods){var ops=Prepare.instructions(data.methods.get(id));for(int at:p.instructions.get(id))if(ops.get(at) instanceof InvokeInstruction call&&call.owner().asInternalName().equals("FmcGlobal$MemBits")&&call.name().equalsString("trans"))System.out.println("MEMCALL "+id+" "+at+" "+ops.subList(Math.max(0,at-4),at+1));}}
}
