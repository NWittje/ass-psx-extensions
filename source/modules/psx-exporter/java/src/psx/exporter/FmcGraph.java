package psx.exporter;

import java.nio.*;
import java.util.*;

/** Worker-side owned graph copying. Native bindings and pure factories are
 * supplied only by the installed qualified profile; artifacts cannot select
 * Java classes, fields, constructors or live external objects. No JVM address
 * is exported. This class does not establish recall exclusion or publish roots. */
final class FmcGraph {
    static final int MAX_NODES=65536,MAX_CELLS=262144,MAX_BYTES=8*1024*1024,EXTERNAL=0x80000000;
    record Field(int id,int kind,boolean nullable,int[] types,int[] externalRoles){}
    record Type(int id,int kind,int minimum,int maximum,Field[] fields,Field component){}
    record Root(int role,int type){}
    record Profile(boolean captureQualified,boolean recallQualified,Type[] types,Root[] roots){}
    record Cell(int id,int kind,long bits){}
    record Node(int id,int type,int kind,Cell[] cells,byte[] bytes){}
    record Graph(int[] roots,Node[] nodes){}
    interface Access {
        int type(Object object)throws Exception;
        Object root(int index)throws Exception;
        long bits(Object object,int field)throws Exception;
        Object reference(Object object,int field)throws Exception;
        Object create(Node node,Object[] constructed)throws Exception;
        void bits(Object object,int field,long bits)throws Exception;
        void reference(Object object,int field,Object value)throws Exception;
        int externalRole(Object object)throws Exception;
        Object external(int role)throws Exception;
    }
    private final Profile profile;
    FmcGraph(Profile profile){this.profile=profile;validateProfile();}
    private static void require(boolean value,String reason){if(!value)throw new IllegalArgumentException("FMC graph "+reason);}
    private Type type(int id){require(id>0&&id<=profile.types.length,"unknown native type");return profile.types[id-1];}
    private static int width(int kind){return switch(kind){case 2,3->1;case 4,5,11,12->2;case 6,8,10->4;case 7,9->8;default->0;};}
    private static void sorted(int[] values,int max){int last=0;for(int v:values){require(v>last&&v<=max,"reference rule");last=v;}}
    private void field(Field f){
        require(f.id>0&&f.kind>=1&&f.kind<=9,"field rule");
        if(f.kind==9){sorted(f.types,profile.types.length);sorted(f.externalRoles,65535);require(f.nullable||f.types.length+f.externalRoles.length>0,"empty reference rule");}
        else require(!f.nullable&&f.types.length==0&&f.externalRoles.length==0,"scalar rule");
    }
    private void validateProfile(){
        require(profile.types.length>0&&profile.types.length<=4096&&profile.roots.length>0&&profile.roots.length<=1024,"profile size");
        require(!profile.recallQualified||profile.captureQualified,"recall without capture proof");
        for(int i=0;i<profile.types.length;i++) {
            Type t=profile.types[i];require(t.id==i+1,"type order");
            if(t.kind==1){require(t.minimum==0&&t.maximum==0&&t.fields.length>0&&t.fields.length<=MAX_CELLS&&t.component==null,"record rule");int last=0;for(Field f:t.fields){field(f);require(f.id>last,"field order");last=f.id;}}
            else{int w=width(t.kind);require(w>0&&t.fields.length==0&&t.minimum>=0&&t.maximum>=t.minimum&&t.maximum<=MAX_BYTES/w,"array rule");if(t.kind==10){require(t.component!=null&&t.component.id==1&&t.component.kind==9,"array component");field(t.component);}else require(t.component==null,"primitive component");}
        }
        int prior=0;for(Root r:profile.roots){require(r.role>prior,"root order");type(r.type);prior=r.role;}
    }
    private static long scalar(long bits,int kind){
        require(switch(kind){case 1->bits==0||bits==1;case 2->Long.compareUnsigned(bits,255)<=0;case 3,4->Long.compareUnsigned(bits,65535)<=0;case 5,7->Long.compareUnsigned(bits,0xffffffffL)<=0;case 6,8->true;default->false;},"raw scalar width");return bits;
    }
    Graph capture(Access access)throws Exception {
        require(profile.captureQualified,"capture profile unavailable");
        var objects=new ArrayList<Object>();var ids=new IdentityHashMap<Object,Integer>();var nodes=new ArrayList<Node>();
        int[] roots=new int[profile.roots.length];
        for(int i=0;i<roots.length;i++) {
            Object object=access.root(i);require(object!=null&&access.externalRole(object)==0&&access.type(object)==profile.roots[i].type,"native root binding");roots[i]=owned(object,objects,ids);
        }
        long bytes=8L+8L*roots.length;int cells=0;
        for(int index=0;index<objects.size();index++) {
            Object object=objects.get(index);Type t=type(access.type(object));Cell[] copied=new Cell[0];byte[] raw=new byte[0];
            if(t.kind==1) {
                require(t.fields.length<=MAX_CELLS-cells,"cell capacity");cells+=t.fields.length;bytes+=16L*t.fields.length;
                copied=new Cell[t.fields.length];
                for(int i=0;i<copied.length;i++){Field f=t.fields[i];long bits=f.kind==9?Integer.toUnsignedLong(reference(access.reference(object,f.id),f,access,objects,ids)):scalar(access.bits(object,f.id),f.kind);copied[i]=new Cell(f.id,f.kind,bits);}
            }else{
                int count=length(object,t.kind);require(count>=t.minimum&&count<=t.maximum,"native array capacity");
                int size=Math.multiplyExact(count,width(t.kind));require(bytes+16+size<=MAX_BYTES,"encoded graph capacity");
                raw=new byte[size];ByteBuffer out=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
                for(int i=0;i<count;i++)if(t.kind==10)out.putInt(reference(((Object[])object)[i],t.component,access,objects,ids));else writeElement(out,object,t.kind,i);
                bytes+=raw.length;
            }
            bytes+=16;require(bytes<=MAX_BYTES,"encoded graph capacity");nodes.add(new Node(index+1,t.id,t.kind,copied,raw));
        }
        var graph=new Graph(roots,nodes.toArray(Node[]::new));validate(graph);return graph;
    }
    private static int owned(Object object,List<Object> objects,IdentityHashMap<Object,Integer> ids) {
        Integer id=ids.get(object);if(id!=null)return id;require(objects.size()<MAX_NODES,"object capacity");int next=objects.size()+1;ids.put(object,next);objects.add(object);return next;
    }
    private int reference(Object object,Field rule,Access access,List<Object> objects,IdentityHashMap<Object,Integer> ids)throws Exception {
        if(object==null){require(rule.nullable,"null reference");return 0;}
        int role=access.externalRole(object);if(role!=0){require(role>0&&role<=65535&&Arrays.binarySearch(rule.externalRoles,role)>=0,"external binding");return EXTERNAL|role;}
        require(Arrays.binarySearch(rule.types,access.type(object))>=0,"owned reference type");return owned(object,objects,ids);
    }
    private static int length(Object o,int kind){return switch(kind){case 2->((boolean[])o).length;case 3->((byte[])o).length;case 4->((char[])o).length;case 5->((short[])o).length;case 6->((int[])o).length;case 7->((long[])o).length;case 8->((float[])o).length;case 9->((double[])o).length;case 10->((Object[])o).length;case 11->((String)o).length();case 12->((StringBuilder)o).length();default->throw new IllegalArgumentException("FMC node kind");};}
    private static void writeElement(ByteBuffer b,Object o,int kind,int i){switch(kind){
        case 2->b.put((byte)(((boolean[])o)[i]?1:0));case 3->b.put(((byte[])o)[i]);case 4->b.putChar(((char[])o)[i]);case 5->b.putShort(((short[])o)[i]);case 6->b.putInt(((int[])o)[i]);case 7->b.putLong(((long[])o)[i]);case 8->b.putInt(Float.floatToRawIntBits(((float[])o)[i]));case 9->b.putLong(Double.doubleToRawLongBits(((double[])o)[i]));case 11->b.putChar(((String)o).charAt(i));case 12->b.putChar(((StringBuilder)o).charAt(i));default->throw new IllegalArgumentException("FMC array kind");}}
    private void reference(int value,Field rule,Graph graph){
        if(value==0){require(rule.nullable,"null imported reference");return;}
        if((value&EXTERNAL)!=0)require((value&~EXTERNAL)>0&&Arrays.binarySearch(rule.externalRoles,value&~EXTERNAL)>=0,"imported external role");
        else{require(value>0&&value<=graph.nodes.length,"imported object identity");require(Arrays.binarySearch(rule.types,graph.nodes[value-1].type)>=0,"imported object type");}
    }
    void validate(Graph graph){
        require(graph.roots.length==profile.roots.length&&graph.nodes.length>0&&graph.nodes.length<=MAX_NODES,"graph size");
        long bytes=8L+8L*graph.roots.length;int cells=0;
        for(int i=0;i<graph.roots.length;i++){int root=graph.roots[i];require(root>0&&root<=graph.nodes.length&&graph.nodes[root-1].type==profile.roots[i].type,"imported root binding");}
        for(int i=0;i<graph.nodes.length;i++) {
            Node n=graph.nodes[i];Type t=type(n.type);require(n.id==i+1&&n.kind==t.kind,"node binding");bytes+=16;
            if(n.kind==1){require(n.bytes.length==0&&n.cells.length==t.fields.length&&n.cells.length<=MAX_CELLS-cells,"record coverage");cells+=n.cells.length;bytes+=16L*n.cells.length;
                for(int k=0;k<n.cells.length;k++){Cell c=n.cells[k];Field f=t.fields[k];require(c.id==f.id&&c.kind==f.kind,"field binding");if(f.kind==9){require((c.bits&0xffffffff00000000L)==0,"reference width");reference((int)c.bits,f,graph);}else scalar(c.bits,c.kind);}
            }else{int w=width(n.kind);require(n.cells.length==0&&n.bytes.length%w==0&&n.bytes.length/w>=t.minimum&&n.bytes.length/w<=t.maximum,"array shape");bytes+=n.bytes.length;
                if(n.kind==2)for(byte v:n.bytes)require(v==0||v==1,"boolean array");
                if(n.kind==10){ByteBuffer b=ByteBuffer.wrap(n.bytes).order(ByteOrder.LITTLE_ENDIAN);while(b.hasRemaining())reference(b.getInt(),t.component,graph);}
            }
            require(bytes<=MAX_BYTES,"graph byte capacity");
        }
        boolean[] reached=new boolean[graph.nodes.length];int[] queue=new int[graph.nodes.length];int start=0,end=0;
        for(int id:graph.roots)if(!reached[id-1]){reached[id-1]=true;queue[end++]=id;}
        while(start<end){Node n=graph.nodes[queue[start++]-1];if(n.kind==1){for(Cell c:n.cells)if(c.kind==9)end=visit((int)c.bits,reached,queue,end);}else if(n.kind==10){ByteBuffer b=ByteBuffer.wrap(n.bytes).order(ByteOrder.LITTLE_ENDIAN);while(b.hasRemaining())end=visit(b.getInt(),reached,queue,end);}}
        require(end==graph.nodes.length,"unowned imported node");
    }
    private static int visit(int id,boolean[] reached,int[] queue,int end){if(id>0&&!reached[id-1]){reached[id-1]=true;queue[end++]=id;}return end;}
    Object[] prepare(Graph graph,Access access)throws Exception {
        var rules=new ArrayList<FmcFactoryOrder.Rule>();for(Type t:profile.types)if(t.kind==1)rules.add(new FmcFactoryOrder.Rule(t.id,new int[0]));
        return prepare(graph,access,rules);
    }
    Object[] prepare(Graph graph,Access access,List<FmcFactoryOrder.Rule> factories)throws Exception {
        require(profile.recallQualified,"recall profile unavailable");validate(graph);var live=liveObjects(access);Object[] objects=new Object[graph.nodes.length];
        int[] order=FmcFactoryOrder.order(graph,factories);
        // Allocate in qualified constructor-parent order before linking mutable
        // cycles. Final links are bound by constructors, then checked by access.
        // Factories need their own purity proof; none initializes the whole FMC.
        for(int i:order){
            Node n=graph.nodes[i];int count=n.kind==1?0:n.bytes.length/width(n.kind);
            if(n.kind==11||n.kind==12){char[] value=new char[count];ByteBuffer b=ByteBuffer.wrap(n.bytes).order(ByteOrder.LITTLE_ENDIAN);for(int k=0;k<count;k++)value[k]=b.getChar();String text=new String(value);objects[i]=n.kind==11?text:new StringBuilder(text);}
            else objects[i]=access.create(n,objects);
            require(objects[i]!=null&&access.externalRole(objects[i])==0&&access.type(objects[i])==n.type,"factory binding");
            require(!live.containsKey(objects[i]),"factory returned live native memory");
            if(n.kind!=1)require(length(objects[i],n.kind)==count,"factory capacity");
        }
        var identities=new IdentityHashMap<Object,Boolean>();for(Object object:objects)require(identities.put(object,Boolean.TRUE)==null,"factory reused object");
        for(int i=0;i<objects.length;i++){
            Node n=graph.nodes[i];Object target=objects[i];
            if(n.kind==1)for(Cell c:n.cells){if(c.kind==9)access.reference(target,c.id,resolve((int)c.bits,objects,access));else access.bits(target,c.id,c.bits);}
            else if(n.kind!=11&&n.kind!=12){ByteBuffer b=ByteBuffer.wrap(n.bytes).order(ByteOrder.LITTLE_ENDIAN);for(int k=0;k<length(target,n.kind);k++)if(n.kind==10)((Object[])target)[k]=resolve(b.getInt(),objects,access);else readElement(b,target,n.kind,k);}
        }
        Object[] roots=new Object[graph.roots.length];for(int i=0;i<roots.length;i++)roots[i]=objects[graph.roots[i]-1];return roots;
    }
    private IdentityHashMap<Object,Integer> liveObjects(Access access)throws Exception {
        var list=new ArrayList<Object>();var ids=new IdentityHashMap<Object,Integer>();
        for(int i=0;i<profile.roots.length;i++){Object object=access.root(i);require(object!=null&&access.type(object)==profile.roots[i].type,"current root binding");owned(object,list,ids);}
        int fields=0;
        for(int i=0;i<list.size();i++){
            Object object=list.get(i);Type t=type(access.type(object));
            if(t.kind==1){require(t.fields.length<=MAX_CELLS-fields,"live field capacity");fields+=t.fields.length;for(Field f:t.fields)if(f.kind==9)reference(access.reference(object,f.id),f,access,list,ids);}
            else{int count=length(object,t.kind);require(count>=t.minimum&&count<=t.maximum,"live array capacity");if(t.kind==10)for(Object value:(Object[])object)reference(value,t.component,access,list,ids);}
        }
        return ids;
    }
    private static Object resolve(int id,Object[] objects,Access access)throws Exception {
        if(id==0)return null;if((id&EXTERNAL)==0)return objects[id-1];Object object=access.external(id&~EXTERNAL);require(object!=null&&access.externalRole(object)==(id&~EXTERNAL),"live external role replaced");return object;
    }
    private static void readElement(ByteBuffer b,Object o,int kind,int i){switch(kind){case 2->((boolean[])o)[i]=b.get()!=0;case 3->((byte[])o)[i]=b.get();case 4->((char[])o)[i]=b.getChar();case 5->((short[])o)[i]=b.getShort();case 6->((int[])o)[i]=b.getInt();case 7->((long[])o)[i]=b.getLong();case 8->((float[])o)[i]=Float.intBitsToFloat(b.getInt());case 9->((double[])o)[i]=Double.longBitsToDouble(b.getLong());default->throw new IllegalArgumentException("FMC array kind");}}
    byte[] encode(Graph graph){
        validate(graph);int count=8+graph.roots.length*8;for(Node n:graph.nodes)count=Math.addExact(count,16+(n.kind==1?16*n.cells.length:n.bytes.length));
        ByteBuffer b=ByteBuffer.allocate(count).order(ByteOrder.LITTLE_ENDIAN);b.putInt(graph.roots.length).putInt(graph.nodes.length);
        for(int i=0;i<graph.roots.length;i++)b.putInt(profile.roots[i].role).putInt(graph.roots[i]);
        for(Node n:graph.nodes){b.putInt(n.id).putInt(n.type).putInt(n.kind).putInt(n.kind==1?n.cells.length:n.bytes.length);if(n.kind==1)for(Cell c:n.cells)b.putInt(c.id).putInt(c.kind).putLong(c.bits);else b.put(n.bytes);}
        return b.array();
    }
    Graph decode(byte[] data){
        require(data.length>=8&&data.length<=MAX_BYTES,"graph framing");ByteBuffer b=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int rootCount=b.getInt(),nodeCount=b.getInt();require(rootCount==profile.roots.length&&nodeCount>0&&nodeCount<=MAX_NODES&&rootCount<=b.remaining()/8,"graph counts");
        int[] roots=new int[rootCount];for(int i=0;i<rootCount;i++){require(b.getInt()==profile.roots[i].role,"encoded root role");roots[i]=b.getInt();}
        require(nodeCount<=b.remaining()/16,"encoded node framing");Node[] nodes=new Node[nodeCount];int cells=0;
        for(int i=0;i<nodeCount;i++){int id=b.getInt(),type=b.getInt(),kind=b.getInt(),count=b.getInt();require(count>=0,"encoded node length");Cell[] copied=new Cell[0];byte[] raw=new byte[0];
            if(kind==1){require(count<=MAX_CELLS-cells&&count<=b.remaining()/16,"encoded cells");cells+=count;copied=new Cell[count];for(int k=0;k<count;k++)copied[k]=new Cell(b.getInt(),b.getInt(),b.getLong());}
            else{require(width(kind)>0&&count<=b.remaining(),"encoded bytes");raw=new byte[count];b.get(raw);}nodes[i]=new Node(id,type,kind,copied,raw);
        }
        require(!b.hasRemaining(),"trailing bytes");Graph graph=new Graph(roots,nodes);validate(graph);return graph;
    }
}
