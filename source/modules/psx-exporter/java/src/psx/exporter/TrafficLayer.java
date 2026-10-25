package psx.exporter;
import java.nio.file.*;
import java.util.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.lang.classfile.ClassFile;

/** Worker/preparation-only optional-layer identities; no installed class definition. */
final class TrafficLayer {
    record Result(int capabilities,Map<String,byte[]> overlay,List<String> evidence){}
    static boolean available(){try{
        if(TrafficNative.abiVersion()!=1||TrafficNative.intervalTicks()<=0)return false;
        TrafficNative.failed();TrafficNative.capabilities(0);
        var source=java.nio.ByteBuffer.allocateDirect(TrafficSnapshot.BYTES);
        var readback=java.nio.ByteBuffer.allocateDirect(TrafficSnapshot.BYTES);
        if(!TrafficNative.initialize(source)||TrafficNative.publish(source)||TrafficNative.poll(readback)!=0)return false;
        TrafficNative.stats(readback);return true;
    }catch(Throwable failure){return false;}}
    static Result prepare(Prepare base,Map<String,byte[]> lower){
        TrafficPrepare traffic=new TrafficPrepare(base);
        try{
            if(!available())throw new IOException("optional traffic JNI1 unavailable");
            traffic.qualify();Map<String,byte[]> complete=traffic.transform(lower),overlay=new TreeMap<>();
            for(String name:traffic.required)overlay.put(name,complete.get(name));
            return new Result(1,Map.copyOf(overlay),List.copyOf(traffic.evidence));
        }catch(Throwable failure){traffic.evidence.add("traffic unavailable: "+failure);return new Result(0,Map.of(),List.copyOf(traffic.evidence));}
    }
    static Result bounded(Result result,int lowerBytes){
        long bytes=lowerBytes;boolean valid=lowerBytes>=0&&result.overlay.size()<=128;
        for(byte[] value:result.overlay.values()){bytes+=value.length;valid&=value.length<=1024*1024;}
        if(valid&&bytes<=8*1024*1024)return result;
        List<String> evidence=new ArrayList<>(result.evidence);evidence.add("traffic unavailable: combined prepared artifact budget");
        return new Result(0,Map.of(),List.copyOf(evidence));
    }
    static void write(Result result,Prepare base,Map<String,byte[]> lower,Path output,Properties p)throws Exception{
        String names=String.join(",",new TreeSet<>(result.overlay.keySet()));
        p.setProperty("traffic.capabilities",Integer.toString(result.capabilities));p.setProperty("traffic.classes",names);
        p.setProperty("traffic.classes.sha256",Identity.hash(names.getBytes(StandardCharsets.UTF_8)));
        p.setProperty("traffic.class-count",Integer.toString(result.overlay.size()));
        Files.createDirectories(output.resolve("traffic-classes"));
        for(var entry:result.overlay.entrySet()){
            String name=entry.getKey();Files.write(output.resolve("traffic-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
            p.setProperty(name+".traffic.original",Identity.hash(base.inputs.get(name)));
            p.setProperty(name+".traffic.base",Identity.hash(lower.getOrDefault(name,base.inputs.get(name))));
            p.setProperty(name+".traffic.prepared",Identity.hash(entry.getValue()));
        }
        Files.write(output.resolve("traffic-inventory.txt"),result.evidence,StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties p,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        int capabilities=Integer.parseInt(p.getProperty("traffic.capabilities","0"));String names=p.getProperty("traffic.classes","");
        if(capabilities==0&&names.isEmpty())return Map.of();if(capabilities!=1||names.isEmpty())throw new IllegalArgumentException("traffic capability");
        if(!Identity.hash(names.getBytes(StandardCharsets.UTF_8)).equals(p.getProperty("traffic.classes.sha256")))throw new IllegalArgumentException("traffic inventory identity");
        Map<String,byte[]> overlay=new TreeMap<>();int total=0;
        for(String name:names.split(",",-1)){
            if(!name.matches("[A-Za-z_$][A-Za-z0-9_$]*")||overlay.containsKey(name)||!originals.containsKey(name))throw new IllegalArgumentException("traffic class identity");
            byte[] bytes;try(InputStream input=Files.newInputStream(output.resolve("traffic-classes").resolve(name+".class"))){bytes=input.readNBytes(1024*1024+1);}total+=bytes.length;
            String original=originals.get(name),lowerHash=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            if(bytes.length>1024*1024||total>8*1024*1024||!original.equals(p.getProperty(name+".traffic.original"))||!lowerHash.equals(p.getProperty(name+".traffic.base"))||!Identity.hash(bytes).equals(p.getProperty(name+".traffic.prepared")))throw new IllegalArgumentException("traffic layer hash "+name);
            if(!ClassFile.of().parse(bytes).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("traffic class name");
            overlay.put(name,bytes);
        }
        if(overlay.size()>128||overlay.size()!=Integer.parseInt(p.getProperty("traffic.class-count","0"))||!overlay.containsKey("Traffic$Intruder")||!overlay.containsKey("Traffic")||!overlay.containsKey("Core"))throw new IllegalArgumentException("incomplete traffic layer");
        return overlay;
    }
}
