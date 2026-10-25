package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** A page pointer used solely to dispatch a protected page consumer is not an
 * entered-content read. Every potential content-bearing original override
 * must retain the exact receiver-tagged poll; an unknown target rejects this
 * projection. The pointer cannot escape to a local, field or stack copy. */
final class FmcBackgroundAdmissionDispatchProjection {
    private static final FmcQualification.Member PAGE=new FmcQualification.Member("ControlDisplayUnit","page","LCduHub$Page;");
    private static final Map<FmcBackgroundAdmissionProfile.Plan,Set<Prepare.MethodId>> CONTENT=Collections.synchronizedMap(new WeakHashMap<>());
    private FmcBackgroundAdmissionDispatchProjection(){}
    static Set<FmcBackgroundAdmissionProjection.Site> qualify(Prepare data,Map<String,byte[]> lower,FmcBackgroundAdmissionProfile.Plan plan){
        var content=CONTENT.computeIfAbsent(plan,k->{var fields=new HashSet<>(k.fields());fields.removeAll(FmcBackgroundAdmissionProfile.BANK_CONTENT);var evidence=FmcRecallBoundary.inspect(data,fields);var reached=new HashSet<Prepare.MethodId>(evidence.readers());reached.addAll(evidence.writers());var pending=new ArrayDeque<>(reached);while(!pending.isEmpty())for(var caller:evidence.callers().getOrDefault(pending.removeFirst(),Set.of()))if(reached.add(caller))pending.add(caller);return Set.copyOf(reached);});
        return qualify(data,lower,content,plan.polls(),plan.receivers(),plan.globalPageContent());
    }
    static Set<FmcBackgroundAdmissionProjection.Site> qualify(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> content,Set<Prepare.MethodId> polls,Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers){
        return qualify(data,lower,content,polls,receivers,Set.of());
    }
    static Set<FmcBackgroundAdmissionProjection.Site> qualify(Prepare data,Map<String,byte[]> lower,Set<Prepare.MethodId> content,Set<Prepare.MethodId> polls,Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers,Set<Prepare.MethodId> globalPages){
        var safe=new HashMap<String,Boolean>();var counts=new HashMap<Prepare.MethodId,Integer>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var ops=Prepare.instructions(entry.getValue());var incoming=entries(entry.getValue());int count=0;for(int at=0;at+1<ops.size();at++)if(pair(data,ops,at,incoming,safe,content,polls,receivers,globalPages))count++;if(count>0)counts.put(entry.getKey(),count);}
        var result=new HashSet<FmcBackgroundAdmissionProjection.Site>();var models=FmcQualification.finalModels(data,lower);
        for(var entry:counts.entrySet()){var id=entry.getKey();var method=models.get(id.owner()).methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).findFirst().orElseThrow();var ops=Prepare.instructions(method);var incoming=entries(method);int count=0;for(int at=0;at+1<ops.size();at++)if(pair(data,ops,at,incoming,safe,content,polls,receivers,globalPages)){count++;result.add(new FmcBackgroundAdmissionProjection.Site(id,at));}require(count==entry.getValue(),"prepared dispatch-only pointer drift "+id);}
        return Set.copyOf(result);
    }
    private static boolean pair(Prepare data,List<Instruction> ops,int at,Set<Integer> entries,Map<String,Boolean> safe,Set<Prepare.MethodId> content,Set<Prepare.MethodId> polls,Map<Prepare.MethodId,FmcExclusionPrepare.Receiver> receivers,Set<Prepare.MethodId> globalPages){
        if(!(ops.get(at) instanceof FieldInstruction f)||f.opcode()!=Opcode.GETFIELD||!PAGE.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()))||entries.contains(at+1)||!(ops.get(at+1) instanceof InvokeInstruction call)||call.opcode()!=Opcode.INVOKEVIRTUAL||!call.type().equalsString("()V")||!subtype(data,call.owner().asInternalName(),"CduHub$Page")||!(call.name().equalsString("processAll")||call.name().equalsString("processTitle")||call.name().stringValue().startsWith("processText")))return false;
        return safe.computeIfAbsent(call.owner().asInternalName()+"."+call.name(),k->{boolean found=false;for(var id:data.methods.keySet())if(id.name().equals(call.name().stringValue())&&id.descriptor().equals("()V")&&subtype(data,id.owner(),call.owner().asInternalName())){var method=data.methods.get(id);if((method.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)continue;found=true;if(content.contains(id)){var receiver=receivers.get(id);if(!polls.contains(id)||!globalPages.contains(id)&&(receiver==null||receiver.slot()!=0||!"CduHub$Page".equals(receiver.tagOwner())))return false;}else if(method.code().isEmpty()&&(method.flags().flagsMask()&ClassFile.ACC_ABSTRACT)==0)return false;}return found;});
    }
    private static boolean subtype(Prepare data,String type,String parent){var seen=new HashSet<String>();while(seen.add(type)){if(type.equals(parent))return true;var model=data.models.get(type);if(model==null||model.superclass().isEmpty())return false;type=model.superclass().orElseThrow().asInternalName();}return false;}
    private static Set<Integer> entries(MethodModel method){var labels=new HashMap<Label,Integer>();int at=0;var code=method.code().orElseThrow();for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),at);if(e instanceof Instruction)at++;}var entries=new HashSet<Integer>();for(var op:Prepare.instructions(method)){if(op instanceof BranchInstruction b)entries.add(labels.get(b.target()));if(op instanceof LookupSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}if(op instanceof TableSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}}for(var h:code.exceptionHandlers())entries.add(labels.get(h.handler()));return entries;}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC dispatch projection "+why);}
}
