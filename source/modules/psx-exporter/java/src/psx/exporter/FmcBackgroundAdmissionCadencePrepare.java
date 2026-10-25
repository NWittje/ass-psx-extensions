package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;
/** Fifteen complete FMC cadence leaves. Deferral precedes every native input,
 * timer and bank-consumption instruction; a positive old call drains its full
 * original lifetime. Physical Core/FmcGlobal/Routes/EIU callers remain native.
 * Shared one-shot helpers are deliberately not entry frontiers. */
final class FmcBackgroundAdmissionCadencePrepare {
    record Site(Prepare.MethodId method,Prepare.MethodId caller,int bci){}
    record Profile(Set<Prepare.MethodId> methods,List<Site> sites){}
    private static Prepare.MethodId id(String owner,String name){return new Prepare.MethodId(owner,name,"()V");}
    private static Site site(String owner,String name,String parent,int bci){return new Site(id(owner,name),id("FmcGlobal",parent),bci);}
    private static final List<Site> SITES=List.of(
        site("FmcFixInfo","runAllegro","runAllegro",12),site("FmcFixInfo","runLento","runLento",53),
        new Site(id("FmcNg","runDelta"),id("Routes","runDelta"),18),site("FmcNg","runLento","runLento",59),site("FmcNg","runModerato","runModerato",220),
        site("FmcPerf","runLento","runLento",23),site("FmcPerf","runModerato","runModerato",202),
        site("FmcProgress1","runLento","runLento",38),site("FmcProgress2","runLento","runLento",41),
        site("FmcVnav","runLargo","runLargo",1340),site("FmcVnav","runLento","runLento",32),site("FmcVnav","runModerato","runModerato",208),
        site("FmcVnavCalc","runDelta","runDelta",10),site("FmcVnavCalc","runLento","runLento",35),site("FmcVnavCalc","runModerato","runModerato",217));
    private static final Map<String,String> OWNERS=Map.of(
        "FmcFixInfo","A84FDB1FA2036AAD95AB82B166B776470A6629FF03D591732ECAB454CFB58A77",
        "FmcNg","A0503BAEAB6D48C80031E8BBF9BEEEF7BED7509608D2D575C39A79E0CF193264",
        "FmcPerf","20A94BEF3544D992C4B39F73158A585E1D280163B747372886EFB56B501C67AD",
        "FmcProgress1","021CE26C9B7D0B641567342F84214A72A1F3E5E5A9A72CC2988928657298CE1C",
        "FmcProgress2","94C472AB8D7EFB52983118FAFF1DC1799E33FA0D1947542206B8B38D2E6DEB10",
        "FmcVnav","A764CF1D893A9C9FD281B8612C7D97E0E4BF3E82B71842DC64D6EE90680F4709",
        "FmcVnavCalc","A3D8231E1BD677331805A009931003B8A48F1BF0854F1F68351319AB15FBEACA");
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC cadence "+why);}
    private static boolean sourceIdentity(byte[] bytes,String expected){try{return Identity.hash(bytes).equalsIgnoreCase(expected);}catch(Exception failure){throw new IllegalArgumentException("FMC cadence native source digest",failure);}}
    static Profile inspect(Prepare data){
        for(var owner:OWNERS.entrySet())require(data.inputs.containsKey(owner.getKey())&&sourceIdentity(data.inputs.get(owner.getKey()),owner.getValue()),"qualified complete native cycle/helper source changed "+owner.getKey());
        var selected=new HashSet<Prepare.MethodId>();for(var site:SITES){require(selected.add(site.method),"duplicate trusted leaf");var m=data.methods.get(site.method);require(m!=null&&m.code().isPresent()&&(m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0&&m.methodType().equalsString("()V"),"exact static void leaf");
            // These are synchronous cycle functions, not asynchronous frame
            // loops. Their native helper calls, handlers and output stay intact.
            for(var op:Prepare.instructions(m))if(op instanceof InvokeInstruction c)require(!(c.owner().asInternalName().startsWith("Core$")||c.name().equalsString("sleep")||c.name().equalsString("wait")||c.name().equalsString("join")||c.owner().asInternalName().equals("Network")&&!c.name().equalsString("notClient")),"mixed transport/frame frontier "+site.method);
        }
        var calls=new HashMap<Prepare.MethodId,List<Site>>();for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(op instanceof InvokeInstruction c){var target=new Prepare.MethodId(c.owner().asInternalName(),c.name().stringValue(),c.type().stringValue());if(selected.contains(target)){require(c.opcode()==Opcode.INVOKESTATIC,"leaf static dispatch");calls.computeIfAbsent(target,k->new ArrayList<>()).add(new Site(target,entry.getKey(),bci));}}bci+=op.sizeInBytes();}}
        for(var site:SITES)require(calls.getOrDefault(site.method,List.of()).equals(List.of(site)),"whole original caller/byte-position changed "+site.method);
        // The one intermediate Delta call is likewise an exact recurring
        // path. Do not replace or gate the mixed physical caller itself.
        var routes=id("Routes","runDelta");int count=0;for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(op instanceof InvokeInstruction c&&c.owner().asInternalName().equals(routes.owner())&&c.name().equalsString(routes.name())&&c.type().equalsString(routes.descriptor())){require(entry.getKey().equals(id("FmcGlobal","runDelta"))&&bci==7,"Delta recurring caller changed");count++;}bci+=op.sizeInBytes();}}require(count==1,"one Delta recurring caller");
        return new Profile(Set.copyOf(selected),SITES);
    }
    /** The integrator should union profile.methods into its single original
     * exclusion weave. This direct form is for isolated qualification only. */
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,Profile profile){require(inspect(data).equals(profile),"trusted source profile replaced");return FmcExclusionPrepare.transform(data,lower,profile.methods);}
    private FmcBackgroundAdmissionCadencePrepare(){}
}