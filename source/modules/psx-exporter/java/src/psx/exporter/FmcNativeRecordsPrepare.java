package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Legal same-owner bridges into the original native record and reload routines. */
public final class FmcNativeRecordsPrepare {
    public static final String IMPORT="$psxExporter$fmcNativeImport", INITIALIZE="$psxExporter$fmcNativeInitialize", PAUSE="$psxExporter$fmcNativePause", FINISH="$psxExporter$fmcNativeFinish", IS_PAUSED="$psxExporter$fmcNativeIsPaused";
    private static final int FLAGS=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
    private FmcNativeRecordsPrepare(){}
    static Map<String,byte[]> transform(Prepare d,Map<String,byte[]> lower){return emit(d.cf,FmcNativeRecordsProfile.inspect(d),d.inputs,lower);}
    static Map<String,byte[]> emit(ClassFile cf,FmcNativeRecordsProfile.Profile profile,Map<String,byte[]> original,Map<String,byte[]> lower){
        Map<String,byte[]> result=new HashMap<>();
        for(String owner:List.of("Glob","CduHub","Core")){
            byte[] input=lower.getOrDefault(owner,original.get(owner));if(input==null)throw new IllegalArgumentException("native bridge owner "+owner);
            ClassModel model=cf.parse(input);
            for(MethodModel m:model.methods())if(Set.of(IMPORT,INITIALIZE,PAUSE,FINISH,IS_PAUSED).contains(m.methodName().stringValue()))throw new IllegalArgumentException("native bridge collision");
            byte[] out=cf.transformClass(model,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){
                    if(owner.equals("Glob"))b.withMethodBody(IMPORT,MethodTypeDesc.of(CD_void,CD_int.arrayType(),CD_int.arrayType(),CD_String.arrayType()),FLAGS,c->importBody(c,profile));
                    if(owner.equals("CduHub"))b.withMethodBody(INITIALIZE,MethodTypeDesc.of(CD_void),FLAGS,c->c.invokestatic(ClassDesc.of("Option"),"readGlobAirlinePol",MethodTypeDesc.of(CD_void)).invokestatic(ClassDesc.of("CduHub"),"situLoadInit",MethodTypeDesc.of(CD_void)).invokestatic(ClassDesc.of("RoutesStby"),"situLoadInit",MethodTypeDesc.of(CD_void)).return_());
                    if(owner.equals("Core")){
                        b.withMethodBody(PAUSE,MethodTypeDesc.of(CD_void),FLAGS,c->c.iconst_1().invokestatic(ClassDesc.of("Core"),"setPaused",MethodTypeDesc.of(CD_void,CD_int)).invokestatic(ClassDesc.of("Core"),"setLoaderPaused",MethodTypeDesc.of(CD_void)).return_());
                        // Native setPaused(0) releases loaderPaused without the
                        // whole-aircraft initializer; final ordinary Pause follows.
                        b.withMethodBody(FINISH,MethodTypeDesc.of(CD_void),FLAGS,c->c.iconst_0().invokestatic(ClassDesc.of("Core"),"setPaused",MethodTypeDesc.of(CD_void,CD_int)).iconst_1().invokestatic(ClassDesc.of("Core"),"setPaused",MethodTypeDesc.of(CD_void,CD_int)).return_());
                        b.withMethodBody(IS_PAUSED,MethodTypeDesc.of(CD_boolean),FLAGS,c->c.invokestatic(ClassDesc.of("Core"),"isPaused",MethodTypeDesc.of(CD_boolean)).ireturn());
                    }
                }
            });
            if(!cf.verify(out).isEmpty())throw new IllegalArgumentException("native bridge verifier "+owner+": "+cf.verify(out).getFirst());result.put(owner,out);
        }
        Map<String,byte[]> complete=new HashMap<>(original);complete.putAll(lower);
        TerrainAccess.verifyAdded(complete,result,complete);
        return Map.copyOf(result);
    }
    private static void importBody(CodeBuilder c,FmcNativeRecordsProfile.Profile p){
        ClassDesc glob=ClassDesc.of("Glob");
        for(var bank:FmcNativeRecordsProfile.Bank.values()){
            int local=bank.ordinal();List<FmcNativeRecordsProfile.Slot> slots=p.slots(bank);Label nonnull=c.newLabel(),length=c.newLabel();
            c.aload(local).ifnonnull(nonnull);reject(c);c.labelBinding(nonnull).aload(local).arraylength().loadConstant(slots.size()).if_icmpeq(length);reject(c);c.labelBinding(length);
            ClassDesc type=type(bank);Label nativeArray=c.newLabel();c.getstatic(glob,field(bank),type.arrayType()).ifnonnull(nativeArray);reject(c);c.labelBinding(nativeArray);
            Label nativeLength=c.newLabel();c.getstatic(glob,field(bank),type.arrayType()).arraylength().loadConstant(nativeLength(bank)).if_icmpeq(nativeLength);reject(c);c.labelBinding(nativeLength);
            for(int n=0;n<slots.size();n++){
                var s=slots.get(n);Label index=c.newLabel(),object=c.newLabel();
                c.getstatic(glob,s.selector(),CD_int).loadConstant(s.index()).if_icmpeq(index);reject(c);c.labelBinding(index);
                c.getstatic(glob,field(bank),type.arrayType()).loadConstant(s.index()).aaload().ifnonnull(object);reject(c);c.labelBinding(object);
                Label key=c.newLabel();c.loadConstant(s.key()).getstatic(glob,field(bank),type.arrayType()).loadConstant(s.index()).aaload().getfield(type,"varName",CD_String).invokevirtual(CD_String,"equals",MethodTypeDesc.of(CD_boolean,CD_Object)).ifne(key);reject(c);c.labelBinding(key);
                if(bank==FmcNativeRecordsProfile.Bank.QS){Label value=c.newLabel();c.aload(local).loadConstant(n).aaload().ifnonnull(value);reject(c);c.labelBinding(value);}
            }
        }
        // Native SituationFiler.load publishes all Qi, then Qh, then Qs, mode1.
        // There is no direct state write, content parser or compensating reset.
        for(var bank:FmcNativeRecordsProfile.Bank.values()){
            var slots=p.slots(bank);ClassDesc type=type(bank);
            for(int n=0;n<slots.size();n++){
                c.getstatic(glob,field(bank),type.arrayType()).loadConstant(slots.get(n).index()).aaload().aload(bank.ordinal()).loadConstant(n);
                if(bank==FmcNativeRecordsProfile.Bank.QS)c.aaload();else c.iaload();
                c.loadConstant((int)'w').iconst_1().invokevirtual(type,"transfer",bank==FmcNativeRecordsProfile.Bank.QS?MethodTypeDesc.of(CD_String,CD_String,CD_char,CD_int):MethodTypeDesc.of(CD_int,CD_int,CD_char,CD_int)).pop();
            }
        }
        c.return_();
    }
    private static void reject(CodeBuilder c){ClassDesc type=ClassDesc.of("java.lang.IllegalArgumentException");c.new_(type).dup().loadConstant("native FMC record framing or initialized bank mismatch").invokespecial(type,"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();}
    private static ClassDesc type(FmcNativeRecordsProfile.Bank b){return ClassDesc.of(switch(b){case QI->"Qi";case QH->"Qh";case QS->"Qs";});}
    private static String field(FmcNativeRecordsProfile.Bank b){return b.name().toLowerCase(Locale.ROOT);}
    private static int nativeLength(FmcNativeRecordsProfile.Bank b){return switch(b){case QI->278;case QH->431;case QS->575;};}
}
