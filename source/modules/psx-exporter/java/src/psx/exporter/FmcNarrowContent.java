package psx.exporter;

import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;

/** Worker-only entered-data schema for FMC2. These detached records contain no
 * vendor objects, live binding indices, aircraft/navigation state, predictions,
 * MOD state or manual fuel. Codec admission is not native capture/recall
 * qualification; only the separately completed native adapter may advertise it.
 * Native reconstruction must regenerate synthetic slots0/1 and derived state. */
final class FmcNarrowContent {
    static final int MAX_WAYPOINTS=251,MAX_TEXT=8192;
    static final int HEADER=1,PERF=2,ROUTE=3,WAYPOINT=4,WAYPOINT_ARRAY=5,TEXT=6;
    record Input(String owner,String name,String descriptor,String role){}
    static final List<Input> HEADER_INPUTS=List.of(
        input("FmcRte$Rte","sbOrig","Ljava/lang/StringBuilder;","origin"),
        input("FmcRte$Rte","sbDest","Ljava/lang/StringBuilder;","destination"),
        input("FmcRte$Rte","sbCoRte","Ljava/lang/StringBuilder;","companyRoute"),
        input("FmcRte$Rwy","name","Ljava/lang/String;","departureRunway"),
        input("FmcRte$Rwy","name","Ljava/lang/String;","arrivalRunway"),
        selection("selSid"),selection("selSidTrans"),selection("selSidRwy"),selection("selStar"),
        selection("selStarTrans"),selection("selApp"),selection("selAppTrans"),
        input("FmcDepArr$DepArrSelections","rwyExtNm10","I","runwayExtension"),
        input("FmcDepArr$DepArrSelections","rwyExtFpaDeg10","I","runwayExtensionAngle"));
    static final List<Input> PERF_INPUTS=List.of(
        perf("zfWeight","F"),perf("fuelReserves","I"),perf("costIndex","I"),perf("crzAlt","I"),
        perf("crzCg","I"),perf("stepSize","I"),perf("stepSizeNg","I"),perf("minFuelTemp","I"),
        perf("grWtBig","Z"),perf("zfWtBig","Z"),perf("cgBig","Z"));
    static final List<Input> WAYPOINT_INPUTS=List.of(
        wpt("rteType","C"),wpt("pathTerm","C"),wpt("turnDir","C"),wpt("turnDirValid","Z"),
        wpt("spdLimType","C"),wpt("altLimType","C"),wpt("reserved","C"),wpt("wptCol41","C"),
        wpt("wptCol42","C"),wpt("wptCol43","C"),wpt("identStrFix","Ljava/lang/String;"),
        wpt("identStrVia","Ljava/lang/String;"),wpt("optionStr","Ljava/lang/String;"),wpt("lat","F"),
        wpt("lon","F"),wpt("crs10","I"),wpt("spdLim","I"),wpt("alt1","I"),wpt("alt2","I"),
        wpt("rnp","I"),wpt("identStrReqNav","Ljava/lang/String;"),wpt("univ","I"),wpt("legDist","I"),
        wpt("efc","I"),wpt("vertAngle","I"),wpt("ctrFixLat","F"),wpt("ctrFixLon","F"));
    private static Input input(String owner,String name,String descriptor,String role){return new Input(owner,name,descriptor,role);}
    private static Input selection(String name){return input("FmcDepArr$DepArrSelections",name,"Ljava/lang/String;",name);}
    private static Input perf(String name,String descriptor){return input("FmcPerf",name,descriptor,name);}
    private static Input wpt(String name,String descriptor){return input("RouteWpt",name,descriptor,name);}
    private static final class Entry {
        final int type;final long[] bits;final Object[] refs;
        Entry(int type,int count){this.type=type;bits=new long[count];refs=new Object[count];}
    }
    static final class Content {
        private final Entry[] roots;
        private Content(Entry[] roots){this.roots=roots;}
        String headerText(int field){return (String)roots[0].refs[field-1];}
        long headerBits(int field){return roots[0].bits[field-1];}
        long perfBits(int field){return roots[1].bits[field-1];}
        int waypoints(){return ((Entry[])roots[2].refs[0]).length;}
        long waypointBits(int index,int field){return ((Entry[])roots[2].refs[0])[index].bits[field-1];}
        String waypointText(int index,int field){return (String)((Entry[])roots[2].refs[0])[index].refs[field-1];}
    }
    private FmcNarrowContent(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC narrow content "+why);}
    private static int kind(String descriptor){return switch(descriptor){case "Z"->1;case "C"->3;case "I"->5;case "F"->7;case "Ljava/lang/String;","Ljava/lang/StringBuilder;"->9;default->throw new IllegalArgumentException("FMC narrow content descriptor");};}
    private static List<Input> fields(int type){return switch(type){case HEADER->HEADER_INPUTS;case PERF->PERF_INPUTS;case WAYPOINT->WAYPOINT_INPUTS;default->throw new IllegalArgumentException("FMC narrow content record type");};}
    private static FmcGraph.Field scalar(int id,int kind){return new FmcGraph.Field(id,kind,false,new int[0],new int[0]);}
    private static FmcGraph.Field reference(int id,int type){return new FmcGraph.Field(id,9,false,new int[]{type},new int[0]);}
    private static FmcGraph.Type record(int id,List<Input> fields){
        var values=new FmcGraph.Field[fields.size()];for(int i=0;i<values.length;i++){int k=kind(fields.get(i).descriptor);values[i]=k==9?reference(i+1,TEXT):scalar(i+1,k);}
        return new FmcGraph.Type(id,1,0,0,values,null);
    }
    static FmcGraph codec(){return new FmcGraph(new FmcGraph.Profile(true,true,new FmcGraph.Type[]{
        record(HEADER,HEADER_INPUTS),record(PERF,PERF_INPUTS),new FmcGraph.Type(ROUTE,1,0,0,new FmcGraph.Field[]{reference(1,WAYPOINT_ARRAY)},null),
        record(WAYPOINT,WAYPOINT_INPUTS),new FmcGraph.Type(WAYPOINT_ARRAY,10,0,MAX_WAYPOINTS,new FmcGraph.Field[0],reference(1,WAYPOINT)),
        new FmcGraph.Type(TEXT,11,0,MAX_TEXT,new FmcGraph.Field[0],null)},new FmcGraph.Root[]{new FmcGraph.Root(1,HEADER),new FmcGraph.Root(2,PERF),new FmcGraph.Root(3,ROUTE)}));}
    /** Raw values retain the native scalar units/bits, with an explicit null
     * scalar slot for each string and zero unused raw slot. No conversion from
     * displayed CDU text or cached predictions occurs here. */
    private static Entry entry(int type,long[] raw,String[] texts){
        var fields=fields(type);require(raw!=null&&texts!=null&&raw.length==fields.size()&&texts.length==fields.size(),"complete field vectors");var e=new Entry(type,fields.size());
        for(int i=0;i<raw.length;i++){
            int k=kind(fields.get(i).descriptor);if(k==9){require(raw[i]==0&&texts[i]!=null&&texts[i].length()<=MAX_TEXT,"text field");e.refs[i]=texts[i];}
            else{require(texts[i]==null,"scalar/text distinction");long value=raw[i];require(switch(k){case 1->value==0||value==1;case 3->Long.compareUnsigned(value,65535)<=0;case 5,7->Long.compareUnsigned(value,0xffffffffL)<=0;default->false;},"raw scalar width");
                if(k==7)require(Float.isFinite(Float.intBitsToFloat((int)value)),"finite native float");e.bits[i]=value;}
        }
        return e;
    }
    static Content of(long[] headerRaw,String[] headerText,long[] perfRaw,long[][] waypointRaw,String[][] waypointText){
        require(perfRaw!=null&&waypointRaw!=null&&waypointText!=null&&waypointRaw.length==waypointText.length&&waypointRaw.length<=MAX_WAYPOINTS,"complete entered route");
        Entry header=entry(HEADER,headerRaw,headerText),perf=entry(PERF,perfRaw,new String[PERF_INPUTS.size()]),route=new Entry(ROUTE,1);var waypoints=new Entry[waypointRaw.length];
        for(int i=0;i<waypoints.length;i++)waypoints[i]=entry(WAYPOINT,waypointRaw[i],waypointText[i]);route.refs[0]=waypoints;return new Content(new Entry[]{header,perf,route});
    }
    private static final class Access implements FmcGraph.Access {
        final Content content;
        Access(Content content){this.content=content;}
        public int type(Object o){if(o instanceof Entry e){require(e.type>=HEADER&&e.type<=WAYPOINT,"record identity");return e.type;}if(o instanceof Entry[])return WAYPOINT_ARRAY;if(o instanceof String)return TEXT;throw new IllegalArgumentException("FMC narrow content foreign object");}
        public Object root(int index){return content.roots[index];}
        public long bits(Object object,int field){return ((Entry)object).bits[field-1];}
        public Object reference(Object object,int field){return ((Entry)object).refs[field-1];}
        public Object create(FmcGraph.Node node,Object[] prior){return node.type()==WAYPOINT_ARRAY?new Entry[node.bytes().length/4]:new Entry(node.type(),node.cells().length);}
        public void bits(Object object,int field,long raw){((Entry)object).bits[field-1]=raw;}
        public void reference(Object object,int field,Object value){((Entry)object).refs[field-1]=value;}
        public int externalRole(Object object){return 0;}
        public Object external(int role){throw new IllegalArgumentException("FMC narrow content external binding");}
    }
    static FmcGraph.Graph graph(Content content)throws Exception{return codec().capture(new Access(Objects.requireNonNull(content)));}
    static byte[] encode(Content content)throws Exception{return codec().encode(graph(content));}
    static Content decode(byte[] bytes)throws Exception{
        var codec=codec();var empty=of(new long[14],emptyHeader(),new long[11],new long[0][],new String[0][]);
        Object[] roots=codec.prepare(codec.decode(bytes),new Access(empty));var content=new Content(new Entry[]{(Entry)roots[0],(Entry)roots[1],(Entry)roots[2]});
        // Recheck semantic finite-width/text invariants after graph validation.
        validate(content);return content;
    }
    private static String[] emptyHeader(){var text=new String[14];Arrays.fill(text,0,12,"");return text;}
    private static void validate(Content c){
        entry(HEADER,c.roots[0].bits,Arrays.copyOf(c.roots[0].refs,14,String[].class));entry(PERF,c.roots[1].bits,new String[11]);
        for(var w:(Entry[])c.roots[2].refs[0])entry(WAYPOINT,w.bits,Arrays.copyOf(w.refs,27,String[].class));
    }
    static byte[] contractHash(){
        var b=new StringBuilder("FMC2-entered-route-PERFINIT-v1;synthetic-0/1-not-saved;executed-active-bank1/2;bank0-is-mod-not-saved;manual-fuel-and-flight-number-preserved;max251/8192;");
        for(var group:List.of(HEADER_INPUTS,PERF_INPUTS,WAYPOINT_INPUTS))for(var f:group)b.append(f.owner).append('.').append(f.name).append(':').append(f.descriptor).append('@').append(f.role).append(';');
        try{return MessageDigest.getInstance("SHA-256").digest(b.toString().getBytes(StandardCharsets.UTF_8));}catch(NoSuchAlgorithmException impossible){throw new IllegalStateException(impossible);}
    }
}
