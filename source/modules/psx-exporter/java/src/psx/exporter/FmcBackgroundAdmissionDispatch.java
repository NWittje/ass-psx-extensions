package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Exact negative receiver proof for native callback fields. Every original
 * writer must create a fresh known callback; one opaque/aliased writer keeps
 * conservative virtual dispatch. This never attributes callbacks by nearby
 * field names or changes any native event execution. */
final class FmcBackgroundAdmissionDispatch {
    private final Prepare data;
    private final Map<Prepare.MethodId,List<Instruction>> instructions=new HashMap<>();
    private final Map<FmcQualification.Member,Set<String>> types=new HashMap<>();
    private final Set<FmcQualification.Member> opaque=new HashSet<>();
    private final Map<Prepare.MethodId,Boolean> dormantConstructors=new HashMap<>();
    private final Map<Prepare.MethodId,Boolean> readAccessors=new HashMap<>();
    private final Map<String,String> pages;
    private final Map<Prepare.MethodId,Set<Integer>> entries=new HashMap<>();
    FmcBackgroundAdmissionDispatch(Prepare data){
        this(data,Map.of());
    }
    FmcBackgroundAdmissionDispatch(Prepare data,Map<String,String> pages){
        this.data=data;
        this.pages=Map.copyOf(pages);
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            var ops=Prepare.instructions(entry.getValue());instructions.put(entry.getKey(),ops);
            for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)&&f.type().equalsString("LSwitchEvent;")){
                var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());
                if(field==null)continue;
                Fresh made=constructed(ops,at);if(made!=null){var incoming=entries.computeIfAbsent(entry.getKey(),k->entries(data.methods.get(k)));for(int i=made.first()+1;i<=at;i++)if(incoming.contains(i)){made=null;break;}}if(made==null){if(nullStore(entry.getKey(),at))types.computeIfAbsent(field,k->new HashSet<>());else opaque.add(field);}else types.computeIfAbsent(field,k->new HashSet<>()).add(made.type());
            }
        }
        resolveParameters();
    }
    private record Write(Prepare.MethodId method,int at){}
    private record Call(Prepare.MethodId method,int at,InvokeInstruction instruction){}
    private record Formal(int index,boolean constructor){}
    private boolean nullStore(Prepare.MethodId method,int at){var ops=instructions.get(method);return at>0&&ops.get(at-1).opcode()==Opcode.ACONST_NULL&&!entries.computeIfAbsent(method,k->entries(data.methods.get(k))).contains(at);}
    /** Every original writer is either an exact fresh expression, null, or a
     * pure formal assignment whose complete caller census supplies independently
     * proven callback fields. A cycle/unknown writer remains conservative. */
    private void resolveParameters(){
        var writes=new HashMap<FmcQualification.Member,List<Write>>();var calls=new HashMap<String,List<Call>>();
        for(var entry:instructions.entrySet()){var ops=entry.getValue();for(int at=0;at<ops.size();at++){
            if(ops.get(at) instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)&&f.type().equalsString("LSwitchEvent;")){
                var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field!=null)writes.computeIfAbsent(field,k->new ArrayList<>()).add(new Write(entry.getKey(),at));
            }
            if(ops.get(at) instanceof InvokeInstruction c)calls.computeIfAbsent(c.name().stringValue()+c.type().stringValue(),k->new ArrayList<>()).add(new Call(entry.getKey(),at,c));
        }}
        boolean progress;do{progress=false;for(var field:Set.copyOf(opaque)){
            var known=new HashSet<>(types.getOrDefault(field,Set.of()));boolean unknown=false;var sites=writes.getOrDefault(field,List.of());if(sites.isEmpty())continue;
            for(var write:sites){if(nullStore(write.method(),write.at()))continue;var ops=instructions.get(write.method());var fresh=constructed(ops,write.at());
                if(fresh!=null){var incoming=entries.computeIfAbsent(write.method(),k->entries(data.methods.get(k)));boolean exclusive=true;for(int i=fresh.first()+1;i<=write.at();i++)if(incoming.contains(i))exclusive=false;if(exclusive){known.add(fresh.type());continue;}}
                var formal=formal(write);if(formal==null){unknown=true;break;}var mt=data.methods.get(write.method()).methodTypeSymbol();boolean found=false;
                for(var source:calls.getOrDefault(write.method().name()+write.method().descriptor(),List.of())){var c=source.instruction();
                    if(formal.constructor()){if(c.opcode()!=Opcode.INVOKESPECIAL||!c.owner().asInternalName().equals(write.method().owner()))continue;}
                    else {if(!possibleCall(c,write.method()))continue;if(c.opcode()!=Opcode.INVOKEVIRTUAL&&c.opcode()!=Opcode.INVOKESPECIAL&&c.opcode()!=Opcode.INVOKEINTERFACE){unknown=true;break;}}
                    found=true;var parent=instructions.get(source.method());int end=source.at();for(int n=mt.parameterCount()-1;n>formal.index();n--){end=expressionStart(parent,end);if(end<0)break;}
                    int first=end>0&&((parent.get(end-1) instanceof FieldInstruction f&&f.type().equalsString("LSwitchEvent;")&&(f.opcode()==Opcode.GETSTATIC||f.opcode()==Opcode.GETFIELD))||parent.get(end-1).opcode()==Opcode.ACONST_NULL)?end-1:end>0?expressionStart(parent,end):-1;if(first<0){unknown=true;break;}var incoming=entries.computeIfAbsent(source.method(),k->entries(data.methods.get(k)));for(int i=first+1;i<=source.at();i++)if(incoming.contains(i))unknown=true;if(unknown)break;
                    var supplied=argumentTypes(source.method(),parent,end,calls,new HashSet<>());if(supplied==null){unknown=true;break;}known.addAll(supplied);
                }
                if(unknown||!found){unknown=true;break;}
            }
            if(!unknown){types.put(field,Set.copyOf(known));opaque.remove(field);progress=true;}
        }}while(progress);
    }
    private record Argument(Prepare.MethodId method,int index){}
    private Set<String> argumentTypes(Prepare.MethodId caller,List<Instruction> ops,int end,Map<String,List<Call>> calls,Set<Argument> visiting){
        if(end<=0)return null;var value=ops.get(end-1);if(value.opcode()==Opcode.ACONST_NULL)return Set.of();var made=constructed(ops,end);if(made!=null)return Set.of(made.type());
        if(value instanceof FieldInstruction f&&f.type().equalsString("LSwitchEvent;")&&(f.opcode()==Opcode.GETSTATIC||f.opcode()==Opcode.GETFIELD)){
            var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());return field!=null&&!opaque.contains(field)?types.get(field):null;
        }
        if(!(value instanceof LoadInstruction parameter)||parameter.typeKind()!=TypeKind.REFERENCE)return null;
        var callerModel=data.methods.get(caller);var mt=callerModel.methodTypeSymbol();boolean staticMethod=(callerModel.flags().flagsMask()&ClassFile.ACC_STATIC)!=0;int index=-1,slot=staticMethod?0:1;for(int n=0;n<mt.parameterCount();n++){if(slot==parameter.slot()&&mt.parameterType(n).descriptorString().equals("LSwitchEvent;"))index=n;slot+=mt.parameterType(n).descriptorString().matches("[JD]")?2:1;}if(index<0)return null;
        for(var op:ops)if(op instanceof StoreInstruction store&&store.slot()==parameter.slot()||op instanceof IncrementInstruction increment&&increment.slot()==parameter.slot())return null;
        var key=new Argument(caller,index);if(!visiting.add(key))return null;try{var known=new HashSet<String>();boolean found=false;
            for(var source:calls.getOrDefault(caller.name()+caller.descriptor(),List.of())){var c=source.instruction();if(caller.name().equals("<init>")){if(c.opcode()!=Opcode.INVOKESPECIAL||!c.owner().asInternalName().equals(caller.owner()))continue;}else{if(!possibleCall(c,caller))continue;if(staticMethod?c.opcode()!=Opcode.INVOKESTATIC:c.opcode()!=Opcode.INVOKEVIRTUAL&&c.opcode()!=Opcode.INVOKESPECIAL&&c.opcode()!=Opcode.INVOKEINTERFACE)return null;}found=true;
                var parent=instructions.get(source.method());int argumentEnd=source.at();for(int n=mt.parameterCount()-1;n>index;n--){argumentEnd=expressionStart(parent,argumentEnd);if(argumentEnd<0)break;}
                if(argumentEnd<=0)return null;var producer=parent.get(argumentEnd-1);int first=producer instanceof FieldInstruction f&&f.type().equalsString("LSwitchEvent;")?argumentEnd-1:expressionStart(parent,argumentEnd);if(first<0)return null;
                var incoming=entries.computeIfAbsent(source.method(),k->entries(data.methods.get(k)));for(int i=first+1;i<=source.at();i++)if(incoming.contains(i))return null;
                var supplied=argumentTypes(source.method(),parent,argumentEnd,calls,visiting);if(supplied==null)return null;known.addAll(supplied);
            }if(!found&&caller.name().equals("<init>")&&dormantConstructors.computeIfAbsent(caller,k->FmcBackgroundAdmissionDormantConstructor.provenDormant(data,k)))return Set.of();return found?Set.copyOf(known):null;
        }finally{visiting.remove(key);}
    }
    private Formal formal(Write write){
        var method=data.methods.get(write.method());var ops=instructions.get(write.method());var mt=method.methodTypeSymbol();if(!(ops.get(write.at()) instanceof FieldInstruction field)||field.opcode()!=Opcode.PUTFIELD)return null;
        if((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0){
            if(write.at()<2||!(ops.get(write.at()-1) instanceof LoadInstruction parameter)||parameter.typeKind()!=TypeKind.REFERENCE||!(ops.get(write.at()-2) instanceof LoadInstruction self)||self.slot()!=0||self.typeKind()!=TypeKind.REFERENCE)return null;
            int slot=1,index=-1;for(int n=0;n<mt.parameterCount();n++){if(slot==parameter.slot()&&mt.parameterType(n).descriptorString().equals("LSwitchEvent;"))index=n;slot+=mt.parameterType(n).descriptorString().matches("[JD]")?2:1;}
            if(index<0)return null;for(var op:ops)if(op instanceof StoreInstruction store&&store.slot()==parameter.slot()||op instanceof IncrementInstruction increment&&increment.slot()==parameter.slot())return null;
            var incoming=entries.computeIfAbsent(write.method(),k->entries(data.methods.get(k)));if(incoming.contains(write.at()-1)||incoming.contains(write.at()))return null;return new Formal(index,write.method().name().equals("<init>"));
        }
        var shape=ops.stream().map(Instruction::opcode).toList();boolean one=mt.parameterCount()==1&&mt.parameterType(0).descriptorString().equals("LSwitchEvent;")&&shape.equals(List.of(Opcode.ALOAD_0,Opcode.ALOAD_1,Opcode.PUTFIELD,Opcode.RETURN))&&write.at()==2;
        boolean two=mt.parameterCount()==2&&mt.parameterType(1).descriptorString().equals("LSwitchEvent;")&&mt.parameterType(0).descriptorString().startsWith("L")&&shape.equals(List.of(Opcode.ALOAD_0,Opcode.ALOAD_1,Opcode.PUTFIELD,Opcode.ALOAD_0,Opcode.ALOAD_2,Opcode.PUTFIELD,Opcode.RETURN))&&write.at()==5;
        if(method.flags().flagsMask()!=0||!mt.returnType().descriptorString().equals("V")||(!one&&!two)||!method.code().orElseThrow().exceptionHandlers().isEmpty())return null;
        for(var op:ops)if(op instanceof FieldInstruction f&&!f.owner().asInternalName().equals(write.method().owner()))return null;return new Formal(mt.parameterCount()-1,false);
    }
    /** Decode only side-effect-free operand chains; all other shapes remain
     * unknown. Instruction boundaries and control-flow entries are checked by
     * the caller before using a reference's type as negative evidence. */
    private int expressionStart(List<Instruction> ops,int end){
        if(end<=0)return -1;var op=ops.get(end-1);
        if(op instanceof LoadInstruction||op instanceof ConstantInstruction)return end-1;
        if(op instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.name().equalsString("<init>")){var fresh=constructed(ops,end);return fresh==null?-1:fresh.first();}
        if(op instanceof InvokeInstruction call&&readAccessor(call))return end-1;
        if(op instanceof FieldInstruction f){if(f.opcode()==Opcode.GETSTATIC)return end-1;if(f.opcode()==Opcode.GETFIELD)return expressionStart(ops,end-1);}
        return -1;
    }
    private record Fresh(String type,int first){}
    /** Old Java compilers expose a private static object through a synthetic
     * getter. It is one operand, never a callback producer or an event cut.
     * Require the complete original getter, mapped model and method index to
     * agree before looking below that operand on the caller's stack. */
    private boolean readAccessor(InvokeInstruction call){
        if(call.opcode()!=Opcode.INVOKESTATIC||call.typeSymbol().parameterCount()!=0||!call.typeSymbol().returnType().descriptorString().startsWith("L"))return false;
        var id=new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());
        return readAccessors.computeIfAbsent(id,key->{
            var model=data.models.get(key.owner());var method=data.methods.get(key);var original=data.inputs.get(key.owner());
            if(model==null||method==null||original==null||method.flags().flagsMask()!=(ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC)||method.code().isEmpty()||!method.code().orElseThrow().exceptionHandlers().isEmpty())return false;
            var mapped=model.methods().stream().filter(m->m.methodName().equalsString(key.name())&&m.methodType().equalsString(key.descriptor())).toList();
            var nativeMethods=data.cf.parse(original).methods().stream().filter(m->m.methodName().equalsString(key.name())&&m.methodType().equalsString(key.descriptor())).toList();
            if(mapped.size()!=1||nativeMethods.size()!=1||!Arrays.equals(accessorBody(key,method),accessorBody(key,mapped.getFirst()))||!Arrays.equals(accessorBody(key,method),accessorBody(key,nativeMethods.getFirst())))return false;
            var ops=Prepare.instructions(method);
            return ops.size()==2&&ops.getFirst() instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.type().stringValue().equals(method.methodTypeSymbol().returnType().descriptorString())&&ops.get(1).opcode()==Opcode.ARETURN;
        });
    }
    private byte[] accessorBody(Prepare.MethodId id,MethodModel method){return data.cf.build(java.lang.constant.ClassDesc.of(id.owner()),b->b.withMethod(method.methodName().stringValue(),method.methodTypeSymbol(),method.flags().flagsMask(),m->{if(method.code().isPresent())m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e));}));}
    private Fresh constructed(List<Instruction> ops,int store){
        if(store<3||!(ops.get(store-1) instanceof InvokeInstruction call)||call.opcode()!=Opcode.INVOKESPECIAL||!call.name().equalsString("<init>"))return null;
        int cursor=store-1;for(int n=call.typeSymbol().parameterCount()-1;n>=0;n--){cursor=expressionStart(ops,cursor);if(cursor<0)return null;}
        return cursor>=2&&ops.get(cursor-1).opcode()==Opcode.DUP&&ops.get(cursor-2) instanceof NewObjectInstruction fresh&&fresh.className().asInternalName().equals(call.owner().asInternalName())?new Fresh(fresh.className().asInternalName(),cursor-2):null;
    }
    boolean mayCall(Prepare.MethodId caller,Prepare.MethodId callee,Set<Integer> reached){
        var ops=instructions.get(caller);
        for(int at:reached)if(ops.get(at) instanceof InvokeInstruction call&&call.name().equalsString(callee.name())&&call.type().equalsString(callee.descriptor())){
            if(!target(call,callee))continue;
            String page=pageReceiver(caller,ops,at,call);
            if(page!=null){if(resolve(page,call.name().stringValue(),call.type().stringValue()).equals(callee))return true;continue;}
            if(!call.owner().asInternalName().equals("SwitchEvent")||!call.name().equalsString("switching")||!call.type().equalsString("(I)V"))return true;
            FieldInstruction receiver=null;boolean internalBranches=false;
            int argument=expressionStart(ops,at);
            if(argument>0&&ops.get(argument-1) instanceof FieldInstruction field&&field.type().equalsString("LSwitchEvent;")&&(field.opcode()==Opcode.GETSTATIC||field.opcode()==Opcode.GETFIELD))receiver=field;
            if(receiver==null){receiver=conditionalReceiver(caller,ops,at);internalBranches=receiver!=null;}            if(receiver==null)return true;
            int receiverAt=ops.indexOf(receiver);var incoming=entries.computeIfAbsent(caller,k->entries(data.methods.get(k)));
            if(!internalBranches)for(int i=receiverAt+1;i<=at;i++)if(incoming.contains(i))return true;
            var field=FmcQualification.resolveField(data.models,receiver.owner().asInternalName(),receiver.name().stringValue(),receiver.type().stringValue(),new HashSet<>());
            if(field==null||opaque.contains(field)||!types.containsKey(field))return true;
            for(String type:types.get(field))if(resolve(type,call.name().stringValue(),call.type().stringValue()).equals(callee))return true;
        }
        return false;
    }
    /** The native circuit-breaker and malfunction arguments have an internal
     * two-constant branch while the callback stays beneath them. Only their
     * exact boolean/Character expression shapes and internal edges qualify. */
    private FieldInstruction conditionalReceiver(Prepare.MethodId caller,List<Instruction> ops,int at){
        if(at<7)return null;boolean character=at>=8&&Prepare.call(ops.get(at-5),"java/lang/Character","isUpperCase","(C)Z")&&ops.get(at-5).opcode()==Opcode.INVOKESTATIC;int receiverAt=at-(character?8:7),loadAt=receiverAt+1;
        if(!(ops.get(receiverAt) instanceof FieldInstruction receiver)||!receiver.type().equalsString("LSwitchEvent;")||(receiver.opcode()!=Opcode.GETSTATIC&&receiver.opcode()!=Opcode.GETFIELD)
            ||!(ops.get(loadAt) instanceof LoadInstruction load)||load.typeKind()!=TypeKind.REFERENCE||!(ops.get(loadAt+1) instanceof FieldInstruction value)||value.opcode()!=Opcode.GETFIELD||!value.type().equalsString(character?"C":"Z")
            ||!(ops.get(at-4) instanceof BranchInstruction choice)||choice.opcode()!=Opcode.IFEQ||ops.get(at-3).opcode()!=(character?Opcode.ICONST_0:Opcode.ICONST_2)
            ||!(ops.get(at-2) instanceof BranchInstruction join)||join.opcode()!=Opcode.GOTO||ops.get(at-1).opcode()!=Opcode.ICONST_1)return null;
        var method=data.methods.get(caller);var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();var offsets=new HashMap<Integer,Integer>();int offset=0;for(int i=0;i<ops.size();i++){offsets.put(offset,i);offset+=ops.get(i).sizeInBytes();}
        if(!Objects.equals(offsets.get(code.labelToBci(choice.target())),at-1)||!Objects.equals(offsets.get(code.labelToBci(join.target())),at))return null;
        int first=expressionStart(ops,receiverAt+1);if(first<0)return null;var allowed=Set.of(at-4,at-2);
        for(int i=0;i<ops.size();i++){var op=ops.get(i);var targets=new ArrayList<Label>();if(op instanceof BranchInstruction b)targets.add(b.target());if(op instanceof LookupSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}if(op instanceof TableSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}for(var target:targets){var index=offsets.get(code.labelToBci(target));if(index==null||index>first&&index<=at&&!allowed.contains(i))return null;}}
        for(var h:code.exceptionHandlers()){var index=offsets.get(code.labelToBci(h.handler()));if(index==null||index>first&&index<=at)return null;}return receiver;
    }
    private String pageReceiver(Prepare.MethodId caller,List<Instruction> ops,int at,InvokeInstruction call){
        if(pages.isEmpty()||call.opcode()!=Opcode.INVOKEVIRTUAL||!subtype(call.owner().asInternalName(),"CduHub$Page",new HashSet<>()))return null;
        int count=call.typeSymbol().parameterCount(),start=at-count-3;if(start<0)return null;
        for(int i=at-count;i<at;i++)if(!(ops.get(i) instanceof ConstantInstruction)&&!(ops.get(i) instanceof LoadInstruction)&&!(ops.get(i) instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC))return null;
        if(!Prepare.field(ops.get(start),Opcode.GETSTATIC,"CduHub","pg")||!(ops.get(start+1) instanceof FieldInstruction index)||index.opcode()!=Opcode.GETSTATIC||!index.owner().asInternalName().equals("CduHub")||!index.type().equalsString("I")||ops.get(start+2).opcode()!=Opcode.AALOAD)return null;
        var incoming=entries.computeIfAbsent(caller,k->entries(data.methods.get(k)));for(int i=start+1;i<=at;i++)if(incoming.contains(i))return null;
        return pages.get(index.name().stringValue());
    }
    private static Set<Integer> entries(MethodModel method){
        var code=(java.lang.classfile.attribute.CodeAttribute)method.code().orElseThrow();var byOffset=new HashMap<Integer,Integer>();var ops=Prepare.instructions(method);int offset=0;
        for(int i=0;i<ops.size();i++){byOffset.put(offset,i);offset+=ops.get(i).sizeInBytes();}
        var targets=new HashSet<Label>();for(var op:ops){if(op instanceof BranchInstruction b)targets.add(b.target());if(op instanceof LookupSwitchInstruction s){targets.add(s.defaultTarget());for(var c:s.cases())targets.add(c.target());}if(op instanceof TableSwitchInstruction s){targets.add(s.defaultTarget());for(var c:s.cases())targets.add(c.target());}}for(var h:code.exceptionHandlers())targets.add(h.handler());
        var result=new HashSet<Integer>();for(var label:targets){var index=byOffset.get(code.labelToBci(label));if(index==null)throw new IllegalArgumentException("FMC callback branch target is not an instruction boundary");result.add(index);}return result;
    }
    private boolean target(InvokeInstruction call,Prepare.MethodId callee){
        String owner=call.owner().asInternalName();if(resolve(owner,call.name().stringValue(),call.type().stringValue()).equals(callee))return true;
        return (call.opcode()==Opcode.INVOKEVIRTUAL||call.opcode()==Opcode.INVOKEINTERFACE)&&subtype(callee.owner(),owner,new HashSet<>());
    }
    /** A symbolic base/interface can reach an override or an inherited
     * implementation. Every such original caller contributes its argument;
     * direct known registrations cannot conceal an opaque polymorphic caller. */
    private boolean possibleCall(InvokeInstruction call,Prepare.MethodId callee){
        if(target(call,callee))return true;
        if(call.opcode()!=Opcode.INVOKEVIRTUAL&&call.opcode()!=Opcode.INVOKEINTERFACE)return false;
        for(String actual:data.models.keySet())if(subtype(actual,call.owner().asInternalName(),new HashSet<>())&&resolve(actual,call.name().stringValue(),call.type().stringValue()).equals(callee))return true;
        return false;
    }
    private boolean subtype(String owner,String base,Set<String> seen){
        if(owner.equals(base))return true;if(!seen.add(owner))return false;var model=data.models.get(owner);if(model==null)return false;
        if(model.superclass().isPresent()&&subtype(model.superclass().get().asInternalName(),base,seen))return true;for(var type:model.interfaces())if(subtype(type.asInternalName(),base,seen))return true;return false;
    }
    private Prepare.MethodId resolve(String owner,String name,String descriptor){
        var id=new Prepare.MethodId(owner,name,descriptor);if(data.methods.containsKey(id))return id;
        var model=data.models.get(owner);return model==null||model.superclass().isEmpty()?id:resolve(model.superclass().get().asInternalName(),name,descriptor);
    }
    private static boolean scalar(Instruction i){return i instanceof ConstantInstruction||i instanceof LoadInstruction l&&l.typeKind()==TypeKind.INT;}
}
