package psx.exporter;

import java.lang.classfile.ClassFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Independently optional PIS1 preparation; an incomplete layer cannot activate. */
final class StatusControlsLayer {
    record Result(int capabilities,Map<String,byte[]> overlay,List<String> evidence){}
    static Result prepare(Prepare data,Map<String,byte[]> lower){
        StatusControlsPrepare status=new StatusControlsPrepare(data);
        try{
            if(!StatusControlsObserver.available())throw new IllegalArgumentException("IOS Status JNI1 unavailable");
            status.qualify();if(status.groups.capabilities==0)throw new IllegalArgumentException("No IOS Status source groups qualified");
            var complete=status.groups.transform(lower);Map<String,byte[]> overlay=new TreeMap<>();
            for(String name:status.groups.required.keySet())overlay.put(name,complete.get(name));
            return new Result(status.groups.capabilities,Map.copyOf(overlay),List.copyOf(status.groups.evidence));
        }catch(Throwable failure){return new Result(0,Map.of(),List.of("IOS Status unavailable: "+failure));}
    }
    static Result bounded(Result result,int lowerBytes){
        long size=lowerBytes;for(byte[] bytes:result.overlay.values())size+=bytes.length;
        if(lowerBytes>=0&&result.overlay.size()<=128&&result.overlay.values().stream().allMatch(b->b.length<=1024*1024)&&size<=8*1024*1024)return result;
        return new Result(0,Map.of(),List.of("IOS Status unavailable: complete prepared-artifact budget"));
    }
    static void write(Result result,Prepare data,Map<String,byte[]> lower,Path output,Properties properties)throws Exception{
        String names=String.join(",",new TreeSet<>(result.overlay.keySet()));
        properties.setProperty("iosstatus.capabilities",Integer.toString(result.capabilities));
        properties.setProperty("iosstatus.classes",names);properties.setProperty("iosstatus.classes.sha256",Identity.hash(names.getBytes(StandardCharsets.UTF_8)));
        properties.setProperty("iosstatus.class-count",Integer.toString(result.overlay.size()));
        Files.createDirectories(output.resolve("iosstatus-classes"));
        for(var entry:result.overlay.entrySet()){
            String name=entry.getKey();Files.write(output.resolve("iosstatus-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
        }
        PreparedLayerIndex.write("iosstatus",result.overlay,data,lower,output,properties);
        Files.write(output.resolve("iosstatus-inventory.txt"),result.evidence,StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties properties,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        int capabilities=Integer.parseInt(properties.getProperty("iosstatus.capabilities","0"));String names=properties.getProperty("iosstatus.classes","");
        if(capabilities==0&&names.isEmpty())return Map.of();
        if(capabilities<=0||(capabilities&~15)!=0||names.isEmpty()||!Identity.hash(names.getBytes(StandardCharsets.UTF_8)).equals(properties.getProperty("iosstatus.classes.sha256")))throw new IllegalArgumentException("IOS Status capability/inventory");
        Properties index=PreparedLayerIndex.read("iosstatus",output,properties,Integer.parseInt(properties.getProperty("iosstatus.class-count","0")));
        Map<String,byte[]> overlay=new TreeMap<>();long total=0;
        for(String name:names.split(",",-1)){
            if(!name.matches("[A-Za-z_$][A-Za-z0-9_$]*")||overlay.containsKey(name)||!originals.containsKey(name))throw new IllegalArgumentException("IOS Status class identity");
            byte[] bytes;try(var input=Files.newInputStream(output.resolve("iosstatus-classes").resolve(name+".class"))){bytes=input.readNBytes(1024*1024+1);}total+=bytes.length;
            String original=originals.get(name),base=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            if(bytes.length>1024*1024||total>8*1024*1024||!base.equals(index.getProperty(name+".base"))||!Identity.hash(bytes).equals(index.getProperty(name+".prepared"))||!ClassFile.of().parse(bytes).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("IOS Status class qualification "+name);
            overlay.put(name,bytes);
        }
        if(overlay.size()>128||overlay.size()!=Integer.parseInt(properties.getProperty("iosstatus.class-count","0"))||!overlay.containsKey("Core"))throw new IllegalArgumentException("Incomplete IOS Status preparation");
        return overlay;
    }
}
