package psx.exporter;

import java.lang.classfile.ClassFile;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Independent optional transaction before Recorder-LAST. */
final class InstrumentLayer {
    record Result(Map<String,byte[]> overlay,List<String> evidence,boolean lower){}
    private InstrumentLayer(){}
    static boolean available(){try{
        if(BridgeNative.instrumentRecorderAbiVersion()!=1)return false;
        BridgeNative.instrumentRecorderPoll(ByteBuffer.allocateDirect(48));BridgeNative.instrumentRecorderPublish(ByteBuffer.allocateDirect(64),0);
        return !BridgeNative.instrumentRecorderInitialize("");
    }catch(Throwable unavailable){return false;}}
    static Result prepare(Prepare data,Map<String,byte[]> lower,boolean display,int bytes){
        InstrumentPrepare work=new InstrumentPrepare(data);try{
            if(!display||!available())throw new IllegalArgumentException("independent instrument JNI/display prerequisite");
            work.qualify();Map<String,byte[]> overlay=work.transform(lower);long size=bytes;
            for(byte[] value:overlay.values()){if(value.length>1024*1024)throw new IllegalArgumentException("instrument class byte limit");size+=value.length;}
            if(size>8*1024*1024)throw new IllegalArgumentException("instrument complete preparation byte limit");
            return new Result(overlay,List.copyOf(work.evidence),work.lowerQualified);
        }catch(Throwable unavailable){work.evidence.add("instrument unavailable: "+unavailable);return new Result(Map.of(),List.copyOf(work.evidence),false);}
    }
    static void write(Result result,Prepare data,Map<String,byte[]> lower,Path output,Properties p)throws Exception{
        p.setProperty("instrument.capabilities",result.overlay.isEmpty()?"0":"1");p.setProperty("instrument.classes",String.join(",",new TreeSet<>(result.overlay.keySet())));
        p.setProperty("instrument.lower.capabilities",result.lower?"1":"0");
        p.setProperty("instrument.class-count",Integer.toString(result.overlay.size()));Files.createDirectories(output.resolve("instrument-classes"));
        for(var entry:result.overlay.entrySet())Files.write(output.resolve("instrument-classes").resolve(entry.getKey()+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
        PreparedLayerIndex.write("instrument",result.overlay,data,lower,output,p);Files.write(output.resolve("instrument-inventory.txt"),result.evidence,StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties p,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        String capability=p.getProperty("instrument.capabilities","0"),names=p.getProperty("instrument.classes","");
        if(capability.equals("0")&&names.isEmpty())return Map.of();
        Set<String> expected=new TreeSet<>(Set.of("ThrStand","Visuals"));if(lowerAvailable(p))expected.addAll(InstrumentLowerPrepare.OWNERS);
        if(!Set.of("0","1").contains(p.getProperty("instrument.lower.capabilities","0"))||!capability.equals("1")||!names.equals(String.join(",",expected))||!p.getProperty("instrument.class-count","").equals(Integer.toString(expected.size())))throw new IllegalArgumentException("instrument complete source capability");
        Properties index=PreparedLayerIndex.read("instrument",output,p,expected.size());Map<String,byte[]> overlay=new TreeMap<>();
        for(String name:names.split(",")){
            String original=originals.get(name);if(original==null)throw new IllegalArgumentException("instrument original identity "+name);
            byte[] value;try(var input=Files.newInputStream(output.resolve("instrument-classes").resolve(name+".class"))){value=input.readNBytes(1024*1024+1);}
            String expectedBase=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            if(value.length>1024*1024||!expectedBase.equals(index.getProperty(name+".base"))||!Identity.hash(value).equals(index.getProperty(name+".prepared"))||!ClassFile.of().parse(value).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("instrument overlay identity "+name);
            overlay.put(name,value);
        }return overlay;
    }
    static boolean lowerAvailable(Properties p){return p.getProperty("instrument.lower.capabilities","0").equals("1");}
}
