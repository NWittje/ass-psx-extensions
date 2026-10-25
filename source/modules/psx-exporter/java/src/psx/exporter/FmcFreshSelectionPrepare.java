package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Constructs procedure selections only inside a newly constructed private
 * route. The seven indices come from the qualified live binding, never an
 * artifact. No current object is accepted and no FMC initializer is invoked.
 * This preparation component does not qualify recall or publication. */
public final class FmcFreshSelectionPrepare {
    static final String SELECT="FmcDepArr$DepArrSelections";
    static final String NAME="$psxExporter$fmcFreshSelectedRoute";
    static final List<String> STRINGS=List.of("selSid","selSidTrans","selSidRwy","selStar","selStarTrans","selApp","selAppTrans");
    static final Set<String> INTS=Set.of("globIdx","rwyExtNm10","rwyExtFpaDeg10","reserved");
    record Qualified(Map<String,byte[]> changed,Set<Prepare.MethodId> initializedConstructors){
        int constructors(){return initializedConstructors.size();}
    }
    private FmcFreshSelectionPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC fresh selections "+why);}
    private static void load(Instruction i,int slot){require(i instanceof LoadInstruction l&&l.slot()==slot&&l.typeKind()==(slot==0?TypeKind.REFERENCE:TypeKind.INT),"constructor receiver/input");}
    private static void store(Instruction i,String name,ClassDesc type){require(i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals(SELECT)&&f.name().equalsString(name)&&f.typeSymbol().equals(type),"constructor field "+name);}
    private static void constructor(ClassModel model){
        require(model!=null,"missing selector class");
        var declared=new HashSet<String>();
        for(var field:model.fields()){
            String name=field.fieldName().stringValue();boolean string=STRINGS.contains(name);
            require((string||INTS.contains(name))&&declared.add(name)&&field.fieldTypeSymbol().equals(string?CD_String:CD_int)
                &&(field.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0,"selector field profile");
        }
        require(declared.size()==11,"complete selector fields");
        var methods=model.methods().stream().filter(m->m.methodName().equalsString("<init>")&&m.methodType().equalsString("(I)V")).toList();
        require(methods.size()==1,"selector constructor descriptor");var method=methods.getFirst();
        require((method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==0
            &&method.code().isPresent()&&method.code().orElseThrow().exceptionHandlers().isEmpty(),"ordinary selector constructor");
        var ins=Prepare.instructions(method);require(ins.size()==30,"exact selector initialization order");int at=0;
        load(ins.get(at++),0);
        require(ins.get(at++) instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals("java/lang/Object")&&call.name().equalsString("<init>")&&call.type().equalsString("()V"),"Object constructor only");
        for(String name:STRINGS){load(ins.get(at++),0);require(ins.get(at++) instanceof ConstantInstruction c&&"".equals(c.constantValue()),"empty native selector");store(ins.get(at++),name,CD_String);}
        load(ins.get(at++),0);require(ins.get(at++) instanceof ConstantInstruction c&&Integer.valueOf(0).equals(c.constantValue()),"native reserved default");store(ins.get(at++),"reserved",CD_int);
        load(ins.get(at++),0);load(ins.get(at++),1);store(ins.get(at++),"globIdx",CD_int);
        require(ins.get(at).opcode()==Opcode.RETURN,"constructor completion");
    }
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var finalModels=FmcQualification.finalModels(data,lower);constructor(data.models.get(SELECT));constructor(finalModels.get(SELECT));
        // The complete constructor instruction profile above proves that the
        // two extension settings retain native zero defaults and no external
        // object, static state, callback or initialization is touched.
        var fresh=FmcFreshRoutePrepare.transform(data,lower);var changed=new HashMap<>(fresh.changed());
        var header=data.cf.parse(changed.get(FmcFreshRoutePrepare.HEADER));
        var fields=header.fields().stream().filter(f->f.fieldName().equalsString("depArrSel")).toList();
        ClassDesc select=ClassDesc.of(SELECT),owner=ClassDesc.of(FmcFreshRoutePrepare.HEADER);
        require(fields.size()==1&&fields.getFirst().fieldTypeSymbol().equals(select)&&(fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0,"fresh selector binding field");
        require(header.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"factory collision");
        byte[] output=data.cf.transformClass(header,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){b.withMethodBody(NAME,MethodTypeDesc.of(CD_Object,CD_int,CD_int,CD_int,CD_int,CD_int,CD_int,CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                for(int i=0;i<6;i++)c.iload(i);
                c.invokestatic(owner,FmcFreshRoutePrepare.NAME,MethodTypeDesc.of(CD_Object,CD_int,CD_int,CD_int,CD_int,CD_int,CD_int)).checkcast(owner).dup();
                c.new_(select).dup().iload(6).invokespecial(select,"<init>",MethodTypeDesc.of(CD_void,CD_int));
                c.putfield(owner,"depArrSel",select).areturn();
            });}
        });
        require(data.cf.verify(output).isEmpty(),"factory verifier");
        var before=new HashMap<>(changed);changed.put(FmcFreshRoutePrepare.HEADER,output);TerrainAccess.verifyAdded(before,changed,data.inputs);
        var constructors=new HashSet<>(fresh.constructors());
        require(constructors.add(new Prepare.MethodId(SELECT,"<init>","(I)V")),"distinct selector constructor");
        return new Qualified(Map.copyOf(changed),Set.copyOf(constructors));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var result=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"fresh_route_procedure_selector_component\",\"captureQualified\":false,\"recallQualified\":false,\"constructors\":"+result.constructors()+",\"selectorInputSha256\":\""+Identity.hash(data.inputs.get(SELECT))+"\",\"preparedHeaderSha256\":\""+Identity.hash(result.changed().get(FmcFreshRoutePrepare.HEADER))+"\",\"limits\":\"Trusted live indices, complete entered profile, initialized source, rebuilding, exclusion and publication remain required. No installed class defined/initialized/invoked.\"}\n";
        Files.writeString(Path.of(args[1]),report,StandardOpenOption.CREATE_NEW);System.out.println("Fresh route/procedure constructor component only; native FMC capture/recall unavailable");
    }
}
