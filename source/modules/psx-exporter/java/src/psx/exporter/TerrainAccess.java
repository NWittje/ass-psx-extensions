package psx.exporter;

import java.io.*;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Preparation-only member resolution. Classes are read as bytes, never defined or initialized. */
final class TerrainAccess {
    private record Ref(boolean field,String owner,String name,String descriptor,Opcode opcode) {}
    private record Member(String owner,int flags) {}
    private final Map<String,ClassModel> models=new HashMap<>();
    private final Map<String,byte[]> definitions;
    private final ClassFile cf=ClassFile.of();
    private TerrainAccess(Map<String,byte[]> classes){definitions=classes;}
    private ClassModel model(String name){
        ClassModel found=models.get(name);if(found!=null)return found;
        byte[] bytes=definitions.get(name);
        if(bytes!=null){
            found=cf.parse(bytes);if(!found.thisClass().asInternalName().equals(name))throw new IllegalArgumentException("terrain owner identity "+name);
            models.put(name,found);return found;
        }
        try(InputStream input=TerrainAccess.class.getResourceAsStream("/"+name+".class")){
            if(input==null)throw new IllegalArgumentException("terrain unresolved owner "+name);
            found=cf.parse(input.readAllBytes());models.put(name,found);return found;
        }catch(IOException failure){throw new IllegalArgumentException("terrain owner bytes "+name,failure);}
    }
    private static String pkg(String name){int slash=name.lastIndexOf('/');return slash<0?"":name.substring(0,slash);}
    private static boolean accessible(String caller,String declaring,int flags){
        if((flags&ClassFile.ACC_PUBLIC)!=0)return true;
        if((flags&ClassFile.ACC_PRIVATE)!=0)return caller.equals(declaring);
        // No terrain injection requires cross-package protected or nest-private access.
        // Reject such references conservatively rather than assume receiver/nest qualification.
        return pkg(caller).equals(pkg(declaring));
    }
    private Member resolve(Ref ref,String owner,Set<String> visited){
        if(!visited.add(owner))return null;ClassModel type=model(owner);
        if(ref.field){
            for(FieldModel f:type.fields())if(f.fieldName().equalsString(ref.name)&&f.fieldType().equalsString(ref.descriptor))return new Member(owner,f.flags().flagsMask());
        }else{
            for(MethodModel m:type.methods())if(m.methodName().equalsString(ref.name)&&m.methodType().equalsString(ref.descriptor))return new Member(owner,m.flags().flagsMask());
            if(ref.name.equals("<init>"))return null;
        }
        // Field lookup searches interfaces before the superclass; method lookup the reverse.
        if(!ref.field&&type.superclass().isPresent()){
            Member found=resolve(ref,type.superclass().get().asInternalName(),visited);if(found!=null)return found;
        }
        for(var parent:type.interfaces()){
            Member found=resolve(ref,parent.asInternalName(),visited);if(found!=null)return found;
        }
        if(ref.field&&type.superclass().isPresent())return resolve(ref,type.superclass().get().asInternalName(),visited);
        return null;
    }
    private void verify(String caller,Ref ref){
        ClassModel owner=model(ref.owner);
        if(!accessible(caller,ref.owner,owner.flags().flagsMask()))throw new IllegalArgumentException("terrain inaccessible owner "+caller+" -> "+ref);
        Member member=resolve(ref,ref.owner,new HashSet<>());
        if(member==null)throw new IllegalArgumentException("terrain unresolved member "+caller+" -> "+ref);
        if(!accessible(caller,member.owner,member.flags))throw new IllegalArgumentException("terrain inaccessible member "+caller+" -> "+ref);
        boolean requiredStatic=ref.opcode==Opcode.GETSTATIC||ref.opcode==Opcode.PUTSTATIC||ref.opcode==Opcode.INVOKESTATIC;
        if(requiredStatic!=((member.flags&ClassFile.ACC_STATIC)!=0))throw new IllegalArgumentException("terrain member staticness "+caller+" -> "+ref);
        if(!ref.field&&(ref.opcode==Opcode.INVOKEINTERFACE)&&((owner.flags().flagsMask()&ClassFile.ACC_INTERFACE)==0))throw new IllegalArgumentException("terrain interface owner "+ref);
    }
    private static Ref reference(CodeElement instruction){
        if(instruction instanceof FieldInstruction f)return new Ref(true,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),f.opcode());
        if(instruction instanceof InvokeInstruction m)return new Ref(false,m.owner().asInternalName(),m.name().stringValue(),m.type().stringValue(),m.opcode());
        return null;
    }
    private static Map<String,Map<Ref,Integer>> references(ClassModel type){
        Map<String,Map<Ref,Integer>> result=new HashMap<>();
        for(MethodModel method:type.methods())if(method.code().isPresent()){
            Map<Ref,Integer> counts=new HashMap<>();for(CodeElement instruction:method.code().get()){
                Ref ref=reference(instruction);if(ref!=null)counts.merge(ref,1,Integer::sum);
            }
            result.put(method.methodName().stringValue()+method.methodType().stringValue(),counts);
        }return result;
    }
    static void verifyAdded(Map<String,byte[]> before,Map<String,byte[]> after,Map<String,byte[]> originals){
        Map<String,byte[]> complete=new HashMap<>(originals);complete.putAll(after);TerrainAccess audit=new TerrainAccess(complete);
        for(var entry:after.entrySet()){
            // Identical bytes contain no newly added reference. Preserve the
            // same added-reference contract without repeatedly traversing the
            // unchanged lower layers (including large native frame classes).
            byte[] previous=before.get(entry.getKey());
            if(previous!=null&&Arrays.equals(previous,entry.getValue()))continue;
            Map<String,Map<Ref,Integer>> old=before.containsKey(entry.getKey())?references(audit.cf.parse(before.get(entry.getKey()))):Map.of();
            for(var method:references(audit.model(entry.getKey())).entrySet())for(var ref:method.getValue().entrySet())
                if(ref.getValue()>old.getOrDefault(method.getKey(),Map.of()).getOrDefault(ref.getKey(),0))audit.verify(entry.getKey(),ref.getKey());
        }
    }
}
