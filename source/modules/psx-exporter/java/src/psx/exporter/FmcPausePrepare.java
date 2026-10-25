package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Ordinary Pause bridges for a separately qualified FMC transaction. They do
 * not load a situation, reinitialize an FMC, reset dynamics or create an epoch.
 * Worker/EDT command use only; pause alone never proves reader exclusion. */
final class FmcPausePrepare {
    static final String PREFIX="$psxExporter$fmcPause";
    private FmcPausePrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        if((data.requireMethod("Core","isPaused","()Z").flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("FMC pause getter static descriptor");
        for(String name:List.of("setGlobPauseMode","setPaused")){
            String descriptor=name.equals("setPaused")?"(I)V":"(II)V";var method=data.requireMethod("Core",name,descriptor);
            if((method.flags().flagsMask()&ClassFile.ACC_STATIC)==0)throw new IllegalArgumentException("FMC pause static descriptor");
        }
        var transfer=data.requireMethod("Qi","transfer","(ICI)I");if((transfer.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)throw new IllegalArgumentException("FMC pause instance descriptor");
        var request=data.models.get("Qi").fields().stream().filter(f->f.fieldName().equalsString("iVar")&&f.fieldType().equalsString("I")).findFirst().orElseThrow();
        if((request.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))!=0)throw new IllegalArgumentException("FMC pause request storage");
        for(var field:List.of(new Prepare.Field("Core","paused","Z"),new Prepare.Field("Core","globSimPaused","I"),new Prepare.Field("Core","loaderPaused","Z"),new Prepare.Field("Glob","qi","[LQi;"),new Prepare.Field("Glob","qiSimPaused","I"))){
            var model=data.models.get(field.owner());if(model==null||model.fields().stream().noneMatch(f->f.fieldName().equalsString(field.name())&&f.fieldType().equalsString(field.type())&&(f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0))throw new IllegalArgumentException("FMC pause field contract");
        }
        byte[] input=lower.getOrDefault("Core",data.inputs.get("Core"));var source=data.cf.parse(input);
        if(source.methods().stream().anyMatch(m->m.methodName().stringValue().startsWith(PREFIX)))throw new IllegalArgumentException("FMC pause bridge collision");
        ClassDesc core=ClassDesc.of("Core"),glob=ClassDesc.of("Glob"),qi=ClassDesc.of("Qi");int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
        byte[] qiInput=lower.getOrDefault("Qi",data.inputs.get("Qi"));var qiSource=data.cf.parse(qiInput);
        if(qiSource.methods().stream().anyMatch(m->m.methodName().equalsString(PREFIX+"Requested")))throw new IllegalArgumentException("FMC requested-pause bridge collision");
        byte[] qiOutput=data.cf.transformClass(qiSource,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement element){b.with(element);}
            public void atEnd(ClassBuilder b){
                // Qi.transfer's read branch clears its native fresh flag.
                // Peek the private value under the same instance monitor,
                // without consuming freshness or publishing a network update.
                b.withMethodBody(PREFIX+"Requested",MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_SYNTHETIC,
                    c->c.aload(0).getfield(qi,"iVar",CD_int).ireturn());
            }
        });
        byte[] output=data.cf.transformClass(source,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement element){b.with(element);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(PREFIX+"Read",MethodTypeDesc.of(CD_int),flags,c->{appliedMask(c,core);c.ireturn();});
                b.withMethodBody(PREFIX+"Enter",MethodTypeDesc.of(CD_int),flags,c->{
                    rejectLoader(c,core);appliedMask(c,core);c.istore(0);validateMask(c,0);
                    // A queued network/menu change is not an applied Pause or
                    // Motion Freeze observation. Reject before mutation rather
                    // than adopting a differently timed requested selection.
                    var coherent=c.newLabel();readMask(c,glob,qi);c.iload(0).if_icmpeq(coherent);fail(c,"native Pause/Freeze request still pending");c.labelBinding(coherent);
                    c.iconst_0().iconst_3().invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int))
                        .iconst_1().invokestatic(core,"setPaused",MethodTypeDesc.of(CD_void,CD_int)).iload(0).ireturn();
                });
                b.withMethodBody(PREFIX+"Finish",MethodTypeDesc.of(CD_void,CD_int),flags,c->{
                    rejectLoader(c,core);validateMask(c,0);
                    // Restore entry Motion Freeze through its ordinary native
                    // bit operation, then retain ordinary Pause. No rates,
                    // integration history, forces or control positions change.
                    var clear=c.newLabel();var selected=c.newLabel();c.iconst_1().iload(0).iconst_2().iand().ifeq(clear).iconst_3().goto_(selected).labelBinding(clear).iconst_1().labelBinding(selected)
                        .invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int))
                        .iconst_0().iconst_3().invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int))
                        .iconst_1().invokestatic(core,"setPaused",MethodTypeDesc.of(CD_void,CD_int)).return_();
                });
            }
        });
        var errors=data.cf.verify(output);if(!errors.isEmpty())throw new IllegalArgumentException("FMC pause verification "+errors.getFirst());
        var qiErrors=data.cf.verify(qiOutput);if(!qiErrors.isEmpty())throw new IllegalArgumentException("FMC requested pause verification "+qiErrors.getFirst());
        var before=new HashMap<>(lower);before.put("Core",input);before.put("Qi",qiInput);var changed=Map.of("Core",output,"Qi",qiOutput);TerrainAccess.verifyAdded(before,changed,data.inputs);return changed;
    }
    private static void readMask(CodeBuilder c,ClassDesc glob,ClassDesc qi){c.getstatic(glob,"qi",qi.arrayType()).getstatic(glob,"qiSimPaused",CD_int).aaload().invokevirtual(qi,PREFIX+"Requested",MethodTypeDesc.of(CD_int));}
    private static void appliedMask(CodeBuilder c,ClassDesc core){c.getstatic(core,"paused",CD_boolean).getstatic(core,"globSimPaused",CD_int).iconst_2().iand().ior();}
    private static void validateMask(CodeBuilder c,int local){var okay=c.newLabel();c.iload(local).loadConstant(~3).iand().ifeq(okay);fail(c,"invalid native Pause/Freeze mask");c.labelBinding(okay);}
    private static void rejectLoader(CodeBuilder c,ClassDesc core){var okay=c.newLabel();c.getstatic(core,"loaderPaused",CD_boolean).ifeq(okay);fail(c,"native situation loader active");c.labelBinding(okay);}
    private static void fail(CodeBuilder c,String reason){ClassDesc type=ClassDesc.of("java.lang.IllegalStateException");c.new_(type).dup().loadConstant(reason).invokespecial(type,"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();}
}
