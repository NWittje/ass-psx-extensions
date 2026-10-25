package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;

/** Constructor allocation facts for the finite native published receiver
 * graph. Unknown array bindings remain explicit; a guessed capacity is never
 * accepted as an allocation proof. */
final class FmcReconstructionCapacity {
    static final int MAX_RECEIVERS=32768;
    /** Source-sized principal families. Four native bodies, their standalone
     * abeam store, six standby stores,21 menus and eight runway arrays. */
    static final int PRINCIPAL_RECEIVERS=4*253*7+253*6+253+6*253+21*200+8*30;
    static int parentLimit(String owner){return switch(owner){case "RouteWpt"->5*253;case "FmcWindOat"->5*253+3;case "Routes$RteBody"->4;case "FmcRte$Rte"->3;case "RoutesStby$Rte"->6;case "FmcDepArr$SidPack"->5;case "FmcDepArr$ArrPack","FmcDepArr$AppPack"->8;default->64;};}
    static int arrayLimit(FmcQualification.Member field){return switch(field.owner()+"."+field.name()){
        case "Routes.abmPt","Routes.abmSort","Routes$RteBody.leg","Routes$RteBody.via","RoutesStby$Rte.leg"->253;
        case "FmcDepArr$SidPack.sl","FmcDepArr$ArrPack.li","FmcDepArr$AppPack.li"->200;
        case "FmcRte$Rte.depRwy","FmcRte$Rte.arrRwy","FmcDepArr.otherDepRwyL","FmcDepArr.otherDepRwyR"->30;
        case "FmcWindOat.windOatAlt","FmcNg.useAltnArptOf4"->4;
        case "FmcNg.pendAltnArptOf4"->6;
        default->throw new IllegalArgumentException("unclosed native receiver array capacity "+field);
    };}
    private static boolean nativeArray(FmcQualification.Member f){return f.descriptor().startsWith("[L")&&!f.descriptor().equals("[Ljava/lang/String;");}
    static void checkArray(FmcQualification.Member field,Object value){if(value==null||!nativeArray(field))return;if(!(value instanceof Object[] rows)||rows.length>arrayLimit(field))throw new IllegalArgumentException("native receiver array outside qualified source bound "+field);}
    static int checkedBudget(FmcReconstructionExecRoots.Profile profile){
        // Each receiver is reached by one of these finite edges. Shared roots
        // and cycles only decrease this sum. The64-parent remainder is checked
        // on the current graph before mutation; it is not an allocation guess.
        long bound=profile.banks().size();
        for(var fields:profile.statics().values())for(var f:fields)bound+=nativeArray(f)?arrayLimit(f):1;
        for(var fields:profile.children().values())for(var f:fields)bound+=(long)parentLimit(f.owner())*(nativeArray(f)?arrayLimit(f):1);
        if(bound>MAX_RECEIVERS)throw new IllegalArgumentException("native receiver schema exceeds qualified worker budget "+bound);return (int)bound;
    }
    static int checkedTargetBudget(Prepare data,FmcReconstructionExecRoots.Profile profile,Set<String> metadataOwners){
        long bound=0;for(var bank:profile.banks())bound+=tagMultiplicity(data,bank.array().descriptor().substring(1),metadataOwners);
        for(var fields:profile.statics().values())for(var f:fields)bound+=(long)(nativeArray(f)?arrayLimit(f):1)*tagMultiplicity(data,f.descriptor(),metadataOwners);
        for(var fields:profile.children().values())for(var f:fields)bound+=(long)parentLimit(f.owner())*(nativeArray(f)?arrayLimit(f):1)*tagMultiplicity(data,f.descriptor(),metadataOwners);
        if(bound>MAX_RECEIVERS)throw new IllegalArgumentException("native metadata schema exceeds qualified worker budget "+bound);return (int)bound;
    }
    private static int tagMultiplicity(Prepare data,String descriptor,Set<String> owners){
        while(descriptor.startsWith("["))descriptor=descriptor.substring(1);if(!descriptor.startsWith("L"))return 0;
        String type=descriptor.substring(1,descriptor.length()-1);if(type.startsWith("java/")&&!type.equals("java/lang/Object"))return 0;
        int maximum=0;for(String candidate:data.models.keySet()){
            String current=candidate;int count=0;boolean matches=type.equals("java/lang/Object");var seen=new HashSet<String>();
            while(data.models.containsKey(current)&&seen.add(current)){matches|=current.equals(type);if(owners.contains(current))count++;var model=data.models.get(current);if(model.superclass().isEmpty())break;current=model.superclass().orElseThrow().asInternalName();}
            if(matches)maximum=Math.max(maximum,count);
        }return maximum;
    }
    /** An upper roster only. The worker binds static getter owners and owners
     * of actual objects; unused potential owners are never initialized. */
    static Set<String> workerOwnerUpperBound(Prepare data,FmcReconstructionExecRoots.Profile profile,Set<String> metadataOwners,Collection<String> pollOwners){
        var names=new TreeSet<String>();names.addAll(profile.statics().keySet());names.addAll(profile.children().keySet());names.addAll(metadataOwners);names.addAll(pollOwners);names.addAll(FmcFreshRoutePrepare.OWNERS);
        names.addAll(Set.of("Core","Option","Visuals","Tcas","NavigationDisplay","Instructor","InstructorSituation","InstructorSaveSitu","Glob","Q","Qi","Qs","FmcPerf","FmcRte","FmcDepArr","FmcDepArr$DepTransPack","FmcDepArr$ArrTransPack","NavRadios","NavRadios$Vhf","NavRadios$Ils","NavRadios$Vor","FmcMaintenance","FmcGlobal","FmcGlobal$Pos","Routes","InertialReference","InertialReference$InertialReferenceUnit","ControlDisplayUnit","ControlDisplayUnit$ScreenLine","FmcVnav","FmcVnav$Pred"));
        names.removeAll(FmcReconstructionExecRoots.CREATED);names.removeIf(n->n.startsWith("java/"));
        var initial=List.copyOf(names);for(String name:initial){String current=name;var seen=new HashSet<String>();while(data.models.containsKey(current)&&seen.add(current)){var model=data.models.get(current);if(model.superclass().isEmpty())break;current=model.superclass().orElseThrow().asInternalName();if(data.models.containsKey(current))names.add(current);}}
        if(names.size()>256||names.stream().anyMatch(n->!data.models.containsKey(n)))throw new IllegalArgumentException("unclosed native initialized owner upper roster "+names.size());return Set.copyOf(names);
    }
    record Allocation(FmcQualification.Member field,Set<Integer> lengths,Set<Prepare.MethodId> unresolved){}
    static void qualifyArrays(Prepare data,FmcReconstructionExecRoots.Profile profile){
        for(var allocation:arrays(data,profile)){
            int cap=arrayLimit(allocation.field());
            if(allocation.lengths().equals(Set.of(cap))&&allocation.unresolved().isEmpty())continue;
            var f=allocation.field();var writer=new Prepare.MethodId("FmcDepArr$ThreadOtherSids","run","()V");
            if(!f.owner().equals("FmcDepArr")||!Set.of("otherDepRwyL","otherDepRwyR").contains(f.name())||!allocation.lengths().isEmpty()||!allocation.unresolved().equals(Set.of(writer)))throw new IllegalArgumentException("unclosed native receiver allocation "+allocation);
            var ops=Prepare.instructions(data.methods.get(writer));int publications=0;
            for(int i=1;i<ops.size();i++)if(ops.get(i) instanceof FieldInstruction field&&field.opcode()==Opcode.PUTSTATIC&&field.owner().asInternalName().equals(f.owner())&&field.name().equalsString(f.name())){
                if(!(ops.get(i-1) instanceof InvokeInstruction call)||!call.owner().asInternalName().equals("ScanRunway")||!call.name().equalsString("getInfo")||!call.type().equalsString("(Ljava/lang/String;)[LScanRunway$RwyInfo;"))throw new IllegalArgumentException("native runway array producer drift");publications++;
            }
            var producer=Prepare.instructions(data.methods.get(new Prepare.MethodId("ScanRunway","getInfo","(Ljava/lang/String;)[LScanRunway$RwyInfo;")));boolean nativeCap=false;
            for(int i=1;i<producer.size();i++)if(producer.get(i) instanceof NewReferenceArrayInstruction array&&array.componentType().asInternalName().equals("ScanRunway$RwyInfo")&&producer.get(i-1) instanceof ConstantInstruction n&&Objects.equals(n.constantValue(),30))nativeCap=true;
            // getInfo returns a compact array after populating its30-slot
            // workspace. The actual result length is checked before expansion.
            if(publications!=1||!nativeCap||cap!=30)throw new IllegalArgumentException("native runway workspace capacity drift");
        }
    }
    static List<Allocation> arrays(Prepare data,FmcReconstructionExecRoots.Profile profile){
        var fields=new TreeSet<FmcQualification.Member>();profile.statics().values().forEach(fields::addAll);profile.children().values().forEach(fields::addAll);
        fields.removeIf(f->{String t=f.descriptor();while(t.startsWith("["))t=t.substring(1);return !f.descriptor().startsWith("[")||!t.startsWith("L")||!data.models.containsKey(t.substring(1,t.length()-1));});
        var names=new HashSet<String>();var lengths=new HashMap<FmcQualification.Member,Set<Integer>>();var unresolved=new HashMap<FmcQualification.Member,Set<Prepare.MethodId>>();for(var f:fields){names.add(f.name());lengths.put(f,new TreeSet<>());unresolved.put(f,new HashSet<>());}
        for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){var ops=Prepare.instructions(e.getValue());for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)&&names.contains(f.name().stringValue())){
            var field=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());if(field==null||!fields.contains(field))continue;
            if(at>=2&&ops.get(at-1) instanceof NewReferenceArrayInstruction&&ops.get(at-2) instanceof ConstantInstruction constant&&constant.constantValue() instanceof Integer value&&value>=0)lengths.get(field).add(value);else unresolved.get(field).add(e.getKey());
        }}var rows=new ArrayList<Allocation>();for(var f:fields)rows.add(new Allocation(f,Set.copyOf(lengths.get(f)),Set.copyOf(unresolved.get(f))));return List.copyOf(rows);
    }
    public static void main(String[] args)throws Exception{var data=new Prepare(Prepare.readJar(java.nio.file.Path.of(args[0])));var normal=args.length>1?FmcReconstructionNormalExec.inspect(data,Set.of(),Set.of(),Set.of()):null;var p=FmcReconstructionExecRoots.transform(data,Map.of(),normal);qualifyArrays(data,p);for(var a:arrays(data,p))System.out.println(a);var owners=new HashSet<String>();FmcReconstructionExecAudit.inspect(data).writes().forEach(f->owners.add(f.owner()));System.out.println("checked finite edge budget="+checkedBudget(p)+" metadata target budget="+checkedTargetBudget(data,p,owners)+" initialized owner upper roster="+workerOwnerUpperBound(data,p,owners,List.of()).size());}
}
