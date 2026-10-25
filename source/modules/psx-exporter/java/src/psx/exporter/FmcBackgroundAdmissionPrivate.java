package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Negative alias proofs for native private formatting work. A matching class
 * name alone never proves privacy: these original methods have no reference
 * inputs or imported objects, and cannot publish the fresh calculation object.
 * Their returned String is a copied value, so unrelated weather/UI formatting
 * cannot observe the owned route's mutable calculation workspace. */
final class FmcBackgroundAdmissionPrivate {
    private static final String DMT="Expression$DegMinTenth";
    private static final Set<String> BUILDERS=Set.of("<init>()V","<init>(I)V","append(C)Ljava/lang/StringBuilder;",
        "append(I)Ljava/lang/StringBuilder;","append(F)Ljava/lang/StringBuilder;","append(D)Ljava/lang/StringBuilder;",
        "append(Ljava/lang/String;)Ljava/lang/StringBuilder;","toString()Ljava/lang/String;");
    private FmcBackgroundAdmissionPrivate(){}
    private static final Map<Prepare,Boolean> VREF_FACTORY=Collections.synchronizedMap(new WeakHashMap<>());
    static boolean unrelatedReceiver(Prepare data,Prepare.MethodId caller,Prepare.MethodId target){
        return unrelatedReceiver(data,caller,target,Set.of());
    }
    static boolean unrelatedReceiver(Prepare data,Prepare.MethodId caller,Prepare.MethodId target,Set<FmcQualification.Member> publishedReferences){
        if(!target.owner().equals("Calc$PlaceBearingDistance"))return false;
        var method=data.methods.get(caller);if(method==null||method.code().isEmpty())return false;
        String descriptor="L"+target.owner()+";";
        if(method.methodTypeSymbol().parameterList().stream().anyMatch(p->p.descriptorString().contains(descriptor)))return false;
        boolean source=false;
        for(var op:Prepare.instructions(method)){
            if(op instanceof FieldInstruction f&&f.type().stringValue().contains(descriptor)){
                if(!f.type().equalsString(descriptor)||f.opcode()!=Opcode.GETFIELD&&f.opcode()!=Opcode.GETSTATIC)return false;
                var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),descriptor,new HashSet<>());
                if(member==null||publishedReferences.contains(member)||!Set.of("Weather$RadarZone","Weather$BasicCore","WeatherSigmet","Pnf","Tcas$Target","RadioAtcBasis").contains(member.owner())||!freshOnlyField(data,member,target.owner()))return false;
                source=true;
            }
            if(op instanceof InvokeInstruction call&&call.typeSymbol().returnType().descriptorString().contains(descriptor))return false;
            if(op instanceof TypeCheckInstruction cast&&cast.type().asInternalName().equals(target.owner()))return false;
            if(op instanceof NewObjectInstruction n&&n.className().asInternalName().equals(target.owner()))source=true;
        }
        return source;
    }
    /** Direct scalar accesses through the same proved private holder also
     * retain their native path. Receiver-call exclusion alone is insufficient
     * when the caller reads the calculated lat/lon itself after that call. */
    static boolean unrelatedAccess(Prepare data,Prepare.MethodId caller,FmcQualification.Member field,Set<FmcQualification.Member> publishedReferences){
        if(field.owner().equals("FmcAppRef$V")&&Set.of("ref25","ref30").contains(field.name())&&field.descriptor().equals("I"))return freshVrefAccess(data,caller);
        if(!field.owner().equals("Calc$PlaceBearingDistance")||field.descriptor().startsWith("L")||field.descriptor().startsWith("["))return false;
        var declaring=data.models.get(field.owner());if(declaring==null)return false;var declarations=declaring.fields().stream().filter(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.descriptor())).toList();if(declarations.size()!=1||(declarations.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)return false;
        var method=data.methods.get(caller);if(method==null||method.code().isEmpty()||method.methodTypeSymbol().returnType().descriptorString().startsWith("L")||method.methodTypeSymbol().returnType().descriptorString().startsWith("["))return false;
        if(!unrelatedReceiver(data,caller,new Prepare.MethodId(field.owner(),"<init>","()V"),publishedReferences))return false;
        for(var op:Prepare.instructions(method)){
            if(op.opcode()==Opcode.ARETURN)return false;
            if(op instanceof InvokeInstruction call)for(var parameter:call.typeSymbol().parameterList()){
                String type=parameter.descriptorString();if(type.equals("Ljava/lang/Object;")||type.equals("[Ljava/lang/Object;")||type.contains("L"+field.owner()+";")&&!call.owner().asInternalName().equals(field.owner()))return false;
            }
        }
        return true;
    }
    /** The numeric VREF factory returns a fresh value record. Retained V
     * holders remain protected: only callers with no other V source, reference
     * publication, cast or generic-object escape get this negative proof. */
    private static boolean freshVrefAccess(Prepare data,Prepare.MethodId caller){
        if(!VREF_FACTORY.computeIfAbsent(data,FmcBackgroundAdmissionPrivate::vrefFactory))return false;
        var factory=new Prepare.MethodId("FmcAppRef","getVref","(II)LFmcAppRef$V;");if(caller.equals(factory))return true;
        var method=data.methods.get(caller);if(method==null||method.code().isEmpty())return false;String type="LFmcAppRef$V;";
        if(method.methodTypeSymbol().parameterList().stream().anyMatch(p->p.descriptorString().contains(type))||method.methodTypeSymbol().returnType().descriptorString().startsWith("L")||method.methodTypeSymbol().returnType().descriptorString().startsWith("["))return false;
        boolean source=false;for(var op:Prepare.instructions(method)){
            if(op.opcode()==Opcode.ARETURN||op.opcode()==Opcode.AASTORE)return false;
            if(op instanceof FieldInstruction f&&(f.type().stringValue().contains(type)||f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)&&reference(f.type().stringValue()))return false;
            if(op instanceof NewObjectInstruction n&&n.className().asInternalName().equals("FmcAppRef$V"))return false;
            if(op instanceof TypeCheckInstruction cast&&cast.type().asInternalName().equals("FmcAppRef$V"))return false;
            if(op instanceof InvokeInstruction call){String result=call.typeSymbol().returnType().descriptorString();if(result.contains(type)){if(!Prepare.call(op,factory.owner(),factory.name(),factory.descriptor()))return false;source=true;}for(var p:call.typeSymbol().parameterList())if(p.descriptorString().contains(type)||p.descriptorString().equals("Ljava/lang/Object;")||p.descriptorString().equals("[Ljava/lang/Object;"))return false;}
        }return source;
    }
    private static boolean reference(String type){return type.startsWith("L")||type.startsWith("[");}
    private static boolean vrefFactory(Prepare data){
        var model=data.models.get("FmcAppRef$V");if(model==null||model.fields().size()!=2)return false;for(var f:model.fields())if(!Set.of("ref25","ref30").contains(f.fieldName().stringValue())||!f.fieldType().equalsString("I")||(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)return false;
        var ctor=data.methods.get(new Prepare.MethodId("FmcAppRef$V","<init>","()V"));if(ctor==null||ctor.code().isEmpty())return false;var ctorOps=Prepare.instructions(ctor);if(ctorOps.size()!=3||!(ctorOps.get(0) instanceof LoadInstruction l)||l.slot()!=0||!Prepare.call(ctorOps.get(1),"java/lang/Object","<init>","()V")||ctorOps.get(2).opcode()!=Opcode.RETURN)return false;
        var method=data.methods.get(new Prepare.MethodId("FmcAppRef","getVref","(II)LFmcAppRef$V;"));if(method==null||method.code().isEmpty()||(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)return false;var ops=Prepare.instructions(method);if(ops.size()<5||!(ops.get(0) instanceof NewObjectInstruction n)||!n.className().asInternalName().equals("FmcAppRef$V")||ops.get(1).opcode()!=Opcode.DUP||!Prepare.call(ops.get(2),"FmcAppRef$V","<init>","()V")||!(ops.get(3) instanceof StoreInstruction local)||local.typeKind()!=TypeKind.REFERENCE)return false;int slot=local.slot(),returns=0;
        var labels=new HashMap<Label,Integer>();int ordinal=0;for(var e:method.code().orElseThrow()){if(e instanceof LabelTarget target)labels.put(target.label(),ordinal);if(e instanceof Instruction)ordinal++;}var entries=new HashSet<Integer>();for(var op:ops){if(op instanceof BranchInstruction b)entries.add(labels.get(b.target()));if(op instanceof LookupSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}if(op instanceof TableSwitchInstruction s){entries.add(labels.get(s.defaultTarget()));for(var c:s.cases())entries.add(labels.get(c.target()));}}
        for(int at=4;at<ops.size();at++){var op=ops.get(at);if(op instanceof StoreInstruction s&&s.slot()==slot)return false;if(op instanceof LoadInstruction load&&load.typeKind()==TypeKind.REFERENCE&&load.slot()!=slot)return false;if(op instanceof NewObjectInstruction||op instanceof TypeCheckInstruction||op instanceof InvokeDynamicInstruction||op instanceof NewReferenceArrayInstruction||op.opcode()==Opcode.AASTORE)return false;
            if(op instanceof FieldInstruction f&&(!f.owner().asInternalName().equals("FmcAppRef$V")||!f.type().equalsString("I")||f.opcode()!=Opcode.GETFIELD&&f.opcode()!=Opcode.PUTFIELD))return false;
            if(op instanceof InvokeInstruction call&&(!call.owner().asInternalName().equals("java/lang/Math")||call.opcode()!=Opcode.INVOKESTATIC||call.typeSymbol().parameterList().stream().anyMatch(p->reference(p.descriptorString()))||reference(call.typeSymbol().returnType().descriptorString())))return false;
            if(op.opcode()==Opcode.ARETURN){if(at==0||entries.contains(at)||!(ops.get(at-1) instanceof LoadInstruction load)||load.typeKind()!=TypeKind.REFERENCE||load.slot()!=slot)return false;returns++;}
        }return returns>0;
    }
    private static boolean freshOnlyField(Prepare data,FmcQualification.Member member,String type){
        int writes=0;
        for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){
            var ops=Prepare.instructions(e.getValue());
            for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction f&&f.type().equalsString(member.descriptor())&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)
                &&member.equals(FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>()))) {
                writes++;
                if(!e.getKey().owner().equals(member.owner())||!Set.of("<init>","<clinit>").contains(e.getKey().name())||at<3
                    ||!Prepare.call(ops.get(at-1),type,"<init>","()V")||ops.get(at-2).opcode()!=Opcode.DUP
                    ||!(ops.get(at-3) instanceof NewObjectInstruction n)||!n.className().asInternalName().equals(type))return false;
            }
        }
        return writes==1;
    }
    static Set<Prepare.MethodId> formatters(Prepare data){
        var found=new HashSet<Prepare.MethodId>();boolean more=true;
        while(more){more=false;for(var e:data.methods.entrySet())if(e.getKey().owner().equals("Expression")&&!found.contains(e.getKey())&&pure(e.getValue(),found)){
            found.add(e.getKey());more=true;
        }}
        return Set.copyOf(found);
    }
    private static boolean pure(MethodModel method,Set<Prepare.MethodId> known){
        if(method.code().isEmpty()||(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0||!method.methodTypeSymbol().returnType().descriptorString().equals("Ljava/lang/String;"))return false;
        for(var p:method.methodTypeSymbol().parameterList())if(p.descriptorString().startsWith("L")||p.descriptorString().startsWith("["))return false;
        int scratch=0;boolean delegate=false;
        for(var op:Prepare.instructions(method)){
            if(op instanceof NewObjectInstruction n){if(n.className().asInternalName().equals(DMT))scratch++;else if(!n.className().asInternalName().equals("java/lang/StringBuilder"))return false;}
            if(op instanceof FieldInstruction f&&!(f.opcode()==Opcode.GETFIELD&&f.owner().asInternalName().equals(DMT)&&Set.of("I","F","D").contains(f.type().stringValue())))return false;
            if(op instanceof InvokeDynamicInstruction||op instanceof NewPrimitiveArrayInstruction||op instanceof NewReferenceArrayInstruction||op instanceof NewMultiArrayInstruction)return false;
            if(switch(op.opcode()){case AASTORE,IASTORE,LASTORE,FASTORE,DASTORE,BASTORE,CASTORE,SASTORE,MONITORENTER,MONITOREXIT->true;default->false;})return false;
            if(op instanceof InvokeInstruction call){
                String owner=call.owner().asInternalName(),shape=call.name().stringValue()+call.type().stringValue();
                if(owner.equals(DMT)){if(!Set.of("<init>()V","set(D)V").contains(shape))return false;}
                else if(owner.equals("java/lang/StringBuilder")){if(!BUILDERS.contains(shape))return false;}
                else if(owner.equals("java/lang/Math")){if(call.opcode()!=Opcode.INVOKESTATIC)return false;}
                else if(known.contains(new Prepare.MethodId(owner,call.name().stringValue(),call.type().stringValue())))delegate=true;
                else return false;
            }
        }
        return scratch==1||scratch==0&&delegate;
    }
}
