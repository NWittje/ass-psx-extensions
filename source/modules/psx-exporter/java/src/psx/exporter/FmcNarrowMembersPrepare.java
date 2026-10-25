package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Compose the explicit entered-field tables once, before adding factory or
 * observer methods. Read-only binding fields distinguish executed/selected/
 * inactive roles; they are never restored from an artifact. This component
 * is not a coherent source, publication journal or capability advertisement. */
public final class FmcNarrowMembersPrepare {
    record Qualified(Map<String,byte[]> changed,List<FmcMemberPrepare.Field> fields,
        Map<FmcQualification.Member,Set<Prepare.MethodId>> writers){}
    private FmcNarrowMembersPrepare(){}
    private static void require(boolean yes,String reason){if(!yes)throw new IllegalArgumentException("FMC narrow members "+reason);}
    private static FmcMemberPrepare.Field field(int id,String owner,String name,String type,boolean writable){return new FmcMemberPrepare.Field(id,owner,name,type,writable);}
    static List<FmcMemberPrepare.Field> fields(){
        var all=new ArrayList<FmcMemberPrepare.Field>();int id=0;
        for(var group:List.of(FmcEnteredHeaderPrepare.INPUTS,FmcPerfInputsPrepare.INPUTS,FmcRouteInputsPrepare.INPUTS))
            for(var f:group)all.add(field(++id,f.owner(),f.name(),f.descriptor(),true));
        all.add(field(++id,"FmcRte$Rte","origRwy","LFmcRte$Rwy;",false));
        all.add(field(++id,"FmcRte$Rte","destRwy","LFmcRte$Rwy;",false));
        all.add(field(++id,"FmcRte$Rte","rteBody","LRoutes$RteBody;",false));
        all.add(field(++id,"FmcRte$Rte","depArrSel","LFmcDepArr$DepArrSelections;",false));
        all.add(field(++id,"Routes$RteBody","leg","[LRouteWpt;",false));
        all.add(field(++id,"Routes$RteBody","legsAmo","I",false));
        all.add(field(++id,"Routes$RteBody","rteLink","LFmcRte$Rte;",false));
        all.add(field(++id,"FmcRte$Rte","rteId","I",false));
        all.add(field(++id,"FmcRte","rte0","LFmcRte$Rte;",false));
        all.add(field(++id,"Routes","rteBody0","LRoutes$RteBody;",false));
        all.add(field(++id,"FmcRte","rte1","LFmcRte$Rte;",false));
        all.add(field(++id,"FmcRte","rte2","LFmcRte$Rte;",false));
        all.add(field(++id,"Routes","rteBody1","LRoutes$RteBody;",false));
        all.add(field(++id,"Routes","rteBody2","LRoutes$RteBody;",false));
        all.add(field(++id,"FmcRte","rteActive","I",false));
        require(all.size()==66,"complete explicit field table");return List.copyOf(all);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower)throws Exception{
        return qualify(data,lower,false);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower,boolean readOnly)throws Exception{
        // Validate each installed semantic frontier before combining its member
        // switches. Do not chain the three independently generated switches:
        // their overlapping owners/IDs deliberately reject such composition.
        FmcEnteredHeaderPrepare.qualify(data,lower);FmcPerfInputsPrepare.qualify(data,lower);FmcRouteInputsPrepare.qualify(data,lower);
        FmcExecutedRoutePrepare.transform(data,lower);FmcRouteArrayOwnership.qualify(data,lower);
        var all=fields();var models=FmcQualification.finalModels(data,lower);var selected=new HashSet<FmcQualification.Member>();var writers=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();
        for(var f:all){
            var owner=models.get(f.owner());require(owner!=null,"owner");var declared=owner.fields().stream().filter(m->m.fieldName().equalsString(f.name())).toList();
            require(declared.size()==1&&declared.getFirst().fieldType().equalsString(f.descriptor()),"binding descriptor");
            boolean expectedStatic=f.owner().equals("FmcPerf")||f.owner().equals("FmcRte")||f.owner().equals("Routes");
            require(((declared.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==expectedStatic,"binding staticness");
            var member=new FmcQualification.Member(f.owner(),f.name(),f.descriptor());require(selected.add(member),"duplicate member");writers.put(member,new HashSet<>());
        }
        for(var entry:models.entrySet())for(var m:entry.getValue().methods())if(m.code().isPresent())for(var i:Prepare.instructions(m)){
            if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
                var actual=FmcQualification.resolve(models,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(actual!=null)writers.get(actual).add(new Prepare.MethodId(entry.getKey(),m.methodName().stringValue(),m.methodType().stringValue()));
            }
        }
        require(writers.values().stream().noneMatch(Set::isEmpty),"complete writer discovery");
        var frozen=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();writers.forEach((k,v)->frozen.put(k,Set.copyOf(v)));
        var access=readOnly?all.stream().map(f->field(f.id(),f.owner(),f.name(),f.descriptor(),false)).toList():all;
        return new Qualified(FmcMemberPrepare.transform(data,lower,access),access,Collections.unmodifiableMap(frozen));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var result=qualify(new Prepare(Prepare.readJar(jar)),Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"narrow_combined_member_component\",\"captureQualified\":false,\"recallQualified\":false,\"members\":"+result.fields().size()+",\"enteredMembers\":51,\"readOnlyBindings\":15,\"owners\":"+result.changed().size()+",\"limit\":\"Exact combined legal access only; constructor/ordinary writer coverage, current source/lifecycle, native rebuilding/alias/exclusion/JNI/service remain required. No installed class definition/initialization/invocation.\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("51 entered members and15 read-only binding fields composed once; full native adapter unavailable");
    }
}
