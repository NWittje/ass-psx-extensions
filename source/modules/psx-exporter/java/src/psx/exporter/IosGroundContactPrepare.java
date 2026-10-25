package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Isolated preparation of native gear contact calculations, NOT scheduler execution.
 * Original methods remain intact. Strike publication/failure calls are omitted only
 * from the new contact method; their argument/return-free contract is mandatory.
 * Receiver/exclusion qualification remains an assembled-layer obligation. */
public final class IosGroundContactPrepare {
    public static final String CONTACT="psxExporterIosContact",BANK="psxExporterIosGroundBank";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void),FLOAT=MethodTypeDesc.of(CD_float,CD_float);
    public record Method(String owner,String name,String descriptor,List<Integer> omittedCalls,List<String> fields){}
    public record Selection(String attitude,List<Method> methods){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static MethodModel method(ClassModel c,String name,String desc){
        var all=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();require(all.size()==1,"Missing/ambiguous contact member "+c.thisClass()+"."+name);
        var m=all.getFirst();require(m.methodType().equalsString(desc)&&m.code().isPresent()&&
            (m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==0,"Changed contact binding "+name);
        require(m.code().get().exceptionHandlers().isEmpty(),"Contact handlers need qualification");return m;
    }
    private static ClassModel declaration(Map<String,ClassModel> classes,String owner,String field){
        var seen=new HashSet<String>();
        while(seen.add(owner)){
            var c=classes.get(owner);require(c!=null,"Missing contact storage owner "+owner);
            if(c.fields().stream().anyMatch(f->f.fieldName().equalsString(field)))return c;
            owner=c.superclass().orElseThrow(()->new IllegalArgumentException("Missing contact field "+field)).asInternalName();
        }throw new IllegalArgumentException("Contact inheritance cycle");
    }
    private static Method selectMethod(Map<String,ClassModel> classes,ClassModel c,String attitude,String name,String desc){
        var m=method(c,name,desc);require(IosExclusionPrepare.localProblem(m).isEmpty(),"Unbounded contact body");
        var omitted=new ArrayList<Integer>();var fields=new TreeSet<String>();int offset=0;
        for(var e:m.code().get())if(e instanceof Instruction i){
            require(!Set.of(Opcode.NEW,Opcode.NEWARRAY,Opcode.ANEWARRAY,Opcode.MULTIANEWARRAY,Opcode.INVOKEDYNAMIC,
                Opcode.MONITORENTER,Opcode.MONITOREXIT,Opcode.ATHROW).contains(i.opcode()),"Impure contact instruction "+i.opcode());
            if(i instanceof FieldInstruction f){
                String owner=f.owner().asInternalName();var d=declaration(classes,owner,f.name().stringValue());
                var storage=d.fields().stream().filter(x->x.fieldName().equals(f.name())).findFirst().orElseThrow();
                require(storage.fieldType().equals(f.type()),"Contact descriptor changed");
                boolean write=i.opcode()==Opcode.PUTSTATIC||i.opcode()==Opcode.PUTFIELD;
                require((storage.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0||d.thisClass().equals(c.thisClass()),"Inaccessible contact storage");
                require(!write||(storage.flags().flagsMask()&ClassFile.ACC_FINAL)==0,"Immutable contact output");
                require(!write||d.thisClass().asInternalName().equals(attitude)||d.thisClass().asInternalName().equals(attitude+"$Attitude"),"Contact changes non-contact state");
                require(owner.equals("Aerodynamics")||owner.startsWith(attitude),"Unqualified contact dependency "+owner);
                fields.add(d.thisClass().asInternalName()+"."+f.name()+":"+f.type());
            }
            if(i instanceof InvokeInstruction call){
                String owner=call.owner().asInternalName(),n=call.name().stringValue();
                if(owner.equals("Strike")){
                    require(i.opcode()==Opcode.INVOKESTATIC&&call.type().equalsString("()V")&&
                        Set.of("resetNoseAndPodStrikeByGearAtt","setPodStrikeByGearAtt","setNoseStrikeByGearAtt").contains(n),"Changed Strike omission contract");omitted.add(offset);
                }else if(owner.equals(attitude)&&n.startsWith("access$")){
                    var target=classes.get(attitude).methods().stream().filter(x->x.methodName().equals(call.name())&&x.methodType().equals(call.type())).findFirst().orElseThrow();
                    require(call.type().equalsString("(Z)Z"),"Changed strike accessor");
                    for(var a:target.code().orElseThrow())if(a instanceof Instruction x){
                        require(Set.of(Opcode.ILOAD_0,Opcode.DUP,Opcode.PUTSTATIC,Opcode.IRETURN).contains(x.opcode()),"Impure strike accessor");
                        if(x instanceof FieldInstruction f)require(f.owner().asInternalName().equals(attitude)&&f.name().equalsString("strike")&&f.type().equalsString("Z"),"Foreign strike storage");
                    }
                }else if(owner.equals("FlightControls")&&n.equals("getAerodynamicElevator")&&call.type().equalsString("()F")){
                    var leaf=classes.get(owner).methods().stream().filter(x->x.methodName().equals(call.name())&&x.methodType().equals(call.type())).findFirst().orElseThrow();
                    require((leaf.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE))==ClassFile.ACC_STATIC&&
                        leaf.code().isPresent()&&leaf.code().get().exceptionHandlers().isEmpty()&&IosExclusionPrepare.localProblem(leaf).isEmpty(),"Changed contact elevator getter");
                    for(var element:leaf.code().get())if(element instanceof Instruction op){
                        require(Set.of(Opcode.GETSTATIC,Opcode.LDC_W,Opcode.LDC,Opcode.FCONST_2,Opcode.FCMPL,Opcode.IFLE,Opcode.FADD,Opcode.FRETURN).contains(op.opcode()),"Impure contact elevator getter");
                        if(op instanceof FieldInstruction f){require(f.type().equalsString("F")&&
                            ((f.owner().asInternalName().equals("Gear")&&f.name().equalsString("airDrag"))||
                            (f.owner().asInternalName().equals("FlightControls")&&f.name().equalsString("aeroEleSum"))),"Changed elevator getter dependency");
                            fields.add(f.owner().asInternalName()+"."+f.name()+":"+f.type());}
                    }
                }else require(owner.equals("java/lang/Math")&&i.opcode()==Opcode.INVOKESTATIC&&
                    ((n.equals("abs")&&Set.of("(F)F","(D)D","(I)I","(J)J").contains(call.type().stringValue()))||
                    (Set.of("min","max").contains(n)&&Set.of("(FF)F","(DD)D","(II)I","(JJ)J").contains(call.type().stringValue()))),
                    "Unqualified contact call/descriptor "+owner+"."+n+call.type());
            }
            offset+=i.sizeInBytes();
        }
        return new Method(c.thisClass().asInternalName(),name,desc,List.copyOf(omitted),List.copyOf(fields));
    }
    public static Selection select(Map<String,ClassModel> classes){
        String family="GearAttitude";var root=classes.get(family);require(root!=null,"Missing contact catalogue");
        var selector=IosGroundSelectionPrepare.select(root,classes.get("Gear"));
        require(selector.layout().value().equals(ClassDesc.of(family+"$Attitude")),"Changed attitude base");
        var selected=new ArrayList<Method>();int children=0;
        for(var c:classes.values())if(c.thisClass().asInternalName().equals(family+"$Attitude")||
            c.superclass().map(s->s.asInternalName().equals(family+"$Attitude")).orElse(false)){
            for(var m:c.methods())require(!m.methodName().equalsString(CONTACT)&&!m.methodName().equalsString(BANK),"Contact already prepared");
            selected.add(selectMethod(classes,c,family,"runAllegro","()V"));
            if(c.methods().stream().anyMatch(m->m.methodName().equalsString("getBank")))selected.add(selectMethod(classes,c,family,"getBank","(F)F"));
            if(!c.thisClass().asInternalName().equals(family+"$Attitude"))children++;
        }
        require(children==selector.layout().modes().size(),"Incomplete contact catalogue/subclass coverage");
        selected.sort(Comparator.comparing(Method::owner).thenComparing(Method::name));return new Selection(family,List.copyOf(selected));
    }
    public static Map<String,byte[]> prepare(ClassFile cf,Map<String,ClassModel> classes,Selection selected){
        require(select(classes).equals(selected),"Contact source differs from selection");var output=new TreeMap<String,byte[]>();
        for(var row:selected.methods()){
            var original=classes.get(row.owner());var current=output.containsKey(row.owner())?cf.parse(output.get(row.owner())):original;
            var nativeMethod=method(original,row.name(),row.descriptor());String added=row.name().equals("runAllegro")?CONTACT:BANK;
            byte[] result=cf.transformClass(current,ClassTransform.endHandler(b->b.withMethod(added,
                row.name().equals("runAllegro")?VOID:FLOAT,ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC,
                mb->mb.transformCode(nativeMethod.code().orElseThrow(),(code,e)->{
                    if(e instanceof InvokeInstruction call&&call.owner().asInternalName().equals("Strike"))return;
                    code.with(e);
                }))));
            require(cf.verify(result).isEmpty(),"Prepared contact verification failed");output.put(row.owner(),result);
        }
        return Collections.unmodifiableMap(output);
    }
    private IosGroundContactPrepare(){}
}
