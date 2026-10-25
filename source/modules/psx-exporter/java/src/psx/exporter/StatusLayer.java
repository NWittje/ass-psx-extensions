package psx.exporter;
import java.nio.file.*;
import java.util.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.lang.classfile.ClassFile;

/** Worker/preparation-only optional-layer identities; no installed class definition. */
final class StatusLayer {
    record Result(int capabilities,Map<String,byte[]> overlay,List<String> evidence){}
    static boolean available(){try{
        if(StatusNative.abiVersion()!=1||StatusNative.intervalTicks()<=0)return false;
        StatusNative.failed();StatusNative.capabilities(0);StatusNative.publish(0,0,0,0);return true;
    }catch(Throwable failure){return false;}}
    static Result prepare(Prepare base,Map<String,byte[]> lower){
        StatusPrepare status=new StatusPrepare(base);
        try{
            if(!available())throw new IOException("optional status JNI1 unavailable");
            status.qualify();Map<String,byte[]> complete=status.transform(lower),overlay=new TreeMap<>();
            for(String name:status.required)overlay.put(name,complete.get(name));
            return new Result(1,Map.copyOf(overlay),List.copyOf(status.evidence));
        }catch(Throwable failure){status.evidence.add("status unavailable: "+failure);return new Result(0,Map.of(),List.copyOf(status.evidence));}
    }
    static Result bounded(Result result,int lowerBytes){
        long bytes=lowerBytes;boolean valid=lowerBytes>=0&&result.overlay.size()<=128;
        for(byte[] value:result.overlay.values()){bytes+=value.length;valid&=value.length<=1024*1024;}
        if(valid&&bytes<=8*1024*1024)return result;
        List<String> evidence=new ArrayList<>(result.evidence);evidence.add("status unavailable: combined prepared artifact budget");
        return new Result(0,Map.of(),List.copyOf(evidence));
    }
    static void write(Result result,Prepare base,Map<String,byte[]> lower,Path output,Properties p)throws Exception{
        String names=String.join(",",new TreeSet<>(result.overlay.keySet()));
        p.setProperty("status.capabilities",Integer.toString(result.capabilities));p.setProperty("status.classes",names);
        p.setProperty("status.classes.sha256",Identity.hash(names.getBytes(StandardCharsets.UTF_8)));
        p.setProperty("status.class-count",Integer.toString(result.overlay.size()));
        Files.createDirectories(output.resolve("status-classes"));
        for(var entry:result.overlay.entrySet()){
            String name=entry.getKey();Files.write(output.resolve("status-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
            p.setProperty(name+".status.original",Identity.hash(base.inputs.get(name)));
            p.setProperty(name+".status.base",Identity.hash(lower.getOrDefault(name,base.inputs.get(name))));
            p.setProperty(name+".status.prepared",Identity.hash(entry.getValue()));
        }
        Files.write(output.resolve("status-inventory.txt"),result.evidence,StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties p,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        int capabilities=Integer.parseInt(p.getProperty("status.capabilities","0"));String names=p.getProperty("status.classes","");
        if(capabilities==0&&names.isEmpty())return Map.of();if(capabilities!=1||names.isEmpty())throw new IllegalArgumentException("status capability");
        if(!Identity.hash(names.getBytes(StandardCharsets.UTF_8)).equals(p.getProperty("status.classes.sha256")))throw new IllegalArgumentException("status inventory identity");
        Map<String,byte[]> overlay=new TreeMap<>();int total=0;
        for(String name:names.split(",",-1)){
            if(!name.matches("[A-Za-z_$][A-Za-z0-9_$]*")||overlay.containsKey(name)||!originals.containsKey(name))throw new IllegalArgumentException("status class identity");
            byte[] bytes;try(InputStream input=Files.newInputStream(output.resolve("status-classes").resolve(name+".class"))){bytes=input.readNBytes(1024*1024+1);}total+=bytes.length;
            String original=originals.get(name),lowerHash=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            if(bytes.length>1024*1024||total>8*1024*1024||!original.equals(p.getProperty(name+".status.original"))||!lowerHash.equals(p.getProperty(name+".status.base"))||!Identity.hash(bytes).equals(p.getProperty(name+".status.prepared")))throw new IllegalArgumentException("status layer hash "+name);
            if(!ClassFile.of().parse(bytes).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("status class name");
            overlay.put(name,bytes);
        }
        if(overlay.size()>128||overlay.size()!=Integer.parseInt(p.getProperty("status.class-count","0"))||!overlay.containsKey("MalfunctionsConseq")||!overlay.containsKey("Core"))throw new IllegalArgumentException("incomplete status layer");
        return overlay;
    }
}
