package psx.exporter;

import java.util.*;

/** Worker-only deep copy of executed route entries and PERF INIT. The native
 * binding supplies the independently qualified 66-member table, initialized
 * lifecycle and complete writer/buffer witnesses. Implementing Native alone
 * cannot qualify a source. No Pause, native initialization or write occurs.
 * Synthetic ownship slots0/1, MOD, predictions and manual fuel are absent. */
final class FmcNarrowSource {
    interface Native {
        boolean qualified();
        long lifecycle();long generation();long qpc();
        Object reference(Object receiver,int member)throws Throwable;
        long bits(Object receiver,int member)throws Throwable;
    }
    record Snapshot(FmcNarrowContent.Content content,long lifecycle,long generation,
                    int active,long copyBeginQpc,long copyFinishQpc){}
    private FmcNarrowSource(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalStateException("FMC entered source "+why);}
    private static Object required(Object value){require(value!=null,"missing native role");return value;}
    private static String text(Object value){require(value instanceof String,"immutable entered text");return (String)value;}
    private static int integer(long value){require(Long.compareUnsigned(value,0xffffffffL)<=0,"native integer width");return (int)value;}
    private static StringBuilder buffer(Object value){require(value instanceof StringBuilder,"entered buffer type");return (StringBuilder)value;}
    private static String copyText(StringBuilder value){int length=value.length();require(length<=FmcNarrowContent.MAX_TEXT,"entered text bound");return value.substring(0,length);}
    static Snapshot copy(Native nativeState)throws Throwable {
        Objects.requireNonNull(nativeState);require(nativeState.qualified(),"qualification unavailable");
        long lifecycle=nativeState.lifecycle(),generation=nativeState.generation(),begin=nativeState.qpc();
        require(lifecycle>0&&generation>0&&begin>0,"source identity/time");
        long witness=FmcCoherence.beginCopy();require(witness>=0,"concurrent writer");
        int active=integer(nativeState.bits(null,66));require(active==1||active==2,"no executed route");
        // Installed Routes.setActRteBody selects bank1/2 by rteActive. Bank0
        // is native MOD scratch storage; it can be blank while ACT is valid.
        int headerMember=active==1?62:63,bodyMember=active==1?64:65;
        Object header=required(nativeState.reference(null,headerMember)),body=required(nativeState.reference(null,bodyMember));
        require(integer(nativeState.bits(header,59))==active&&nativeState.reference(header,54)==body
            &&nativeState.reference(body,58)==header,"executed header/body roles");
        Object selections=required(nativeState.reference(header,55)),origin=required(nativeState.reference(header,52)),destination=required(nativeState.reference(header,53));
        Object storage=nativeState.reference(body,56);require(storage instanceof Object[]&&((Object[])storage).length==253,"qualified native array");
        Object[] legs=(Object[])storage;int count=integer(nativeState.bits(body,57));require(count>=3&&count<=253,"native leg count");
        var buffers=new StringBuilder[]{buffer(nativeState.reference(header,1)),buffer(nativeState.reference(header,2)),buffer(nativeState.reference(header,3))};
        // Binding changes are witnessed too: a mutator that entered before the
        // first scope must retire before copying. Binding may fail immediately;
        // this worker never waits for a native writer.
        FmcBufferCoherence.bind(nativeState::lifecycle,buffers);
        long[] headerRaw=new long[14];String[] headerText=new String[14];
        for(int i=0;i<3;i++)headerText[i]=copyText(buffers[i]);
        headerText[3]=text(nativeState.reference(origin,4));headerText[4]=text(nativeState.reference(destination,4));
        for(int i=0;i<7;i++)headerText[i+5]=text(nativeState.reference(selections,i+5));
        headerRaw[12]=nativeState.bits(selections,12);headerRaw[13]=nativeState.bits(selections,13);
        long[] perfRaw=new long[11];for(int i=0;i<11;i++)perfRaw[i]=nativeState.bits(null,i+14);
        int real=count-2;long[][] raw=new long[real][27];String[][] texts=new String[real][27];
        Object[] receivers=new Object[real];var unique=new IdentityHashMap<Object,Boolean>();
        for(int i=0;i<real;i++){
            Object waypoint=required(legs[i+2]);require(unique.put(waypoint,Boolean.TRUE)==null,"aliased real waypoint");receivers[i]=waypoint;
            for(int field=0;field<27;field++){
                if(FmcNarrowContent.WAYPOINT_INPUTS.get(field).descriptor().equals("Ljava/lang/String;"))texts[i][field]=text(nativeState.reference(waypoint,field+25));
                else raw[i][field]=nativeState.bits(waypoint,field+25);
            }
        }
        var content=FmcNarrowContent.of(headerRaw,headerText,perfRaw,raw,texts);
        // Check bindings independently of raw-value equality. Replacing an
        // object with equal content is still a source discontinuity.
        require(nativeState.reference(null,headerMember)==header&&nativeState.reference(null,bodyMember)==body
            &&nativeState.reference(header,54)==body&&nativeState.reference(body,58)==header
            &&nativeState.reference(header,55)==selections&&nativeState.reference(header,52)==origin&&nativeState.reference(header,53)==destination
            &&nativeState.reference(body,56)==legs&&integer(nativeState.bits(body,57))==count
            &&integer(nativeState.bits(null,66))==active,"native role changed during copy");
        var actualBuffers=new StringBuilder[]{buffer(nativeState.reference(header,1)),buffer(nativeState.reference(header,2)),buffer(nativeState.reference(header,3))};
        require(FmcBufferCoherence.matches(lifecycle,actualBuffers),"entered buffer replaced");
        for(int i=0;i<real;i++)require(legs[i+2]==receivers[i],"waypoint membership replaced");
        long afterLifecycle=nativeState.lifecycle(),afterGeneration=nativeState.generation(),finish=nativeState.qpc();
        require(finish>=begin&&nativeState.qualified()&&FmcCoherence.endCopy(witness,lifecycle,afterLifecycle,generation,afterGeneration),"source changed during copy");
        return new Snapshot(content,lifecycle,generation,active,begin,finish);
    }
}
