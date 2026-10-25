package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

final class FmcReconstructionControlAliases {
    static final String NAME="$psxExporter$fmcControlAliases";
    static final List<String> FIELDS=List.of("sbRteTitleL","sbRteTitleR","sbLegsTitleL","sbLegsTitleR","sb6bL","sb6bR","sbRteTitleCduL","sbRteTitleCduR","sbLegsTitleCduL","sbLegsTitleCduR","sbFltNoTitleBuild");
    private FmcReconstructionControlAliases(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){byte[] input=lower.getOrDefault("FmcRte",data.inputs.get("FmcRte"));var model=data.cf.parse(input);for(String field:FIELDS){var declarations=data.models.get("FmcRte").fields().stream().filter(f->f.fieldName().equalsString(field)&&f.fieldType().equalsString("Ljava/lang/StringBuilder;")&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0).toList();if(declarations.size()!=1)throw new IllegalArgumentException("FMC exact control buffer root");}if(model.methods().stream().anyMatch(m->m.methodName().equalsString(NAME)))throw new IllegalArgumentException("FMC control aliases collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){b.withMethodBody(NAME,MethodTypeDesc.of(CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.loadConstant(FIELDS.size()).anewarray(CD_Object).astore(0);for(int i=0;i<FIELDS.size();i++)c.aload(0).loadConstant(i).getstatic(ClassDesc.of("FmcRte"),FIELDS.get(i),ClassDesc.of("java.lang.StringBuilder")).aastore();c.aload(0).areturn();});}});if(!data.cf.verify(output).isEmpty())throw new IllegalArgumentException("FMC control alias verifier");var changed=new HashMap<>(lower);changed.put("FmcRte",output);var before=new HashMap<>(lower);before.put("FmcRte",input);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    static Object[] read(FmcInitialization.Context context)throws Throwable{if(!context.current())throw new IllegalStateException("FMC control alias lifecycle");var owner=context.owner("FmcRte");var method=owner.getDeclaredMethod(NAME);if(!method.isSynthetic()||!Modifier.isPublic(method.getModifiers())||!Modifier.isStatic(method.getModifiers())||method.getReturnType()!=Object[].class)throw new IllegalStateException("FMC control alias bridge");var getter=MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,NAME,MethodType.methodType(Object[].class));Object[] values=(Object[])getter.invokeExact();if(values.length!=FIELDS.size()||!context.current())throw new IllegalStateException("FMC control alias roster");for(Object value:values)if(!(value instanceof StringBuilder))throw new IllegalStateException("FMC control alias unavailable");return values;}
}
