package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Exact live current-position/IRS receivers, obtained through initialized
 * same-owner read-only bridges on a worker. Artifact objects, type scans of
 * unowned simulator objects and native initializer/input callbacks are absent. */
final class FmcBackgroundAdmissionInputRoots {
    static final String READ="$psxExporter$fmcCurrentInputRoots";
    static final List<FmcQualification.Member> FIELDS=List.of(
        new FmcQualification.Member("Routes","nrp","LFmcGlobal$Pos;"),
        new FmcQualification.Member("FmcGlobal","posL","LFmcGlobal$Pos;"),
        new FmcQualification.Member("FmcGlobal","posR","LFmcGlobal$Pos;"),
        new FmcQualification.Member("InertialReference","iru","[LInertialReference$InertialReferenceUnit;"));
    record Profile(Map<String,byte[]> changed,Map<String,List<FmcQualification.Member>> roots){}
    private FmcBackgroundAdmissionInputRoots(){}
    static Profile transform(Prepare data,Map<String,byte[]> lower){
        var grouped=new TreeMap<String,List<FmcQualification.Member>>();var before=new HashMap<>(lower);var changed=new HashMap<String,byte[]>();
        for(var field:FIELDS){var declared=data.models.get(field.owner()).fields().stream().filter(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.descriptor())).toList();
            require(declared.size()==1&&(declared.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"exact original current reference");grouped.computeIfAbsent(field.owner(),k->new ArrayList<>()).add(field);}
        for(var group:grouped.entrySet()){
            String owner=group.getKey();byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(READ)),"current bridge collision");
            byte[] bytes=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
                b.withMethodBody(READ,MethodTypeDesc.of(CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{c.loadConstant(group.getValue().size()).anewarray(CD_Object).astore(0);for(int i=0;i<group.getValue().size();i++){var field=group.getValue().get(i);c.aload(0).loadConstant(i).getstatic(ClassDesc.of(owner),field.name(),ClassDesc.ofDescriptor(field.descriptor())).aastore();}c.aload(0).areturn();});}});
            require(data.cf.verify(bytes).isEmpty(),"current bridge verifier");changed.put(owner,bytes);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);var roots=new TreeMap<String,List<FmcQualification.Member>>();grouped.forEach((k,v)->roots.put(k,List.copyOf(v)));return new Profile(Map.copyOf(changed),Map.copyOf(roots));
    }
    static Object[] read(FmcInitialization.Context context,Profile profile)throws Throwable{
        require(context.current(),"current worker binding");var result=new ArrayList<Object>();
        for(var entry:profile.roots.entrySet()){
            var owner=context.owner(entry.getKey());var method=owner.getDeclaredMethod(READ);require(method.getReturnType()==Object[].class&&method.getModifiers()==(Modifier.PUBLIC|Modifier.STATIC|ClassFile.ACC_SYNTHETIC)&&method.isSynthetic(),"current bridge flags");
            var getter=MethodHandles.privateLookupIn(owner,MethodHandles.lookup()).findStatic(owner,READ,MethodType.methodType(Object[].class));
            Object[] values=(Object[])getter.invokeExact();require(values.length==entry.getValue().size(),"current root shape");
            for(int i=0;i<values.length;i++){var field=entry.getValue().get(i);Object object=values[i];require(object!=null,"current root unavailable");
                if(field.name().equals("iru")){require(object instanceof Object[]&&((Object[])object).length==3,"current IRS unit roster");var unit=context.owner("InertialReference$InertialReferenceUnit");result.add(object);for(Object row:(Object[])object){require(row!=null&&unit.isInstance(row),"current IRS unit identity");result.add(row);}}
                else{require(context.owner("FmcGlobal$Pos").isInstance(object),"current position receiver");result.add(object);}
            }
        }
        require(context.current(),"current worker lifecycle changed");return result.toArray();
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC current input roots "+why);}
}
