package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound actual-value readback, after complete native FMC subset selection.
 * Copies primitive/reference state only; no target argument, flag consumption,
 * monitor, allocation, publication or native scheduler/dynamics invocation. */
public final class IosFmcReadbackPrepare {
    public static final String READ="psxExporterIosReadFmcMode",OBSERVE="psxExporterIosObserveFmc";
    public record Selection(ClassDesc global,ClassDesc position,ClassDesc qi,ClassDesc glob,ClassDesc state){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static void field(ClassModel c,String name,ClassDesc type,boolean statik,boolean publicAccess){
        var all=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();require(all.size()==1,"Missing FMC observation field "+name);var f=all.getFirst();int flags=f.flags().flagsMask();
        require(f.fieldTypeSymbol().equals(type)&&((flags&ClassFile.ACC_STATIC)!=0)==statik&&(flags&ClassFile.ACC_FINAL)==0&&
            (publicAccess?(flags&ClassFile.ACC_PUBLIC)!=0:(flags&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible FMC observation field "+name);
    }
    public static Selection select(ClassModel global,ClassModel position,ClassModel aero,ClassModel qi,ClassModel q,ClassModel glob,ClassModel network,ClassModel state){
        IosFmcModePrepare.select(qi,q,glob,network);IosFmcBookkeepingPrepare.select(global,position,aero,glob,qi);
        for(var m:qi.methods())require(!m.methodName().equalsString(READ),"Existing FMC actual-mode getter");
        for(var m:global.methods())require(!m.methodName().equalsString(OBSERVE),"Existing FMC observation binding");
        require((state.flags().flagsMask()&(ClassFile.ACC_PUBLIC|ClassFile.ACC_INTERFACE))==ClassFile.ACC_PUBLIC,"Readback class must be public concrete project state");
        for(String name:List.of("lat","lon","biasDist","distForNet"))field(position,name,CD_double,false,false);
        field(qi,"min",CD_int,false,false);field(qi,"max",CD_int,false,false);
        for(String name:List.of("validity","registerValue","cachedMode","timer","serial","min","max"))field(state,name,CD_int,false,true);
        for(String name:List.of("fresh","updated"))field(state,name,CD_boolean,false,true);
        for(String name:List.of("register","left","right"))field(state,name,CD_Object,false,true);
        for(String name:List.of("leftLatitude","leftLongitude","leftBias","leftNetworkDistance","rightLatitude","rightLongitude","rightBias","rightNetworkDistance"))field(state,name,CD_double,false,true);
        return new Selection(global.thisClass().asSymbol(),position.thisClass().asSymbol(),qi.thisClass().asSymbol(),glob.thisClass().asSymbol(),state.thisClass().asSymbol());
    }
    /** Prepare the reader first, then the separately qualified mode helper.
     * Only the new getter is added; original Qi transfer is unchanged. */
    public static byte[] prepareQi(ClassFile cf,ClassModel global,ClassModel position,ClassModel aero,ClassModel qi,ClassModel q,ClassModel glob,ClassModel network,ClassModel state,Selection s){
        require(select(global,position,aero,qi,q,glob,network,state).equals(s),"Reader preparation differs from full qualification");
        return cf.transformClass(qi,ClassTransform.endHandler(b->b.withMethodBody(READ,MethodTypeDesc.of(CD_int),ClassFile.ACC_PUBLIC|ClassFile.ACC_FINAL|ClassFile.ACC_SYNTHETIC,c->
            c.aload(0).getfield(s.qi,"iVar",CD_int).ireturn())));
    }
    /** Prepare observation before bookkeeping. Each original input must have
     * passed select; external composition must verify all generated bindings. */
    public static byte[] prepareGlobal(ClassFile cf,ClassModel global,ClassModel position,ClassModel aero,ClassModel qi,ClassModel q,ClassModel glob,ClassModel network,ClassModel state,Selection s){
        require(select(global,position,aero,qi,q,glob,network,state).equals(s),"Observer preparation differs from full qualification");
        return cf.transformClass(global,ClassTransform.endHandler(b->b.withMethodBody(OBSERVE,MethodTypeDesc.of(CD_void,s.state),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var done=c.newLabel();c.aload(0).ifnull(done).aload(0).iconst_0().putfield(s.state,"validity",CD_int)
                .getstatic(s.glob,"qi",s.qi.arrayType()).ifnull(done).getstatic(s.glob,"qiFmcPosBiasMode",CD_int).iflt(done)
                .getstatic(s.glob,"qiFmcPosBiasMode",CD_int).getstatic(s.glob,"qi",s.qi.arrayType()).arraylength().if_icmpge(done)
                .getstatic(s.glob,"qi",s.qi.arrayType()).getstatic(s.glob,"qiFmcPosBiasMode",CD_int).aaload().astore(1).aload(1).ifnull(done)
                .getstatic(s.global,"posL",s.position).astore(2).aload(2).ifnull(done).getstatic(s.global,"posR",s.position).astore(3).aload(3).ifnull(done);
            c.aload(0).aload(1).putfield(s.state,"register",CD_Object).aload(0).aload(2).putfield(s.state,"left",CD_Object).aload(0).aload(3).putfield(s.state,"right",CD_Object)
                .aload(0).aload(1).invokevirtual(s.qi,READ,MethodTypeDesc.of(CD_int)).putfield(s.state,"registerValue",CD_int);
            for(String name:List.of("serialNr","min","max"))c.aload(0).aload(1).getfield(s.qi,name,CD_int).putfield(s.state,name.equals("serialNr")?"serial":name,CD_int);
            for(String name:List.of("fresh","updated"))c.aload(0).aload(1).getfield(s.qi,name,CD_boolean).putfield(s.state,name,CD_boolean);
            c.aload(0).getstatic(s.global,"fmcPosBiasMode",CD_int).putfield(s.state,"cachedMode",CD_int)
                .aload(0).getstatic(s.global,"situInitStabilizeTimer",CD_int).putfield(s.state,"timer",CD_int);
            for(int side=2;side<=3;side++){
                String prefix=side==2?"left":"right";
                for(var pair:List.of(new String[]{"lat","Latitude"},new String[]{"lon","Longitude"},new String[]{"biasDist","Bias"},new String[]{"distForNet","NetworkDistance"}))
                    c.aload(0).aload(side).getfield(s.position,pair[0],CD_double).putfield(s.state,prefix+pair[1],CD_double);
            }
            c.aload(0).bipush(127).putfield(s.state,"validity",CD_int).labelBinding(done).return_();
        })));
    }
    private IosFmcReadbackPrepare(){}
}
