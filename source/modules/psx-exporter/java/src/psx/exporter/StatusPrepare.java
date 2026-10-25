package psx.exporter;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Parses vendor classes as data. The complete optional layer is a transaction. */
final class StatusPrepare {
    static final String PREFIX="$psxStatus$";
    static final ClassDesc OBS=ClassDesc.of("psx.exporter.StatusObserver");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    final Prepare base;
    final Set<String> fields=Set.of("MalfunctionsConseq.fatalDamage","MalfunctionsConseq.fatalDamageNew","Core.paused","Core.globSimPaused","Core.loaderPaused");
    final Set<Prepare.MethodId> guards=new HashSet<>();
    final Set<String> required=new TreeSet<>();
    final List<String> evidence=new ArrayList<>();
    StatusPrepare(Prepare base){this.base=base;}
    void field(String owner,String name,String desc){
        var model=base.models.get(owner);if(model==null)throw new IllegalArgumentException("status owner "+owner);
        var f=model.fields().stream().filter(x->x.fieldName().equalsString(name)&&x.fieldType().equalsString(desc)).findFirst().orElseThrow();
        if((f.flags().flagsMask()&ClassFile.ACC_STATIC)==0||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)!=0)throw new IllegalArgumentException("status field access "+owner+"."+name);
        evidence.add("field "+owner+"."+name+desc);
    }
    void qualify(){
        field("MalfunctionsConseq","fatalDamage","Z");field("MalfunctionsConseq","fatalDamageNew","Z");field("Core","paused","Z");field("Core","globSimPaused","I");field("Core","loaderPaused","Z");
        for(String method:List.of("runAllegro","resetFatal"))base.requireMethod("MalfunctionsConseq",method,"()V");base.requireMethod("MalfunctionsConseq","setFatal","(I)V");
        var scheduler=Prepare.instructions(base.requireMethod("Core$AllegroThread","run","()V"));int fatal=-1,pause=-1;
        for(int i=0;i<scheduler.size();i++)if(scheduler.get(i) instanceof InvokeInstruction x){if(x.owner().asInternalName().equals("MalfunctionsConseq")&&x.name().equalsString("runAllegro")&&x.type().equalsString("()V"))fatal=i;if(x.owner().asInternalName().equals("Core")&&x.name().equalsString("isPaused")&&pause<0)pause=i;}
        if(fatal<0||pause<=fatal)throw new IllegalArgumentException("status paused execution order");evidence.add("fatal Allegro invocation precedes pause branch");
        for(var entry:base.methods.entrySet())if(entry.getValue().code().isPresent()){
            boolean writer=false;
            var instructions=Prepare.instructions(entry.getValue());
            for(int index=0;index<instructions.size();index++)if(instructions.get(index) instanceof FieldInstruction f&&fields.contains(f.owner().asInternalName()+"."+f.name().stringValue())){
                evidence.add((f.opcode()==Opcode.PUTSTATIC?"writer ":"reader ")+entry.getKey()+" "+f.opcode()+" "+f.owner().asInternalName()+"."+f.name().stringValue());
                if(f.opcode()==Opcode.PUTSTATIC){writer=true;
                    if(f.owner().asInternalName().equals("MalfunctionsConseq")){
                        var id=entry.getKey();boolean clear=id.owner().equals("MalfunctionsConseq")&&id.name().equals("resetFatal")&&id.descriptor().equals("()V");
                        boolean active=id.owner().equals("MalfunctionsConseq")&&id.name().equals("runAllegro")&&id.descriptor().equals("()V")&&f.name().equalsString("fatalDamage");
                        boolean requested=id.owner().equals("MalfunctionsConseq")&&id.name().equals("setFatal")&&id.descriptor().equals("(I)V")&&f.name().equalsString("fatalDamageNew");
                        if(!(clear||active||requested)||index==0||instructions.get(index-1).opcode()!=(clear?Opcode.ICONST_0:Opcode.ICONST_1))throw new IllegalArgumentException("status unqualified fatal latch semantics "+id);
                    }
                }
            }
            if(writer){guards.add(entry.getKey());required.add(entry.getKey().owner());}
        }
        for(var id:List.of(new Prepare.MethodId("MalfunctionsConseq","setFatal","(I)V"),new Prepare.MethodId("MalfunctionsConseq","resetFatal","()V"),new Prepare.MethodId("MalfunctionsConseq","runAllegro","()V")))if(!guards.contains(id))throw new IllegalArgumentException("status missing fatal writer "+id);
        required.add("MalfunctionsConseq");Collections.sort(evidence);
    }
    Map<String,byte[]> transform(Map<String,byte[]> lower){
        Map<String,byte[]> output=new TreeMap<>(lower),before=new TreeMap<>(lower);
        for(String name:required){var owner=ClassDesc.ofInternalName(name);byte[] input=lower.getOrDefault(name,base.inputs.get(name));before.putIfAbsent(name,input);
            byte[] bytes=base.cf.transformClass(base.cf.parse(input),new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){
                    if(!(e instanceof MethodModel m)||m.code().isEmpty()){b.with(e);return;}
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());boolean guard=guards.contains(id),sample=name.equals("MalfunctionsConseq")&&id.name().equals("runAllegro")&&id.descriptor().equals("()V");
                    if(!guard&&!sample){b.with(e);return;}
                    b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel code)){mb.with(me);return;}mb.transformCode(code,new CodeTransform(){
                        Label begin,end,handler;
                        public void atStart(CodeBuilder cb){if(guard)cb.invokestatic(owner,PREFIX+"enter",VOID);begin=cb.newLabel();end=cb.newLabel();handler=cb.newLabel();cb.labelBinding(begin);}
                        public void accept(CodeBuilder cb,CodeElement ce){if(ce instanceof ReturnInstruction){if(sample)cb.invokestatic(owner,PREFIX+"sample",VOID);if(guard)cb.invokestatic(owner,PREFIX+"leave",VOID);}cb.with(ce);}
                        public void atEnd(CodeBuilder cb){cb.labelBinding(end).labelBinding(handler);if(guard)cb.invokestatic(owner,PREFIX+"leave",VOID);cb.athrow();cb.exceptionCatchAll(begin,end,handler);}
                    });});
                }
                public void atEnd(ClassBuilder b){
                    if(guards.stream().anyMatch(id->id.owner().equals(name)))for(String method:List.of("enter","leave"))safe(b,method,cb->cb.invokestatic(OBS,method,VOID));
                    if(name.equals("MalfunctionsConseq"))safe(b,"sample",cb->{
                        cb.invokestatic(OBS,"begin",MethodTypeDesc.of(CD_boolean));cb.ifThen(copy->{
                            Prepare.read(copy,"MalfunctionsConseq","fatalDamage","Z");Prepare.read(copy,"MalfunctionsConseq","fatalDamageNew","Z");Prepare.read(copy,"Core","paused","Z");Prepare.read(copy,"Core","globSimPaused","I");Prepare.read(copy,"Core","loaderPaused","Z");
                            copy.invokestatic(OBS,"publish",MethodTypeDesc.of(CD_void,CD_boolean,CD_boolean,CD_boolean,CD_int,CD_boolean));
                        });cb.invokestatic(OBS,"end",VOID);
                    });
                }
            });var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("status verify "+name+" "+errors.getFirst());output.put(name,bytes);
        }
        TerrainAccess.verifyAdded(before,output,base.inputs);return output;
    }
    static void safe(ClassBuilder b,String method,java.util.function.Consumer<CodeBuilder> body){b.withMethodBody(PREFIX+method,VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
        cb.trying(block->body.accept(block),c->c.catching(CD_Throwable,h->{h.pop();h.trying(x->{x.invokestatic(OBS,"disable",VOID);x.invokestatic(OBS,"end",VOID);},cc->cc.catching(CD_Throwable,x->x.pop()));}));cb.return_();
    });}
}
