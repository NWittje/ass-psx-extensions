package psx.exporter;
import java.nio.file.*;
import java.util.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.lang.classfile.ClassFile;

/** Worker/preparation-only optional-layer identities; no installed class definition. */
final class EnvironmentLayer {
    record Result(int groups,Map<String,byte[]> overlay,List<String> evidence){}
    static boolean available(){try{
        if(EnvironmentNative.abiVersion()!=1||EnvironmentNative.intervalTicks()<=0)return false;
        EnvironmentNative.capabilities(0);EnvironmentNative.state(0,false);
        EnvironmentNative.publish(0,0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
          0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
        return true;
    }catch(Throwable failure){return false;}}
    static boolean contextAvailable(){try{
        if(EnvironmentContextNative.abiVersion()!=1)return false;
        EnvironmentContextNative.capabilities(false);
        EnvironmentContextNative.publish(0,0,0,0,0,0,0,-1,0,0,0);
        return true;
    }catch(Throwable failure){return false;}}
    static Result prepare(Prepare base,Map<String,byte[]> lower){
        EnvironmentPrepare environment=new EnvironmentPrepare(base);
        try{
            if(!available())throw new IOException("optional environment JNI1 unavailable");
            environment.qualify();
            if(environment.context&&!contextAvailable()){environment.context=false;environment.evidence.add("context unavailable: independent optional JNI1 probe");}
            Map<String,byte[]> complete;
            try{complete=environment.transform(lower);}catch(Throwable contextFailure){
                if(!environment.context)throw contextFailure;
                environment.context=false;environment.evidence.add("context transformation rejected independently: "+contextFailure);
                complete=environment.transform(lower);
            }
            try {
                if(!WeatherChangeObserver.available())throw new IOException("independent manual WX JNI1 unavailable");
                WeatherChangePrepare changes=new WeatherChangePrepare(base);changes.qualify();var count=new HashSet<>(environment.required);count.addAll(changes.required);if(count.size()>128)throw new IOException("manual weather class budget");
                complete=changes.transform(complete);environment.required.addAll(changes.required);environment.evidence.addAll(changes.evidence);
            } catch(Throwable unavailable) { environment.evidence.add("manual WX change capability unavailable: "+unavailable); }
            try {
                if(!EnvironmentVisibilityObserver.available())throw new IOException("independent local visibility JNI1 unavailable");
                complete=EnvironmentVisibilityPrepare.transform(base,complete);environment.required.add("Weather");
                environment.evidence.add("local visibility: qualified computed integer immediately before native Calc.getSmoothExpo(FFFF)F; native smoothing preserved");
            }catch(Throwable unavailable){environment.evidence.add("local visibility target unavailable: "+unavailable);}
            Map<String,byte[]> overlay=new TreeMap<>();
            for(String name:environment.required)overlay.put(name,complete.get(name));
            return new Result(environment.capabilities,Map.copyOf(overlay),List.copyOf(environment.evidence));
        }catch(Throwable failure){environment.evidence.add("environment unavailable: "+failure);return new Result(0,Map.of(),List.copyOf(environment.evidence));}
    }
    static Result bounded(Result result,int lowerBytes){
        long bytes=lowerBytes;boolean valid=lowerBytes>=0&&result.overlay.size()<=128;
        for(byte[] value:result.overlay.values()){bytes+=value.length;valid&=value.length<=1024*1024;}
        if(valid&&bytes<=8*1024*1024)return result;
        List<String> evidence=new ArrayList<>(result.evidence);evidence.add("environment unavailable: combined prepared artifact budget");
        return new Result(0,Map.of(),List.copyOf(evidence));
    }
    static void write(Result result,Prepare base,Map<String,byte[]> lower,Path output,Properties p)throws Exception{
        String names=String.join(",",new TreeSet<>(result.overlay.keySet()));
        p.setProperty("environment.groups",Integer.toString(result.groups));p.setProperty("environment.classes",names);
        boolean context=result.overlay.containsKey("Core")&&ClassFile.of().parse(result.overlay.get("Core")).methods().stream().anyMatch(m->m.methodName().equalsString(EnvironmentPrepare.PREFIX+"context"));
        p.setProperty("environment.context",Boolean.toString(context));
        p.setProperty("environment.local-visibility",Boolean.toString(localVisibility(result.overlay)));
        p.setProperty("environment.manual-weather",Boolean.toString(result.overlay.containsKey("Qs")&&ClassFile.of().parse(result.overlay.get("Qs")).methods().stream().anyMatch(m->m.methodName().equalsString(WeatherChangePrepare.PREFIX+"packet"))));
        p.setProperty("environment.classes.sha256",Identity.hash(names.getBytes(StandardCharsets.UTF_8)));
        p.setProperty("environment.class-count",Integer.toString(result.overlay.size()));
        Files.createDirectories(output.resolve("environment-classes"));
        ByteArrayOutputStream identities=new ByteArrayOutputStream();identities.write(new byte[]{'P','X','W','I',1,0,(byte)result.overlay.size(),0});
        for(var entry:new TreeMap<>(result.overlay).entrySet()){
            String name=entry.getKey();Files.write(output.resolve("environment-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
            for(String hash:List.of(Identity.hash(base.inputs.get(name)),Identity.hash(lower.getOrDefault(name,base.inputs.get(name))),Identity.hash(entry.getValue())))identities.write(HexFormat.of().parseHex(hash));
        }
        byte[] identityBytes=identities.toByteArray();Files.write(output.resolve("environment-identities.bin"),identityBytes,StandardOpenOption.CREATE_NEW);
        p.setProperty("environment.identity-index.sha256",Identity.hash(identityBytes));
        Files.write(output.resolve("environment-inventory.txt"),result.evidence,StandardOpenOption.CREATE_NEW);
    }
    static Map<String,byte[]> read(Properties p,Path output,Map<String,String> originals,Map<String,byte[]> lower)throws Exception{
        int groups=Integer.parseInt(p.getProperty("environment.groups","0"));String names=p.getProperty("environment.classes","");
        if(groups==0&&names.isEmpty())return Map.of();if(groups<1||groups>3||names.isEmpty())throw new IllegalArgumentException("environment capability");
        if(!Identity.hash(names.getBytes(StandardCharsets.UTF_8)).equals(p.getProperty("environment.classes.sha256")))throw new IllegalArgumentException("environment inventory identity");
        byte[] identities=null;String identityHash=p.getProperty("environment.identity-index.sha256");
        if(identityHash!=null){try(InputStream input=Files.newInputStream(output.resolve("environment-identities.bin"))){identities=input.readNBytes(8+96*128+1);}
            int count=names.split(",",-1).length;
            if(identities.length!=8+96*count||identities.length>8+96*128||identities[0]!='P'||identities[1]!='X'||identities[2]!='W'||identities[3]!='I'||identities[4]!=1||identities[5]!=0||Byte.toUnsignedInt(identities[6])!=count||identities[7]!=0||!Identity.hash(identities).equals(identityHash))throw new IllegalArgumentException("environment identity index");
        }
        Map<String,byte[]> overlay=new TreeMap<>();int total=0,identityOffset=8;
        for(String name:names.split(",",-1)){
            if(!name.matches("[A-Za-z_$][A-Za-z0-9_$]*")||overlay.containsKey(name)||!originals.containsKey(name))throw new IllegalArgumentException("environment class identity");
            byte[] bytes;try(InputStream input=Files.newInputStream(output.resolve("environment-classes").resolve(name+".class"))){bytes=input.readNBytes(1024*1024+1);}total+=bytes.length;
            String original=originals.get(name),lowerHash=lower.containsKey(name)?Identity.hash(lower.get(name)):original;
            String[] hashes={original,lowerHash,Identity.hash(bytes)};String[] suffixes={"original","base","prepared"};
            for(int i=0;i<3;i++){
                String legacy=p.getProperty(name+".environment."+suffixes[i]);String expected=identities==null?legacy:HexFormat.of().formatHex(identities,identityOffset,identityOffset+32);
                if(!hashes[i].equals(expected)||(legacy!=null&&!hashes[i].equals(legacy)))throw new IllegalArgumentException("environment layer hash "+name);identityOffset+=32;
            }
            if(bytes.length>1024*1024||total>8*1024*1024)throw new IllegalArgumentException("environment layer budget "+name);
            if(!ClassFile.of().parse(bytes).thisClass().asInternalName().equals(name))throw new IllegalArgumentException("environment class name");
            overlay.put(name,bytes);
        }
        if(overlay.size()>128||overlay.size()!=Integer.parseInt(p.getProperty("environment.class-count","0"))||!overlay.containsKey("Core")||!overlay.containsKey("EarthTime")||((groups&2)!=0&&(!overlay.containsKey("Weather")||!overlay.containsKey("Aerodynamics"))))throw new IllegalArgumentException("incomplete environment layer");
        boolean context=ClassFile.of().parse(overlay.get("Core")).methods().stream().anyMatch(m->m.methodName().equalsString(EnvironmentPrepare.PREFIX+"context"));
        if(context!=Boolean.parseBoolean(p.getProperty("environment.context","false")))throw new IllegalArgumentException("environment context qualification");
        boolean manual=overlay.containsKey("Qs")&&ClassFile.of().parse(overlay.get("Qs")).methods().stream().anyMatch(m->m.methodName().equalsString(WeatherChangePrepare.PREFIX+"packet"));
        if(manual!=Boolean.parseBoolean(p.getProperty("environment.manual-weather","false")))throw new IllegalArgumentException("manual weather qualification");
        if(localVisibility(overlay)!=Boolean.parseBoolean(p.getProperty("environment.local-visibility","false")))throw new IllegalArgumentException("local visibility qualification");
        return overlay;
    }
    private static boolean localVisibility(Map<String,byte[]> overlay){return overlay.containsKey("Weather")&&ClassFile.of().parse(overlay.get("Weather")).methods().stream().anyMatch(m->m.methodName().equalsString(EnvironmentVisibilityPrepare.METHOD));}
}
