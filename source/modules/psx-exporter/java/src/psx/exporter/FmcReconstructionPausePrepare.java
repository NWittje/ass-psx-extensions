package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only ordinary Pause/Freeze precondition. Mask1 is Pause, mask2 is Motion
 * Freeze; this getter requests neither and never consumes a native bank. */
final class FmcReconstructionPausePrepare {
    static final String STATE="$psxExporter$fmcRecallPauseState";
    static final Set<FmcQualification.Member> FIELDS=Set.of(new FmcQualification.Member("Core","paused","Z"),new FmcQualification.Member("Core","globSimPaused","I"));
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        for(var field:FIELDS)if(data.models.get("Core").fields().stream().filter(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.descriptor())&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0).count()!=1)throw new IllegalArgumentException("FMC exact native Pause field");
        byte[] input=lower.getOrDefault("Core",data.inputs.get("Core"));var model=data.cf.parse(input);
        if(model.methods().stream().anyMatch(m->m.methodName().equalsString(STATE)))throw new IllegalArgumentException("FMC Pause getter collision");
        var owner=ClassDesc.of("Core");byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){b.withMethodBody(STATE,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var paused=c.newLabel();c.getstatic(owner,"paused",CD_boolean).ifne(paused).iconst_m1().ireturn().labelBinding(paused).getstatic(owner,"globSimPaused",CD_int).ireturn();});}
        });
        if(!data.cf.verify(output).isEmpty())throw new IllegalArgumentException("FMC Pause getter verifier");var before=new HashMap<>(lower);before.put("Core",input);var finalClasses=new HashMap<>(lower);finalClasses.put("Core",output);TerrainAccess.verifyAdded(before,finalClasses,data.inputs);return Map.of("Core",output);
    }
}
