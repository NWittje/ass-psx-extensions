package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Composed command bridges; the earlier read-only requested-mask peek and
 * installed native Pause component must already be qualified. All new helper
 * references are checked against the actual final classes/helper bytes. */
final class FmcPauseCommandPrepare {
    static final String PREFIX=FmcPausePrepare.PREFIX+"Command";
    private FmcPauseCommandPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower) {
        var combined=new HashMap<>(data.inputs);combined.putAll(lower);
        var peek=FmcNativePauseQualification.method(combined,new Prepare.MethodId("Qi",FmcPausePrepare.PREFIX+"Requested","()I"));
        if(peek.flags().flagsMask()!=(ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_SYNTHETIC))
            throw new IllegalArgumentException("FMC command Pause requires qualified monitor peek");
        for(var spec:List.of(new Prepare.MethodId("Core","setGlobPauseMode","(II)V"),new Prepare.MethodId("Core","setPaused","(I)V")))
            if((FmcNativePauseQualification.method(combined,spec).flags().flagsMask()&ClassFile.ACC_STATIC)==0)
                throw new IllegalArgumentException("FMC command native Pause descriptor/staticness");
        byte[] input=combined.get("Core");var source=data.cf.parse(input);
        if(source.methods().stream().anyMatch(m->m.methodName().stringValue().startsWith(PREFIX)))
            throw new IllegalArgumentException("FMC command Pause bridge collision");
        ClassDesc core=ClassDesc.of("Core"),glob=ClassDesc.of("Glob"),qi=ClassDesc.of("Qi"),helper=ClassDesc.of("psx.exporter.FmcPauseCommand");
        var none=MethodTypeDesc.of(CD_void);int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
        byte[] output=data.cf.transformClass(source,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(PREFIX+"Enter",none,flags,c->{
                    rejectLoader(c,core);applied(c,core);c.istore(0);validate(c,0);
                    var coherent=c.newLabel();c.getstatic(glob,"qi",qi.arrayType()).getstatic(glob,"qiSimPaused",CD_int).aaload()
                        .invokevirtual(qi,FmcPausePrepare.PREFIX+"Requested",MethodTypeDesc.of(CD_int)).iload(0).if_icmpeq(coherent);
                    fail(c,"native Pause/Freeze request still pending");c.labelBinding(coherent);
                    // Receipt survives an exception from either native setter.
                    // Rejected preflight does not create one or consume Qi freshness.
                    c.iload(0).invokestatic(helper,"acceptEntry",MethodTypeDesc.of(CD_void,CD_int));
                    authority(c,helper);c.iconst_0().iconst_3().invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int));
                    authority(c,helper);c.iconst_1().invokestatic(core,"setPaused",MethodTypeDesc.of(CD_void,CD_int)).return_();
                });
                b.withMethodBody(PREFIX+"Finish",MethodTypeDesc.of(CD_void,CD_int),flags,c->{
                    rejectLoader(c,core);validate(c,0);c.iload(0).invokestatic(helper,"acceptFinish",MethodTypeDesc.of(CD_void,CD_int));
                    authority(c,helper);
                    var clear=c.newLabel();var chosen=c.newLabel();c.iconst_1().iload(0).iconst_2().iand().ifeq(clear).iconst_3().goto_(chosen)
                        .labelBinding(clear).iconst_1().labelBinding(chosen).invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int));
                    authority(c,helper);c.iconst_0().iconst_3().invokestatic(core,"setGlobPauseMode",MethodTypeDesc.of(CD_void,CD_int,CD_int));
                    authority(c,helper);c.iconst_1().invokestatic(core,"setPaused",MethodTypeDesc.of(CD_void,CD_int)).return_();
                });
            }
        });
        var errors=data.cf.verify(output);if(!errors.isEmpty())throw new IllegalArgumentException("FMC command Pause verification "+errors.getFirst());
        var changed=Map.of("Core",output);TerrainAccess.verifyAdded(combined,changed,combined);return changed;
    }
    private static void authority(CodeBuilder c,ClassDesc helper){c.invokestatic(helper,"requireAuthority",MethodTypeDesc.of(CD_void));}
    private static void applied(CodeBuilder c,ClassDesc core){c.getstatic(core,"paused",CD_boolean).getstatic(core,"globSimPaused",CD_int).iconst_2().iand().ior();}
    private static void rejectLoader(CodeBuilder c,ClassDesc core){var okay=c.newLabel();c.getstatic(core,"loaderPaused",CD_boolean).ifeq(okay);fail(c,"native situation loader active");c.labelBinding(okay);}
    private static void validate(CodeBuilder c,int slot){var okay=c.newLabel();c.iload(slot).loadConstant(~3).iand().ifeq(okay);fail(c,"invalid native Pause/Freeze mask");c.labelBinding(okay);}
    private static void fail(CodeBuilder c,String reason){var type=ClassDesc.of("java.lang.IllegalStateException");c.new_(type).dup().loadConstant(reason).invokespecial(type,"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();}
}
