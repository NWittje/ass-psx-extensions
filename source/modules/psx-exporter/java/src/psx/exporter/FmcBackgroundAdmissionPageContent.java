package psx.exporter;

import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Qualification of FMC page producers which consume global entered/cache
 * content even while their own glyph receiver is not displayed. Other pages
 * remain receiver-qualified; neither page names nor registry membership imply
 * a global FMC consumer. Input/specialJob handlers are never classified here. */
final class FmcBackgroundAdmissionPageContent {
    private FmcBackgroundAdmissionPageContent(){}
    static Set<Prepare.MethodId> global(Prepare data,Set<FmcQualification.Member> selected,Set<FmcQualification.Member> roots,Set<FmcQualification.Member> children,Set<Prepare.MethodId> candidates,Set<FmcQualification.Member> published){
        if(candidates.isEmpty())return Set.of();return global(data,selected,roots,children,candidates,published,FmcReconstructionExecScope.pages(data));
    }
    static Set<Prepare.MethodId> global(Prepare data,Set<FmcQualification.Member> selected,Set<FmcQualification.Member> roots,Set<FmcQualification.Member> children,Set<Prepare.MethodId> candidates,Set<FmcQualification.Member> published,Map<String,String> pages){
        var fields=new HashSet<>(selected);fields.removeAll(FmcBackgroundAdmissionProfile.BANK_CONTENT);
        var seeds=new HashSet<Prepare.MethodId>();var flow=new FmcBackgroundAdmissionReceiverFlow(data,roots,children);
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var ops=Prepare.instructions(entry.getValue());for(int at:FmcBackgroundAdmissionClosure.pausedInstructions(data,entry.getValue()))if(ops.get(at) instanceof FieldInstruction f){var field=FmcQualification.resolve(data,fields,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());if(field==null)continue;if(f.opcode()==Opcode.GETSTATIC||f.opcode()==Opcode.PUTSTATIC||flow.fieldReceiver(entry.getKey(),at)==FmcBackgroundAdmissionReceiverFlow.Origin.OWNED){seeds.add(entry.getKey());break;}}}
        var evidence=FmcRecallBoundary.inspect(data,Set.of());var dispatch=new FmcBackgroundAdmissionDispatch(data,pages);var reached=new HashSet<>(seeds);var pending=new ArrayDeque<>(seeds);var active=new HashMap<Prepare.MethodId,Set<Integer>>();
        while(!pending.isEmpty()){var id=pending.removeFirst();for(var caller:evidence.callers().getOrDefault(id,Set.of()))if(!reached.contains(caller)&&dispatch.mayCall(caller,id,active.computeIfAbsent(caller,k->FmcBackgroundAdmissionClosure.pausedInstructions(data,data.methods.get(k))))&&!FmcBackgroundAdmissionPrivate.unrelatedReceiver(data,caller,id,published)&&reached.add(caller))pending.add(caller);}
        reached.retainAll(candidates);return Set.copyOf(reached);
    }
}
