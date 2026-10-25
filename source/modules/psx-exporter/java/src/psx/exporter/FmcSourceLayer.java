package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Optional entered-content source. Preparation is data-only and transactional;
 * the worker must separately bind initialized owners and current compatibility.
 * Recall is deliberately independent: no write bridge is exposed by this layer. */
final class FmcSourceLayer {
    record Result(Map<String,byte[]> overlay,List<String> evidence){}
    static final List<FmcMemberPrepare.Field> CONTEXT=List.of(
        new FmcMemberPrepare.Field(101,"Option","ngFmc","Z",false),
        new FmcMemberPrepare.Field(102,"Option","engineModel","I",false),
        new FmcMemberPrepare.Field(103,"Core","navDir","Ljava/lang/String;",false));
    static final String[] OWNERS={"FmcRte$Rte","FmcRte$Rwy","FmcDepArr$DepArrSelections","FmcPerf","RouteWpt","Routes$RteBody","FmcRte","Routes","Option","Core"};
    private FmcSourceLayer(){}
    static boolean available(){try{return FmcNative.abiVersion()==1&&!FmcNative.classStates(null,null)&&!FmcNative.identity(null)&&!FmcNative.configure(null,0);}catch(Throwable absent){return false;}}
    static Result qualify(Prepare data,Map<String,byte[]> lower){
        try{
            NavigationSelection.qualify(data.inputs.get("Core"));
            var source=FmcNarrowSourcePrepare.qualify(data,lower);
            var complete=new HashMap<>(source.changed());
            complete.putAll(FmcMemberPrepare.transform(data,complete,CONTEXT));
            var selected=new HashSet<FmcQualification.Member>();
            for(var field:CONTEXT)selected.add(new FmcQualification.Member(field.owner(),field.name(),field.descriptor()));
            var writers=new HashSet<Prepare.MethodId>();var models=FmcQualification.finalModels(data,lower);
            for(var entry:models.entrySet())for(var method:entry.getValue().methods())if(method.code().isPresent())
                for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)
                    &&FmcQualification.resolve(models,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){
                    var id=new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue());
                    if(id.name().equals("<clinit>"))continue; // Runtime admission requires VM initialization of these exact owners.
                    if(id.name().startsWith("<"))throw new IllegalArgumentException("FMC compatibility constructor writer "+id);
                    writers.add(id);
                }
            if(writers.isEmpty())throw new IllegalArgumentException("FMC compatibility writer coverage");
            var polls=Map.of(new Prepare.MethodId("Option","monitor","()V"),
                Set.of(new FmcQualification.Member("Option","ngFmc","Z"),new FmcQualification.Member("Option","engineModel","I")));
            complete.putAll(FmcWriterPrepare.transform(data,complete,writers,java.lang.constant.ClassDesc.of("psx.exporter.FmcCompatibilityWitness"),
                Set.of(new Prepare.MethodId("Core","navFold","(Ljava/lang/String;)Ljava/lang/String;")),polls));
            var before=new HashMap<>(data.inputs);before.putAll(lower);
            TerrainAccess.verifyAdded(before,complete,data.inputs);
            var overlay=new TreeMap<String,byte[]>();
            for(var entry:complete.entrySet())if(!Arrays.equals(entry.getValue(),lower.getOrDefault(entry.getKey(),data.inputs.get(entry.getKey())))){
                if(!data.cf.verify(entry.getValue()).isEmpty())throw new IllegalArgumentException("FMC source final verification");
                overlay.put(entry.getKey(),entry.getValue());
            }
            return new Result(Map.copyOf(overlay),List.of("Entered source: 66 read-only members; "+source.writers().size()+" ordinary field writers; "+source.bufferSites()+" buffer sites; initialized fixed-array membership.",
                "Compatibility: NG/legacy, engine model and applied navigation directory; "+writers.size()+" guarded writer methods.",
                "Recall unavailable: pending finite-job admission, ordinary-thread commit and receiver-qualified reconstruction have not passed complete qualification."));
        }catch(Throwable failure){return new Result(Map.of(),List.of("FMC source unavailable: "+failure));}
    }
    static Result prepare(Prepare data,Map<String,byte[]> lower,String jar){
        if(!FmcQualification.INSTALLED_JAR.equalsIgnoreCase(jar)||!available())return new Result(Map.of(),List.of("FMC source unavailable: installed identity or independent JNI1 probe"));
        return qualify(data,lower);
    }
    static Result bounded(Result r,int lowerBytes){return r.overlay().size()>128||r.overlay().values().stream().anyMatch(b->b.length>1024*1024)||lowerBytes+r.overlay().values().stream().mapToInt(b->b.length).sum()>8*1024*1024
        ?new Result(Map.of(),List.of("FMC source unavailable: complete preparation budget")):r;}
    static void write(Result r,Prepare data,Map<String,byte[]> lower,Path output,Properties properties)throws Exception{
        properties.setProperty("fmc.capabilities",r.overlay().isEmpty()?"0":"32");
        properties.setProperty("fmc.classes",String.join(",",new TreeSet<>(r.overlay().keySet())));
        // The already hash-qualified class index pins every original vendor
        // class. Do not duplicate these 35 hashes in the bounded root manifest.
        properties.setProperty("fmc.originals","class-index");
        Files.createDirectories(output.resolve("fmc-classes"));
        for(var e:r.overlay().entrySet()){
            Files.write(output.resolve("fmc-classes/"+e.getKey()+".class"),e.getValue(),StandardOpenOption.CREATE_NEW);
            properties.setProperty(e.getKey()+".fmc.base",Identity.hash(lower.getOrDefault(e.getKey(),data.inputs.get(e.getKey()))));
            properties.setProperty(e.getKey()+".fmc.prepared",Identity.hash(e.getValue()));
        }
        Files.write(output.resolve("fmc-inventory.txt"),r.evidence(),StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties p,Path root,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        if(!"32".equals(p.getProperty("fmc.capabilities")))return Map.of();
        String source=p.getProperty("fmc.originals");
        if(source!=null&&!source.equals("class-index"))throw new IllegalArgumentException("FMC original identity source");
        var names=new TreeSet<>(Arrays.asList(p.getProperty("fmc.classes","").split(",")));
        if(names.isEmpty()||names.size()>128)throw new IllegalArgumentException("FMC source class count");
        var result=new TreeMap<String,byte[]>();int size=0;
        for(String name:names){
            String original=originals.get(name);
            if(!name.matches("[A-Za-z0-9_$]+")||original==null||!original.equals(source==null?p.getProperty(name+".fmc.original"):original)
                ||!Objects.equals(lower.containsKey(name)?Identity.hash(lower.get(name)):original,p.getProperty(name+".fmc.base")))throw new IllegalArgumentException("FMC source lower identity");
            byte[] bytes;try(var in=Files.newInputStream(root.resolve("fmc-classes/"+name+".class"))){bytes=in.readNBytes(1024*1024+1);}
            size+=bytes.length;if(bytes.length>1024*1024||size>8*1024*1024||!Identity.hash(bytes).equals(p.getProperty(name+".fmc.prepared")))throw new IllegalArgumentException("FMC source prepared identity");
            result.put(name,bytes);
        }
        return Map.copyOf(result);
    }
}
