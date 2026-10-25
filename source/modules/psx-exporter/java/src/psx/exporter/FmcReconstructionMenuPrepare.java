package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Invalidates the shared transition-menu view after loading NEW selections.
 * Native reset hides old rows using zero lengths/show flags. It does not load
 * procedures, edit entered routes, replace temp arrays or consume banks. */
final class FmcReconstructionMenuPrepare {
    static final String RESET="$psxExporter$fmcHeaderMenuReset",ALIASES="$psxExporter$fmcHeaderMenuAliases",LINKS="$psxExporter$fmcHeaderMenuLinks";
    static final String DEP="FmcDepArr$DepTransPack",ARR="FmcDepArr$ArrTransPack",H="FmcRte$Rte";
    static final List<String> GLOBALS=List.of("depTransPk1","depTransPk2","arrTransPk1","arrTransPk2");
    static Set<FmcQualification.Member> writes(){var result=new HashSet<FmcQualification.Member>();
        for(String field:List.of("apt","selSid","selTra","selRwy"))result.add(new FmcQualification.Member(DEP,field,"Ljava/lang/String;"));for(String field:List.of("selSidDone","selTraDone","selRwyDone","showSidTransPack"))result.add(new FmcQualification.Member(DEP,field,"Z"));for(String field:List.of("transStartIdx","arrayLenSidAndTrans","arrayLenRwys","maxLen","maxPage"))result.add(new FmcQualification.Member(DEP,field,"I"));
        for(String field:List.of("apt","selStar","selStarTra","selApp","selAppTra"))result.add(new FmcQualification.Member(ARR,field,"Ljava/lang/String;"));for(String field:List.of("selStarDone","selStarTraDone","selAppDone","selAppTraDone","showArrTransPack","hasVfrFa"))result.add(new FmcQualification.Member(ARR,field,"Z"));for(String field:List.of("starTraStartIdx","appTraStartIdx","arrayLenStarAndTrans","arrayLenAppAndTrans","maxLen","maxPage","rwyExtNm10","rwyExtFpaDeg10"))result.add(new FmcQualification.Member(ARR,field,"I"));
        return Set.copyOf(result);
    }
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId(H,RESET,"(Ljava/lang/Object;)V"));}
    static Set<FmcQualification.Member> inputs(){var result=new HashSet<FmcQualification.Member>();for(int i=0;i<4;i++)result.add(new FmcQualification.Member("FmcDepArr",GLOBALS.get(i),ClassDesc.of(i<2?DEP:ARR).descriptorString()));result.add(new FmcQualification.Member(H,"rteId","I"));result.add(new FmcQualification.Member(H,"sidTransPackLnk",ClassDesc.of(DEP).descriptorString()));result.add(new FmcQualification.Member(H,"arrTransPackLnk",ClassDesc.of(ARR).descriptorString()));return Set.copyOf(result);}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC transition menu "+why);}
    private static void qualifyLinks(Prepare data){
        var startup=new Prepare.MethodId("FmcRte","postCduInit","()V");var expected=Map.of(98,"rte0:depTransPk1:sidTransPackLnk",107,"rte1:depTransPk1:sidTransPackLnk",116,"rte2:depTransPk2:sidTransPackLnk",143,"rte0:arrTransPk1:arrTransPackLnk",152,"rte1:arrTransPk1:arrTransPackLnk",161,"rte2:arrTransPk2:arrTransPackLnk");var found=new HashMap<Integer,String>();
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){var code=Prepare.instructions(entry.getValue());int bci=0;for(int i=0;i<code.size();i++){var op=code.get(i);if(op instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals(H)&&Set.of("sidTransPackLnk","arrTransPackLnk").contains(f.name().stringValue())){require(entry.getKey().equals(startup)&&i>=2&&code.get(i-2) instanceof FieldInstruction route&&route.opcode()==Opcode.GETSTATIC&&route.owner().asInternalName().equals("FmcRte")&&code.get(i-1) instanceof FieldInstruction menu&&menu.opcode()==Opcode.GETSTATIC&&menu.owner().asInternalName().equals("FmcDepArr"),"sole native startup link stores");var route=(FieldInstruction)code.get(i-2);var menu=(FieldInstruction)code.get(i-1);require(found.put(bci,route.name().stringValue()+":"+menu.name().stringValue()+":"+f.name().stringValue())==null,"unique startup link site");}bci+=op.sizeInBytes();}}
        require(found.equals(expected),"complete RTE0/RTE1 shared and RTE2 transition-cache mapping");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        qualifyLinks(data);
        for(String type:List.of(DEP,ARR)){var nativeReset=data.methods.get(new Prepare.MethodId(type,"reset","()V"));require(nativeReset!=null&&nativeReset.code().isPresent()&&nativeReset.code().get().exceptionHandlers().isEmpty(),"pure native reset");var actual=new HashSet<FmcQualification.Member>();for(var op:Prepare.instructions(nativeReset)){
            if(op instanceof FieldInstruction f){require(f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals(type),"only own derived scalar writes");actual.add(new FmcQualification.Member(type,f.name().stringValue(),f.type().stringValue()));}
            else require(op instanceof LoadInstruction||op instanceof ConstantInstruction||op instanceof ReturnInstruction,"no cache-loader/helper/array effect");
        }require(actual.equals(writes().stream().filter(f->f.owner().equals(type)).collect(java.util.stream.Collectors.toSet())),"complete exact native reset footprint");}
        byte[] input=lower.getOrDefault(H,data.inputs.get(H));var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(RESET,ALIASES,LINKS).contains(m.methodName().stringValue())),"entry collision");ClassDesc h=ClassDesc.of(H),menus=ClassDesc.of("FmcDepArr");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            b.withMethodBody(ALIASES,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.iconst_4().anewarray(CD_Object).astore(0);for(int i=0;i<4;i++)c.aload(0).loadConstant(i).getstatic(menus,GLOBALS.get(i),ClassDesc.of(i<2?DEP:ARR)).aastore();c.aload(0).areturn();});
            b.withMethodBody(LINKS,MethodTypeDesc.of(CD_boolean,CD_Object),flags,c->{var second=c.newLabel();var bad=c.newLabel();c.aload(0).checkcast(h).astore(1).aload(1).getfield(h,"rteId",CD_int).istore(2).iload(2).iflt(bad).iload(2).iconst_2().if_icmpgt(bad).iload(2).iconst_2().if_icmpeq(second);for(int i:new int[]{0,2})c.aload(1).getfield(h,i==0?"sidTransPackLnk":"arrTransPackLnk",ClassDesc.of(i==0?DEP:ARR)).getstatic(menus,GLOBALS.get(i),ClassDesc.of(i==0?DEP:ARR)).if_acmpne(bad);c.iconst_1().ireturn().labelBinding(second);for(int i:new int[]{1,3})c.aload(1).getfield(h,i==1?"sidTransPackLnk":"arrTransPackLnk",ClassDesc.of(i==1?DEP:ARR)).getstatic(menus,GLOBALS.get(i),ClassDesc.of(i==1?DEP:ARR)).if_acmpne(bad);c.iconst_1().ireturn().labelBinding(bad).iconst_0().ireturn();});
            b.withMethodBody(RESET,MethodTypeDesc.of(CD_void,CD_Object),flags,c->{c.aload(0).invokestatic(h,LINKS,MethodTypeDesc.of(CD_boolean,CD_Object));var bad=c.newLabel();c.ifeq(bad);for(int i=0;i<4;i++){var type=ClassDesc.of(i<2?DEP:ARR);c.getstatic(menus,GLOBALS.get(i),type).invokevirtual(type,"reset",MethodTypeDesc.of(CD_void));}c.return_().labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalStateException")).dup().loadConstant("FMC transition menu invariant links changed").invokespecial(ClassDesc.of("java.lang.IllegalStateException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();});
        }});require(data.cf.verify(output).isEmpty(),"final verifier");var changed=new HashMap<>(lower);changed.put(H,output);var before=new HashMap<>(lower);before.put(H,input);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
