package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Direct PERF INIT load and authoritative banks. The current ballast/manual
 * fuel control is read for native bank encoding and is never assigned. Sensed
 * fuel/history are absent from the write set; current gross mass uses the
 * separately qualified native current-fuel addition. No broad readGlobAll,
 * datalink accept, performance initializer or input freshness read is invoked.
 * All bank receivers/limits need worker admission before this ordinary-thread
 * entry, and the complete generated call tree is a mutation-guard root. */
final class FmcReconstructionPerfPrepare {
    static final String NAME="$psxExporter$fmcPublishPerfInit";
    static final String BANKS="$psxExporter$fmcPerfInitBanks";
    static final String BALLAST="$psxExporter$fmcCurrentBallast";
    static String bank(long[] inputs,int ballast){
        require(inputs!=null&&inputs.length==11,"complete private input vector");float zfw=Float.intBitsToFloat((int)inputs[0]);
        require(Float.isFinite(zfw)&&zfw==(float)Math.round(zfw)&&Float.floatToRawIntBits(zfw)!=0x80000000,"native integral zero-fuel mass");
        int reserves=(int)inputs[1],costIndex=(int)inputs[2],cruiseAltitude=(int)inputs[3],cruiseCg=(int)inputs[4];
        require(Math.round((float)reserves)==reserves,"native reserves int/float bank roundtrip");
        require(costIndex==-1||costIndex>=0&&costIndex<=9999,"native cost index");
        require(cruiseAltitude==-9999||cruiseAltitude>=-1005&&cruiseAltitude<=45100,"native cruise altitude feet");
        require(cruiseCg>=85&&cruiseCg<=330&&(inputs[10]!=0||cruiseCg==200),"native entered/unset cruise CG");
        int step=(int)inputs[5];require(step==-1||step>=0&&step<=9000&&step%1000==0,"native single-digit legacy STEP SIZE");
        return "0a"+Math.round(zfw)+'b'+(int)inputs[1]+'c'+(int)inputs[2]+'d'+(int)inputs[3]+'e'+ballast+'f'+(int)inputs[4]+'g'
            +(step<0?'-':(char)('0'+step/1000))+'-'+(inputs[8]==0?'i':'I')+(inputs[9]==0?'j':'J')+(inputs[10]==0?'k':'K')+'l';
    }
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("FmcPerf",NAME,"([J)V"));}
    static Set<FmcQualification.Member> writes(){var result=new HashSet<FmcQualification.Member>(FmcBankWriterPrepare.FIELDS);for(var f:FmcPerfInputsPrepare.INPUTS)result.add(new FmcQualification.Member(f.owner(),f.name(),f.descriptor()));
        result.add(new FmcQualification.Member("FmcPerf","grWeight","F"));result.add(new FmcQualification.Member("FmcPerf","fuelReservesInUse","I"));result.add(new FmcQualification.Member("FmcPerf","costIndexFac","F"));result.add(new FmcQualification.Member("FmcPerf","crzCgAltAdd","I"));return Set.copyOf(result);}
    private FmcReconstructionPerfPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction PERF "+why);}
    private static final ClassDesc P=ClassDesc.of("FmcPerf"),G=ClassDesc.of("Glob"),QS=ClassDesc.of("Qs"),QI=ClassDesc.of("Qi"),SB=ClassDesc.of("java.lang.StringBuilder");
    private static void field(Map<String,ClassModel> models,String owner,String name,String type,boolean stat){
        var m=models.get(owner);require(m!=null,"field owner "+owner);var fields=m.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
        require(fields.size()==1&&fields.getFirst().fieldType().equalsString(type)&&((fields.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==stat,"field binding "+owner+"."+name);
    }
    private static void nativeBank(Prepare data,String method,String bank,char kind){
        var m=data.methods.get(new Prepare.MethodId("FmcPerf",method,"()V"));require(m!=null&&m.code().isPresent(),"native bank consumer "+method);
        require(Prepare.instructions(m).stream().anyMatch(i->i instanceof FieldInstruction f&&f.owner().asInternalName().equals("Glob")&&f.name().equalsString(bank)&&f.type().equalsString("I")),"native bank index "+bank);
        require(Prepare.instructions(m).stream().anyMatch(i->i instanceof InvokeInstruction call&&call.owner().asInternalName().equals(kind=='s'?"Qs":"Qi")&&call.name().equalsString("transfer")),"native typed bank consumer");
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);for(var input:FmcPerfInputsPrepare.INPUTS)field(models,input.owner(),input.name(),input.descriptor(),true);
        field(models,"FmcPerf","ballast","I",true);field(models,"Glob","qs","[LQs;",true);field(models,"Glob","qi","[LQi;",true);
        for(String bank:List.of("qsFmcPerfInit","qiStepSizeNg","qiMinFuelTemp"))field(models,"Glob",bank,"I",true);
        nativeBank(data,"readGlobAll","qsFmcPerfInit",'s');nativeBank(data,"readStepSizeNg","qiStepSizeNg",'i');nativeBank(data,"runLento","qiMinFuelTemp",'i');
        var changed=new HashMap<>(FmcContentReconcilePrepare.transform(data,lower));changed=new HashMap<>(FmcCostIndexReconcilePrepare.transform(data,changed));changed=new HashMap<>(FmcPerfDerivedPrepare.transform(data,changed));
        byte[] original=changed.get("FmcPerf");var model=data.cf.parse(original);require(model.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)||m.methodName().equalsString(BANKS)),"entry collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(BALLAST,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->c.getstatic(P,"ballast",CD_int).ireturn());
                b.withMethodBody(BANKS,MethodTypeDesc.of(CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.iconst_3().anewarray(CD_Object).astore(0);
                    c.aload(0).iconst_0().getstatic(G,"qs",QS.arrayType()).getstatic(G,"qsFmcPerfInit",CD_int).aaload().aastore();
                    for(int i=0;i<2;i++)c.aload(0).loadConstant(i+1).getstatic(G,"qi",QI.arrayType()).getstatic(G,i==0?"qiStepSizeNg":"qiMinFuelTemp",CD_int).aaload().aastore();
                    c.aload(0).areturn();
                });
                b.withMethodBody(NAME,MethodTypeDesc.of(CD_void,CD_long.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();c.aload(0).arraylength().loadConstant(11).if_icmpne(bad);
                    for(int i=0;i<11;i++){var input=FmcPerfInputsPrepare.INPUTS.get(i);c.aload(0).loadConstant(i).laload().l2i();
                        if(input.descriptor().equals("F"))c.invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));
                        c.putstatic(P,input.name(),ClassDesc.ofDescriptor(input.descriptor()));
                    }
                    c.invokestatic(P,FmcContentReconcilePrepare.BRIDGE,MethodTypeDesc.of(CD_void))
                        .invokestatic(P,FmcCostIndexReconcilePrepare.BRIDGE,MethodTypeDesc.of(CD_void))
                        .invokestatic(P,FmcPerfDerivedPrepare.BRIDGE,MethodTypeDesc.of(CD_void));
                    c.new_(SB).dup().loadConstant("0a").invokespecial(SB,"<init>",MethodTypeDesc.of(CD_void,CD_String)).astore(1);
                    c.aload(1).getstatic(P,"zfWeight",CD_float).invokestatic(ClassDesc.of("java.lang.Math"),"round",MethodTypeDesc.of(CD_int,CD_float))
                        .invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_int)).loadConstant((int)'b').invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).pop();
                    for(var item:List.of(Map.entry("fuelReserves",'c'),Map.entry("costIndex",'d'),Map.entry("crzAlt",'e'),Map.entry("ballast",'f'),Map.entry("crzCg",'g')))
                        c.aload(1).getstatic(P,item.getKey(),CD_int).invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_int)).loadConstant((int)item.getValue()).invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).pop();
                    // Native legacy STEP SIZE consumes the first character
                    // after g in thousands of feet; the second is reserved.
                    var nonnegative=c.newLabel();var stepDone=c.newLabel();c.aload(1).getstatic(P,"stepSize",CD_int).ifge(nonnegative).loadConstant((int)'-').goto_(stepDone)
                        .labelBinding(nonnegative).getstatic(P,"stepSize",CD_int).loadConstant(1000).idiv().loadConstant((int)'0').iadd().labelBinding(stepDone)
                        .invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).loadConstant((int)'-').invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).pop();
                    for(var item:List.of(Map.entry("grWtBig",'i'),Map.entry("zfWtBig",'j'),Map.entry("cgBig",'k'))){var lowerCase=c.newLabel();var done=c.newLabel();
                        c.aload(1).getstatic(P,item.getKey(),CD_boolean).ifeq(lowerCase).loadConstant((int)Character.toUpperCase(item.getValue())).goto_(done).labelBinding(lowerCase).loadConstant((int)item.getValue()).labelBinding(done)
                            .invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).pop();
                    }
                    c.aload(1).loadConstant((int)'l').invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_char)).pop();
                    c.getstatic(G,"qs",QS.arrayType()).getstatic(G,"qsFmcPerfInit",CD_int).aaload().aload(1).invokevirtual(SB,"toString",MethodTypeDesc.of(CD_String)).loadConstant((int)'w').iconst_1()
                        .invokevirtual(QS,"transfer",MethodTypeDesc.of(CD_String,CD_String,CD_char,CD_int)).pop();
                    for(var item:List.of(Map.entry("qiStepSizeNg","stepSizeNg"),Map.entry("qiMinFuelTemp","minFuelTemp")))c.getstatic(G,"qi",QI.arrayType()).getstatic(G,item.getKey(),CD_int).aaload()
                        .getstatic(P,item.getValue(),CD_int).loadConstant((int)'w').iconst_1().invokevirtual(QI,"transfer",MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int)).pop();
                    c.return_();c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC reconstruction complete PERF inputs")
                        .invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
                });
            }
        });require(data.cf.verify(output).isEmpty(),"generated verifier");var before=new HashMap<>(changed);changed.put("FmcPerf",output);TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
