package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.ClassDesc;
import java.util.*;

/** ND data production is finite, separate from native drawing. Its physical
 * prefix and original Pause return remain native. A ticket taken before that
 * prefix drains a call already past Pause; a closed call cannot enter the FMC
 * tail or change the power/blank flags used by the immutable image interval. */
final class FmcBackgroundAdmissionRenderingProducer {
    static final Prepare.MethodId METHOD=new Prepare.MethodId("NavigationDisplay","setDataFromEiu","(LAfdsFcc$Fcc;ILFmcRte$Rte;LEiuSystem$Eiu;LInertialReference$InertialReferenceUnit;LInertialReference$InertialReferenceUnit;ILAirData$AirDataComputer;LEfisControlPanel;)V");
    static final Prepare.MethodId RADIO=new Prepare.MethodId("RadioAtcBasis","runLargo","()V");
    private FmcBackgroundAdmissionRenderingProducer(){}
    static FmcExclusionPrepare.Receiver receiver(){return new FmcExclusionPrepare.Receiver(0,"LNavigationDisplay;","NavigationDisplay");}
    static int qualify(Prepare data){
        var m=data.methods.get(METHOD);require(m!=null&&m.code().isPresent()&&(m.flags().flagsMask()&ClassFile.ACC_STATIC)==0,"finite instance producer");
        var code=(CodeAttribute)m.code().orElseThrow();var ops=Prepare.instructions(m);int bci=0,tail=-1;var calls=new ArrayList<String>();var stores=new ArrayList<String>();
        for(int at=0;at<ops.size();at++){var op=ops.get(at);if(bci==111)tail=at;
            if(bci<111){
                require(!(op instanceof LoadInstruction load&&load.typeKind()==TypeKind.REFERENCE&&load.slot()==3),"header operand is consumed only in admitted tail");
                require(!(op instanceof InvokeDynamicInstruction)&&!(op instanceof NewObjectInstruction)&&!(op instanceof NewReferenceArrayInstruction)&&!(op instanceof NewPrimitiveArrayInstruction)&&op.opcode()!=Opcode.ATHROW,"no opaque prefix publication");
                if(op instanceof InvokeInstruction c)calls.add(c.owner().asInternalName()+"."+c.name().stringValue()+c.type().stringValue());
                if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){require(f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals("NavigationDisplay"),"prefix private presentation stores");stores.add(f.name().stringValue());}
                if(op instanceof ReturnInstruction)require(bci==110&&op.opcode()==Opcode.RETURN,"exact native Pause return");
                if(op instanceof BranchInstruction branch){int to=code.labelToBci(branch.target());require(to<=111,"no prefix bypass of tail admission");if(bci==102)require(op.opcode()==Opcode.IFEQ&&to==111,"exact native Pause decision");}
                require(!(op instanceof LookupSwitchInstruction)&&!(op instanceof TableSwitchInstruction),"prefix branch roster");
            }
            if(op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&Set.of("eiuIsPowered","blankAll").contains(f.name().stringValue())&&f.owner().asInternalName().equals("NavigationDisplay"))require(bci>=111,"power/blank content is behind the admitted tail");
            bci+=op.sizeInBytes();
        }
        require(tail>0&&calls.equals(List.of("DeltaTime.nextDeltaTime()F","Core.isPaused()Z")),"exact prefix calls");
        require(stores.equals(List.of("delt","efisSideL","updNd","updRange","updHeading","updGroundSpd","updTas","map30TimeoutUpd","actWptNameUpd","actWptDistUpd","updActWptEta","updHeading")),"exact presentation prefix");
        require(FmcBackgroundAdmissionClosure.purePause(data),"source-proven ordinary Pause return");
        for(var h:code.exceptionHandlers())require(code.labelToBci(h.tryStart())>=111&&code.labelToBci(h.handler())>=111,"no native handler enters FMC tail from unadmitted prefix");
        var roots=new HashSet<FmcQualification.Member>();for(String name:FmcRenderingPrepare.NAMES)roots.add(new FmcQualification.Member("IntegratedDisplaySystem",name,"LNavigationDisplay;"));
        var flow=new FmcBackgroundAdmissionReceiverFlow(data,roots,Set.of());int count=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var nativeOps=Prepare.instructions(entry.getValue());for(int at=0;at<nativeOps.size();at++)if(Prepare.call(nativeOps.get(at),METHOD.owner(),METHOD.name(),METHOD.descriptor())){require(entry.getKey().equals(new Prepare.MethodId("EiuSystem","runDelta","()V"))&&flow.receiver(entry.getKey(),at)==FmcBackgroundAdmissionReceiverFlow.Origin.OWNED,"sole four exact bound producer receivers");count++;}}
        require(count==4,"complete original producer call roster");return tail;
    }
    /** These pointers are passed directly to the exact bound producer. Its
     * preserved physical prefix never dereferences that parameter, and its
     * complete old-call ticket drains the only content-consuming tail. */
    static Set<FmcBackgroundAdmissionProjection.Site> argumentSites(Prepare data){
        qualify(data);var caller=new Prepare.MethodId("EiuSystem","runDelta","()V");var ops=Prepare.instructions(data.methods.get(caller));var result=new HashSet<FmcBackgroundAdmissionProjection.Site>();
        for(int at=0;at<ops.size();at++)if(Prepare.call(ops.get(at),METHOD.owner(),METHOD.name(),METHOD.descriptor())){
            int source=at-15;require(source>=0&&ops.get(source) instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC&&f.owner().asInternalName().equals("FmcRte")&&Set.of("rteCduL","rteCduR").contains(f.name().stringValue())&&f.type().equalsString("LFmcRte$Rte;"),"exact direct native producer header argument");
            for(int n=source+1;n<at;n++)require(ops.get(n).opcode()==Opcode.GETSTATIC||ops.get(n).opcode()==Opcode.AALOAD,"no header local/copy/publication before producer admission");
            result.add(new FmcBackgroundAdmissionProjection.Site(caller,source));
        }require(result.size()==4,"four direct header operands only");return Set.copyOf(result);
    }
    static void qualifyRadio(Prepare data){
        int count=0;for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){int bci=0;for(var op:Prepare.instructions(entry.getValue())){if(Prepare.call(op,RADIO.owner(),RADIO.name(),RADIO.descriptor())){require(entry.getKey().equals(new Prepare.MethodId("Core$LargoThread","run","()V"))&&bci==145,"radio sole native periodic caller");count++;}bci+=op.sizeInBytes();}}
        require(count==1&&FmcBackgroundAdmissionClosure.purePause(data),"radio paused periodic pre-entry lifetime");var cadence=data.methods.get(new Prepare.MethodId("Core$LargoThread","run","()V"));var ops=Prepare.instructions(cadence);require(data.models.get("Core$LargoThread").superclass().orElseThrow().asInternalName().equals("java/lang/Thread")&&ops.stream().anyMatch(op->op instanceof ConstantInstruction c&&Long.valueOf(3210).equals(c.constantValue()))&&ops.stream().anyMatch(op->Prepare.call(op,"Core$LargoThread","sleep","(J)V")),"original radio retry sleep remains native");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){int tail=qualify(data);var m=data.cf.parse(lower.getOrDefault(METHOD.owner(),data.inputs.get(METHOD.owner()))).methods().stream().filter(v->v.methodName().equalsString(METHOD.name())&&v.methodType().equalsString(METHOD.descriptor())).findFirst().orElseThrow();require(Arrays.equals(body(data,m),body(data,data.methods.get(METHOD))),"producer must precede field instrumentation");return FmcExclusionPrepare.deferredPrefix(data,lower,METHOD,receiver(),tail);}
    private static byte[] body(Prepare data,MethodModel m){return data.cf.build(ClassDesc.of(METHOD.owner()),b->b.withMethod(m.methodName().stringValue(),m.methodTypeSymbol(),m.flags().flagsMask(),x->x.transformCode(m.code().orElseThrow(),(c,e)->c.with(e))));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC ND producer "+why);}
}
