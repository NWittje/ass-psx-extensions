package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Original EXEC's route-control result. Activation publishes a control bank;
 * the native recurrent reader normally propagates its scalar/pointer state
 * afterwards. A closed recall must complete that propagation before ACK.
 * There is no second EXEC in this native protocol. */
final class FmcReconstructionActivationPrepare {
    static final String NAME="$psxExporter$fmcPropagateExecutedControl";
    private static final Prepare.MethodId ORIGINAL=new Prepare.MethodId("FmcRte","readGlobViewAct","()V");
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("FmcRte",NAME,"()V"));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC native activation "+why);}
    private static boolean constant(Instruction op,int value){return op instanceof ConstantInstruction c&&Objects.equals(c.constantValue(),value);}
    private static boolean branch(Instruction op,Opcode kind,int target,CodeAttribute code){return op instanceof BranchInstruction b&&b.opcode()==kind&&code.labelToBci(b.target())==target;}
    static void qualify(Prepare data){
        for(String side:List.of("L","R")){
            var method=data.requireMethod("FmcRte","setModInact"+side,"(I)Z");var code=(CodeAttribute)method.code().orElseThrow();var p=FmcReconstructionTransitionEffects.positions(method);
            require(Prepare.field(p.get(45),Opcode.GETSTATIC,"FmcRte","rte1")&&Prepare.field(p.get(51),Opcode.GETFIELD,"Routes$RteBody","legsAmo")&&constant(p.get(54),4)&&branch(p.get(55),Opcode.IF_ICMPLT,81,code),"RTE1 native activation threshold "+side);
            require(Prepare.field(p.get(68),Opcode.GETSTATIC,"FmcRte","rte2")&&Prepare.field(p.get(74),Opcode.GETFIELD,"Routes$RteBody","legsAmo")&&constant(p.get(77),4)&&branch(p.get(78),Opcode.IF_ICMPGE,108,code),"RTE2 native activation threshold "+side);
            require(constant(p.get(82),3)&&constant(p.get(83),'0')&&Prepare.call(p.get(85),"java/lang/StringBuilder","setCharAt","(IC)V")&&Prepare.call(p.get(102),"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")&&constant(p.get(106),0)&&p.get(107).opcode()==Opcode.IRETURN,"short activation cancels pending and returns false "+side);
            require(constant(p.get(224),2)&&branch(p.get(225),Opcode.IF_ICMPNE,263,code)&&constant(p.get(249),2)&&constant(p.get(251),3)&&Prepare.call(p.get(252),"java/lang/StringBuilder","charAt","(I)C")&&Prepare.call(p.get(255),"java/lang/StringBuilder","setCharAt","(IC)V")&&constant(p.get(264),3)&&constant(p.get(265),'0')&&Prepare.call(p.get(267),"java/lang/StringBuilder","setCharAt","(IC)V")&&Prepare.call(p.get(284),"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")&&constant(p.get(288),0)&&p.get(289).opcode()==Opcode.IRETURN,"native activation result bank and false continuation "+side);
            require(Prepare.instructions(method).stream().noneMatch(op->op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&FmcReconstructionControlPrepare.FIELDS.contains(f.name().stringValue())&&f.owner().asInternalName().equals("FmcRte")),"activation bank producer cannot directly propagate scalar state "+side);
            var run=data.requireMethod("FmcRte$Exec"+side,"run","()V");var rp=FmcReconstructionTransitionEffects.positions(run);var rc=(CodeAttribute)run.code().orElseThrow();
            require(constant(rp.get(44),2)&&Prepare.call(rp.get(45),"FmcRte","setModInact"+side,"(I)Z")&&branch(rp.get(48),Opcode.IFEQ,59,rc)&&constant(rp.get(51),2)&&Prepare.call(rp.get(52),"FmcRte","setModAct","(I)Z")&&Prepare.call(rp.get(59),"FmcVnav","startCduRefreshTimer","()V")&&Prepare.call(rp.get(62),"FmcRte","makeLnavRecaptReq","()V")&&rp.get(65).opcode()==Opcode.RETURN,"one original EXEC activation continuation "+side);
        }
        var act=data.requireMethod("FmcRte","setModAct","(I)Z");var p=FmcReconstructionTransitionEffects.positions(act);var code=(CodeAttribute)act.code().orElseThrow();
        require(constant(p.get(959),0)&&constant(p.get(963),'0')&&branch(p.get(965),Opcode.IF_ICMPNE,981,code)&&constant(p.get(969),0)&&constant(p.get(971),2)&&Prepare.call(p.get(972),"java/lang/StringBuilder","charAt","(I)C")&&Prepare.call(p.get(975),"java/lang/StringBuilder","setCharAt","(IC)V"),"MOD replaces zero left view with active route");
        require(constant(p.get(982),1)&&constant(p.get(986),'0')&&branch(p.get(988),Opcode.IF_ICMPNE,1004,code)&&constant(p.get(992),1)&&constant(p.get(994),2)&&Prepare.call(p.get(995),"java/lang/StringBuilder","charAt","(I)C")&&Prepare.call(p.get(998),"java/lang/StringBuilder","setCharAt","(IC)V")&&Prepare.call(p.get(1692),"Qs","transfer","(Ljava/lang/String;CI)Ljava/lang/String;")&&Prepare.call(p.get(1696),"FmcRte","runModerato","()V"),"MOD native control result and immediate propagation");
        var reader=FmcReconstructionTransitionEffects.positions(data.requireMethod("FmcRte","readGlobViewAct","()V"));require(Prepare.field(reader.get(33),Opcode.PUTSTATIC,"FmcRte","rteActive")&&Prepare.field(reader.get(115),Opcode.PUTSTATIC,"FmcRte","rteToBeAct")&&Prepare.call(reader.get(194),"FmcRte","filterNavSourcesAndPages","()V")&&Prepare.call(reader.get(325),"FmcVnav","copyTransitions","(I)V")&&Prepare.call(reader.get(334),"Fans","copyEmergArptFromRte","()V"),"ordinary bank propagation and NEW route guidance caches");
    }
    /** Complete original propagation, including its handlers and derived
     * takeoff/transition/emergency/CDU effects. The calling worker must already
     * have proved FmcRte initialized and the original-produced bank result.
     * All callees and constructed native jobs remain in ordinary EXEC's
     * existing mutation/lifecycle and pre-start ownership authority closure. */
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        qualify(data);var baseline=FmcExecMutationPrepare.profile(data);require(baseline.methods().contains(ORIGINAL),"bank reader already belongs to selected ordinary EXEC authority");
        byte[] input=lower.getOrDefault("FmcRte",data.inputs.get("FmcRte"));var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"complete propagation bridge collision");var original=data.methods.get(ORIGINAL);int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){b.withMethod(NAME,original.methodTypeSymbol(),flags,m->m.transformCode(original.code().orElseThrow(),(c,e)->c.with(e)));}});
        require(data.cf.verify(output).isEmpty(),"complete native propagation verifier");var clone=data.cf.parse(output).methods().stream().filter(m->m.methodName().equalsString(NAME)).findFirst().orElseThrow();var before=(CodeAttribute)original.code().orElseThrow();var after=(CodeAttribute)clone.code().orElseThrow();
        require(Arrays.equals(canonical(data,original),canonical(data,clone)),"complete original propagation instructions/handlers unchanged");
        var changed=new HashMap<>(lower);changed.put("FmcRte",output);var legalBefore=new HashMap<>(data.inputs);legalBefore.putAll(lower);legalBefore.put("FmcRte",input);var legalAfter=new HashMap<>(legalBefore);legalAfter.putAll(changed);TerrainAccess.verifyAdded(legalBefore,legalAfter,data.inputs);var expanded=FmcExecMutationPrepare.profile(data,changed,roots());var added=new HashSet<>(expanded.methods());added.removeAll(baseline.methods());require(added.equals(roots()),"propagation adds no unqualified native continuation/job");return Map.copyOf(changed);
    }
    private static byte[] canonical(Prepare data,MethodModel method){return data.cf.build(java.lang.constant.ClassDesc.of("FmcRte"),b->b.withMethod(NAME,method.methodTypeSymbol(),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e))));}
    /** Preflight predicts only the original EXEC-produced control bank. The
     * reserved fifth byte and native other-side route choice remain current. */
    static FmcNarrowControl.State expected(FmcNarrowControl.Execution execution,int newCount){
        require(execution!=null&&newCount>=3&&newCount<=253,"NEW body/count");var before=execution.beforeExec();FmcNarrowControl.validate(before);char[] bank=before.bank().toCharArray();
        if(before.active()==0){require(newCount>=4,"native activation requires two entered legs");int side=execution.logicalSide(),target=execution.replacement().target();require(target==1&&before.pending()==target&&(side==0?before.viewLeft():before.viewRight())==target,"trusted pending activation");bank[2]=bank[3];bank[3]='0';}
        else{require(before.active()==execution.replacement().target()&&before.pending()==0&&(execution.logicalSide()==0?before.viewLeft():before.viewRight())==0,"trusted active MOD execution");for(int i=0;i<2;i++)if(bank[i]=='0')bank[i]=bank[2];}
        var after=new FmcNarrowControl.State(bank[2]-'0',bank[0]-'0',bank[1]-'0',bank[3]-'0',new String(bank));FmcNarrowControl.validate(after);return after;
    }
}
