package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** A native page pointer consumed immediately as its immutable subsystem ID
 * does not expose entered FMC state. Preserve that exact projection in normal
 * render/control work; every other page access/publication remains protected. */
final class FmcBackgroundAdmissionProjection {
    record Site(Prepare.MethodId method,int ordinal){}
    private static final FmcQualification.Member PAGE=new FmcQualification.Member("ControlDisplayUnit","page","LCduHub$Page;");
    private static final FmcQualification.Member REGISTRY=new FmcQualification.Member("CduHub","pg","[LCduHub$Page;");
    private static final Set<FmcQualification.Member> DISCRIMINATORS=Set.of(new FmcQualification.Member("CduHub$Page","subsys","I"),new FmcQualification.Member("CduHub$Page","qiIdVal","I"));
    private FmcBackgroundAdmissionProjection(){}
    static Set<Site> qualify(Prepare data,Map<String,byte[]> lower){
        if(!data.models.containsKey(PAGE.owner())||!data.models.containsKey("CduHub$Page"))return Set.of();
        var originals=new HashMap<Prepare.MethodId,Map<FmcQualification.Member,Integer>>();var identities=new HashMap<Prepare.MethodId,Integer>();var stores=new HashMap<FmcQualification.Member,Integer>();var mutable=new HashSet<FmcQualification.Member>();
        for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){
            var ops=Prepare.instructions(e.getValue());var pairs=new HashMap<FmcQualification.Member,Integer>();var entries=entries(e.getValue());int identityCount=0;
            for(int at=0;at<ops.size();at++){
                var op=ops.get(at);
                if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC))for(var field:DISCRIMINATORS)if(member(data,f,field)){
                    if(f.opcode()!=Opcode.PUTFIELD||!e.getKey().owner().equals(field.owner())||!e.getKey().name().equals("<init>"))mutable.add(field);stores.merge(field,1,Integer::sum);
                }
                if(at+1<ops.size()){var field=pair(data,ops.get(at),ops.get(at+1));if(field!=null)pairs.merge(field,1,Integer::sum);}
                if(identity(data,ops,at,entries))identityCount++;
            }
            if(!pairs.isEmpty())originals.put(e.getKey(),Map.copyOf(pairs));
            if(identityCount>0)identities.put(e.getKey(),identityCount);
        }
        if(originals.isEmpty()&&identities.isEmpty())return Set.of();for(var pairs:originals.values())for(var field:pairs.keySet()){require(!mutable.contains(field),"discriminator mutable after construction "+field);require(stores.getOrDefault(field,0)==1,"sole native discriminator constructor store "+field);}
        var sites=new HashSet<Site>();var models=FmcQualification.finalModels(data,lower);
        for(var e:originals.entrySet()){
            var m=models.get(e.getKey().owner()).methods().stream().filter(v->v.methodName().equalsString(e.getKey().name())&&v.methodType().equalsString(e.getKey().descriptor())).findFirst().orElseThrow();
            var ops=Prepare.instructions(m);var counts=new HashMap<FmcQualification.Member,Integer>();for(int at=0;at+1<ops.size();at++){var field=pair(data,ops.get(at),ops.get(at+1));if(field!=null){counts.merge(field,1,Integer::sum);sites.add(new Site(e.getKey(),at));sites.add(new Site(e.getKey(),at+1));}}
            require(counts.equals(e.getValue()),"prepared native projection count drift "+e.getKey());
        }
        for(var e:identities.entrySet()){
            var m=models.get(e.getKey().owner()).methods().stream().filter(v->v.methodName().equalsString(e.getKey().name())&&v.methodType().equalsString(e.getKey().descriptor())).findFirst().orElseThrow();var ops=Prepare.instructions(m);var entries=entries(m);int count=0;
            for(int at=0;at<ops.size();at++)if(identity(data,ops,at,entries)){count++;sites.add(new Site(e.getKey(),at));}require(count==e.getValue(),"prepared native identity-only def-use drift "+e.getKey());
        }
        return Set.copyOf(sites);
    }
    /** The page value stays on the stack and is consumed only by the final
     * identity comparison. The other operand is an exact native pg[] lookup;
     * index loads never dereference the page value. No entry into the middle
     * of this block, duplication, local/field publication or call is allowed. */
    private static boolean identity(Prepare data,List<Instruction> ops,int at,Set<Integer> entries){
        if(at+4>=ops.size()||!(ops.get(at) instanceof FieldInstruction page)||page.opcode()!=Opcode.GETFIELD||!member(data,page,PAGE)
            ||!(ops.get(at+1) instanceof FieldInstruction registry)||registry.opcode()!=Opcode.GETSTATIC||!member(data,registry,REGISTRY))return false;
        int index=at+2;var start=ops.get(index++);boolean scalar=start instanceof LoadInstruction l&&l.typeKind()==TypeKind.INT||start instanceof ConstantInstruction c&&c.constantValue() instanceof Integer||start instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.type().equalsString("I");
        if(!scalar){if(!(start instanceof LoadInstruction l)||l.typeKind()!=TypeKind.REFERENCE)return false;int links=0;while(index<ops.size()&&links++<5){if(!(ops.get(index++) instanceof FieldInstruction field)||field.opcode()!=Opcode.GETFIELD)return false;String type=field.type().stringValue();if(type.equals("I")){scalar=true;break;}if(!type.startsWith("L"))return false;}}
        if(!scalar||index+1>=ops.size()||ops.get(index++).opcode()!=Opcode.AALOAD||!Set.of(Opcode.IF_ACMPEQ,Opcode.IF_ACMPNE).contains(ops.get(index).opcode()))return false;
        for(int p=at+1;p<=index;p++)if(entries.contains(p))return false;return true;
    }
    private static Set<Integer> entries(MethodModel method){
        var labels=new HashMap<Label,Integer>();int ordinal=0;var code=method.code().orElseThrow();for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ordinal);if(e instanceof Instruction)ordinal++;}
        var result=new HashSet<Integer>();for(var op:Prepare.instructions(method)){if(op instanceof BranchInstruction b)result.add(labels.get(b.target()));if(op instanceof LookupSwitchInstruction s){result.add(labels.get(s.defaultTarget()));for(var c:s.cases())result.add(labels.get(c.target()));}if(op instanceof TableSwitchInstruction s){result.add(labels.get(s.defaultTarget()));for(var c:s.cases())result.add(labels.get(c.target()));}}
        for(var h:code.exceptionHandlers())result.add(labels.get(h.handler()));return Set.copyOf(result);
    }
    private static FmcQualification.Member pair(Prepare data,Instruction first,Instruction second){if(first instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD&&member(data,f,PAGE)&&second instanceof FieldInstruction s&&s.opcode()==Opcode.GETFIELD)for(var field:DISCRIMINATORS)if(member(data,s,field))return field;return null;}
    private static boolean member(Prepare data,FieldInstruction f,FmcQualification.Member expected){return expected.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC immutable page projection "+why);}
}
