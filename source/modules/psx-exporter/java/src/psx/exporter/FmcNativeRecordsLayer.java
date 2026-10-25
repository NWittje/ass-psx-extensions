package psx.exporter;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Offline bridge composition and bounded immutable profile metadata. */
final class FmcNativeRecordsLayer {
    record Result(Map<String,byte[]> overlay,FmcNativeRecordsProfile.Profile profile,List<String> evidence){}
    static boolean available(){try{return FmcNativeRecordsNative.abiVersion()==1
        &&!FmcNativeRecordsNative.configure(null,0,0)&&!FmcNativeRecordsNative.begin(0,0,0)
        &&FmcNativeRecordsNative.remainingNanos(0)==0;}catch(Throwable absent){return false;}}
    static Result prepare(Prepare data,Map<String,byte[]> lower,String jar,int lowerBytes){
        try{
            if(!FmcNativeRecordsProfile.JAR_SHA256.equalsIgnoreCase(jar)||!available())throw new IllegalArgumentException("native service/JAR unavailable");
            var profile=FmcNativeRecordsProfile.inspect(data);
            var overlay=FmcNativeRecordsPrepare.emit(data.cf,profile,data.inputs,lower);
            if(overlay.values().stream().anyMatch(b->b.length>1024*1024)||lowerBytes+overlay.values().stream().mapToInt(b->b.length).sum()>8*1024*1024)throw new IllegalArgumentException("native service preparation budget");
            return new Result(overlay,profile,List.of("Native FMC3 service:201 original serialized records, same-owner native bridges, original initialization and Pause; no preservation"));
        }catch(Throwable failure){return new Result(Map.of(),null,List.of("Native FMC3 service unavailable: "+failure));}
    }
    static byte[] metadata(FmcNativeRecordsProfile.Profile profile){
        var text=new StringBuilder();for(var slot:profile.slots())text.append(slot.bank()).append('|').append(slot.index()).append('|').append(slot.selector()).append('|').append(slot.key()).append('\n');
        return text.toString().getBytes(StandardCharsets.US_ASCII);
    }
    static FmcNativeRecordsProfile.Profile profile(byte[] bytes){
        if(bytes.length==0||bytes.length>65536)throw new IllegalArgumentException("native prepared profile bound");
        for(byte b:bytes)if(b<0||b=='\r'||b==0)throw new IllegalArgumentException("native prepared profile text");
        var slots=new ArrayList<FmcNativeRecordsProfile.Slot>();String text=new String(bytes,StandardCharsets.US_ASCII);
        if(!text.endsWith("\n"))throw new IllegalArgumentException("native prepared profile terminator");
        for(String line:text.split("\n")){String[] parts=line.split("\\|",-1);if(parts.length!=4)throw new IllegalArgumentException("native prepared profile row");
            slots.add(new FmcNativeRecordsProfile.Slot(FmcNativeRecordsProfile.Bank.valueOf(parts[0]),Integer.parseInt(parts[1]),parts[2],parts[3]));}
        return FmcNativeRecordsProfile.fromPrepared(slots);
    }
    static void write(Result result,Prepare data,Map<String,byte[]> lower,Path output,Properties p)throws Exception{
        p.setProperty("native.fmc.capabilities",result.overlay().isEmpty()?"0":"224");
        if(result.overlay().isEmpty())return;
        Path directory=output.resolve("native-fmc-classes");Files.createDirectories(directory);
        for(var entry:result.overlay().entrySet()){
            String name=entry.getKey();Files.write(directory.resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
            p.setProperty(name+".native.fmc.base",Identity.hash(lower.getOrDefault(name,data.inputs.get(name))));
            p.setProperty(name+".native.fmc.prepared",Identity.hash(entry.getValue()));
        }
        byte[] bytes=metadata(result.profile());Files.write(output.resolve("native-fmc-profile.txt"),bytes,StandardOpenOption.CREATE_NEW);
        p.setProperty("native.fmc.profile.sha256",Identity.hash(bytes));
    }
    static Map<String,byte[]> read(Properties p,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        if(!"224".equals(p.getProperty("native.fmc.capabilities")))return Map.of();
        Map<String,byte[]> overlay=new TreeMap<>();
        for(String name:List.of("Glob","Core","CduHub")){
            String base=lower.containsKey(name)?Identity.hash(lower.get(name)):originals.get(name);
            if(!Objects.equals(base,p.getProperty(name+".native.fmc.base")))throw new IllegalArgumentException("native FMC lower identity");
            byte[] bytes=bounded(output.resolve("native-fmc-classes/"+name+".class"),1024*1024);
            if(!Identity.hash(bytes).equals(p.getProperty(name+".native.fmc.prepared")))throw new IllegalArgumentException("native FMC overlay identity");overlay.put(name,bytes);
        }
        byte[] bytes=bounded(output.resolve("native-fmc-profile.txt"),65536);
        if(!Identity.hash(bytes).equals(p.getProperty("native.fmc.profile.sha256")))throw new IllegalArgumentException("native FMC profile file identity");
        FmcNativeRecordsService.preparedProfile=profile(bytes);FmcNativeRecordsService.prepared=true;return Map.copyOf(overlay);
    }
    private static byte[] bounded(Path path,int limit)throws Exception{try(var in=Files.newInputStream(path)){byte[] bytes=in.readNBytes(limit+1);if(bytes.length>limit)throw new IllegalArgumentException("native FMC preparation bound");return bytes;}}
    private FmcNativeRecordsLayer(){}
}
