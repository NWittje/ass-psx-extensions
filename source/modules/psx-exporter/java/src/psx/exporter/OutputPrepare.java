package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** External allowlist verification of the installed OpenAL cache gain boundary. */
final class OutputPrepare {
    static final ClassDesc SOUND=ClassDesc.of("Sound"), CACHE=ClassDesc.of("Sound$Cache"), OUTPUT=ClassDesc.of("psx.exporter.AudioOutput"), AL=ClassDesc.of("org.lwjgl.openal.AL10");
    static final String PREFIX="$psxOutput$";
    static final String[] CACHES={"engRR","engT","engU","engW1","engW2","engW3","engW4","engC1","engC2","engC3","engC4","ram"};
    static final Map<String,Integer> GAIN=Map.of("setPitchAndGain",2,"setGain",1,"setPanAndGain",2,"cachePlayPanGain",2,"cachePlay",1);
    final Prepare base;
    OutputPrepare(Prepare base){this.base=base;}
    void qualify(){
        AudioPrepare fields=new AudioPrepare(base);
        for(String name:CACHES)if(!fields.field("Sound",name,true).descriptor().equals("LSound$Cache;"))throw new IllegalArgumentException("sound cache changed");
        fields.field("Sound","soundInitiated",true);fields.field("Sound","source",true);fields.field("Sound$Cache","id",false);
        base.requireMethod("Sound","runAllegro","()V");
        for(var entry:GAIN.entrySet()){
            String desc=entry.getValue()==1?"(LSound$Cache;F)V":"(LSound$Cache;FF)V";
            var m=base.requireMethod("Sound",entry.getKey(),desc);
            long count=Prepare.instructions(m).stream().filter(i->i instanceof InvokeInstruction call&&call.owner().asInternalName().equals("org/lwjgl/openal/AL10")&&call.name().equalsString("alSourcef")&&call.type().equalsString("(IIF)V")).count();
            if(count!=(entry.getKey().equals("setPitchAndGain")?2:1))throw new IllegalArgumentException("gain boundary changed "+entry.getKey());
        }
        // Old JavaSound loops are not a supported output path. Reject any current call into them.
        for(var m:base.methods.values())if(m.code().isPresent())for(var ins:Prepare.instructions(m))if(ins instanceof InvokeInstruction call&&call.owner().asInternalName().equals("Audio")&&call.name().equalsString("init"))throw new IllegalArgumentException("additional JavaSound output path");
    }
    Map<String,byte[]> transform(Map<String,byte[]> input){
        var output=new TreeMap<>(input);var model=base.cf.parse(input.getOrDefault("Sound",base.inputs.get("Sound")));
        byte[] changed=base.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){
                if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                Integer slot=GAIN.get(m.methodName().stringValue());boolean pulse=m.methodName().equalsString("runAllegro");
                if(slot==null&&!pulse){b.with(e);return;}
                b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel c)){mb.with(me);return;}
                    mb.transformCode(c,new CodeTransform(){
                        public void atStart(CodeBuilder cb){if(slot!=null)cb.aload(0).fload(slot).invokestatic(SOUND,PREFIX+"gain",MethodTypeDesc.of(CD_float,CACHE,CD_float)).fstore(slot);}
                        public void accept(CodeBuilder cb,CodeElement ce){if(pulse&&ce instanceof ReturnInstruction)cb.invokestatic(SOUND,PREFIX+"pulse",MethodTypeDesc.of(CD_void));cb.with(ce);}
                    });
                });
            }
            public void atEnd(ClassBuilder b){
                b.withMethodBody(PREFIX+"gain",MethodTypeDesc.of(CD_float,CACHE,CD_float),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                    cb.getstatic(SOUND,"soundInitiated",CD_boolean).ifThen(Opcode.IFEQ,inactive->inactive.fload(1).freturn());
                    cb.trying(body->{for(int i=0;i<CACHES.length;i++){final int index=i;body.aload(0).getstatic(SOUND,CACHES[i],CACHE).ifThen(Opcode.IF_ACMPEQ,same->same.loadConstant(index).aload(0).getstatic(SOUND,"source",ClassDesc.of("java.nio.IntBuffer")).aload(0).getfield(CACHE,"id",CD_int).invokevirtual(ClassDesc.of("java.nio.IntBuffer"),"get",MethodTypeDesc.of(CD_int,CD_int)).fload(1).invokestatic(OUTPUT,"gain",MethodTypeDesc.of(CD_float,CD_int,CD_Object,CD_int,CD_float)).freturn());}},c->c.catching(CD_Throwable,h->h.pop().invokestatic(OUTPUT,"failed",MethodTypeDesc.of(CD_void))));cb.fload(1).freturn();
                });
                b.withMethodBody(PREFIX+"pulse",MethodTypeDesc.of(CD_void),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                    cb.invokestatic(OUTPUT,"needsService",MethodTypeDesc.of(CD_boolean)).ifThen(Opcode.IFEQ,inactive->inactive.return_());
                    cb.trying(body->{
                        body.getstatic(SOUND,"soundInitiated",CD_boolean).ifThen(active->{
                            active.invokestatic(OUTPUT,"control",MethodTypeDesc.of(CD_long)).lstore(0);
                            for(int i=0;i<CACHES.length;i++){
                                final int index=i;
                                active.getstatic(SOUND,"source",ClassDesc.of("java.nio.IntBuffer")).getstatic(SOUND,CACHES[i],CACHE).getfield(CACHE,"id",CD_int).invokevirtual(ClassDesc.of("java.nio.IntBuffer"),"get",MethodTypeDesc.of(CD_int,CD_int)).istore(2);
                                active.loadConstant(i).getstatic(SOUND,CACHES[i],CACHE).iload(2).invokestatic(OUTPUT,"missing",MethodTypeDesc.of(CD_boolean,CD_int,CD_Object,CD_int)).istore(3);
                                active.iload(3).ifThen(missing->missing.loadConstant(index).iload(2).loadConstant(4106).invokestatic(AL,"alGetSourcef",MethodTypeDesc.of(CD_float,CD_int,CD_int)).invokestatic(OUTPUT,"remember",MethodTypeDesc.of(CD_void,CD_int,CD_float)));
                                active.iload(3).lload(0).invokestatic(OUTPUT,"changed",MethodTypeDesc.of(CD_boolean,CD_long)).ior().ifThen(refresh->refresh.iload(2).loadConstant(4106).loadConstant(index).lload(0).invokestatic(OUTPUT,"restored",MethodTypeDesc.of(CD_float,CD_int,CD_long)).invokestatic(AL,"alSourcef",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_float)));
                            }
                            active.lload(0).invokestatic(OUTPUT,"completed",MethodTypeDesc.of(CD_void,CD_long));
                        });
                    },c->c.catching(CD_Throwable,h->h.pop().invokestatic(OUTPUT,"failed",MethodTypeDesc.of(CD_void))));cb.return_();
                });
            }
        });
        var errors=base.cf.verify(changed);if(!errors.isEmpty())throw new IllegalArgumentException("sound verification "+errors.getFirst());output.put("Sound",changed);return output;
    }
}
