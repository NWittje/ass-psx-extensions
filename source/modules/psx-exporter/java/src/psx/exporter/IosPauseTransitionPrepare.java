package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound expected-mask transition required by the two-stage operation.
 * Selection reuses the qualified native pause access/writer contracts. Bodies
 * contain no monitor/IO/allocation; complete external exclusion is mandatory.
 */
public final class IosPauseTransitionPrepare {
    public static final String COMMIT="psxExporterIosPauseTransition",SET="psxExporterIosSetPauseMask",READ="psxExporterIosObservePauseMask";
    private static final MethodTypeDesc TWO=MethodTypeDesc.of(CD_int,CD_int,CD_int),GET=MethodTypeDesc.of(CD_int);
    private static final int FLAGS=ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC;
    private IosPauseTransitionPrepare(){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static void collision(ClassModel c,String... names){for(var m:c.methods())for(String n:names)require(!m.methodName().equalsString(n),"Pause transition already prepared");}
    public static IosPausePrepare.Selection select(ClassModel core,ClassModel qi,ClassModel q,ClassModel glob){
        collision(core,SET,READ);collision(qi,COMMIT);return IosPausePrepare.select(core,qi,q,glob);
    }
    public static byte[] qi(ClassFile cf,ClassModel model,IosPausePrepare.Selection s){
        var l=s.layout();require(model.thisClass().asSymbol().equals(l.qi()),"Wrong transition Qi");collision(model,COMMIT);
        return cf.transformClass(model,ClassTransform.endHandler(b->b.withMethodBody(COMMIT,TWO,FLAGS|ClassFile.ACC_FINAL,c->{
            var reject=c.newLabel();var same=c.newLabel();
            c.iload(1).iflt(reject).iload(1).iconst_3().if_icmpgt(reject)
             .iload(2).iconst_1().if_icmplt(reject).iload(2).iconst_3().if_icmpgt(reject)
             .aload(0).getfield(l.q(),"serialNr",CD_int).loadConstant(129).if_icmpne(reject)
             .aload(0).getfield(l.qi(),"min",CD_int).ifne(reject)
             .aload(0).getfield(l.qi(),"max",CD_int).iconst_3().if_icmpne(reject)
             .aload(0).getfield(l.qi(),"iVar",CD_int).iload(1).if_icmpne(reject)
             .iload(1).iload(2).if_icmpeq(same)
             .aload(0).iload(2).putfield(l.qi(),"iVar",CD_int)
             .aload(0).iconst_1().putfield(l.q(),"updated",CD_boolean).labelBinding(same)
             .aload(0).iconst_1().putfield(l.q(),"fresh",CD_boolean).iload(2).ireturn()
             .labelBinding(reject).iconst_m1().ireturn();
        })));
    }
    private static void register(CodeBuilder c,IosPausePrepare.Layout l,Label reject){
        c.getstatic(l.glob(),"qi",l.qi().arrayType()).astore(2).aload(2).ifnull(reject)
         .getstatic(l.glob(),"qiSimPaused",CD_int).istore(3).iload(3).iflt(reject)
         .iload(3).aload(2).arraylength().if_icmpge(reject).aload(2).iload(3).aaload().astore(4).aload(4).ifnull(reject);
    }
    private static void pending(CodeBuilder c,IosPausePrepare.Layout l,Label reject){
        c.getstatic(l.core(),"loaderPaused",CD_boolean).ifne(reject)
         .getstatic(l.core(),"situLoadedPauseTimer",CD_int).ifne(reject)
         .getstatic(l.core(),"situLoadedPauseCode",CD_int).ifne(reject)
         .getstatic(l.core(),"pauseDelay",CD_int).ifne(reject);
    }
    public static byte[] core(ClassFile cf,ClassModel model,IosPausePrepare.Selection s){
        var l=s.layout();require(model.thisClass().asSymbol().equals(l.core()),"Wrong transition Core");collision(model,SET,READ);
        return cf.transformClass(model,ClassTransform.endHandler(b->{
            b.withMethodBody(SET,TWO,FLAGS|ClassFile.ACC_STATIC,c->{
                var reject=c.newLabel();
                c.iload(0).iflt(reject).iload(0).iconst_3().if_icmpgt(reject)
                 .iload(1).iconst_1().if_icmplt(reject).iload(1).iconst_3().if_icmpgt(reject);
                pending(c,l,reject);
                c.getstatic(l.core(),"globSimPaused",CD_int).iload(0).if_icmpne(reject)
                 .getstatic(l.core(),"paused",CD_boolean).iload(0).iconst_1().iand().if_icmpne(reject);
                register(c,l,reject);
                c.aload(4).iload(0).iload(1).invokevirtual(l.qi(),COMMIT,TWO).istore(5).iload(5).iflt(reject)
                 .iload(5).putstatic(l.core(),"globSimPaused",CD_int)
                 .iload(5).iconst_1().iand().putstatic(l.core(),"paused",CD_boolean)
                 .iload(5).ireturn().labelBinding(reject).iconst_m1().ireturn();
            });
            b.withMethodBody(READ,GET,FLAGS|ClassFile.ACC_STATIC,c->{
                var reject=c.newLabel();pending(c,l,reject);register(c,l,reject);
                c.aload(4).invokevirtual(l.qi(),IosPausePrepare.VALUE,GET).istore(5)
                 .iload(5).iflt(reject).iload(5).iconst_3().if_icmpgt(reject)
                 .iload(5).getstatic(l.core(),"globSimPaused",CD_int).if_icmpne(reject)
                 .getstatic(l.core(),"paused",CD_boolean).iload(5).iconst_1().iand().if_icmpne(reject)
                 .iload(5).ireturn().labelBinding(reject).iconst_m1().ireturn();
            });
        }));
    }
}
