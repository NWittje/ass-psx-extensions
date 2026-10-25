package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only, worker-side active-route binding. A saved RTE number never chooses
 * the recall target. Zero active route selects live RTE1 for a later recall,
 * but zero is not evidence of an executed route for capture.
 *
 * The caller must already have a qualified initialization/lifecycle and copy
 * witness. This component does not call an initializer, consume a Q bank,
 * replace a native object, clear MOD, or advertise capture/recall capability. */
public final class FmcActiveRoutePrepare {
    static final String BRIDGE="$psxExporter$fmcActiveRouteBindings";
    // Output roles: target header, inactive header, target body, target DEP/ARR.
    static final int OUTPUTS=4;
    record Member(String owner,String name,String descriptor,boolean stat,String writer,String writerDescriptor){}
    static final List<Member> MEMBERS=List.of(
        new Member("FmcRte","rte1","LFmcRte$Rte;",true,"<clinit>","()V"),
        new Member("FmcRte","rte2","LFmcRte$Rte;",true,"<clinit>","()V"),
        new Member("FmcRte","rteActive","I",true,"readGlobViewAct","()V"),
        new Member("FmcRte$Rte","rteId","I",false,"<init>","(IIIIILRoutes$RteBody;)V"),
        new Member("FmcRte$Rte","rteBody","LRoutes$RteBody;",false,"<init>","(IIIIILRoutes$RteBody;)V"),
        new Member("FmcRte$Rte","depArrSel","LFmcDepArr$DepArrSelections;",false,"postCduInit","()V"),
        new Member("Routes$RteBody","rteLink","LFmcRte$Rte;",false,"<init>","(IIIIILRoutes$RteBody;)V"));
    private FmcActiveRoutePrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC active route "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower)throws Exception{
        var finalModels=FmcQualification.finalModels(data,lower);
        var selected=new HashSet<FmcQualification.Member>();var writes=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();
        for(var field:MEMBERS){
            byte[] bytes=lower.getOrDefault(field.owner(),data.inputs.get(field.owner()));require(bytes!=null,"owner "+field.owner());
            var model=data.cf.parse(bytes);var matches=model.fields().stream().filter(f->f.fieldName().equalsString(field.name())).toList();
            require(matches.size()==1&&matches.getFirst().fieldType().equalsString(field.descriptor()),"descriptor "+field.name());
            require(((matches.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==field.stat(),"staticness "+field.name());
            var member=new FmcQualification.Member(field.owner(),field.name(),field.descriptor());selected.add(member);writes.put(member,new HashSet<>());
        }
        var owners=new HashSet<>(data.models.keySet());owners.addAll(lower.keySet());
        for(String name:owners)for(var method:data.cf.parse(lower.getOrDefault(name,data.inputs.get(name))).methods()){
            if(method.code().isEmpty())continue;
            for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTSTATIC||f.opcode()==Opcode.PUTFIELD)){
                var actual=FmcQualification.resolve(finalModels,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(actual!=null)writes.get(actual).add(new Prepare.MethodId(name,method.methodName().stringValue(),method.methodType().stringValue()));
            }
        }
        for(var field:MEMBERS){
            String writerOwner=field.name().equals("depArrSel")?"FmcRte":field.name().equals("rteLink")?"FmcRte$Rte":field.owner();
            require(writes.get(new FmcQualification.Member(field.owner(),field.name(),field.descriptor()))
                .equals(Set.of(new Prepare.MethodId(writerOwner,field.writer(),field.writerDescriptor()))),"writer coverage "+field.name());
        }
        byte[] input=lower.getOrDefault("FmcRte",data.inputs.get("FmcRte"));var model=data.cf.parse(input);
        require(model.methods().stream().noneMatch(m->m.methodName().equalsString(BRIDGE)),"bridge collision");
        ClassDesc f=ClassDesc.of("FmcRte"),r=ClassDesc.of("FmcRte$Rte"),b=ClassDesc.of("Routes$RteBody"),d=ClassDesc.of("FmcDepArr$DepArrSelections");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder builder,ClassElement element){builder.with(element);}
            public void atEnd(ClassBuilder builder){
                builder.withMethodBody(BRIDGE,MethodTypeDesc.of(CD_int,CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();var targetTwo=c.newLabel();var chosen=c.newLabel();
                    c.aload(0).arraylength().loadConstant(OUTPUTS).if_icmpne(bad);
                    c.getstatic(f,"rteActive",CD_int).istore(1);
                    c.iload(1).iflt(bad).iload(1).iconst_2().if_icmpgt(bad);
                    c.getstatic(f,"rte1",r).astore(2).getstatic(f,"rte2",r).astore(3);
                    c.aload(2).ifnull(bad).aload(3).ifnull(bad).aload(2).aload(3).if_acmpeq(bad);
                    c.aload(2).getfield(r,"rteId",CD_int).iconst_1().if_icmpne(bad);
                    c.aload(3).getfield(r,"rteId",CD_int).iconst_2().if_icmpne(bad);
                    for(int slot:new int[]{2,3}){
                        c.aload(slot).getfield(r,"rteBody",b).astore(4).aload(4).ifnull(bad);
                        c.aload(4).getfield(b,"rteLink",r).aload(slot).if_acmpne(bad);
                        c.aload(slot).getfield(r,"depArrSel",d).ifnull(bad);
                    }
                    c.aload(2).getfield(r,"rteBody",b).aload(3).getfield(r,"rteBody",b).if_acmpeq(bad);
                    c.aload(2).getfield(r,"depArrSel",d).aload(3).getfield(r,"depArrSel",d).if_acmpeq(bad);
                    c.iload(1).iconst_2().if_icmpeq(targetTwo).goto_(chosen);
                    c.labelBinding(targetTwo).aload(2).astore(4).aload(3).astore(2).aload(4).astore(3);
                    c.labelBinding(chosen).aload(0).iconst_0().aload(2).aastore();
                    c.aload(0).iconst_1().aload(3).aastore();
                    c.aload(0).iconst_2().aload(2).getfield(r,"rteBody",b).aastore();
                    c.aload(0).iconst_3().aload(2).getfield(r,"depArrSel",d).aastore();
                    c.iload(1).ireturn();
                    c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalStateException")).dup().loadConstant("FMC active route binding unavailable")
                        .invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
                });
            }
        });
        require(data.cf.verify(output).isEmpty(),"generated verification");var before=new HashMap<>(lower);before.put("FmcRte",input);
        var changed=new HashMap<>(lower);changed.put("FmcRte",output);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    public static void main(String[] arguments)throws Exception{
        require(arguments.length==2,"installed JAR and owned report required");var jar=Path.of(arguments[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        String report="{\"schema\":1,\"state\":\"current_active_route_binding_component\",\"captureQualified\":false,\"recallQualified\":false,"
            +"\"members\":7,\"outputRoles\":4,\"noActiveTarget\":1,\"inputSha256\":\""+Identity.hash(data.inputs.get("FmcRte"))
            +"\",\"preparedSha256\":\""+Identity.hash(changed.get("FmcRte"))+"\",\"limits\":\"Read-only unused component; no installed class definition/init/invocation. Initialization/copy witness, route content, MOD clearing, native refresh, exclusion and complete JNI/service remain unqualified.\"}\n";
        Files.writeString(Path.of(arguments[1]),report,StandardOpenOption.CREATE_NEW);
        System.out.println("Current active-route binding qualifies as data; native FMC recall remains unavailable");
    }
}
