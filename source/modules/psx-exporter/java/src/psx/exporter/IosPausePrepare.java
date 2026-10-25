package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound primitive pause bridge preparation. Installed classes remain data.
 * This qualifies field access and bounded commit/readback instructions only.
 * Complete writer/scheduler exclusion, safe publication and native application
 * qualification are prerequisites to binding; this is not a capability. */
public final class IosPausePrepare {
    public static final String VALUE="psxExporterIosPauseValue",COMMIT="psxExporterIosPauseCommit",
        FULL="psxExporterIosFullPause",OBSERVE="psxExporterIosObserveFullPause";
    private static final int FLAGS=ClassFile.ACC_PUBLIC|ClassFile.ACC_SYNTHETIC;
    private static final MethodTypeDesc GET=MethodTypeDesc.of(CD_int),SET=MethodTypeDesc.of(CD_int,CD_int);
    public record Layout(ClassDesc core,ClassDesc qi,ClassDesc q,ClassDesc glob){}
    public record Selection(Layout layout,int nativeValueWrite,int nativePauseWrite,int nativeMaskWrite){}
    private static void require(boolean ok,String why){if(!ok)throw new IllegalArgumentException(why);}
    private static FieldModel field(ClassModel c,String name,String descriptor,boolean stat,boolean foreign){
        var matches=c.fields().stream().filter(f->f.fieldName().equalsString(name)).toList();
        require(matches.size()==1,"Missing/duplicate pause field: "+name);var f=matches.getFirst();
        require(f.fieldType().equalsString(descriptor)&&((f.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==stat&&
            (f.flags().flagsMask()&ClassFile.ACC_FINAL)==0&&(!foreign||(f.flags().flagsMask()&ClassFile.ACC_PRIVATE)==0),"Changed/inaccessible pause field: "+name);
        return f;
    }
    private static MethodModel method(ClassModel c,String name,String descriptor){
        var matches=c.methods().stream().filter(m->m.methodName().equalsString(name)).toList();
        require(matches.size()==1,"Missing/overloaded pause method: "+name);var m=matches.getFirst();
        require(m.methodType().equalsString(descriptor)&&m.code().isPresent()&&
            (m.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED|ClassFile.ACC_PRIVATE))==0,"Changed pause method binding: "+name);
        return m;
    }
    private static int write(MethodModel m,ClassDesc owner,String name,String descriptor,Opcode opcode){
        int offset=0,found=-1;
        for(var e:m.code().orElseThrow())if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f&&f.opcode()==opcode&&f.owner().asSymbol().equals(owner)&&f.name().equalsString(name)&&f.type().equalsString(descriptor)){
                require(found==-1,"Duplicate critical pause write: "+name);found=offset;
            }
            offset+=i.sizeInBytes();
        }
        require(found>=0,"Missing critical pause write: "+name);return found;
    }
    private static String packageOf(ClassDesc c){String n=c.descriptorString();return n.substring(0,n.lastIndexOf('/')+1);}
    private static void noCollision(ClassModel c,String... names){for(var m:c.methods())for(String name:names)require(!m.methodName().equalsString(name),"Existing IOS pause bridge");}
    public static Selection select(ClassModel core,ClassModel qi,ClassModel q,ClassModel glob){
        var l=new Layout(core.thisClass().asSymbol(),qi.thisClass().asSymbol(),q.thisClass().asSymbol(),glob.thisClass().asSymbol());
        for(var type:List.of(l.qi,l.q,l.glob))require(packageOf(type).equals(packageOf(l.core)),"Pause dependencies are not in one native package");
        require(qi.superclass().isPresent()&&qi.superclass().orElseThrow().asSymbol().equals(l.q),"Changed Qi superclass");
        for(String name:List.of("serialNr","fresh","updated"))
            require(qi.fields().stream().noneMatch(f->f.fieldName().equalsString(name)),"Shadowed inherited register field: "+name);
        for(String name:List.of("paused","loaderPaused"))field(core,name,"Z",true,false);
        for(String name:List.of("globSimPaused","pauseDelay","situLoadedPauseTimer","situLoadedPauseCode"))field(core,name,"I",true,false);
        field(qi,"iVar","I",false,false);field(qi,"min","I",false,false);field(qi,"max","I",false,false);
        field(q,"serialNr","I",false,true);field(q,"fresh","Z",false,true);field(q,"updated","Z",false,true);
        field(glob,"qi",l.qi.arrayType().descriptorString(),true,true);field(glob,"qiSimPaused","I",true,true);
        noCollision(core,FULL,OBSERVE);noCollision(qi,VALUE,COMMIT);
        var transfer=method(qi,"transfer","(ICI)I");require((transfer.flags().flagsMask()&ClassFile.ACC_STATIC)==0,"Changed Qi transfer staticness");
        var paused=method(core,"setPaused","(I)V");var mask=method(core,"monitorSimSpeedChanges","()V");var read=method(core,"isPaused","()Z");
        for(var m:List.of(paused,mask,read))require((m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"Changed Core pause staticness");
        // Qualify the complete pure pause observation, including both flags and
        // branch destinations. Do not call the native setter or transfer body.
        var expected=ClassFile.of().build(l.core,b->b.withMethodBody("isPaused",MethodTypeDesc.of(CD_boolean),ClassFile.ACC_STATIC,c->{
            var yes=c.newLabel();var no=c.newLabel();var end=c.newLabel();
            c.getstatic(l.core,"paused",CD_boolean).ifne(yes).getstatic(l.core,"loaderPaused",CD_boolean).ifeq(no)
             .labelBinding(yes).iconst_1().goto_(end).labelBinding(no).iconst_0().labelBinding(end).ireturn();
        }));
        require(identity(read).equals(identity(ClassFile.of().parse(expected).methods().getFirst())),"Changed effective pause observation");
        return new Selection(l,write(transfer,l.qi,"iVar","I",Opcode.PUTFIELD),
            write(paused,l.core,"paused","Z",Opcode.PUTSTATIC),write(mask,l.core,"globSimPaused","I",Opcode.PUTSTATIC));
    }
    private static List<String> identity(MethodModel m){
        require(m.code().orElseThrow().exceptionHandlers().isEmpty(),"Pause observation handler changed");
        var labels=new HashMap<Label,Integer>();int count=0;
        for(var e:m.code().orElseThrow()){if(e instanceof LabelTarget t)labels.put(t.label(),count);if(e instanceof Instruction)count++;}
        var out=new ArrayList<String>();
        for(var e:m.code().orElseThrow())if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)out.add(f.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof BranchInstruction b)out.add(b.opcode()+":"+labels.get(b.target()));
            else out.add(i.opcode().name());
        }
        return out;
    }
    public static byte[] qi(ClassFile cf,ClassModel model,Selection s){
        require(model.thisClass().asSymbol().equals(s.layout.qi),"Wrong Qi preparation input");noCollision(model,VALUE,COMMIT);var l=s.layout;
        return cf.transformClass(model,ClassTransform.endHandler(b->{
            b.withMethodBody(VALUE,GET,FLAGS|ClassFile.ACC_FINAL,c->{
                var reject=c.newLabel();
                c.aload(0).getfield(l.q,"serialNr",CD_int).loadConstant(129).if_icmpne(reject)
                 .aload(0).getfield(l.qi,"min",CD_int).ifne(reject)
                 .aload(0).getfield(l.qi,"max",CD_int).iconst_3().if_icmpne(reject)
                 .aload(0).getfield(l.qi,"iVar",CD_int).ireturn().labelBinding(reject).iconst_m1().ireturn();
            });
            b.withMethodBody(COMMIT,SET,FLAGS|ClassFile.ACC_FINAL,c->{
                var reject=c.newLabel();var unchanged=c.newLabel();
                c.iload(1).iflt(reject).iload(1).iconst_3().if_icmpgt(reject)
                 .aload(0).getfield(l.q,"serialNr",CD_int).loadConstant(129).if_icmpne(reject)
                 .aload(0).getfield(l.qi,"min",CD_int).ifne(reject)
                 .aload(0).getfield(l.qi,"max",CD_int).iconst_3().if_icmpne(reject)
                 .aload(0).getfield(l.qi,"iVar",CD_int).iload(1).if_icmpne(reject);
                c.iload(1).iconst_2().iand().iconst_1().ior().istore(2);
                c.aload(0).getfield(l.qi,"iVar",CD_int).iload(2).if_icmpeq(unchanged)
                 .aload(0).iload(2).putfield(l.qi,"iVar",CD_int)
                 .aload(0).iconst_1().putfield(l.q,"updated",CD_boolean).labelBinding(unchanged)
                 .aload(0).iconst_1().putfield(l.q,"fresh",CD_boolean).iload(2).ireturn()
                 .labelBinding(reject).iconst_m1().ireturn();
            });
        }));
    }
    // All array/object validation precedes the first write. This never adopts
    // an owner or acquires a Qi monitor. The external qualified gate is required.
    private static void register(CodeBuilder c,Layout l,Label reject){
        c.getstatic(l.glob,"qi",l.qi.arrayType()).astore(1).aload(1).ifnull(reject)
         .getstatic(l.glob,"qiSimPaused",CD_int).istore(2).iload(2).iflt(reject)
         .iload(2).aload(1).arraylength().if_icmpge(reject)
         .aload(1).iload(2).aaload().astore(3).aload(3).ifnull(reject);
    }
    public static byte[] core(ClassFile cf,ClassModel model,Selection s){
        require(model.thisClass().asSymbol().equals(s.layout.core),"Wrong Core preparation input");noCollision(model,FULL,OBSERVE);var l=s.layout;
        return cf.transformClass(model,ClassTransform.endHandler(b->{
            b.withMethodBody(FULL,SET,FLAGS|ClassFile.ACC_STATIC,c->{
                var reject=c.newLabel();
                c.iload(0).iflt(reject).iload(0).iconst_3().if_icmpgt(reject)
                 .getstatic(l.core,"loaderPaused",CD_boolean).ifne(reject)
                 // A completed loader can still have a scheduled pause-mask
                 // restoration. Do not overwrite/cancel it or race its tail.
                 .getstatic(l.core,"situLoadedPauseTimer",CD_int).ifne(reject)
                 .getstatic(l.core,"situLoadedPauseCode",CD_int).ifne(reject)
                 .getstatic(l.core,"globSimPaused",CD_int).iload(0).if_icmpne(reject);
                register(c,l,reject);
                c.aload(3).iload(0).invokevirtual(l.qi,COMMIT,SET).istore(4).iload(4).iflt(reject)
                 .iload(4).putstatic(l.core,"globSimPaused",CD_int).iconst_1().putstatic(l.core,"paused",CD_boolean)
                 .iconst_0().putstatic(l.core,"pauseDelay",CD_int).iload(4).ireturn().labelBinding(reject).iconst_m1().ireturn();
            });
            b.withMethodBody(OBSERVE,GET,FLAGS|ClassFile.ACC_STATIC,c->{
                var reject=c.newLabel();
                c.getstatic(l.core,"paused",CD_boolean).ifeq(reject).getstatic(l.core,"loaderPaused",CD_boolean).ifne(reject)
                 .getstatic(l.core,"pauseDelay",CD_int).ifne(reject)
                 .getstatic(l.core,"situLoadedPauseTimer",CD_int).ifne(reject)
                 .getstatic(l.core,"situLoadedPauseCode",CD_int).ifne(reject);
                register(c,l,reject);
                c.aload(3).invokevirtual(l.qi,VALUE,GET).istore(4).iload(4).iflt(reject).iload(4).iconst_3().if_icmpgt(reject)
                 .iload(4).iconst_1().iand().ifeq(reject)
                 .iload(4).getstatic(l.core,"globSimPaused",CD_int).if_icmpne(reject).iload(4).ireturn()
                 .labelBinding(reject).iconst_m1().ireturn();
            });
        }));
    }
    private IosPausePrepare(){}
}
