package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Read-only applied Lower-Box facts, independent of native short-circuit gates. */
final class InstrumentLowerPrepare {
    static final Set<String> OWNERS=Set.of("IntegratedDisplaySystem","IntegratedDisplaySystem$Box","EicasDisplaySecondary");
    static final String METHOD=InstrumentPrepare.PREFIX+"lower";
    static final ClassDesc IDS=ClassDesc.of("IntegratedDisplaySystem"),BOX=ClassDesc.of("IntegratedDisplaySystem$Box"),SECONDARY=ClassDesc.of("EicasDisplaySecondary"),IDU=ClassDesc.of("Idu"),EA=ClassDesc.of("ElecAdapter");
    static final int SEEN=233;
    final Prepare base;final List<String> evidence=new ArrayList<>();
    private record Field(String owner,String name,ClassDesc type,boolean isStatic){}
    private static final List<Field> FIELDS=List.of(
        new Field("Core","loaderPaused",CD_boolean,true),new Field("IntegratedDisplaySystem","eicasLwrBox",BOX,true),
        new Field("IntegratedDisplaySystem","graphEicasLwr",SECONDARY,true),new Field("IntegratedDisplaySystem","graphEicasLwrL",SECONDARY,true),new Field("IntegratedDisplaySystem","graphEicasLwrR",SECONDARY,true),
        new Field("IntegratedDisplaySystem$Box","idu",IDU,false),new Field("IntegratedDisplaySystem$Box","ea",EA,false),new Field("IntegratedDisplaySystem$Box","blankTime",CD_int,false),new Field("IntegratedDisplaySystem$Box","blankUpd",CD_boolean,false),new Field("IntegratedDisplaySystem$Box","showUpd",CD_boolean,false),
        new Field("EicasDisplaySecondary","eiuIsPowered",CD_boolean,false),new Field("EicasDisplaySecondary","eicasPage",ClassDesc.of("EicasPage"),false),new Field("EicasDisplaySecondary","blankUpd",CD_boolean,false),new Field("ElecAdapter","isPowered",CD_boolean,false));
    InstrumentLowerPrepare(Prepare base){this.base=base;}
    private MethodModel method(String owner,String name,String descriptor){MethodModel m=base.methods.get(new Prepare.MethodId(owner,name,descriptor));if(m==null||m.code().isEmpty())throw new IllegalArgumentException("Lower eligibility method "+owner+"."+name);return m;}
    void qualify(){
        for(Field expected:FIELDS){ClassModel type=base.models.get(expected.owner);if(type==null)throw new IllegalArgumentException("Lower eligibility owner "+expected.owner);
            FieldModel field=type.fields().stream().filter(f->f.fieldName().equalsString(expected.name)&&f.fieldType().equalsString(expected.type.descriptorString())).findFirst().orElseThrow(()->new IllegalArgumentException("Lower eligibility field "+expected.owner+"."+expected.name));
            if(((field.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)!=expected.isStatic)throw new IllegalArgumentException("Lower eligibility field staticness");
        }
        List<Instruction> box=Prepare.instructions(method("IntegratedDisplaySystem$Box","drawIdu","(Ljava/awt/Graphics2D;Z)V"));
        if(box.size()<3||!DisplayPrepare.field(box.get(1),"IntegratedDisplaySystem$Box","blankTime")||box.get(2).opcode()!=Opcode.IFLE||box.stream().filter(i->DisplayPrepare.field(i,"ElecAdapter","isPowered")).count()!=1)throw new IllegalArgumentException("Lower native timer/power consumers");
        List<Instruction> update=Prepare.instructions(method("IntegratedDisplaySystem$Box","runDelta","()V"));
        if(update.size()<6||!DisplayPrepare.field(update.get(2),"IntegratedDisplaySystem$Box","blankUpd")||!DisplayPrepare.field(update.get(5),"IntegratedDisplaySystem$Box","showUpd")||update.get(1).opcode()!=Opcode.ICONST_0||update.get(4).opcode()!=Opcode.ICONST_0)throw new IllegalArgumentException("Lower native transition clearing");
        List<Instruction> secondary=Prepare.instructions(method("EicasDisplaySecondary","drawIt","(Ljava/awt/Graphics2D;Ljava/awt/Point;Z)V"));
        int page=-1,power=-1;for(int n=0;n<secondary.size();n++){if(DisplayPrepare.field(secondary.get(n),"EicasDisplaySecondary","eicasPage")&&page<0)page=n;if(DisplayPrepare.field(secondary.get(n),"EicasDisplaySecondary","eiuIsPowered")&&power<0)power=n;}
        if(page<0||power<=page||secondary.get(page+1).opcode()!=Opcode.IFNULL)throw new IllegalArgumentException("Lower native page/power short circuit");
        writers();
        evidence.add("Lower snapshot: applied physical Box/ea/idu, Secondary EIU/page/clear; double collect, matching renderer identity, loader pause withholding; no native writes");
        evidence.add("DPI bit7 is pending Box blank/show OR Secondary clear update; it is not DPF blankAll");
        for(String owner:List.of("IntegratedDisplaySystem","IntegratedDisplaySystem$Box","EicasDisplaySecondary","ElecAdapter","Core"))evidence.add("Lower "+owner+" original.sha256="+IdentityHash(base.inputs.get(owner)));
    }
    private static String IdentityHash(byte[] bytes){return RecorderPrepare.hashUnchecked(bytes);}
    private void writers(){
        Set<String> allowed=Set.of("Core.<clinit>","Core.setLoaderPaused","Core.setPaused","Core.unpauseLoader", "IntegratedDisplaySystem.<clinit>","IntegratedDisplaySystem.assignBox",
            "IntegratedDisplaySystem$Box.<init>","IntegratedDisplaySystem$Box.runDelta","IntegratedDisplaySystem$Box$2.switching","EicasDisplaySecondary.<init>","EicasDisplaySecondary.setDataFromEiu",
            "ElecAdapter.<init>","ElecAdapter.setVoltAvailGetAmp","ElecAdapter$1.switching","CircuitBreakers.configP6_EquipCoolDiffPress","CircuitBreakers.configP6_FireDepress","CircuitBreakers.configP6_FireSwUnlock",
            "ElecControls.situLoadInit","Fire.checkLowerCargoExtPower","Fuel.powerEvent","Fuel$1.switching","Fuel$2.switching","Hydraulics.configOptions","Hydraulics$Sys.<init>");
        Map<String,Field> selected=new HashMap<>();for(Field f:FIELDS)selected.put(f.owner+"."+f.name,f);
        Set<String> observed=new TreeSet<>();
        for(var entry:base.methods.entrySet())if(entry.getValue().code().isPresent())for(Instruction i:Prepare.instructions(entry.getValue()))if(i instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC)){
            String key=f.owner().asInternalName()+"."+f.name().stringValue();Field expected=selected.get(key);if(expected==null)continue;
            if(!f.type().stringValue().equals(expected.type.descriptorString())||!allowed.contains(entry.getKey().owner()+"."+entry.getKey().name()))throw new IllegalArgumentException("Lower unqualified writer "+key+" "+entry.getKey());
            observed.add(key+" <- "+entry.getKey());
        }
        for(String item:observed)evidence.add("Lower writer "+item);
    }
    private static void flag(CodeBuilder cb,ClassDesc owner,String name,int bit){cb.aload(0).getfield(owner,name,CD_boolean).loadConstant(bit).imul();}
    private static void addBox(ClassBuilder builder){builder.withMethodBody(METHOD,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC,cb->{
        int adapter=cb.allocateLocal(TypeKind.REFERENCE),bits=cb.allocateLocal(TypeKind.INT);Label missing=cb.newLabel(),timerDone=cb.newLabel();
        cb.aload(0).getfield(BOX,"ea",EA).astore(adapter).aload(adapter).ifnull(missing).iconst_0().istore(bits);
        cb.aload(0).getfield(BOX,"blankTime",CD_int).ifle(timerDone).iconst_1().istore(bits).labelBinding(timerDone);
        cb.iload(bits).aload(adapter).getfield(EA,"isPowered",CD_boolean).loadConstant(8).imul().ior().istore(bits);
        cb.iload(bits);flag(cb,BOX,"blankUpd",128);flag(cb,BOX,"showUpd",128);cb.ior().ior().ireturn();
        cb.labelBinding(missing).loadConstant(Integer.MIN_VALUE).ireturn();
    });}
    private static void addSecondary(ClassBuilder builder){builder.withMethodBody(METHOD,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC,cb->{
        int bits=cb.allocateLocal(TypeKind.INT);Label noPage=cb.newLabel();flag(cb,SECONDARY,"eiuIsPowered",32);cb.istore(bits);
        cb.aload(0).getfield(SECONDARY,"eicasPage",ClassDesc.of("EicasPage")).ifnull(noPage).iload(bits).loadConstant(64).ior().istore(bits).labelBinding(noPage);
        cb.iload(bits);flag(cb,SECONDARY,"blankUpd",128);cb.ior().ireturn();
    });}
    private static void failureGuard(CodeBuilder cb,java.util.function.Consumer<CodeBuilder> body){cb.trying(c->body.accept(c),h->h.catching(CD_Throwable,c->{c.pop();c.trying(n->n.invokestatic(InstrumentPrepare.REC,"lowerFailed",InstrumentPrepare.VOID),f->f.catching(CD_Throwable,n->n.pop()));}));}
    private static void collect(CodeBuilder cb,int box,int renderer,int bits){
        cb.aload(box).invokevirtual(BOX,METHOD,MethodTypeDesc.of(CD_int)).istore(bits);
        cb.iload(bits).aload(renderer).checkcast(SECONDARY).invokevirtual(SECONDARY,METHOD,MethodTypeDesc.of(CD_int)).ior().istore(bits);
    }
    private static void addIds(ClassBuilder builder){builder.withMethodBody(METHOD,MethodTypeDesc.of(CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
        failureGuard(cb,c->{
            int box=c.allocateLocal(TypeKind.REFERENCE),renderer=c.allocateLocal(TypeKind.REFERENCE),assignment=c.allocateLocal(TypeKind.INT),first=c.allocateLocal(TypeKind.INT),second=c.allocateLocal(TypeKind.INT);
            Label unavailable=c.newLabel(),selected=c.newLabel();c.getstatic(ClassDesc.of("Core"),"loaderPaused",CD_boolean).ifne(unavailable);
            c.getstatic(IDS,"eicasLwrBox",BOX).astore(box).aload(box).ifnull(unavailable).aload(box).getfield(BOX,"idu",IDU).astore(renderer).aload(renderer).ifnull(unavailable);
            String[] renderers={"graphEicasLwr","graphEicasLwrL","graphEicasLwrR"};
            for(int n=0;n<3;n++){Label next=c.newLabel();c.aload(renderer).getstatic(IDS,renderers[n],SECONDARY).if_acmpne(next).loadConstant(10+n).istore(assignment).goto_(selected).labelBinding(next);}
            c.goto_(unavailable).labelBinding(selected);collect(c,box,renderer,first);c.iload(first).iflt(unavailable);
            c.invokestatic(ClassDesc.of("java.lang.invoke.VarHandle"),"fullFence",MethodTypeDesc.of(CD_void));collect(c,box,renderer,second);
            c.iload(first).iload(second).if_icmpne(unavailable).getstatic(ClassDesc.of("Core"),"loaderPaused",CD_boolean).ifne(unavailable);
            c.aload(box).getstatic(IDS,"eicasLwrBox",BOX).if_acmpne(unavailable).aload(renderer).aload(box).getfield(BOX,"idu",IDU).if_acmpne(unavailable);
            c.loadConstant((1L<<24)|((long)SEEN<<8)).iload(assignment).i2l().lor().iload(first).i2l().loadConstant(16).lshl().lor().lreturn();
            c.labelBinding(unavailable).loadConstant(0L).lreturn();
        });cb.loadConstant(0L).lreturn();
    });}
    Map<String,byte[]> transform(Map<String,byte[]> lower){Map<String,byte[]> result=new TreeMap<>();
        for(String owner:OWNERS){byte[] before=lower.getOrDefault(owner,base.inputs.get(owner));ClassModel model=base.cf.parse(before);
            if(model.methods().stream().anyMatch(m->m.methodName().equalsString(METHOD)))throw new IllegalArgumentException("Lower helper collision "+owner);
            byte[] value=base.cf.transformClass(model,ClassTransform.endHandler(b->{if(owner.equals("IntegratedDisplaySystem"))addIds(b);else if(owner.equals("IntegratedDisplaySystem$Box"))addBox(b);else addSecondary(b);}));
            if(!base.cf.verify(value).isEmpty())throw new IllegalArgumentException("Lower helper bytecode "+owner);result.put(owner,value);
        }return result;
    }
}
