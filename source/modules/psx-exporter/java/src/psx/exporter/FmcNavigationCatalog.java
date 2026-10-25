package psx.exporter;

import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;

/** Detached navigation binding candidates, read only on the operation worker.
 * Coordinates keep the native radians and elevations/lengths keep native feet.
 * No vendor parser, bank read, pack busy flag or live object is touched. The
 * installed format proof is supplied separately by FmcNavigationPrepare.
 * Resolving this catalog is not permission to publish a route. */
final class FmcNavigationCatalog {
    static final int HEADER=37;
    record Airport(String icao,float lat,float lon,int longestRunway,int elevation,
                   int speedAltitude,int speedKnots,int transitionAltitude,int transitionLevel){}
    record Runway(String airport,String name,float lat,float lon,int length,double trueHeading,
                  int elevation,int displacedThreshold,int width,int stopway,int crossingHeight){}
    enum Kind {SID,STAR,APPROACH}
    record Key(Kind kind,String airport,String procedure,char routeType,String transition){}
    record FamilyKey(Kind kind,String airport,String procedure){}
    record Family(FamilyKey key,List<Procedure> parts){Family{parts=List.copyOf(parts);}}
    /** A procedure has several leg records. A second disjoint occurrence of
     * its complete key is ambiguous; it must never be merged into the first. */
    static final class Procedure {
        final Key key; final long firstRecord; private final List<byte[]> legs;
        Procedure(Key key,long firstRecord,List<byte[]> legs){
            this.key=key;this.firstRecord=firstRecord;
            this.legs=legs.stream().map(byte[]::clone).toList();
        }
        int legs(){return legs.size();}
        byte[] leg(int index){return legs.get(index).clone();}
    }
    record FileIdentity(String name,long length,String sha256){}
    private final Map<String,List<Airport>> airports;
    private final Map<String,List<Runway>> runways;
    private final Map<Key,List<Procedure>> procedures;
    private final Map<FamilyKey,List<Family>> families;
    final List<FileIdentity> identities;
    private final Path sourceRoot;
    private FmcNavigationCatalog(Map<String,List<Airport>> airports,Map<String,List<Runway>> runways,
                                 Map<Key,List<Procedure>> procedures,Map<FamilyKey,List<Family>> families,List<FileIdentity> identities,Path sourceRoot){
        this.airports=freeze(airports);this.runways=freeze(runways);
        this.procedures=freeze(procedures);this.families=freeze(families);this.identities=List.copyOf(identities);this.sourceRoot=sourceRoot.toAbsolutePath().normalize();
    }
    Path sourceRoot(){return sourceRoot;}
    private static <K,V> Map<K,List<V>> freeze(Map<K,List<V>> source){
        var result=new HashMap<K,List<V>>();source.forEach((k,v)->result.put(k,List.copyOf(v)));return Map.copyOf(result);
    }
    private static <K,V> void add(Map<K,List<V>> map,K key,V value){map.computeIfAbsent(key,k->new ArrayList<>()).add(value);}
    private static void require(boolean value,String why){if(!value)throw new IllegalArgumentException("FMC navigation binding "+why);}
    private static <T> T unique(List<T> matches,String role){
        require(matches!=null&&matches.size()==1,role+" missing or ambiguous");return matches.getFirst();
    }
    Airport airport(String icao){return unique(airports.get(icao),"airport "+icao);}
    Runway runway(String icao,String name){return unique(runways.get(icao+"/"+name),"runway "+icao+"/"+name);}
    Procedure procedure(Key key){return unique(procedures.get(key),"procedure "+key);}
    Family family(Kind kind,String airport,String procedure){
        var key=new FamilyKey(kind,airport,procedure);var family=unique(families.get(key),"procedure family "+key);
        // All component keys must be unique too. Several legs belonging to a
        // key are one component; repeating that key after another component
        // does not silently concatenate incompatible definitions.
        for(var part:family.parts())procedure(part.key);return family;
    }
    List<Key> procedureKeys(){return procedures.keySet().stream().sorted(Comparator.comparing(Key::toString)).toList();}
    List<Family> families(Kind kind,String airport){
        return families.keySet().stream().filter(key->key.kind()==kind&&key.airport().equals(airport))
            .map(key->family(key.kind(),key.airport(),key.procedure()))
            .sorted(Comparator.comparingLong(family->family.parts().stream().mapToLong(part->part.firstRecord).min().orElseThrow())).toList();
    }
    List<Runway> runways(String icao){
        var result=new ArrayList<Runway>();
        for(var entry:runways.entrySet())if(entry.getKey().startsWith(icao+"/")){
            require(entry.getValue().size()==1,"duplicate runway in airport catalog "+entry.getKey());result.add(entry.getValue().getFirst());
        }
        result.sort(Comparator.comparing(Runway::name));require(result.size()<=30,"native runway capacity");return List.copyOf(result);
    }
    private static String rawText(byte[] row,int offset,int length){
        char[] chars=new char[length];for(int i=0;i<length;i++){
            int c=127-row[offset+i];require(c>=32&&c<=126,"encoded navigation identifier");chars[i]=(char)c;
        }
        return new String(chars);
    }
    private static String text(byte[] row,int offset,int length){return rawText(row,offset,length).trim();}
    private static String matchingAirport(byte[] row,int offset,String[] wanted){
        // Compare the native encoded four-byte key first. Most records belong
        // to other airports; do not allocate decoded strings for a world scan.
        for(String airport:wanted){boolean same=true;
            for(int i=0;i<4;i++)if(row[offset+i]!=(byte)(127-airport.charAt(i))){same=false;break;}
            if(same)return airport;
        }return null;
    }
    private static ByteBuffer buffer(byte[] row){return ByteBuffer.wrap(row).order(ByteOrder.BIG_ENDIAN);}
    private static void position(float latitude,float longitude){
        require(Float.isFinite(latitude)&&Float.isFinite(longitude)&&Math.abs(latitude)<=Math.PI/2+1e-6
            &&Math.abs(longitude)<=Math.PI+1e-6,"native navigation radians");
    }
    interface Rows {void accept(long index,byte[] row)throws IOException;}
    /** Scan the complete file, including all duplicate keys. Native first-hit
     * lookup cannot establish uniqueness. Input identity includes every byte;
     * partial records, a changing file or short reads reject this candidate. */
    private static FileIdentity scan(Path root,String name,int width,Rows consumer)throws Exception{
        Path path=root.resolve(name);require(Files.isRegularFile(path)&&!Files.isSymbolicLink(path),"navigation file "+name);
        long before=Files.size(path);var modified=Files.getLastModifiedTime(path);
        require(before>=HEADER&&(before-HEADER)%width==0&&before<=256L*1024*1024,"navigation file shape "+name);
        var hash=MessageDigest.getInstance("SHA-256");
        try(var input=new DataInputStream(new BufferedInputStream(Files.newInputStream(path),65536))){
            byte[] header=new byte[HEADER];input.readFully(header);hash.update(header);
            byte[] row=new byte[width];long count=(before-HEADER)/width;
            for(long index=0;index<count;index++){input.readFully(row);hash.update(row);consumer.accept(index,row);}
            require(input.read()==-1,"navigation file grew "+name);
        }
        require(before==Files.size(path)&&modified.equals(Files.getLastModifiedTime(path)),"navigation file changed "+name);
        return new FileIdentity(name,before,HexFormat.of().formatHex(hash.digest()));
    }
    static FmcNavigationCatalog read(Path root,Set<String> requestedAirports,boolean ng,FmcNavigationPrepare.Format format)throws Exception{
        Objects.requireNonNull(format);format.requireQualified();
        require(root.isAbsolute()&&Files.isDirectory(root),"absolute applied navigation directory");
        var wanted=Set.copyOf(requestedAirports);require(wanted.size()<=2,"route airport capacity");
        for(String airport:wanted)require(airport.matches("[A-Z0-9]{4}"),"airport identifier");
        String[] identifiers=wanted.toArray(String[]::new);
        var airports=new HashMap<String,List<Airport>>();var runways=new HashMap<String,List<Runway>>();
        var procedures=new HashMap<Key,List<Procedure>>();var families=new HashMap<FamilyKey,List<Family>>();var identities=new ArrayList<FileIdentity>();
        identities.add(scan(root,"pa.w",57,(index,row)->{
            String key=matchingAirport(row,10,identifiers);if(key==null)return;var b=buffer(row);
            float lat=b.getFloat(0),lon=b.getFloat(4);position(lat,lon);
            add(airports,key,new Airport(key,lat,lon,b.getShort(8),b.getShort(14),b.getShort(16),b.getShort(18),b.getShort(20),b.getShort(22)));
        }));
        identities.add(scan(root,"pg.w",45,(index,row)->{
            String key=matchingAirport(row,8,identifiers);if(key==null)return;var b=buffer(row);
            int number=row[12];char suffix=(char)row[13];require(number>=1&&number<=36&&(suffix==' '||suffix=='L'||suffix=='R'||suffix=='C'),"runway designator");
            String name=(number<10?"0":"")+number+(suffix==' '?"":String.valueOf(suffix));
            float lat=b.getFloat(0),lon=b.getFloat(4);position(lat,lon);double heading=b.getDouble(18);
            require(Double.isFinite(heading)&&b.getInt(14)>=0,"runway geometry");
            add(runways,key+"/"+name,new Runway(key,name,lat,lon,b.getInt(14),heading,b.getShort(26),b.getShort(28),b.getShort(30),b.getShort(32),row[34]));
        }));
        for(String file:List.of("pd.w","pe.w","pf.w")){
            int width=file.equals("pf.w")?64:file.equals("pe.w")?60:61;
            Kind kind=file.equals("pd.w")?Kind.SID:file.equals("pe.w")?Kind.STAR:Kind.APPROACH;
            var groups=new ProcedureGroups(identifiers,ng,kind,file.equals("pe.w")?29:30,procedures,families);
            identities.add(scan(root,file,width,groups::row));groups.complete();
        }
        var result=new FmcNavigationCatalog(airports,runways,procedures,families,identities,root);
        for(String airport:wanted){result.airport(airport);result.runways(airport);}return result;
    }
    private static final class ProcedureGroups {
        final String[] wanted;final boolean ng,approach;final Kind kind;final int codeOffset;final Map<Key,List<Procedure>> result;
        final Map<FamilyKey,List<Family>> families;FamilyKey family;final ArrayList<Procedure> parts=new ArrayList<>();
        Key last;long first;final Set<String> unsupported=new HashSet<>();final ArrayList<byte[]> rows=new ArrayList<>();int retained;
        ProcedureGroups(String[] wanted,boolean ng,Kind kind,int codeOffset,Map<Key,List<Procedure>> result,Map<FamilyKey,List<Family>> families){
            this.wanted=wanted;this.ng=ng;this.kind=kind;this.approach=kind==Kind.APPROACH;this.codeOffset=codeOffset;this.result=result;this.families=families;
        }
        void row(long index,byte[] row){
            String airport=matchingAirport(row,0,wanted);
            // A separator outside the requested airport still ends a group.
            if(airport==null){finishFamily();return;}
            String ident=approach?String.valueOf((char)row[4])+rawText(row,5,3)+(char)row[8]:text(row,4,6);
            char type=(char)row[approach?9:10];String transition=text(row,approach?10:11,5);
            Key key=new Key(kind,airport,ident,type,transition);
            FamilyKey nextFamily=new FamilyKey(kind,airport,ident);
            if(!nextFamily.equals(family)){finishFamily();family=nextFamily;}
            if(!key.equals(last)){finish();last=key;first=index;}
            require(++retained<=16384,"selected-airport procedure capacity");
            rows.add(row.clone());if(row[codeOffset]=='x'||approach&&row[4]=='J')unsupported.add(airport+"/"+ident);
        }
        void finish(){
            if(last!=null){var part=new Procedure(last,first,rows);add(result,last,part);parts.add(part);rows.clear();last=null;}
        }
        void finishFamily(){finish();if(family!=null){add(families,family,new Family(family,parts));parts.clear();family=null;}}
        void complete(){
            finishFamily();
            result.keySet().removeIf(key->key.kind()==kind&&unsupported.contains(key.airport()+"/"+key.procedure())&&(!ng||approach&&key.procedure().charAt(0)=='J'));
            families.keySet().removeIf(key->key.kind()==kind&&unsupported.contains(key.airport()+"/"+key.procedure())&&(!ng||approach&&key.procedure().charAt(0)=='J'));
        }
    }
    boolean stillMatches(Path root)throws Exception{
        for(var identity:identities){Path path=root.resolve(identity.name());
            if(!Files.isRegularFile(path)||Files.size(path)!=identity.length()||!Identity.hash(path).equalsIgnoreCase(identity.sha256()))return false;
        }return true;
    }
}
