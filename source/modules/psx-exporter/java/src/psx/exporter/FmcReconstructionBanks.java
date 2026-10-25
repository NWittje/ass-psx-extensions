package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Legal same-owner bank views and post-application acknowledgement. Views do
 * not call transfer/isUpdated and cannot consume pending input. Acknowledgement
 * is an ordinary-thread mutation root, only after every associated live cache
 * has been reconciled. The quiet-input premise and excluded native readers are
 * required throughout; an artifact never supplies a bank receiver. */
final class FmcReconstructionBanks {
    static final String PENDING="$psxExporter$fmcBankPending",ACK="$psxExporter$fmcBankApplied";
    static final String VALUE="$psxExporter$fmcBankValue",ACCEPTS="$psxExporter$fmcBankAccepts";
    static final String FLUSH="$psxExporter$fmcBankFlush";
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId("Q",ACK,"(Ljava/lang/Object;)V"),new Prepare.MethodId("Qi",FLUSH,"(Ljava/lang/Object;)V"),new Prepare.MethodId("Qs",FLUSH,"(Ljava/lang/Object;)V"));}
    static Set<FmcQualification.Member> writes(){return Set.of(new FmcQualification.Member("Q","fresh","Z"),new FmcQualification.Member("Q","updated","Z"));}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction banks "+why);}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var changed=new HashMap<>(lower);var before=new HashMap<>(lower);
        for(String name:List.of("Q","Qi","Qs")){
            var owner=ClassDesc.of(name);byte[] input=lower.getOrDefault(name,data.inputs.get(name));require(input!=null,"owner "+name);before.put(name,input);
            var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(PENDING,ACK,VALUE,ACCEPTS,FLUSH).contains(m.methodName().stringValue())),"bridge collision");
            for(String field:name.equals("Q")?List.of("fresh","updated"):name.equals("Qi")?List.of("iVar","min","max"):List.of("sVar","min","max"))
                require(model.fields().stream().filter(f->f.fieldName().equalsString(field)).count()==1,"exact field "+name+"."+field);
            byte[] output=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){
                    int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
                    if(name.equals("Q")){
                        b.withMethodBody(PENDING,MethodTypeDesc.of(CD_boolean,CD_Object),flags,c->{c.aload(0).checkcast(owner).getfield(owner,"fresh",CD_boolean)
                            .aload(0).checkcast(owner).getfield(owner,"updated",CD_boolean).ior().ireturn();});
                        b.withMethodBody(ACK,MethodTypeDesc.of(CD_void,CD_Object),flags,c->{for(String field:List.of("fresh","updated"))c.aload(0).checkcast(owner).iconst_0().putfield(owner,field,CD_boolean);c.return_();});
                    }else{
                        b.withMethodBody(FLUSH,MethodTypeDesc.of(CD_void,CD_Object),flags,c->{
                            c.iconst_1().loadConstant((int)(name.equals("Qi")?'i':'s')).aload(0).checkcast(owner).getfield(owner,"serialNr",CD_int);
                            if(name.equals("Qi"))c.aconst_null().aload(0).checkcast(owner).getfield(owner,"iVar",CD_int);else c.aload(0).checkcast(owner).getfield(owner,"sVar",CD_String).iconst_0();
                            c.invokestatic(ClassDesc.of("Network"),"toPrint",MethodTypeDesc.of(CD_void,CD_int,CD_char,CD_int,CD_String,CD_int)).return_();
                        });
                        b.withMethodBody(VALUE,MethodTypeDesc.of(name.equals("Qi")?CD_int:CD_String,CD_Object),flags,c->c.aload(0).checkcast(owner).getfield(owner,name.equals("Qi")?"iVar":"sVar",name.equals("Qi")?CD_int:CD_String).return_(name.equals("Qi")?TypeKind.INT:TypeKind.REFERENCE));
                        b.withMethodBody(ACCEPTS,MethodTypeDesc.of(CD_boolean,CD_Object,name.equals("Qi")?CD_int:CD_String),flags,c->{
                            var bad=c.newLabel();c.aload(0).checkcast(owner).astore(2);
                            if(name.equals("Qi"))c.iload(1).istore(3);else c.aload(1).invokevirtual(CD_String,"length",MethodTypeDesc.of(CD_int)).istore(3);
                            c.iload(3).aload(2).getfield(owner,"min",CD_int).if_icmplt(bad).iload(3).aload(2).getfield(owner,"max",CD_int).if_icmpgt(bad).iconst_1().ireturn().labelBinding(bad).iconst_0().ireturn();
                        });
                    }
                }
            });require(data.cf.verify(output).isEmpty(),"bridge verifier "+name);changed.put(name,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
