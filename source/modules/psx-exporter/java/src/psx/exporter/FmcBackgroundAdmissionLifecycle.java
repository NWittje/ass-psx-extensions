package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Native UI timing and finite-job private argument storage are lifetime
 * effects, not entered FMC data. They remain in the potential operation audit
 * and authority graph. Their original ticks, jobs and render continuations
 * must not be rewound by a data journal or rejected merely for reading them. */
final class FmcBackgroundAdmissionLifecycle {
    static final Set<String> CREATED=Set.of("Acars$ThreadTelexDn","CduHub$ThreadTimeoutMsg","ControlDisplayUnit$ThreadPageDelay","FmcDepArr$ThreadArrList","FmcDepArr$ThreadSidList");
    static final Set<FmcQualification.Member> PRESENTATION=Set.of(new FmcQualification.Member("ControlDisplayUnit","specialBlankTimer","I"),new FmcQualification.Member("ControlDisplayUnit","holdState","I"));
    record Profile(Set<FmcQualification.Member> fields,Set<FmcQualification.Member> privateFields,Set<FmcQualification.Member> presentationFields){}
    private FmcBackgroundAdmissionLifecycle(){}
    static Profile inspect(Prepare data,FmcReconstructionExecAudit.Effects effects){
        var expected=Set.of(new FmcQualification.Member("Acars$ThreadTelexDn","s","Ljava/lang/String;"),
            new FmcQualification.Member("CduHub$ThreadTimeoutMsg","c","LControlDisplayUnit;"),new FmcQualification.Member("CduHub$ThreadTimeoutMsg","t","I"),
            new FmcQualification.Member("ControlDisplayUnit$ThreadPageDelay","delay","I"),new FmcQualification.Member("ControlDisplayUnit$ThreadPageDelay","pg","LCduHub$Page;"),new FmcQualification.Member("ControlDisplayUnit$ThreadPageDelay","this$0","LControlDisplayUnit;"),
            new FmcQualification.Member("FmcDepArr$ThreadArrList","aptStr","Ljava/lang/String;"),new FmcQualification.Member("FmcDepArr$ThreadArrList","mode","I"),
            new FmcQualification.Member("FmcDepArr$ThreadSidList","aptStr","Ljava/lang/String;"),new FmcQualification.Member("FmcDepArr$ThreadSidList","rte","LFmcRte$Rte;"));
        require(effects.writes().containsAll(expected)&&effects.writes().containsAll(PRESENTATION),"complete original lifetime vector");
        for(String owner:CREATED){var model=data.models.get(owner);require(model!=null&&model.superclass().isPresent()&&model.superclass().orElseThrow().asInternalName().equals("java/lang/Thread"),"exact constructed native Thread");
            for(var field:model.fields())if((field.flags().flagsMask()&ClassFile.ACC_STATIC)==0)require(expected.contains(new FmcQualification.Member(owner,field.fieldName().stringValue(),field.fieldType().stringValue())),"private job field drift");
        }
        for(var e:data.methods.entrySet())if(e.getValue().code().isPresent())for(var op:Prepare.instructions(e.getValue()))if(op instanceof FieldInstruction f){
            var member=FmcQualification.resolve(data,expected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
            if(member!=null)require(e.getKey().owner().equals(member.owner())&&Set.of("<init>","run").contains(e.getKey().name()),"job argument escapes original constructor/run lifetime "+member+" "+e.getKey());
        }
        // Exact native presentation writers: no entered-route/header/cache
        // loader stores these fields. They control delayed page/blank updates.
        var writers=Map.of("specialBlankTimer",Set.of(new Prepare.MethodId("ControlDisplayUnit","access$002","(LControlDisplayUnit;I)I")),
            "holdState",Set.of(new Prepare.MethodId("ControlDisplayUnit","selectCduPage","(LCduHub$Page;I)V")));
        for(var f:PRESENTATION)require(effects.writers().get(f).equals(writers.get(f.name())),"native presentation effect drift "+f);
        var fields=new HashSet<>(expected);fields.addAll(PRESENTATION);return new Profile(Set.copyOf(fields),expected,PRESENTATION);
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native lifetime partition "+why);}
}
