package psx.exporter;

import java.lang.classfile.ClassFile;
import java.nio.file.*;
import java.util.*;

/** Independent service-action layer; readouts remain available when it is rejected. */
final class WeightLayer {
    private WeightLayer(){}
    static boolean available(){try{
        if(WeightNative.abiVersion()!=1||WeightNative.actionAbiVersion()!=1||WeightNative.initialize(false))return false;
        WeightNative.poll(java.nio.ByteBuffer.allocateDirect(32),0);WeightNative.begin(0,0);WeightNative.complete(0,false);WeightNative.reject(0);WeightNative.uncertain(0);return true;
    }catch(Throwable unavailable){return false;}}
    static WeightPrepare.Qualified prepare(Prepare data,Map<String,byte[]> lower,String jar,int lowerBytes){
        if(!available())return new WeightPrepare.Qualified(0,Map.of(),List.of("Weight actions unavailable: independent action JNI1"));
        var result=WeightPrepare.qualify(data,lower,jar);
        long size=lowerBytes;for(byte[] bytes:result.overlay().values())size+=bytes.length;
        if(size>8*1024*1024||result.overlay().size()>128||result.overlay().values().stream().anyMatch(b->b.length>1024*1024))return new WeightPrepare.Qualified(0,Map.of(),List.of("Weight actions unavailable: complete preparation budget"));
        return result;
    }
    static void write(WeightPrepare.Qualified result,Prepare data,Map<String,byte[]> lower,Path output,Properties properties)throws Exception{
        String names=String.join(",",new TreeSet<>(result.overlay().keySet()));
        properties.setProperty("weight.capabilities",Integer.toString(result.capabilities()));properties.setProperty("weight.classes",names);
        properties.setProperty("weight.class-count",Integer.toString(result.overlay().size()));
        Files.createDirectories(output.resolve("weight-classes"));
        for(var entry:result.overlay().entrySet()){
            String name=entry.getKey();Files.write(output.resolve("weight-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
        }
        PreparedLayerIndex.write("weight",result.overlay(),data,lower,output,properties);
        Files.write(output.resolve("weight-inventory.txt"),result.evidence(),StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties properties,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        int capabilities=Integer.parseInt(properties.getProperty("weight.capabilities","0"));String names=properties.getProperty("weight.classes","");
        if(capabilities==0&&names.isEmpty())return Map.of();
        if(capabilities!=15||!new TreeSet<>(Arrays.asList(names.split(",",-1))).equals(new TreeSet<>(WeightPrepare.OWNERS)))throw new IllegalArgumentException("Incomplete weight source capability");
        Properties index=PreparedLayerIndex.read("weight",output,properties,Integer.parseInt(properties.getProperty("weight.class-count","0")));
        Map<String,byte[]> overlay=new TreeMap<>();long total=0;
        for(String name:names.split(",",-1)){
            if(!originals.containsKey(name)||overlay.containsKey(name))throw new IllegalArgumentException("Weight source identity");
            byte[] bytes;try(var stream=Files.newInputStream(output.resolve("weight-classes").resolve(name+".class"))){bytes=stream.readNBytes(1024*1024+1);}total+=bytes.length;
            String original=originals.get(name),base=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            if(bytes.length>1024*1024||total>8*1024*1024||!base.equals(index.getProperty(name+".base"))||!Identity.hash(bytes).equals(index.getProperty(name+".prepared"))||!ClassFile.of().parse(bytes).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("Weight source qualification "+name);
            overlay.put(name,bytes);
        }
        return overlay;
    }
}
