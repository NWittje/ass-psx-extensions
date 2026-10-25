package psx.exporter;

import java.nio.*;
import java.nio.charset.*;
import java.security.*;
import java.util.*;

/** Worker-side FMC3 native saved records. Values retain writeBytes/readLine's
 * byte mapping; the codec never interprets an FMC field or restores an aircraft. */
final class FmcNativeRecordsArtifact {
    static final int HEADER=256,MAX=8*1024*1024,MAX_RECORDS=256,MAX_KEY=64,MAX_VALUE=65536;
    record Compatibility(byte[] jar,byte[] profile){
        Compatibility{jar=hash(jar);profile=hash(profile);require(nonzero(jar)&&nonzero(profile),"adapter identity unavailable");}
        public byte[] jar(){return jar.clone();}public byte[] profile(){return profile.clone();}
    }
    record Provenance(String version,int pid,long birth,long lifecycle,long generation,long observation,long frequency,long producer,long sequence,long frame,long finish,byte[] parent){
        Provenance{versionBytes(version);parent=hash(parent);require(pid!=0&&birth!=0&&lifecycle>0&&generation>0&&observation>0&&frequency>0&&producer!=0&&sequence>0&&frame>0&&finish>=observation,"source provenance");}
        public byte[] parent(){return parent.clone();}
    }
    record Context(int validity,int pause,long observation,long utcMilliseconds,double latitude,double longitude,double altitude){}
    record Metadata(byte[] jar,byte[] profile,byte[] navigation,byte[] configuration,int model,Provenance source,Context context){
        Metadata{jar=hash(jar);profile=hash(profile);navigation=hash(navigation);configuration=hash(configuration);Objects.requireNonNull(source);validateContext(context,source.finish);}
        public byte[] jar(){return jar.clone();}public byte[] profile(){return profile.clone();}public byte[] navigation(){return navigation.clone();}public byte[] configuration(){return configuration.clone();}
    }
    record Key(String name,boolean required){Key{keyBytes(name);}}
    record Manifest(List<Key> keys){
        Manifest{keys=List.copyOf(keys);require(!keys.isEmpty()&&keys.size()<=MAX_RECORDS,"manifest count");var names=new HashSet<String>();for(var key:keys)require(names.add(key.name),"duplicate manifest key");}
    }
    record NativeRecord(String key,byte[] value){
        NativeRecord{keyBytes(key);require(value!=null&&value.length<=MAX_VALUE,"value length");value=value.clone();validateValue(value);}
        public byte[] value(){return value.clone();}
        String valueText(){return new String(value,StandardCharsets.ISO_8859_1);}
    }
    record DecodedRecord(String key,String value){}
    record Snapshot(Metadata metadata,boolean complete,List<NativeRecord> records,byte[] payloadHash){
        Snapshot{Objects.requireNonNull(metadata);records=List.copyOf(records);payloadHash=hash(payloadHash);require(records.size()<=MAX_RECORDS,"record count");long size=84L+versionBytes(metadata.source.version).length;for(var r:records)size+=6L+r.key.length()+r.value.length;require(size<=MAX-HEADER,"artifact capacity");}
        public byte[] payloadHash(){return payloadHash.clone();}
    }
    private FmcNativeRecordsArtifact(){}
    private static void require(boolean good,String why){if(!good)throw new IllegalArgumentException("FMC3 "+why);}
    private static byte[] hash(byte[] value){require(value!=null&&value.length==32,"hash width");return value.clone();}
    private static boolean nonzero(byte[] value){int set=0;for(byte v:value)set|=v;return set!=0;}
    static byte[] sha(byte[] value){try{return MessageDigest.getInstance("SHA-256").digest(value);}catch(NoSuchAlgorithmException e){throw new IllegalStateException(e);}}
    private static byte[] keyBytes(String key){
        require(key!=null&&!key.isEmpty()&&key.length()<=MAX_KEY,"key length");for(int i=0;i<key.length();i++){char c=key.charAt(i);require(c>='A'&&c<='Z'||c>='a'&&c<='z'||c>='0'&&c<='9'||c=='_',"key characters");}return key.getBytes(StandardCharsets.US_ASCII);
    }
    private static void validateValue(byte[] value){require(value.length<=MAX_VALUE,"value length");for(byte v:value)require(v!='\r'&&v!='\n',"value line delimiter");}
    private static byte[] versionBytes(String value){
        require(value!=null&&!value.isEmpty()&&value.length()<=64&&value.indexOf('\r')<0&&value.indexOf('\n')<0&&value.indexOf('\0')<0,"source version");
        try{var b=StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value));require(b.remaining()<=64,"version length");return take(b,b.remaining());}catch(CharacterCodingException e){throw new IllegalArgumentException("FMC3 version encoding",e);}
    }
    private static String version(byte[] value){try{String s=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();versionBytes(s);return s;}catch(CharacterCodingException e){throw new IllegalArgumentException("FMC3 version encoding",e);}}
    private static void validateContext(Context c,long finish){
        require(c!=null&&(c.validity&~15)==0&&c.pause>=0&&c.pause<=3,"context flags");require(c.validity==0||c.observation>0&&c.observation<=finish,"context time");
        if((c.validity&1)!=0)require(Double.isFinite(c.latitude)&&Double.isFinite(c.longitude)&&Math.abs(c.latitude)<=Math.PI/2&&Math.abs(c.longitude)<=Math.PI,"context radians");
        if((c.validity&2)!=0)require(Double.isFinite(c.altitude),"context altitude");
    }
    private static byte[] take(ByteBuffer b,int n){require(n>=0&&n<=b.remaining(),"truncated data");byte[] out=new byte[n];b.get(out);return out;}
    private static void records(List<NativeRecord> records,Manifest manifest){
        require(records.size()<=MAX_RECORDS,"record count");var allowed=new HashMap<String,Boolean>();for(var k:manifest.keys)allowed.put(k.name,k.required);var seen=new HashSet<String>();
        for(var r:records){keyBytes(r.key);validateValue(r.value);require(allowed.containsKey(r.key),"unknown record");require(seen.add(r.key),"duplicate record");}
        for(var k:manifest.keys)require(!k.required||seen.contains(k.name),"missing required record");
    }
    static int schema(byte[] bytes){return bytes!=null&&bytes.length>=8&&bytes[0]=='F'&&bytes[1]=='M'&&bytes[2]=='C'&&bytes[3]=='3'&&bytes[4]==3&&bytes[5]==0?3:0;}
    static byte[] encode(Snapshot snapshot,Manifest manifest){
        records(snapshot.records,manifest);var m=snapshot.metadata;var s=m.source;var c=m.context;byte[] text=versionBytes(s.version);long length=84L+text.length;for(var r:snapshot.records)length+=6L+r.key.length()+r.value.length;require(length<=MAX-HEADER,"artifact capacity");
        var p=ByteBuffer.allocate((int)length).order(ByteOrder.LITTLE_ENDIAN);p.putShort((short)text.length).put(text).putLong(s.producer).putLong(s.sequence).putLong(s.frame).putLong(s.finish).putInt(c.validity).putInt(c.pause).putLong(c.observation).putLong(c.utcMilliseconds).putLong(Double.doubleToRawLongBits(c.latitude)).putLong(Double.doubleToRawLongBits(c.longitude)).putLong(Double.doubleToRawLongBits(c.altitude)).putShort((short)snapshot.records.size());
        for(var r:snapshot.records)p.putShort((short)r.key.length()).putInt(r.value.length).put(keyBytes(r.key)).put(r.value);require(!p.hasRemaining(),"payload implementation");
        var out=ByteBuffer.allocate(HEADER+p.capacity()).order(ByteOrder.LITTLE_ENDIAN);out.put(new byte[]{'F','M','C','3'}).putShort((short)3).putShort((short)(snapshot.complete?1:0)).putInt(p.capacity()).putInt(HEADER).put(m.jar).put(m.profile).put(m.navigation).put(m.configuration).put(s.parent).put(sha(p.array())).putInt(s.pid).putInt(m.model).putLong(s.birth).putLong(s.lifecycle).putLong(s.generation).putLong(s.observation).putLong(s.frequency);require(out.position()==HEADER,"header implementation");return out.put(p.array()).array();
    }
    static Snapshot inspect(byte[] bytes,Manifest manifest){
        require(bytes!=null&&bytes.length>=HEADER&&bytes.length<=MAX&&schema(bytes)==3,"schema/size");
        try{var b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);b.position(6);int flags=Short.toUnsignedInt(b.getShort());require(flags<=1,"flags");require(b.getInt()==bytes.length-HEADER&&b.getInt()==HEADER,"framing");byte[] jar=take(b,32),profile=take(b,32),nav=take(b,32),config=take(b,32),parent=take(b,32),payload=take(b,32);require(Arrays.equals(payload,sha(Arrays.copyOfRange(bytes,HEADER,bytes.length))),"payload hash");
            int pid=b.getInt(),model=b.getInt();long birth=b.getLong(),life=b.getLong(),generation=b.getLong(),observation=b.getLong(),frequency=b.getLong();int n=Short.toUnsignedInt(b.getShort());require(n>0&&n<=64,"version length");String text=version(take(b,n));long producer=b.getLong(),sequence=b.getLong(),frame=b.getLong(),finish=b.getLong();var source=new Provenance(text,pid,birth,life,generation,observation,frequency,producer,sequence,frame,finish,parent);var context=new Context(b.getInt(),b.getInt(),b.getLong(),b.getLong(),Double.longBitsToDouble(b.getLong()),Double.longBitsToDouble(b.getLong()),Double.longBitsToDouble(b.getLong()));
            int count=Short.toUnsignedInt(b.getShort());require(count<=MAX_RECORDS&&count<=b.remaining()/6,"record count");var result=new ArrayList<NativeRecord>(count);for(int i=0;i<count;i++){int kn=Short.toUnsignedInt(b.getShort()),vn=b.getInt();require(kn>0&&kn<=MAX_KEY&&vn>=0&&vn<=MAX_VALUE,"record lengths");byte[] key=take(b,kn);for(byte k:key)require(k>0&&k<128,"key encoding");result.add(new NativeRecord(new String(key,StandardCharsets.US_ASCII),take(b,vn)));}require(!b.hasRemaining(),"trailing bytes");records(result,manifest);return new Snapshot(new Metadata(jar,profile,nav,config,model,source,context),flags==1,result,payload);
        }catch(BufferUnderflowException e){throw new IllegalArgumentException("FMC3 truncated envelope",e);}
    }
    static Snapshot decodeForReload(byte[] bytes,Manifest manifest,Compatibility expected){var s=inspect(bytes,manifest);require(s.complete,"incomplete capture");require(Arrays.equals(s.metadata.jar,expected.jar)&&Arrays.equals(s.metadata.profile,expected.profile),"incompatible native adapter");return s;}
    static List<DecodedRecord> decodedValues(Snapshot s,Manifest manifest){records(s.records,manifest);return s.records.stream().map(r->new DecodedRecord(r.key,r.valueText())).toList();}

    /** Input is the complete verified native save, not a recoded map. Unknown
     * aircraft keys are ignored only after validating the whole file framing. */
    static Snapshot extract(byte[] situ,byte[] expectedHash,Metadata metadata,Manifest manifest){
        require(situ!=null&&situ.length>0&&situ.length<=MAX,"native save size");require(Arrays.equals(sha(situ),hash(expectedHash))&&Arrays.equals(metadata.source.parent,expectedHash),"native save hash/provenance");
        var allowed=new HashSet<String>();for(var k:manifest.keys)allowed.add(k.name);var all=new HashSet<String>();var selected=new ArrayList<NativeRecord>();int pos=0,line=0;boolean description=false,ended=false;
        while(pos<situ.length){int end=pos;while(end<situ.length&&situ[end]!='\n')end++;int stop=end;if(stop>pos&&situ[stop-1]=='\r')stop--;for(int i=pos;i<stop;i++)require(situ[i]!='\r',"native CR framing");require(stop-pos<=MAX_VALUE+MAX_KEY+1,"native line length");byte[] bytes=Arrays.copyOfRange(situ,pos,stop);String text=new String(bytes,StandardCharsets.ISO_8859_1);
            if(line==0)require(text.equals("[Aerowinx Precision Simulator - Situation]"),"native header");
            else if(line==1)require(text.equals("[Version "+metadata.source.version+"]"),"native source version");
            else if(text.equals("^-^-^-^ End of description ^-^-^-^")){require(!description&&!ended,"native description marker");description=true;}
            else if(description&&text.equals("[End of file]")){require(!ended,"native duplicate terminator");ended=true;}
            else if(description&&!text.isEmpty()){require(!ended,"native data after terminator");int eq=text.indexOf('=');require(eq>0&&eq<=MAX_KEY,"native record framing");String key=text.substring(0,eq);keyBytes(key);require(all.size()<2048&&all.add(key),"native duplicate/count");byte[] value=Arrays.copyOfRange(bytes,eq+1,bytes.length);validateValue(value);if(allowed.contains(key))selected.add(new NativeRecord(key,value));}
            line++;pos=end+1;
        }
        require(description&&ended&&!all.isEmpty(),"incomplete native save");records(selected,manifest);var s=new Snapshot(metadata,true,selected,new byte[32]);byte[] encoded=encode(s,manifest);return new Snapshot(metadata,true,selected,Arrays.copyOfRange(encoded,176,208));
    }
}
