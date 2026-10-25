package psx.exporter;

import java.nio.*;
import java.nio.charset.*;
import java.security.*;
import java.util.*;

/** Worker-side FMC2 envelope, matching the shared C++ schema2 codec. Import
 * uses the producer's trusted graph profile and exact compatibility, never
 * classes, members or live references chosen by an artifact. Context remains
 * independently timed diagnostic evidence. No native callback calls this. */
final class FmcArtifact {
    static final int HEADER=256,MAX=8*1024*1024;
    record Compatibility(byte[] jar,byte[] profile,byte[] navigation,byte[] configuration,int model){
        Compatibility {jar=hash(jar,true);profile=hash(profile,true);navigation=hash(navigation,true);configuration=hash(configuration,true);require(model==1||model==2,"model");}
        public byte[] jar(){return jar.clone();}public byte[] profile(){return profile.clone();}public byte[] navigation(){return navigation.clone();}public byte[] configuration(){return configuration.clone();}
        boolean same(Compatibility other){return other!=null&&model==other.model&&Arrays.equals(jar,other.jar)&&Arrays.equals(profile,other.profile)&&Arrays.equals(navigation,other.navigation)&&Arrays.equals(configuration,other.configuration);}
    }
    record Provenance(String version,int pid,long birth,long lifecycle,long generation,long observation,long frequency,long producer,long sequence,long frame,long finish,byte[] parent){
        Provenance {FmcArtifact.version(version);parent=hash(parent,false);require(pid!=0&&birth!=0&&lifecycle>0&&generation>0&&frequency>0&&producer!=0&&sequence>0&&frame>0&&observation>0&&finish>=observation,"source provenance");}
        public byte[] parent(){return parent.clone();}
    }
    record Context(int validity,int pause,long observation,long utcMilliseconds,double latitude,double longitude,double altitude){ }
    record Snapshot(Compatibility compatibility,Provenance source,Context context,boolean complete,FmcGraph.Graph memory,byte[] payloadHash){
        Snapshot {payloadHash=hash(payloadHash,false);}
        public byte[] payloadHash(){return payloadHash.clone();}
    }
    private FmcArtifact(){}
    private static void require(boolean value,String reason){if(!value)throw new IllegalArgumentException("FMC2 "+reason);}
    private static byte[] hash(byte[] bytes,boolean nonzero){
        require(bytes!=null&&bytes.length==32,"hash width");int set=0;for(byte value:bytes)set|=value;require(!nonzero||set!=0,"compatibility hash unavailable");return bytes.clone();
    }
    static byte[] sha(byte[] bytes){try{return MessageDigest.getInstance("SHA-256").digest(bytes);}catch(NoSuchAlgorithmException impossible){throw new IllegalStateException("required SHA-256 unavailable",impossible);}}
    private static byte[] take(ByteBuffer b,int length){require(length>=0&&length<=b.remaining(),"truncated data");byte[] result=new byte[length];b.get(result);return result;}
    private static byte[] version(String text){
        require(text!=null&&!text.isEmpty()&&text.indexOf('\r')<0&&text.indexOf('\n')<0&&text.indexOf('\0')<0,"source version");
        try{ByteBuffer encoded=StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(text));require(encoded.remaining()<=64,"source version length");return take(encoded,encoded.remaining());}
        catch(CharacterCodingException error){throw new IllegalArgumentException("FMC2 source version encoding",error);}
    }
    private static String version(byte[] text){
        try{String result=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(text)).toString();version(result);return result;}
        catch(CharacterCodingException error){throw new IllegalArgumentException("FMC2 source version encoding",error);}
    }
    private static void context(Context value,long finish){
        require(value!=null&&(value.validity&~15)==0&&value.pause>=0&&value.pause<=3,"context flags");
        require(value.validity==0||value.observation>0&&value.observation<=finish,"context observation time");
        if((value.validity&1)!=0)require(Double.isFinite(value.latitude)&&Double.isFinite(value.longitude)&&Math.abs(value.latitude)<=Math.PI/2&&Math.abs(value.longitude)<=Math.PI,"reference radians");
        if((value.validity&2)!=0)require(Double.isFinite(value.altitude),"reference MSL feet");
    }
    static int schema(byte[] bytes){
        if(bytes==null||bytes.length<8||bytes[0]!='F'||bytes[1]!='M'||bytes[2]!='C'||bytes[5]!=0||bytes[4]!=bytes[3]-'0')return 0;
        return bytes[3]=='1'?1:bytes[3]=='2'?2:0;
    }
    static byte[] encode(FmcGraph trusted,Compatibility compatibility,Provenance source,Context context,boolean complete,FmcGraph.Graph memory){
        Objects.requireNonNull(compatibility);Objects.requireNonNull(source);context(context,source.finish);
        byte[] text=version(source.version),graph=trusted.encode(memory);int payloadBytes=Math.addExact(82+text.length,graph.length);
        require(payloadBytes<=MAX-HEADER,"artifact capacity");ByteBuffer payload=ByteBuffer.allocate(payloadBytes).order(ByteOrder.LITTLE_ENDIAN);
        payload.putShort((short)text.length).put(text).putLong(source.producer).putLong(source.sequence).putLong(source.frame).putLong(source.finish)
            .putInt(context.validity).putInt(context.pause).putLong(context.observation).putLong(context.utcMilliseconds)
            .putLong(Double.doubleToRawLongBits(context.latitude)).putLong(Double.doubleToRawLongBits(context.longitude)).putLong(Double.doubleToRawLongBits(context.altitude)).put(graph);
        require(!payload.hasRemaining(),"payload implementation");byte[] data=payload.array();ByteBuffer out=ByteBuffer.allocate(HEADER+data.length).order(ByteOrder.LITTLE_ENDIAN);
        out.put(new byte[]{'F','M','C','2'}).putShort((short)2).putShort((short)(complete?1:0)).putInt(data.length).putInt(HEADER)
            .put(compatibility.jar).put(compatibility.profile).put(compatibility.navigation).put(compatibility.configuration).put(source.parent).put(sha(data))
            .putInt(source.pid).putInt(compatibility.model).putLong(source.birth).putLong(source.lifecycle).putLong(source.generation).putLong(source.observation).putLong(source.frequency);
        require(out.position()==HEADER,"header implementation");out.put(data);return out.array();
    }
    static Snapshot decode(byte[] bytes,FmcGraph trusted,Compatibility expected,boolean requireComplete){
        require(bytes!=null&&bytes.length>=HEADER&&bytes.length<=MAX&&schema(bytes)==2,"artifact schema/size");
        try{
            ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);b.position(6);int flags=Short.toUnsignedInt(b.getShort());require(flags<=1,"flags");int length=b.getInt();require(length==bytes.length-HEADER&&b.getInt()==HEADER,"outer framing");
            byte[] jar=take(b,32),profile=take(b,32),nav=take(b,32),configuration=take(b,32),parent=take(b,32),payloadHash=take(b,32);
            int pid=b.getInt(),model=b.getInt();long birth=b.getLong(),lifecycle=b.getLong(),generation=b.getLong(),observation=b.getLong(),frequency=b.getLong();
            var compatibility=new Compatibility(jar,profile,nav,configuration,model);require(compatibility.same(expected),"incompatible checkpoint");require(!requireComplete||flags==1,"incomplete checkpoint");
            require(Arrays.equals(payloadHash,sha(Arrays.copyOfRange(bytes,HEADER,bytes.length))),"payload checksum");
            int versionBytes=Short.toUnsignedInt(b.getShort());require(versionBytes>0&&versionBytes<=64,"source version length");String text=version(take(b,versionBytes));
            long producer=b.getLong(),sequence=b.getLong(),frame=b.getLong(),finish=b.getLong();var source=new Provenance(text,pid,birth,lifecycle,generation,observation,frequency,producer,sequence,frame,finish,parent);
            var context=new Context(b.getInt(),b.getInt(),b.getLong(),b.getLong(),Double.longBitsToDouble(b.getLong()),Double.longBitsToDouble(b.getLong()),Double.longBitsToDouble(b.getLong()));context(context,finish);
            var memory=trusted.decode(take(b,b.remaining()));return new Snapshot(compatibility,source,context,flags!=0,memory,payloadHash);
        }catch(BufferUnderflowException error){throw new IllegalArgumentException("FMC2 truncated envelope",error);}
    }
    static byte[] memoryHash(byte[] bytes){
        require(bytes!=null&&bytes.length>=HEADER+2&&bytes.length<=MAX&&schema(bytes)==2,"memory hash framing");ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);int versionBytes=Short.toUnsignedInt(b.getShort(HEADER));
        int begin=HEADER+82+versionBytes;require(versionBytes>0&&versionBytes<=64&&begin<=bytes.length-8,"memory hash framing");
        // Caller first decodes/validates the artifact. This hash distinguishes
        // owned memory from its process/context/clock provenance.
        return sha(Arrays.copyOfRange(bytes,begin,bytes.length));
    }
}
