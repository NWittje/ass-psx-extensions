package psx.exporter;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Worker-only, hash-bound optional-layer metadata within the existing cache budgets. */
final class PreparedLayerIndex {
    private static final int BYTE_LIMIT=65536;
    private PreparedLayerIndex(){}
    static void write(String layer,Map<String,byte[]> overlay,Prepare data,Map<String,byte[]> lower,Path output,Properties manifest)throws Exception{
        if(overlay.size()>128)throw new IOException("Optional layer class count");
        Properties index=new Properties();
        for(var entry:overlay.entrySet()){
            String name=entry.getKey();byte[] original=data.inputs.get(name);
            if(original==null)throw new IOException("Optional layer original missing "+name);
            index.setProperty(name+".base",Identity.hash(lower.getOrDefault(name,original)));
            index.setProperty(name+".prepared",Identity.hash(entry.getValue()));
        }
        ByteArrayOutputStream buffer=new ByteArrayOutputStream();index.store(buffer,"Qualified optional layer bases and prepared classes");
        byte[] bytes=buffer.toByteArray();if(bytes.length>BYTE_LIMIT)throw new IOException("Optional layer index byte budget");
        Files.write(output.resolve(layer+"-index.properties"),bytes,StandardOpenOption.CREATE_NEW);
        manifest.setProperty(layer+".index.sha256",Identity.hash(bytes));
    }
    static Properties read(String layer,Path output,Properties manifest,int count)throws Exception{
        if(count<1||count>128)throw new IOException("Optional layer class count");
        byte[] bytes;try(var stream=Files.newInputStream(output.resolve(layer+"-index.properties"))){bytes=stream.readNBytes(BYTE_LIMIT+1);}
        if(bytes.length>BYTE_LIMIT||!Identity.hash(bytes).equals(manifest.getProperty(layer+".index.sha256")))throw new IOException("Optional layer index identity");
        Properties index=new Properties();index.load(new ByteArrayInputStream(bytes));
        if(index.size()!=count*2)throw new IOException("Incomplete optional layer index");
        return index;
    }
}
