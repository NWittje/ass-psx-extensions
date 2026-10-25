package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Only the original owned Qi/Qs transfer outputs are withheld while the
 * ordinary EXEC load recipe publishes its coherent group. Native transfers,
 * monitors, return values, all unrelated outputs and explicit final flushes
 * retain their original paths. Helper failure emits normally and faults recall. */
final class FmcBackgroundAdmissionBankPrepare {
    static final String FAULT="$psxExporter$fmcBankOutputFailure",CALL="$psxExporter$fmcBankOutput";
    static final MethodTypeDesc PRINT=MethodTypeDesc.of(CD_void,CD_int,CD_char,CD_int,CD_String,CD_int);
    private static final ClassDesc HELPER=ClassDesc.of("psx.exporter.FmcBackgroundAdmission"),NETWORK=ClassDesc.of("Network");
    private FmcBackgroundAdmissionBankPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        var before=new HashMap<>(lower);var changed=new HashMap<String,byte[]>();
        for(String owner:List.of("Qi","Qs")){
            String descriptor=owner.equals("Qi")?"(ICI)I":"(Ljava/lang/String;CI)Ljava/lang/String;";
            var id=new Prepare.MethodId(owner,"transfer",descriptor);var original=data.methods.get(id);
            require(original!=null&&original.code().isPresent(),"native transfer declaration");
            long count=Prepare.instructions(original).stream().filter(FmcBackgroundAdmissionBankPrepare::print).count();
            require(count==2,"exact two native output branches");
            var self=ClassDesc.of(owner);byte[] input=lower.getOrDefault(owner,data.inputs.get(owner));before.put(owner,input);var model=data.cf.parse(input);
            FmcBackgroundAdmissionOwnership.qualify(data,owner,model);
            require(model.fields().stream().noneMatch(f->f.fieldName().equalsString(FAULT))&&model.methods().stream().noneMatch(m->m.methodName().equalsString(CALL)),"generated output collision");
            int[] found={0};var wrapper=MethodTypeDesc.of(CD_void,CD_int,CD_char,CD_int,CD_String,CD_int,CD_Object);
            byte[] bytes=data.cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(e instanceof MethodModel m&&m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())){
                        b.transformMethod(m,MethodTransform.transformingCode((c,element)->{
                            if(element instanceof Instruction instruction&&print(instruction)){found[0]++;c.aload(0).invokestatic(self,CALL,wrapper);}
                            else c.with(element);
                        }));
                    }else b.with(e);
                }
                public void atEnd(ClassBuilder b){
                    b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
                    b.withMethodBody(CALL,wrapper,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                        int skip=c.allocateLocal(TypeKind.INT);c.iconst_0().istore(skip);
                        var start=c.newLabel();var end=c.newLabel();var bad=c.newLabel();var emit=c.newLabel();
                        c.labelBinding(start).aload(5).invokestatic(self,FmcBackgroundAdmissionOwnership.READ,MethodTypeDesc.of(CD_long,CD_Object));
                        int tag=c.allocateLocal(TypeKind.LONG);c.lstore(tag).lload(tag).lconst_0().lcmp().ifle(end);
                        c.lload(tag).aload(5).invokestatic(HELPER,"suppressBankOutput",MethodTypeDesc.of(CD_boolean,CD_long,CD_Object)).istore(skip);
                        c.labelBinding(end).goto_(emit).exceptionCatchAll(start,end,bad).labelBinding(bad).pop().iconst_1().putstatic(self,FAULT,CD_boolean);
                        var report=c.newLabel();var reported=c.newLabel();var missing=c.newLabel();
                        c.labelBinding(report).invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)).labelBinding(reported).goto_(emit)
                            .exceptionCatchAll(report,reported,missing).labelBinding(missing).pop().labelBinding(emit);
                        var done=c.newLabel();c.iload(skip).ifne(done).iload(0).iload(1).iload(2).aload(3).iload(4).invokestatic(NETWORK,"toPrint",PRINT).labelBinding(done).return_();
                    });
                }
            });
            require(found[0]==2&&data.cf.verify(bytes).isEmpty(),"final output branches/verifier");changed.put(owner,bytes);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static boolean print(Instruction op){return op instanceof InvokeInstruction c&&c.opcode()==Opcode.INVOKESTATIC&&c.owner().asInternalName().equals("Network")&&c.name().equalsString("toPrint")&&c.typeSymbol().equals(PRINT);}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC bank output "+why);}
}
