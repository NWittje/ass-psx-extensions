package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Native pause housekeeping, executed AFTER owner exclusion is released.
 * No mouse/UI reset, network operation or whole native pause setter is invoked.
 * Each remaining effect checks the closing edit revision; an effect already in
 * progress is not rolled back. Native receiver/context qualification is separate. */
public final class IosPauseEffectsPrepare {
    public static final String EFFECTS="psxExporterIosPauseEffects";
    private static final ClassDesc CORE=ClassDesc.of("Core"),SOUND=ClassDesc.of("Sound"),CACHE=ClassDesc.of("Sound$Cache"),
        WAI=ClassDesc.of("OvhdWaiNai"),WIPER=ClassDesc.of("OvhdWaiNai$Wiper"),FUEL=ClassDesc.of("Fuel");
    public record Selection(List<String> effects,List<Integer> nativeSites){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    public static Selection select(Map<String,ClassModel> models){
        for(String name:List.of("Core","Sound","Sound$Cache","RadioIdent","OvhdWaiNai","OvhdWaiNai$Wiper","Elt","Fuel"))
            require(models.containsKey(name),"Missing pause effect owner "+name);
        for(String name:List.of("rain","con1020"))memberField(models.get("Sound"),name,"LSound$Cache;",true,false);
        memberField(models.get("RadioIdent"),"bfoVol0","F",true,false);
        for(String name:List.of("wiperL","wiperR"))memberField(models.get("OvhdWaiNai"),name,"LOvhdWaiNai$Wiper;",true,false);
        memberField(models.get("Fuel"),"fuelBlitz","I",true,true);
        memberMethod(models.get("Sound"),"setGain","(LSound$Cache;F)V",true);
        memberMethod(models.get("OvhdWaiNai$Wiper"),"checkContinuousSound","()V",false);
        memberMethod(models.get("Elt"),"resetSound","()V",true);
        return select(models.get("Core"));
    }
    private static void memberField(ClassModel owner,String name,String type,boolean statik,boolean write){
        var f=owner.fields().stream().filter(x->x.fieldName().equalsString(name)).findFirst().orElseThrow();int flags=f.flags().flagsMask();
        require(f.fieldType().equalsString(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&ClassFile.ACC_PRIVATE)==0&&(!write||(flags&ClassFile.ACC_FINAL)==0),"Inaccessible pause effect field "+name);
    }
    private static void memberMethod(ClassModel owner,String name,String type,boolean statik){
        var m=owner.methods().stream().filter(x->x.methodName().equalsString(name)&&x.methodType().equalsString(type)).findFirst().orElseThrow();int flags=m.flags().flagsMask();
        require(((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&(ClassFile.ACC_PRIVATE|ClassFile.ACC_ABSTRACT))==0,"Inaccessible pause effect method "+name);
    }
    public static Selection select(ClassModel core){
        require(core.methods().stream().noneMatch(m->m.methodName().equalsString(EFFECTS)),"Pause effects already prepared");
        var m=core.methods().stream().filter(x->x.methodName().equalsString("setPaused")&&x.methodType().equalsString("(I)V")).findFirst().orElseThrow();
        require((m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_NATIVE))==ClassFile.ACC_STATIC,"Changed native pause effects context");
        var calls=new ArrayList<String>();var writes=new ArrayList<String>();var sites=new ArrayList<Integer>();int offset=0;boolean twelve=false;
        for(var i:IosComposedBody.nativeInstructions(m)){
            if(i instanceof ConstantInstruction c&&Objects.equals(c.constantValue(),12))twelve=true;
            if(i instanceof InvokeInstruction c){calls.add(c.owner().asInternalName()+"."+c.name()+c.type());sites.add(offset);}
            if(i instanceof FieldInstruction f&&(i.opcode()==Opcode.PUTSTATIC||i.opcode()==Opcode.PUTFIELD))writes.add(f.owner().asInternalName()+"."+f.name()+":"+f.type());
            offset+=i.sizeInBytes();
        }
        require(calls.equals(List.of("Sound.setGain(LSound$Cache;F)V","Sound.setGain(LSound$Cache;F)V",
            "OvhdWaiNai$Wiper.checkContinuousSound()V","OvhdWaiNai$Wiper.checkContinuousSound()V","Elt.resetSound()V",
            "PsxMouseMotion.resetAllMouseFlags(Z)V","PsxMouse.resetAllHoldFlags()V","Core.setMouseCursor(Ljava/awt/Cursor;)V")),"Changed native pause edge ordering");
        require(twelve&&writes.equals(List.of("Core.paused:Z","Core.loaderPaused:Z","Fuel.fuelBlitz:I")),"Changed native pause housekeeping write set");
        var reference=ClassFile.of().parse(reference(ClassFile.of())).methods().getFirst();
        require(IosComposedBody.nativeIdentity(m).equals(IosMethodIdentity.of(reference,i->false)),"Changed complete native pause branch/receiver/effect ordering");
        return new Selection(List.copyOf(calls),List.copyOf(sites));
    }
    private static byte[] reference(ClassFile cf){return cf.build(CORE,b->b.withMethodBody("setPaused",MethodTypeDesc.of(CD_void,CD_int),ClassFile.ACC_STATIC,c->{
        var zero=c.newLabel();var changed=c.newLabel();var stateZero=c.newLabel();var state=c.newLabel();var radio=c.newLabel();var gain=c.newLabel();var bookkeeping=c.newLabel();var paused=c.newLabel();var mouse=c.newLabel();
        c.getstatic(CORE,"paused",CD_boolean).iload(0).ifeq(zero).iconst_1().goto_(changed).labelBinding(zero).iconst_0().labelBinding(changed).if_icmpeq(bookkeeping)
            .iload(0).ifeq(stateZero).iconst_1().goto_(state).labelBinding(stateZero).iconst_0().labelBinding(state).putstatic(CORE,"paused",CD_boolean)
            .getstatic(SOUND,"rain",CACHE).fconst_0().invokestatic(SOUND,"setGain",MethodTypeDesc.of(CD_void,CACHE,CD_float))
            .getstatic(SOUND,"con1020",CACHE).getstatic(CORE,"paused",CD_boolean).ifeq(radio).fconst_0().goto_(gain)
            .labelBinding(radio).getstatic(ClassDesc.of("RadioIdent"),"bfoVol0",CD_float).labelBinding(gain).invokestatic(SOUND,"setGain",MethodTypeDesc.of(CD_void,CACHE,CD_float))
            .getstatic(WAI,"wiperL",WIPER).invokevirtual(WIPER,"checkContinuousSound",MethodTypeDesc.of(CD_void))
            .getstatic(WAI,"wiperR",WIPER).invokevirtual(WIPER,"checkContinuousSound",MethodTypeDesc.of(CD_void)).invokestatic(ClassDesc.of("Elt"),"resetSound",MethodTypeDesc.of(CD_void))
            .labelBinding(bookkeeping).getstatic(CORE,"paused",CD_boolean).ifne(paused).iconst_0().putstatic(CORE,"loaderPaused",CD_boolean).goto_(mouse)
            .labelBinding(paused).bipush(12).putstatic(FUEL,"fuelBlitz",CD_int).labelBinding(mouse)
            .iconst_1().invokestatic(ClassDesc.of("PsxMouseMotion"),"resetAllMouseFlags",MethodTypeDesc.of(CD_void,CD_boolean))
            .invokestatic(ClassDesc.of("PsxMouse"),"resetAllHoldFlags",MethodTypeDesc.of(CD_void))
            .getstatic(ClassDesc.of("Pic"),"mouDefault",ClassDesc.of("java.awt.Cursor")).invokestatic(CORE,"setMouseCursor",MethodTypeDesc.of(CD_void,ClassDesc.of("java.awt.Cursor"))).return_();
    }));}
    public static byte[] prepare(ClassFile cf,ClassModel core){return cf.transformClass(core,ClassTransform.endHandler(b->
        b.withMethodBody(EFFECTS,MethodTypeDesc.of(CD_boolean,CD_int,CD_boolean,CD_long),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var changed=c.newLabel();var resumed=c.newLabel();var radio=c.newLabel();var gain=c.newLabel();
            var bookkeeping=c.newLabel();
            owned(c,changed);c.iload(1).ifeq(bookkeeping);
            c.getstatic(SOUND,"rain",CACHE).fconst_0().invokestatic(SOUND,"setGain",MethodTypeDesc.of(CD_void,CACHE,CD_float));
            owned(c,changed);c.getstatic(SOUND,"con1020",CACHE).getstatic(CORE,"paused",CD_boolean).ifeq(radio).fconst_0().goto_(gain)
                .labelBinding(radio).getstatic(ClassDesc.of("RadioIdent"),"bfoVol0",CD_float).labelBinding(gain)
                .invokestatic(SOUND,"setGain",MethodTypeDesc.of(CD_void,CACHE,CD_float));
            owned(c,changed);c.getstatic(WAI,"wiperL",WIPER).invokevirtual(WIPER,"checkContinuousSound",MethodTypeDesc.of(CD_void));
            owned(c,changed);c.getstatic(WAI,"wiperR",WIPER).invokevirtual(WIPER,"checkContinuousSound",MethodTypeDesc.of(CD_void));
            owned(c,changed);c.invokestatic(ClassDesc.of("Elt"),"resetSound",MethodTypeDesc.of(CD_void)).labelBinding(bookkeeping);
            owned(c,changed);c.getstatic(CORE,"paused",CD_boolean).ifeq(resumed).bipush(12).putstatic(FUEL,"fuelBlitz",CD_int)
                .labelBinding(resumed).iconst_1().ireturn().labelBinding(changed).iconst_0().ireturn();
        })));
    }
    private static void owned(CodeBuilder c,Label changed){
        ClassDesc exclusion=ClassDesc.of("psx.exporter.IosExclusion"),gate=ClassDesc.of("psx.ios.IosMutationGate");
        c.invokestatic(exclusion,"gate",MethodTypeDesc.of(gate)).invokevirtual(gate,"qualified",MethodTypeDesc.of(CD_boolean)).ifeq(changed)
            .invokestatic(exclusion,"gate",MethodTypeDesc.of(gate)).invokevirtual(gate,"revision",MethodTypeDesc.of(CD_long)).lload(2).lcmp().ifne(changed)
            .getstatic(CORE,"globSimPaused",CD_int).iload(0).if_icmpne(changed)
            .getstatic(CORE,"paused",CD_boolean).iload(0).iconst_1().iand().if_icmpne(changed)
            .getstatic(CORE,"loaderPaused",CD_boolean).ifne(changed);
    }
    public static void verifyPrepared(ClassFile cf,ClassModel before,ClassModel after){
        var reference=cf.parse(prepare(cf,before)).methods().stream().filter(m->m.methodName().equalsString(EFFECTS)).findFirst().orElseThrow();
        var actual=after.methods().stream().filter(m->m.methodName().equalsString(EFFECTS)).toList();
        require(actual.size()==1&&IosMethodIdentity.of(reference,i->false).equals(IosMethodIdentity.of(actual.getFirst(),i->false)),
            "Changed pause effect signature, ordering, owned revision or native state checks");
    }
    private IosPauseEffectsPrepare(){}
}
