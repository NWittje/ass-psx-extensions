package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound native gear-attitude selection primitive. Does not finalize ground
 * placement or call runAllegro/Strike/publication. Qualified owner/exclusion and
 * preinitialized native dependencies are required before runtime binding. */
public final class IosGroundSelectionPrepare {
    public static final String SELECT="psxExporterIosSelectGroundAttitude",READY="psxExporterIosGroundCatalogueReady",
        OBSERVE="psxExporterIosGroundSelectionObserved";
    private static final List<String> GEAR=List.of("nose","bodyL","bodyR","wingL","wingR");
    public record Layout(ClassDesc attitude,ClassDesc gear,ClassDesc value,List<String> modes,List<ClassDesc> assemblies){}
    public record Selection(Layout layout,List<Integer> nativeWrites){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static String pkg(ClassDesc c){String d=c.descriptorString();return d.substring(0,d.lastIndexOf('/')+1);}
    private static FieldModel field(ClassModel c,String name){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing/duplicate ground field "+name);return all.getFirst();
    }
    public static Selection select(ClassModel attitude,ClassModel gear){
        var owner=attitude.thisClass().asSymbol();var g=gear.thisClass().asSymbol();require(pkg(owner).equals(pkg(g)),"Ground dependencies changed package");
        for(var m:attitude.methods())require(!Set.of(SELECT,READY,OBSERVE).contains(m.methodName().stringValue()),"Existing IOS ground selector");
        var now=field(attitude,"now");var value=now.fieldTypeSymbol();
        require(value.isClassOrInterface()&&(now.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,"Changed selected attitude storage");
        var modes=attitude.fields().stream().filter(f->f.fieldTypeSymbol().equals(value)&&!f.fieldName().equalsString("now")).toList();
        require(!modes.isEmpty()&&modes.size()<=64,"Invalid bounded native attitude catalogue");
        for(var f:modes)require((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"Changed native mode staticness");
        var names=modes.stream().map(f->f.fieldName().stringValue()).toList();var assemblies=new ArrayList<ClassDesc>();
        for(String name:GEAR){var f=field(gear,name);require((f.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_PRIVATE))==ClassFile.ACC_STATIC&&f.fieldTypeSymbol().isClassOrInterface(),"Inaccessible/changed gear assembly "+name);assemblies.add(f.fieldTypeSymbol());}
        var methods=attitude.methods().stream().filter(m->m.methodName().equalsString("set")).toList();require(methods.size()==1,"Missing/overloaded native ground selector");var method=methods.getFirst();
        require(method.methodType().equalsString("()V")&&method.code().isPresent()&&(method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,"Changed native ground selector context");
        var code=method.code().get();require(code.exceptionHandlers().isEmpty(),"Native ground selector has exceptional effects");
        var labels=new HashMap<Label,Integer>();var sites=new ArrayList<Instruction>();int offset=0;
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),offset);if(e instanceof Instruction i){sites.add(i);offset+=i.sizeInBytes();}}
        var writes=new ArrayList<Integer>();offset=0;
        for(int n=0;n<sites.size();n++){var i=sites.get(n);
            require(Set.of(Opcode.GETSTATIC,Opcode.GETFIELD,Opcode.PUTSTATIC,Opcode.IFEQ,Opcode.IFNE,Opcode.GOTO,Opcode.RETURN).contains(i.opcode()),"Unqualified selector action "+i.opcode());
            if(i instanceof BranchInstruction b)require(labels.containsKey(b.target())&&labels.get(b.target())>offset,"Unbounded native gear selector");
            if(i instanceof FieldInstruction f){
                if(i.opcode()==Opcode.GETSTATIC)require((f.owner().asSymbol().equals(owner)&&names.contains(f.name().stringValue())&&f.typeSymbol().equals(value))||
                    (f.owner().asSymbol().equals(g)&&GEAR.contains(f.name().stringValue())&&f.typeSymbol().equals(assemblies.get(GEAR.indexOf(f.name().stringValue())))),"Unqualified selector input");
                else if(i.opcode()==Opcode.GETFIELD)require(f.name().equalsString("down")&&f.type().equalsString("Z")&&f.owner().asInternalName().startsWith(gear.thisClass().asInternalName()+"$"),"Unqualified gear configuration input");
                else {require(f.owner().asSymbol().equals(owner)&&f.name().equalsString("now")&&f.typeSymbol().equals(value),"Selector changes configuration or foreign state");
                    require(n>0&&sites.get(n-1) instanceof FieldInstruction prior&&prior.opcode()==Opcode.GETSTATIC&&prior.owner().asSymbol().equals(owner)&&names.contains(prior.name().stringValue()),"Selected value is not an existing native mode");writes.add(offset);}
            }
            offset+=i.sizeInBytes();
        }
        require(!writes.isEmpty(),"Native selector has no mode selection");return new Selection(new Layout(owner,g,value,names,List.copyOf(assemblies)),List.copyOf(writes));
    }
    public static byte[] prepare(ClassFile cf,ClassModel model,Selection s){
        var l=s.layout;require(model.thisClass().asSymbol().equals(l.attitude),"Wrong ground selector owner");
        for(var m:model.methods())require(!Set.of(SELECT,READY,OBSERVE).contains(m.methodName().stringValue()),"Duplicate ground bridge");
        return cf.transformClass(model,ClassTransform.endHandler(b->{b.withMethodBody(READY,MethodTypeDesc.of(CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var reject=c.newLabel();
            for(String mode:l.modes)c.getstatic(l.attitude,mode,l.value).ifnull(reject);
            for(int i=0;i<GEAR.size();i++)c.getstatic(l.gear,GEAR.get(i),l.assemblies.get(i)).ifnull(reject);
            c.iconst_1().ireturn().labelBinding(reject).iconst_0().ireturn();
        });b.withMethodBody(SELECT,MethodTypeDesc.of(CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var reject=c.newLabel();c.invokestatic(l.attitude,READY,MethodTypeDesc.of(CD_boolean)).ifeq(reject)
                .invokestatic(l.attitude,"set",MethodTypeDesc.of(CD_void)).iconst_1().ireturn().labelBinding(reject).iconst_0().ireturn();
        });
        var nativeSelection=model.methods().stream().filter(m->m.methodName().equalsString("set")).findFirst().orElseThrow();
        b.withMethod(OBSERVE,MethodTypeDesc.of(CD_boolean),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,
            mb->mb.transformCode(nativeSelection.code().orElseThrow(),new CodeTransform(){
                Label reject;
                public void atStart(CodeBuilder c){reject=c.newLabel();c.invokestatic(l.attitude,READY,MethodTypeDesc.of(CD_boolean)).ifeq(reject);}
                public void accept(CodeBuilder c,CodeElement e){
                    if(e instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC){
                        // Compare the actual selected pointer with the value chosen
                        // by native branches; observation never selects or writes it.
                        c.getstatic(l.attitude,"now",l.value).if_acmpne(reject);
                    }else if(e instanceof ReturnInstruction)c.iconst_1().ireturn();
                    else c.with(e);
                }
                public void atEnd(CodeBuilder c){c.labelBinding(reject).iconst_0().ireturn();}
            }));
        }));
    }
    private IosGroundSelectionPrepare(){}
}
