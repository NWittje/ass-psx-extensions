package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only executed-route binding. Native bank0 is MOD scratch; the installed
 * Routes.setActRteBody selects executed bank1/2. Neither a displayed route page nor
 * Routes.actBody chooses the capture source. Worker copy witnesses and the
 * initialized-lifecycle binding remain mandatory before invoking this bridge. */
public final class FmcExecutedRoutePrepare {
    static final String BRIDGE="$psxExporter$fmcExecutedRouteBindings";
    static final int OUTPUTS=3; // Executed header, body, DEP/ARR selections.
    private static final List<FmcQualification.Member> ROOTS=List.of(
        new FmcQualification.Member("FmcRte","rte1","LFmcRte$Rte;"),
        new FmcQualification.Member("FmcRte","rte2","LFmcRte$Rte;"),
        new FmcQualification.Member("Routes","rteBody1","LRoutes$RteBody;"),
        new FmcQualification.Member("Routes","rteBody2","LRoutes$RteBody;"));
    private FmcExecutedRoutePrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC executed route "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower)throws Exception{
        // Reuse the complete target/inactive member and writer qualification;
        // capture requires an active bank, without changing the recall target API.
        var base=FmcActiveRoutePrepare.transform(data,lower);var models=FmcQualification.finalModels(data,base);
        qualifySelection(models.get("Routes"));
        for(var root:ROOTS){
            var owner=models.get(root.owner());require(owner!=null,"root owner");
            var fields=owner.fields().stream().filter(f->f.fieldName().equalsString(root.name())).toList();
            require(fields.size()==1&&fields.getFirst().fieldType().equalsString(root.descriptor())
                &&(fields.getFirst().flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,"mutable static root descriptor");
            var writers=new HashSet<Prepare.MethodId>();
            for(var entry:models.entrySet())for(var method:entry.getValue().methods())if(method.code().isPresent())
                for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC
                    &&root.equals(FmcQualification.resolve(models,Set.of(root),f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())))
                    writers.add(new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()));
            require(writers.equals(Set.of(new Prepare.MethodId(root.owner(),"<clinit>","()V"))),"root replacement frontier");
        }
        var source=models.get("FmcRte");require(source.methods().stream().noneMatch(m->m.methodName().equalsString(BRIDGE)),"collision");
        ClassDesc f=ClassDesc.of("FmcRte"),r=ClassDesc.of("FmcRte$Rte"),b=ClassDesc.of("Routes$RteBody"),routes=ClassDesc.of("Routes"),d=ClassDesc.of("FmcDepArr$DepArrSelections");
        byte[] prepared=data.cf.transformClass(source,new ClassTransform(){
            public void accept(ClassBuilder builder,ClassElement element){builder.with(element);}
            public void atEnd(ClassBuilder builder){builder.withMethodBody(BRIDGE,MethodTypeDesc.of(CD_int,CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                var bad=c.newLabel();var two=c.newLabel();var selected=c.newLabel();c.aload(0).arraylength().iconst_3().if_icmpne(bad);
                c.getstatic(f,"rteActive",CD_int).istore(1);c.iload(1).iconst_1().if_icmplt(bad).iload(1).iconst_2().if_icmpgt(bad);
                c.iload(1).iconst_2().if_icmpeq(two);
                c.getstatic(f,"rte1",r).astore(2).getstatic(routes,"rteBody1",b).astore(3).goto_(selected);
                c.labelBinding(two).getstatic(f,"rte2",r).astore(2).getstatic(routes,"rteBody2",b).astore(3);
                c.labelBinding(selected).aload(2).ifnull(bad).aload(3).ifnull(bad).aload(2).getfield(r,"rteId",CD_int).iload(1).if_icmpne(bad);
                c.aload(2).getfield(r,"rteBody",b).aload(3).if_acmpne(bad);
                c.aload(3).getfield(b,"rteLink",r).aload(2).if_acmpne(bad);
                c.aload(2).getfield(r,"depArrSel",d).astore(4).aload(4).ifnull(bad);
                // No partial outputs precede complete role validation.
                c.aload(0).iconst_0().aload(2).aastore().aload(0).iconst_1().aload(3).aastore().aload(0).iconst_2().aload(4).aastore();
                c.iload(1).ireturn();
                c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalStateException")).dup().loadConstant("FMC executed route unavailable")
                    .invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
            });}
        });
        require(data.cf.verify(prepared).isEmpty(),"verifier");var changed=new HashMap<>(base);changed.put("FmcRte",prepared);
        TerrainAccess.verifyAdded(base,changed,data.inputs);return Map.copyOf(changed);
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var changed=transform(data,Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"executed_route_binding_component_only\",\"captureQualified\":false,\"recallQualified\":false,\"source\":\"active FmcRte.rte1/2 / Routes.rteBody1/2\",\"preparedSha256\":\""+Identity.hash(changed.get("FmcRte"))+"\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("Executed active bank1/2 source binding qualifies as data; native FMC capability unavailable");
    }

    private static void qualifySelection(ClassModel routes){
        require(routes!=null,"native active selector owner");
        var method=routes.methods().stream().filter(m->m.methodName().equalsString("setActRteBody")&&m.methodType().equalsString("()V")).toList();
        require(method.size()==1&&(method.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"native active selector");
        var ins=Prepare.instructions(method.getFirst());
        Opcode[] op={Opcode.GETSTATIC,Opcode.IFLE,Opcode.ICONST_1,Opcode.GOTO,Opcode.ICONST_0,Opcode.PUTSTATIC,
            Opcode.GETSTATIC,Opcode.GETFIELD,Opcode.GETFIELD,Opcode.GETSTATIC,Opcode.IF_ICMPNE,Opcode.GETSTATIC,Opcode.GOTO,Opcode.GETSTATIC,Opcode.PUTSTATIC,Opcode.RETURN};
        require(ins.size()==op.length,"complete active selector body");for(int i=0;i<op.length;i++)require(ins.get(i).opcode()==op[i],"active selector ordering");
        String[][] fields={{"0","FmcRte","rteActive","I"},{"5","Routes","anyActiveRte","Z"},{"6","Routes","rteBody1","LRoutes$RteBody;"},
            {"7","Routes$RteBody","rteLink","LFmcRte$Rte;"},{"8","FmcRte$Rte","rteId","I"},{"9","FmcRte","rteActive","I"},
            {"11","Routes","rteBody1","LRoutes$RteBody;"},{"13","Routes","rteBody2","LRoutes$RteBody;"},{"14","Routes","actBody","LRoutes$RteBody;"}};
        for(var f:fields)require(ins.get(Integer.parseInt(f[0])) instanceof FieldInstruction i&&i.owner().asInternalName().equals(f[1])&&i.name().equalsString(f[2])&&i.type().equalsString(f[3]),"active selector field roles");
        // Branch targets, as well as their instruction roles, establish which
        // body belongs to the active route. No native selector is invoked.
        var code=method.getFirst().code().orElseThrow();require(code instanceof java.lang.classfile.attribute.CodeAttribute&&code.exceptionHandlers().isEmpty(),"parsed ordinary selector");
        var parsed=(java.lang.classfile.attribute.CodeAttribute)code;int[] offsets=new int[ins.size()];int offset=0;
        for(int i=0;i<ins.size();i++){offsets[i]=offset;offset+=ins.get(i).sizeInBytes();}
        for(int[] edge:new int[][]{{1,4},{3,5},{10,13},{12,14}})
            require(parsed.labelToBci(((BranchInstruction)ins.get(edge[0])).target())==offsets[edge[1]],"active selector branch target");
    }
}
