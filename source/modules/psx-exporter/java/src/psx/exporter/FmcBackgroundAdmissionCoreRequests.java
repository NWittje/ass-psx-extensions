package psx.exporter;
import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.util.*;
/** Admission precedes native request consumption. The original file/load
 * operation and its success/error continuation keep one positive lifetime;
 * closed cadence retains countdowns and runs the surrounding Core frame. */
final class FmcBackgroundAdmissionCoreRequests {
 static final Prepare.MethodId LENTO=new Prepare.MethodId("Core$LentoThread","run","()V"),MODERATO=new Prepare.MethodId("Core$ModeratoThread","run","()V"),LOAD=new Prepare.MethodId("InstructorLoadSitu","loadLastLoaded","()V");
 static final List<FmcBackgroundAdmissionRegionPrepare.Region> REGIONS=List.of(
  new FmcBackgroundAdmissionRegionPrepare.Region(LENTO,715,847,"EarthTime.realCal",4,Set.of(),Set.of(),0,true),
  new FmcBackgroundAdmissionRegionPrepare.Region(MODERATO,196,232,"Core.situReload",3,Set.of(),Set.of(),1,true));
 static void qualify(Prepare data){FmcBackgroundAdmissionSavePrepare.qualify(data);try{require(Identity.hash(data.inputs.get("Core$ModeratoThread")).equalsIgnoreCase("6FBC7E832F767EC8EBB92C4624071562691926FEBD9AE3C0AA1C9FC34F74984A"),"complete native Moderato source");require(Identity.hash(data.inputs.get("InstructorLoadSitu")).equalsIgnoreCase("4AFF10987E8920CC5AC20C86B8C4F4561471D00B5208CD7A52BFADB63C5BA7FD"),"complete native load continuation source");}catch(Exception e){throw new IllegalArgumentException("FMC Core request digest",e);}require(data.methods.containsKey(LOAD),"original load continuation");var callers=new HashSet<String>();for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(e.getValue())){if(op instanceof InvokeInstruction i&&i.owner().asInternalName().equals(LOAD.owner())&&i.name().equalsString(LOAD.name())&&i.type().equalsString(LOAD.descriptor()))callers.add(e.getKey()+"@"+bci);bci+=op.sizeInBytes();}}require(callers.equals(Set.of(LENTO+"@227",MODERATO+"@229",new Prepare.MethodId("NetServerThread","run","()V")+"@1001",new Prepare.MethodId("PsxKeys","keyPressed","(Ljava/awt/event/KeyEvent;)V")+"@587",new Prepare.MethodId("PsxKeys","keyPressed","(Ljava/awt/event/KeyEvent;)V")+"@4277")),"complete original scheduled-load caller roster "+callers);}
 static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower)throws Exception{qualify(data);return FmcBackgroundAdmissionRegionPrepare.transform(data,lower,REGIONS);}
 static Set<Integer> sites(Prepare data,Prepare.MethodId id){qualify(data);var sites=new HashSet<Integer>();int bci=0,at=0;for(var op:Prepare.instructions(data.methods.get(id))){for(var r:REGIONS)if(r.method().equals(id)&&bci>=r.beginBci()&&bci<r.endBci())sites.add(at);if(id.equals(LENTO)&&bci==227)sites.add(at);bci+=op.sizeInBytes();at++;}return Set.copyOf(sites);}
 private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC Core native requests "+why);}
 private FmcBackgroundAdmissionCoreRequests(){}
}
