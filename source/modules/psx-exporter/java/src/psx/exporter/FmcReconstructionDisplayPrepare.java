package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Current LCD glyph banks are selected by native screen-line constructors.
 * Freezing their finite setUpdate consumer retains the cached LCD rendering;
 * scratchpad rendering and the complete graphics frame remain ordinary. */
final class FmcReconstructionDisplayPrepare {
    static final String UNITS="$psxExporter$fmcDisplayUnits",LINES="$psxExporter$fmcDisplayLines",BANKS="$psxExporter$fmcDisplayBanks",PAGES="$psxExporter$fmcDisplayPages";
    static final Set<FmcQualification.Member> PAGE_INPUTS=Set.of(new FmcQualification.Member("ControlDisplayUnit","page","LCduHub$Page;"),new FmcQualification.Member("CduHub$Page","subsys","I"),new FmcQualification.Member("CduHub$Page","qiIdVal","I"));
    static final List<String> UNITS_FIELDS=List.of("cduL","cduR","cduC");
    static final List<String> LINE_FIELDS=List.of("scrTt","scr1s","scr1b","scr2s","scr2b","scr3s","scr3b","scr4s","scr4b","scr5s","scr5b","scr6s","scr6b");
    static Set<FmcQualification.Member> fields(){var s=new HashSet<FmcQualification.Member>();for(String f:UNITS_FIELDS)s.add(new FmcQualification.Member("Core",f,"LControlDisplayUnit;"));for(String f:LINE_FIELDS)s.add(new FmcQualification.Member("ControlDisplayUnit",f,"LControlDisplayUnit$ScreenLine;"));for(String f:List.of("gidx","gci"))s.add(new FmcQualification.Member("ControlDisplayUnit$ScreenLine",f,"I"));return Set.copyOf(s);}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC LCD bank roots "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        for(var field:PAGE_INPUTS)require(field.equals(FmcQualification.resolveField(data.models,field.owner(),field.name(),field.descriptor(),new HashSet<>())),"exact current page member");FmcBackgroundAdmissionProjection.qualify(data,lower);
        for(var f:fields()){
            require(f.equals(FmcQualification.resolveField(data.models,f.owner(),f.name(),f.descriptor(),new HashSet<>())),"exact native member "+f);
            int writes=0;for(var e:data.methods.entrySet())if(e.getValue().code().isPresent()){var ops=Prepare.instructions(e.getValue());for(int at=0;at<ops.size();at++)if(ops.get(at) instanceof FieldInstruction x&&(x.opcode()==Opcode.PUTFIELD||x.opcode()==Opcode.PUTSTATIC)&&f.equals(FmcQualification.resolveField(data.models,x.owner().asInternalName(),x.name().stringValue(),x.type().stringValue(),new HashSet<>()))){writes++;
                boolean startup=f.owner().equals("Core")&&e.getKey().equals(new Prepare.MethodId("Core","startAll","(Ljava/lang/String;)V"))&&at>0&&ops.get(at-1) instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESPECIAL&&call.owner().asInternalName().equals("ControlDisplayUnit")&&call.name().equalsString("<init>");
                require(e.getKey().owner().equals(f.owner())&&(f.owner().equals("Core")?startup:e.getKey().name().equals("<init>")),"native root replaced outside exact startup/constructor "+f+" "+e.getKey());
            }}
            require(writes==1,"sole native constructor root assignment "+f);
        }
        var changed=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String owner:List.of("Core","ControlDisplayUnit","ControlDisplayUnit$ScreenLine")){byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(UNITS,LINES,BANKS,PAGES).contains(m.methodName().stringValue())),"generated collision");var type=ClassDesc.of(owner);
            byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
                if(owner.equals("Core"))b.withMethodBody(UNITS,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.iconst_3().anewarray(CD_Object).astore(0);for(int i=0;i<3;i++)c.aload(0).loadConstant(i).getstatic(type,UNITS_FIELDS.get(i),ClassDesc.of("ControlDisplayUnit")).aastore();c.aload(0).areturn();});
                else if(owner.equals("ControlDisplayUnit")){b.withMethodBody(LINES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->{c.loadConstant(13).anewarray(CD_Object).astore(1);for(int i=0;i<13;i++)c.aload(1).loadConstant(i).aload(0).checkcast(type).getfield(type,LINE_FIELDS.get(i),ClassDesc.of("ControlDisplayUnit$ScreenLine")).aastore();c.aload(1).areturn();});b.withMethodBody(PAGES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->c.iconst_1().anewarray(CD_Object).dup().iconst_0().aload(0).checkcast(type).getfield(type,"page",ClassDesc.of("CduHub$Page")).aastore().areturn());}
                else b.withMethodBody(BANKS,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->{c.iconst_2().anewarray(CD_Object).astore(1);for(int i=0;i<2;i++)c.aload(1).loadConstant(i).getstatic(ClassDesc.of("Glob"),"qs",ClassDesc.of("Qs").arrayType()).aload(0).checkcast(type).getfield(type,i==0?"gidx":"gci",CD_int).aaload().aastore();c.aload(1).areturn();});
            }});require(data.cf.verify(output).isEmpty(),"generated verifier "+owner);changed.put(owner,output);
        }TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
