package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Preparation-only proof that native leg-array membership is assigned in
 * fresh constructors only. Such arrays need no ongoing membership guard after
 * qualified initialization and coherent root publication. This is not proof
 * of waypoint-field coherence, root publication or command exclusion. Unknown
 * future stores/escapes reject the component; they are never exempted by name. */
public final class FmcRouteArrayOwnership {
    private static final FmcQualification.Member LEG=new FmcQualification.Member("Routes$RteBody","leg","[LRouteWpt;");
    private static final Prepare.MethodId CTOR=new Prepare.MethodId("Routes$RteBody","<init>","(I)V");
    record Result(int readingMethods,int constructors,boolean initializedMembershipOnly){}
    private FmcRouteArrayOwnership(){}
    private static void require(boolean yes,String reason){if(!yes)throw new IllegalArgumentException("FMC route array ownership "+reason);}
    static Result qualify(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var audit=FmcRouteArrayFlowAudit.audit(data,lower);
        require(audit.mutations().size()==1,"complete single constructor store frontier");var store=audit.mutations().iterator().next();
        require(store.method().equals(CTOR)&&store.name().equals("AASTORE")&&store.descriptor().equals(FmcRouteArrayFlowAudit.STORE),"constructor-only reference membership");
        int directStores=0;
        for(var entry:models.entrySet())for(var method:entry.getValue().methods())if(method.code().isPresent())for(var i:Prepare.instructions(method)){
            if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)
                &&FmcQualification.resolve(models,Set.of(LEG),f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){
                require(f.opcode()==Opcode.PUTFIELD&&new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()).equals(CTOR),"leg root reassignment outside constructor");directStores++;
            }
        }
        require(directStores==1,"single native leg-array field assignment");
        var constructor=models.get(CTOR.owner()).methods().stream().filter(m->m.methodName().equalsString(CTOR.name())&&m.methodType().equalsString(CTOR.descriptor())).findFirst().orElseThrow();
        var ins=Prepare.instructions(constructor);int allocations=0;
        for(int index=0;index<ins.size();index++)if(ins.get(index) instanceof NewReferenceArrayInstruction n&&n.componentType().asInternalName().equals("RouteWpt")){
            require(index>=2&&index+1<ins.size(),"bounded array allocation pattern");
            require(ins.get(index-2) instanceof LoadInstruction l&&l.slot()==0&&l.typeKind()==TypeKind.REFERENCE,"native constructor receiver");
            require(ins.get(index-1) instanceof ConstantInstruction c&&Integer.valueOf(253).equals(c.constantValue()),"native253-slot allocation");
            require(ins.get(index+1) instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals(LEG.owner())&&f.name().equalsString(LEG.name())&&f.type().equalsString(LEG.descriptor()),"fresh array immediately owned by constructor");allocations++;
        }
        require(allocations==1,"one fresh array allocation");int at=store.ordinal();
        require(at>=3&&ins.get(at-3) instanceof NewObjectInstruction n&&n.className().asInternalName().equals("RouteWpt")&&ins.get(at-2).opcode()==Opcode.DUP
            &&ins.get(at-1) instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals("RouteWpt")&&call.name().equalsString("<init>")&&call.type().equalsString("()V"),"fresh waypoint per native array entry");
        // The existing full constructor closure forbids static reads/writes,
        // callbacks, external references, monitors and changed native bodies.
        // No installed constructor is executed during this inspection.
        var fresh=FmcFreshRoutePrepare.transform(data,lower);
        return new Result(audit.methods(),fresh.constructors().size(),true);
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var result=qualify(new Prepare(Prepare.readJar(jar)),Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"constructor_only_route_array_component\",\"captureQualified\":false,\"recallQualified\":false,\"readingMethods\":"+result.readingMethods()+",\"constructors\":"+result.constructors()+",\"initializedMembershipOnly\":true,\"limit\":\"No ongoing array-store witness required only after qualified initialization and coherent root publication. Waypoint fields, root publication, full adapter/exclusion remain unqualified; no installed class definition/initialization/invocation.\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("Native leg-array membership is constructor-only; field/root/native adapter qualification remains required");
    }
}
