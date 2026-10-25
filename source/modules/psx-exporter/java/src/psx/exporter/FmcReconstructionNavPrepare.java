package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Current manual tuning is never artifact content. Native EXEC's navigation
 * helpers publish only qsNavRadSet plus three derived radio flags; the excluded
 * runModerato consumer cannot apply intermediate modes to live receivers.
 * The current IRS-reference bank is also retained because native header EXEC
 * can otherwise publish the new origin as an alignment reference. */
final class FmcReconstructionNavPrepare {
    static final String SNAP="$psxExporter$fmcCurrentRadioState",RESTORE="$psxExporter$fmcRestoreCurrentRadioState",BANK_SNAP="$psxExporter$fmcRadioBankState",BANK_RESTORE="$psxExporter$fmcRestoreRadioBankState",IRS_MONITOR="$psxExporter$fmcCurrentIrsMonitor";
    static final List<String> FLAGS=List.of("navInvalidTune","parkIls","rwyIlsCompatible");
    static final List<String> RADIOS=List.of("ilsL","ilsC","ilsR","vorL","vorR");
    static Set<FmcQualification.Member> writes(){var s=new HashSet<>(FmcReconstructionBanks.writes());s.add(new FmcQualification.Member("Qs","sVar","Ljava/lang/String;"));s.add(new FmcQualification.Member("Qs","lastComeFrom","I"));for(String f:FLAGS)s.add(new FmcQualification.Member("NavRadios",f,"Z"));return Set.copyOf(s);}
    static Set<FmcQualification.Member> inputs(){return Set.of(new FmcQualification.Member("NavRadios$Vhf","freq","I"),new FmcQualification.Member("NavRadios$Vhf","tuneMode","C"),new FmcQualification.Member("NavRadios$Vhf","crsDegSelected","I"),new FmcQualification.Member("FmcMaintenance","strIrsMonRef","Ljava/lang/String;"));}
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("NavRadios",RESTORE,"([J)V"),new Prepare.MethodId("Qs",BANK_RESTORE,"(Ljava/lang/Object;Ljava/lang/String;[J)V"));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC current navigation "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){var changed=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String owner:List.of("NavRadios","Qs","FmcMaintenance")){byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(SNAP,RESTORE,BANK_SNAP,BANK_RESTORE,IRS_MONITOR).contains(m.methodName().stringValue())),"entry collision");var type=ClassDesc.of(owner);byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            if(owner.equals("FmcMaintenance")){
                b.withMethodBody(IRS_MONITOR,MethodTypeDesc.of(CD_String),flags,c->c.getstatic(type,"strIrsMonRef",CD_String).areturn());
            }else if(owner.equals("Qs")){
                b.withMethodBody(BANK_SNAP,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->{c.aload(0).checkcast(type).astore(1);c.iconst_2().anewarray(CD_Object).astore(2);c.aload(2).iconst_0().aload(1).getfield(type,"sVar",CD_String).aastore();c.iconst_3().newarray(TypeKind.LONG).astore(3);for(int i=0;i<3;i++)c.aload(3).loadConstant(i).aload(1).getfield(type,List.of("fresh","updated","lastComeFrom").get(i),i==2?CD_int:CD_boolean).i2l().lastore();c.aload(2).iconst_1().aload(3).aastore().aload(2).areturn();});
                b.withMethodBody(BANK_RESTORE,MethodTypeDesc.of(CD_void,CD_Object,CD_String,CD_long.arrayType()),flags,c->{c.aload(0).checkcast(type).astore(3);c.aload(3).aload(1).putfield(type,"sVar",CD_String);for(int i=0;i<3;i++)c.aload(3).aload(2).loadConstant(i).laload().l2i().putfield(type,List.of("fresh","updated","lastComeFrom").get(i),i==2?CD_int:CD_boolean);c.return_();});
            }else{
                b.withMethodBody(SNAP,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{var qs=ClassDesc.of("Qs");var g=ClassDesc.of("Glob");var vhf=ClassDesc.of("NavRadios$Vhf");c.bipush(6).anewarray(CD_Object).astore(0);c.aload(0).iconst_0().getstatic(g,"qs",qs.arrayType()).getstatic(g,"qsNavRadSet",CD_int).aaload().aastore();c.aload(0).iconst_4().getstatic(g,"qs",qs.arrayType()).getstatic(g,"qsIrsMonRef",CD_int).aaload().aastore();c.aload(0).iconst_5().invokestatic(ClassDesc.of("FmcMaintenance"),IRS_MONITOR,MethodTypeDesc.of(CD_String)).aastore();c.iconst_3().newarray(TypeKind.LONG).astore(1);for(int i=0;i<3;i++)c.aload(1).loadConstant(i).getstatic(type,FLAGS.get(i),CD_boolean).i2l().lastore();c.aload(0).iconst_1().aload(1).aastore();c.iconst_5().anewarray(CD_Object).astore(2);c.loadConstant(15).newarray(TypeKind.LONG).astore(3);
                    for(int i=0;i<5;i++){var r=ClassDesc.of(i<3?"NavRadios$Ils":"NavRadios$Vor");c.aload(2).loadConstant(i).getstatic(type,RADIOS.get(i),r).aastore();for(int j=0;j<3;j++)c.aload(3).loadConstant(i*3+j).getstatic(type,RADIOS.get(i),r).getfield(vhf,List.of("freq","tuneMode","crsDegSelected").get(j),j==1?CD_char:CD_int).i2l().lastore();}c.aload(0).iconst_2().aload(2).aastore().aload(0).iconst_3().aload(3).aastore().aload(0).areturn();});
                b.withMethodBody(RESTORE,MethodTypeDesc.of(CD_void,CD_long.arrayType()),flags,c->{for(int i=0;i<3;i++)c.aload(0).loadConstant(i).laload().l2i().putstatic(type,FLAGS.get(i),CD_boolean);c.return_();});
            }
        }});require(data.cf.verify(output).isEmpty(),"final verifier "+owner);changed.put(owner,output);}
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
