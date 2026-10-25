package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Direct native view/active/pending publication. Current EIU navigation-source
 * selections are read by the unchanged native title/pointer derivation; none
 * is assigned. The native UI refresh tail is omitted until content is coherent. */
final class FmcReconstructionControlPrepare {
    static final String NAME="$psxExporter$fmcPublishControl",FILTER="$psxExporter$fmcControlPointers",BANK="$psxExporter$fmcControlBank",STATE="$psxExporter$fmcControlState",POINTERS="$psxExporter$fmcControlPointersState",CHOICES="$psxExporter$fmcControlChoices";
    static final List<String> FIELDS=List.of("rteInViewL","rteInViewR","rteActive","rteToBeAct");
    record Qualified(Map<String,byte[]> changed,Set<FmcQualification.Member> writes,Set<FmcQualification.Member> currentInputs){}
    private FmcReconstructionControlPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction control "+why);}
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("FmcRte",NAME,"([ILjava/lang/String;)V"));}
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var model=FmcQualification.finalModels(data,lower).get("FmcRte");require(model!=null&&model.methods().stream().noneMatch(m->Set.of(NAME,FILTER,BANK,STATE,POINTERS,CHOICES).contains(m.methodName().stringValue())),"native owner/collision");
        var nativeFilter=data.methods.get(new Prepare.MethodId("FmcRte","filterNavSourcesAndPages","()V"));require(nativeFilter!=null&&nativeFilter.code().isPresent(),"native pointer derivation");
        require(Prepare.instructions(nativeFilter).stream().filter(i->i instanceof InvokeInstruction call&&call.owner().asInternalName().equals("FmcRte")&&call.name().equalsString("refreshRteCdus")).count()==1,"single foreign UI refresh tail");
        var members=new HashSet<FmcQualification.Member>();var reads=new HashSet<FmcQualification.Member>();
        for(String field:FIELDS)members.add(new FmcQualification.Member("FmcRte",field,"I"));members.addAll(FmcBankWriterPrepare.FIELDS);
        for(var id:List.of(new Prepare.MethodId("FmcRte","filterNavSourcesAndPages","()V"),new Prepare.MethodId("FmcRte$Rte","setTitlesAnd6bCduL","()V"),new Prepare.MethodId("FmcRte$Rte","setTitlesAnd6bCduR","()V"),new Prepare.MethodId("Routes","setActRteBody","()V"))){
            var m=data.methods.get(id);require(m!=null&&m.code().isPresent(),"exact finite derivation "+id);
            for(var op:Prepare.instructions(m))if(op instanceof FieldInstruction f){var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());require(member!=null,"member derivation");if(op.opcode()==Opcode.PUTFIELD||op.opcode()==Opcode.PUTSTATIC)members.add(member);else reads.add(member);}
        }
        require(members.stream().noneMatch(f->f.owner().equals("EiuSystem")),"navigation choices preserved");
        ClassDesc owner=ClassDesc.of("FmcRte"),g=ClassDesc.of("Glob"),qs=ClassDesc.of("Qs");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            b.withMethod(FILTER,MethodTypeDesc.of(CD_void),flags,m->m.transformCode(nativeFilter.code().orElseThrow(),(c,e)->{if(e instanceof InvokeInstruction call&&call.owner().asInternalName().equals("FmcRte")&&call.name().equalsString("refreshRteCdus"))return;c.with(e);}));
            b.withMethodBody(POINTERS,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.loadConstant(18).anewarray(CD_Object).astore(0);int slot=0;for(String field:List.of("rte0","rte1","rte2","rteL","rteR","rteCduL","rteCduR"))c.aload(0).loadConstant(slot++).getstatic(owner,field,ClassDesc.of("FmcRte$Rte")).aastore();for(String field:List.of("rteBody1","rteBody2","actBody"))c.aload(0).loadConstant(slot++).getstatic(ClassDesc.of("Routes"),field,ClassDesc.of("Routes$RteBody")).aastore();for(String field:List.of("sbRteTitleL","sbRteTitleR","sbLegsTitleL","sbLegsTitleR","sbRteTitleCduL","sbRteTitleCduR","sbLegsTitleCduL","sbLegsTitleCduR"))c.aload(0).loadConstant(slot++).getstatic(owner,field,ClassDesc.of("java.lang.StringBuilder")).aastore();c.aload(0).areturn();});
            b.withMethodBody(CHOICES,MethodTypeDesc.of(CD_int.arrayType()),flags,c->c.iconst_3().newarray(TypeKind.INT).dup().iconst_0().getstatic(ClassDesc.of("EiuSystem"),"navSelectedCp",CD_int).iastore().dup().iconst_1().getstatic(ClassDesc.of("EiuSystem"),"navSelectedFo",CD_int).iastore().dup().iconst_2().getstatic(ClassDesc.of("Routes"),"anyActiveRte",CD_boolean).iastore().areturn());            b.withMethodBody(BANK,MethodTypeDesc.of(CD_Object),flags,c->c.getstatic(g,"qs",qs.arrayType()).getstatic(g,"qsFmcRteViAcMo",CD_int).aaload().areturn());
            b.withMethodBody(STATE,MethodTypeDesc.of(CD_int.arrayType()),flags,c->{c.iconst_4().newarray(TypeKind.INT).astore(0);for(int i=0;i<4;i++)c.aload(0).loadConstant(i).getstatic(owner,FIELDS.get(i),CD_int).iastore();c.aload(0).areturn();});
            b.withMethodBody(NAME,MethodTypeDesc.of(CD_void,CD_int.arrayType(),CD_String),flags,c->{var bad=c.newLabel();c.aload(0).arraylength().iconst_4().if_icmpne(bad).aload(1).invokevirtual(CD_String,"length",MethodTypeDesc.of(CD_int)).iconst_5().if_icmpne(bad);
                for(int i=0;i<4;i++){c.aload(0).loadConstant(i).iaload().iflt(bad).aload(0).loadConstant(i).iaload().iconst_2().if_icmpgt(bad);c.aload(1).loadConstant(i).invokevirtual(CD_String,"charAt",MethodTypeDesc.of(CD_char,CD_int)).aload(0).loadConstant(i).iaload().loadConstant((int)'0').iadd().if_icmpne(bad);}
                for(int i=0;i<4;i++)c.aload(0).loadConstant(i).iaload().putstatic(owner,FIELDS.get(i),CD_int);
                c.getstatic(g,"qs",qs.arrayType()).getstatic(g,"qsFmcRteViAcMo",CD_int).aaload().aload(1).loadConstant((int)'w').iconst_1().invokevirtual(qs,"transfer",MethodTypeDesc.of(CD_String,CD_String,CD_char,CD_int)).pop().invokestatic(owner,FILTER,MethodTypeDesc.of(CD_void)).return_();
                c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC validated native control vector").invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();});
        }});require(data.cf.verify(output).isEmpty(),"generated verifier");var changed=new HashMap<>(lower);changed.put("FmcRte",output);var before=new HashMap<>(lower);before.put("FmcRte",lower.getOrDefault("FmcRte",data.inputs.get("FmcRte")));TerrainAccess.verifyAdded(before,changed,data.inputs);return new Qualified(Map.copyOf(changed),Set.copyOf(members),Set.copyOf(reads));
    }
}
