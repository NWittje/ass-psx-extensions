package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Native smoothed PFD indicated limit, read on the metadata worker only. */
final class MaximumSpeedMetadata {
    private MaximumSpeedMetadata() {}

    static List<String> qualify(Map<String,byte[]> inputs) throws Exception {
        var hashes=Map.of(
            "FmcAppRef","121af9d2696b0cad5b49079a35adea347c8fc117941a62208937e4aa1c8ea6a6",
            "PrimaryFlightDisplay","bd9bf034e3e84da1520eb41ee9d1ac005901f07d7954aca33ba152389f61f055");
        ClassFile cf=ClassFile.of();
        for(var e:hashes.entrySet())
            if(!e.getValue().equalsIgnoreCase(Identity.hash(inputs.getOrDefault(e.getKey(),new byte[0]))))
                throw new IllegalArgumentException("Maximum speed unsupported owner "+e.getKey());
        ClassModel fmc=cf.parse(inputs.get("FmcAppRef"));
        FieldModel limit=PerformanceMetadata.field(fmc,"maxSpd","F",true);
        if((limit.flags().flagsMask()&ClassFile.ACC_FINAL)!=0)throw new IllegalArgumentException("Maximum speed mutability");
        PerformanceMetadata.field(cf.parse(inputs.get("Core")),"loaderPaused","Z",true);
        var writers=new ArrayList<String>();int displayReads=0;
        for(var e:inputs.entrySet())for(var m:cf.parse(e.getValue()).methods())if(m.code().isPresent())
            for(var op:Prepare.instructions(m))if(op instanceof FieldInstruction f&&f.owner().asInternalName().equals("FmcAppRef")&&f.name().equalsString("maxSpd")) {
                if(!f.type().equalsString("F")||(f.opcode()!=Opcode.GETSTATIC&&f.opcode()!=Opcode.PUTSTATIC))
                    throw new IllegalArgumentException("Maximum speed access");
                if(f.opcode()==Opcode.PUTSTATIC)writers.add(e.getKey()+"."+m.methodName()+m.methodType());
                else if(e.getKey().equals("PrimaryFlightDisplay"))displayReads++;
            }
        Collections.sort(writers);
        if(!writers.equals(List.of("FmcAppRef.<clinit>()V","FmcAppRef.runAllegro()V"))||displayReads!=2)
            throw new IllegalArgumentException("Maximum speed writer/display coverage");
        return List.of("maxSpd: float indicated-limit knots; original FmcAppRef and PFD hashes qualified",
            "writers: FmcAppRef.<clinit>()V and runAllegro()V; two PFD reads; native smoothing retained");
    }

    static final class Runner {
        final ClassLoader loader;final Path input,target;final long pid,birth;
        Field limit,loading;boolean resolved;String resolutionError;long sequence;
        Runner(ClassLoader loader,Path input,Path directory,long pid,long birth) {
            this.loader=loader;this.input=input;this.pid=pid;this.birth=birth;target=directory.resolve(pid+".json");
        }
        void resolve() throws Exception {
            qualify(Prepare.readJar(input));
            Class<?> fmc=Class.forName("FmcAppRef",false,loader),core=Class.forName("Core",false,loader);
            if(!Agent.sameInputSource(fmc,input)||!Agent.sameInputSource(core,input))throw new IllegalArgumentException("Maximum speed source");
            limit=PerformanceMetadata.Runner.accessible(fmc,"maxSpd",float.class,true);
            loading=PerformanceMetadata.Runner.accessible(core,"loaderPaused",boolean.class,true);
        }
        void sample(boolean ready) {
            long sampled=System.currentTimeMillis(),epoch=Observer.terrainEpoch();
            Float value=null;String error="PSX initialization pending";
            if(ready) {
                if(!resolved){resolved=true;try{resolve();}catch(Throwable failure){resolutionError="Maximum speed qualification unavailable: "+failure.getClass().getSimpleName();}}
                error=resolutionError;
                if(error==null)try {
                    // A single float is atomic; do not reject ordinary native smoothing.
                    // Load/lifecycle bookends qualify the scalar's generation, not a frame bundle.
                    epoch=Observer.terrainEpoch();
                    if(loading.getBoolean(null))throw new IllegalStateException("PSX loading");
                    float observed=limit.getFloat(null);
                    if(loading.getBoolean(null)||epoch!=Observer.terrainEpoch())throw new IllegalStateException("PSX lifecycle changed");
                    if(!Float.isFinite(observed)||observed<=0||observed>450)throw new IllegalArgumentException("Maximum speed range");
                    value=observed;error="";
                }catch(Throwable failure){error="Maximum speed read unavailable: "+failure.getClass().getSimpleName();}
            }
            try { publish(sampled,epoch,value,error); }
            catch(Throwable failure){try{Files.deleteIfExists(target);}catch(Throwable ignored){}}
        }
        void publish(long sampled,long epoch,Float value,String error)throws Exception {
            if(sequence==Long.MAX_VALUE)throw new IllegalStateException("Maximum speed sequence exhausted");
            byte[] bytes=("{\"schema\":1,\"pid\":"+pid+",\"process_start_unix_ms\":"+birth+",\"sequence\":"+(++sequence)+
                ",\"sampled_unix_ms\":"+sampled+",\"lifecycle\":"+epoch+",\"max_speed_kt\":"+value+",\"error\":"+Identity.json(error)+"}").getBytes(StandardCharsets.UTF_8);
            if(bytes.length>4096)throw new IllegalArgumentException("Maximum speed metadata bound");
            Files.createDirectories(target.getParent());Path stage=Files.createTempFile(target.getParent(),pid+"-",".tmp");
            try {
                Files.write(stage,bytes);
                for(int attempt=0;;attempt++)try{Files.move(stage,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);break;}
                catch(AccessDeniedException busy){if(attempt==4)throw busy;Thread.sleep(10);}
            }finally{Files.deleteIfExists(stage);}
        }
    }
}
