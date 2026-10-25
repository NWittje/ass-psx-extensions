package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional transaction over prepared bytes. Installed simulator classes are never defined. */
final class DisplayPrepare {
    static final String PREFIX="$psxDisplay$";
    static final ClassDesc PROBE=ClassDesc.of("psx.exporter.DisplayProbe");
    static final MethodTypeDesc EVENT=MethodTypeDesc.of(CD_void,CD_int,CD_int), VOID=MethodTypeDesc.of(CD_void);
    static final String[] BOXES={"outbdCpBox","inbdCpBox","inbdFoBox","outbdFoBox","eicasUprBox","eicasLwrBox"};
    static final String[] ASSIGNMENTS={"graphPfdCp","graphPfdFo","graphNdCp","graphNdFo","graphNdCp2","graphNdFo2","graphEicasUpr","graphEicasUprL","graphEicasUprR","graphEicasLwr","graphEicasLwrL","graphEicasLwrR"};
    static final String[] ROOTS={"PrimaryFlightDisplay","NavigationDisplay","EicasDisplayPrimary","EicasDisplaySecondary"};
    final Prepare base;final List<String> evidence=new ArrayList<>();
    final Set<String> required=new TreeSet<>();
    final Map<Prepare.MethodId,Integer> targets=new HashMap<>();
    final int[] nativeOrder=new int[6];
    DisplayPrepare(Prepare base){this.base=base;}
    MethodModel method(String owner,String name,String desc){MethodModel m=base.methods.get(new Prepare.MethodId(owner,name,desc));
        if(m==null||m.code().isEmpty()||(m.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))!=0)
            throw new IllegalArgumentException("display method shape "+owner+"."+name+desc);return m;}
    static boolean call(CodeElement e,String owner,String name){return e instanceof InvokeInstruction i&&i.owner().asInternalName().equals(owner)&&i.name().equalsString(name);}
    static boolean field(CodeElement e,String owner,String name){return e instanceof FieldInstruction i&&i.owner().asInternalName().equals(owner)&&i.name().equalsString(name);}
    void target(String owner,String name,String desc,int stage){method(owner,name,desc);targets.put(new Prepare.MethodId(owner,name,desc),stage);required.add(owner);}
    void qualify() throws Exception{
        target("Visuals","update","(Ljava/awt/Graphics;)V",DisplayProbe.UPDATE_IN);
        target("Visuals","paint","(Ljava/awt/Graphics;)V",DisplayProbe.PAINT_IN);
        target("Visuals","drawQuarter","(Ljava/awt/Graphics2D;ZDDDIIII)V",DisplayProbe.QUARTER_IN);
        target("Core","runPresto","()V",DisplayProbe.PRESTO_IN);
        target("IntegratedDisplaySystem","drawThem","(Ljava/awt/Graphics2D;Z)V",DisplayProbe.IDS_IN);
        for(String root:ROOTS)target(root,"drawIt","(Ljava/awt/Graphics2D;Ljava/awt/Point;Z)V",DisplayProbe.ROOT_IN);
        required.add("IntegratedDisplaySystem$Box");required.add("Core$PrestoThread");
        var paint=Prepare.instructions(method("Visuals","paint","(Ljava/awt/Graphics;)V"));
        if(!call(paint.getFirst(),"Core","runPresto"))throw new IllegalArgumentException("display presto must precede native graphics");
        long presto=paint.stream().filter(e->call(e,"Core","runPresto")).count(),lost=paint.stream().filter(e->call(e,"java/awt/image/VolatileImage","contentsLost")).count();
        if(presto!=1||lost!=1)throw new IllegalArgumentException("display paint/retry multiplicity");
        int lostIndex=-1;for(int n=0;n<paint.size();n++)if(call(paint.get(n),"java/awt/image/VolatileImage","contentsLost"))lostIndex=n;
        if(lostIndex<0||!(paint.get(lostIndex+1) instanceof BranchInstruction branch)||branch.opcode()!=Opcode.IFNE)throw new IllegalArgumentException("display retry branch");
        int retryOrdinal=-1,ordinal=0;for(CodeElement e:method("Visuals","paint","(Ljava/awt/Graphics;)V").code().orElseThrow()){
            if(e instanceof LabelTarget l&&l.label()==branch.target())retryOrdinal=ordinal;if(e instanceof Instruction)ordinal++;}
        if(retryOrdinal<=0||retryOrdinal>=lostIndex)throw new IllegalArgumentException("display retry would repeat presto or lacks back edge");
        var quarter=Prepare.instructions(method("Visuals","drawQuarter","(Ljava/awt/Graphics2D;ZDDDIIII)V"));
        if(quarter.stream().filter(e->call(e,"IntegratedDisplaySystem","drawThem")).count()!=1||quarter.stream().filter(e->call(e,"MainPanels","drawThem")).count()!=1)throw new IllegalArgumentException("display quarter boundaries");
        var delta=Prepare.instructions(method("Core","runPresto","()V"));
        if(delta.stream().filter(e->call(e,"Aerodynamics","runDelta")).count()!=1||delta.stream().filter(e->call(e,"Core","isPaused")).count()!=1)throw new IllegalArgumentException("display native paused/dynamics gate");
        var req=Prepare.instructions(method("Core$PrestoThread","run","()V"));
        if(req.stream().filter(e->call(e,"Visuals","repaint")).count()!=1)throw new IllegalArgumentException("display repaint request");
        var ids=Prepare.instructions(method("IntegratedDisplaySystem","drawThem","(Ljava/awt/Graphics2D;Z)V"));
        if(ids.stream().filter(e->call(e,"IntegratedDisplaySystem$Box","drawIdu")).count()!=6)throw new IllegalArgumentException("display six box calls");
        for(String box:BOXES)if(ids.stream().filter(e->field(e,"IntegratedDisplaySystem",box)).count()!=1)throw new IllegalArgumentException("display box identity "+box);
        int pending=-1,index=0;for(CodeElement e:ids){for(int n=0;n<6;n++)if(field(e,"IntegratedDisplaySystem",BOXES[n])){if(pending!=-1)throw new IllegalArgumentException("display box operand overlap");pending=n;}
            if(call(e,"IntegratedDisplaySystem$Box","drawIdu")){if(pending<0||index>=6)throw new IllegalArgumentException("display box operand missing");nativeOrder[index++]=pending;pending=-1;}}
        if(index!=6||pending!=-1)throw new IllegalArgumentException("display box order");
        for(String assignment:ASSIGNMENTS)if(base.models.get("IntegratedDisplaySystem").fields().stream().noneMatch(f->f.fieldName().equalsString(assignment)))throw new IllegalArgumentException("display assignment "+assignment);
        var boxCode=Prepare.instructions(method("IntegratedDisplaySystem$Box","drawIdu","(Ljava/awt/Graphics2D;Z)V"));
        for(String f:new String[]{"blankTime","blankUpd","showUpd"})if(boxCode.stream().filter(e->field(e,"IntegratedDisplaySystem$Box",f)).count()!=1)throw new IllegalArgumentException("display Box gate "+f);
        if(boxCode.stream().filter(e->call(e,"Idu","drawIt")).count()!=1||boxCode.stream().filter(e->field(e,"ElecAdapter","isPowered")).count()!=1||boxCode.stream().filter(e->field(e,"IntegratedDisplaySystem$Box","idu")).count()!=2)throw new IllegalArgumentException("display Box dispatch gates");
        for(String root:ROOTS){var c=Prepare.instructions(method(root,"drawIt","(Ljava/awt/Graphics2D;Ljava/awt/Point;Z)V"));
            if(c.stream().noneMatch(e->field(e,root,"eiuIsPowered")))throw new IllegalArgumentException("display EIU predicate "+root);
            if(!root.equals(ROOTS[0])&&c.stream().filter(e->call(e,"java/awt/Graphics2D","hitClip")).count()!=1)throw new IllegalArgumentException("display clip predicate "+root);
            bodyMarkerUsed=false;boolean found=false;for(CodeElement e:c)if(isBodyMarker(root,e))found=true;
            if(!found)throw new IllegalArgumentException("display drawing body boundary "+root);
        }
        evidence.add("paint: one ordinary presto before graphics; contentsLost/IFNE retry; quarter dispatch unchanged");
        evidence.add("six native Box identities and 12 renderer identities; gate operands retained; no extra predicate queries");
        for(var item:targets.entrySet()){var id=item.getKey();var code=base.methods.get(id).code().orElseThrow();evidence.add("boundary "+id+" native.exception.handlers="+code.exceptionHandlers().size());}
        for(String name:required)evidence.add(name+" original.sha256="+Identity.hash(base.inputs.get(name)));
    }
    static void safe(CodeBuilder cb,java.util.function.Consumer<CodeBuilder> body){
        cb.trying(c->body.accept(c),handlers->handlers.catching(CD_Throwable,c->{c.pop();c.trying(n->n.invokestatic(PROBE,"failed",VOID),h->h.catching(CD_Throwable,n->n.pop()));}));
    }
    private ClassDesc emitting;
    void event(CodeBuilder cb,int stage,int value){cb.loadConstant(stage).loadConstant(value).invokestatic(emitting,PREFIX+"event",EVENT);}
    static void predicate(CodeBuilder cb,ClassDesc owner,int gate,boolean object){cb.dup().loadConstant(gate).invokestatic(owner,object?PREFIX+"object":PREFIX+"gate",MethodTypeDesc.of(CD_void,object?CD_Object:CD_int,CD_int));}
    static void helpers(ClassBuilder b){
        b.withMethodBody(PREFIX+"event",EVENT,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            safe(cb,c->c.iload(0).iload(1).invokestatic(PROBE,"event",EVENT));cb.return_();});
        b.withMethodBody(PREFIX+"gate",MethodTypeDesc.of(CD_void,CD_int,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            safe(cb,c->c.iload(1).iload(0).invokestatic(PROBE,"gate",MethodTypeDesc.of(CD_void,CD_int,CD_int)));cb.return_();});
        b.withMethodBody(PREFIX+"object",MethodTypeDesc.of(CD_void,CD_Object,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            safe(cb,c->{Label nonnull=c.newLabel(),done=c.newLabel();c.iload(1).aload(0).ifnonnull(nonnull).iconst_0().goto_(done).labelBinding(nonnull).iconst_1().labelBinding(done).invokestatic(PROBE,"gate",MethodTypeDesc.of(CD_void,CD_int,CD_int));});cb.return_();});
    }
    CodeTransform body(String owner,String name){ClassDesc self=ClassDesc.ofInternalName(owner);return (cb,e)->{
        if((Arrays.asList(ROOTS).contains(owner)&&!name.equals("drawIt"))||owner.equals("IntegratedDisplaySystem$Box")&&!name.equals("drawIdu")){cb.with(e);return;}
        if(owner.equals("Core")&&call(e,"Aerodynamics","runDelta")){cb.invokestatic(self,PREFIX+"dynamics",VOID);return;}
        if(owner.equals("Core$PrestoThread")&&call(e,"Visuals","repaint"))event(cb,DisplayProbe.REQUEST,0);
        if(owner.equals("IntegratedDisplaySystem")&&name.equals("drawThem")&&call(e,"IntegratedDisplaySystem$Box","drawIdu")){
            // Native stack is Box, Graphics2D, refresh. Each original call consumes one Box field.
            cb.invokestatic(self,PREFIX+"unit"+boxCallIndex++,MethodTypeDesc.of(CD_void,ClassDesc.of("IntegratedDisplaySystem$Box"),ClassDesc.of("java.awt.Graphics2D"),CD_boolean));return;
        }
        if(isBodyMarker(owner,e))event(cb,DisplayProbe.BODY,0);
        cb.with(e);
        if(owner.equals("Visuals")&&name.equals("paint")&&call(e,"java/awt/image/VolatileImage","validate"))event(cb,DisplayProbe.BUFFER_PASS,0);
        if((owner.equals("IntegratedDisplaySystem$Box")||Arrays.asList(ROOTS).contains(owner))&&e instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD){
            int gate=switch(f.name().stringValue()){case "blankTime"->0;case "blankUpd"->1;case "showUpd"->2;case "isPowered"->3;case "eiuIsPowered"->5;case "eicasPage"->6;case "blankAll"->7;default->-1;};
            if(gate>=0)predicate(cb,self,gate,f.type().stringValue().startsWith("L"));
        }
        if(owner.equals("Core")&&name.equals("runPresto")&&call(e,"Core","isPaused"))predicate(cb,self,8,false);
        if(Arrays.asList(ROOTS).contains(owner)&&call(e,"java/awt/Graphics2D","hitClip"))predicate(cb,self,4,false);
    };}
    private int boxCallIndex;
    boolean bodyMarkerUsed;
    boolean isBodyMarker(String owner,CodeElement e){
        if(bodyMarkerUsed)return false;
        boolean put=e instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD;
        boolean hit=owner.equals(ROOTS[0])&&field(e,owner,"brtVoltage")||owner.equals(ROOTS[1])&&field(e,owner,"incompleteRender")&&put
            ||owner.equals(ROOTS[2])&&field(e,owner,"noTrans")&&put||owner.equals(ROOTS[3])&&call(e,"java/awt/Graphics2D","translate");
        if(hit)bodyMarkerUsed=true;return hit;
    }
    void wrapper(ClassBuilder b,ClassDesc owner,MethodModel m,int enter){
        String name=m.methodName().stringValue(),original=PREFIX+"native$"+name;MethodTypeDesc type=MethodTypeDesc.ofDescriptor(m.methodType().stringValue());int flags=m.flags().flagsMask();
        boolean isStatic=(flags&ClassFile.ACC_STATIC)!=0;
        b.withMethodBody(name,type,flags,cb->{
            if(enter==DisplayProbe.ROOT_IN)safe(cb,c->c.iload(c.parameterSlot(2)).invokestatic(PROBE,"beginRenderer",MethodTypeDesc.of(CD_void,CD_boolean)));
            else event(cb,enter,0);
            Label start=cb.newLabel(),end=cb.newLabel(),handler=cb.newLabel();cb.labelBinding(start);
            if(!isStatic)cb.aload(0);for(int i=0;i<type.parameterCount();i++)cb.loadLocal(TypeKind.from(type.parameterType(i)).asLoadable(),cb.parameterSlot(i));
            if(isStatic)cb.invokestatic(owner,original,type);else cb.invokespecial(owner,original,type);
            cb.labelBinding(end);event(cb,enter+1,0);cb.return_();cb.labelBinding(handler);event(cb,DisplayProbe.NATIVE_FAILURE,enter);event(cb,enter+1,1);cb.athrow();cb.exceptionCatchAll(start,end,handler);
        });
    }
    void unitBridge(ClassBuilder b,int unit){ClassDesc ids=ClassDesc.of("IntegratedDisplaySystem"),box=ClassDesc.of("IntegratedDisplaySystem$Box");
        var type=MethodTypeDesc.of(CD_void,box,ClassDesc.of("java.awt.Graphics2D"),CD_boolean);
        b.withMethodBody(PREFIX+"unit"+unit,type,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            safe(cb,c->{c.loadConstant(unit).aload(0).getfield(box,"idu",ClassDesc.of("Idu"));
                Label unknown=c.newLabel(),done=c.newLabel();c.dup().ifnonnull(unknown).pop().iconst_0().goto_(done).labelBinding(unknown);
                for(int n=0;n<ASSIGNMENTS.length;n++){String a=ASSIGNMENTS[n];FieldModel f=base.models.get("IntegratedDisplaySystem").fields().stream().filter(x->x.fieldName().equalsString(a)).findFirst().orElseThrow();Label next=c.newLabel();
                    c.dup().getstatic(ids,a,ClassDesc.ofDescriptor(f.fieldType().stringValue())).if_acmpne(next).pop().loadConstant(n+1).goto_(done).labelBinding(next);}
                c.pop().loadConstant(13).labelBinding(done).iload(2).invokestatic(PROBE,"beginUnit",MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_boolean));});
            Label start=cb.newLabel(),end=cb.newLabel(),handler=cb.newLabel();cb.labelBinding(start).aload(0).aload(1).iload(2).invokevirtual(box,"drawIdu",MethodTypeDesc.of(CD_void,ClassDesc.of("java.awt.Graphics2D"),CD_boolean)).labelBinding(end);
            event(cb,DisplayProbe.UNIT_OUT,0);cb.return_();cb.labelBinding(handler);event(cb,DisplayProbe.NATIVE_FAILURE,DisplayProbe.UNIT_IN);event(cb,DisplayProbe.UNIT_OUT,1);cb.athrow();cb.exceptionCatchAll(start,end,handler);
        });
    }
    Map<String,byte[]> transform(Map<String,byte[]> existing){Map<String,byte[]> result=new TreeMap<>();
        // Native dispatch order differs from the chosen stable physical-unit ID order.
        for(String name:required){ClassModel model=base.cf.parse(existing.getOrDefault(name,base.inputs.get(name)));boxCallIndex=0;ClassDesc owner=ClassDesc.ofInternalName(name);emitting=owner;
            byte[] bytes=base.cf.transformClass(model,(b,e)->{
                if(e instanceof MethodModel m&&m.code().isPresent()){
                    var id=new Prepare.MethodId(name,m.methodName().stringValue(),m.methodType().stringValue());Integer enter=targets.get(id);bodyMarkerUsed=false;
                    if(enter!=null){String original=PREFIX+"native$"+m.methodName().stringValue();
                        b.withMethod(original,MethodTypeDesc.ofDescriptor(m.methodType().stringValue()),(m.flags().flagsMask()&ClassFile.ACC_STATIC)|ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC,mb->{
                            for(MethodElement element:m)if(element instanceof CodeModel code)mb.transformCode(code,body(name,m.methodName().stringValue()));else if(!(element instanceof AccessFlags))mb.with(element);
                        });wrapper(b,owner,m,enter);
                    }else b.transformMethod(m,(mb,me)->{if(me instanceof CodeModel c)mb.transformCode(c,body(name,m.methodName().stringValue()));else mb.with(me);});
                }else b.with(e);
            });
            ClassModel intermediate=base.cf.parse(bytes);
            for(var target:targets.keySet())if(target.owner().equals(name)){
                var original=model.methods().stream().filter(m->m.methodName().equalsString(target.name())&&m.methodType().equalsString(target.descriptor())).findFirst().orElseThrow().code().orElseThrow().exceptionHandlers();
                var retained=intermediate.methods().stream().filter(m->m.methodName().equalsString(PREFIX+"native$"+target.name())&&m.methodType().equalsString(target.descriptor())).findFirst().orElseThrow().code().orElseThrow().exceptionHandlers();
                if(original.size()!=retained.size())throw new IllegalArgumentException("display native exception table changed "+target);
                for(int i=0;i<original.size();i++)if(!original.get(i).catchType().map(x->x.asInternalName()).equals(retained.get(i).catchType().map(x->x.asInternalName())))throw new IllegalArgumentException("display native exception catch type changed "+target);
            }
            bytes=base.cf.transformClass(intermediate,ClassTransform.endHandler(b->{helpers(b);
                if(name.equals("Core"))b.withMethodBody(PREFIX+"dynamics",VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                    event(cb,DisplayProbe.DYNAMICS_IN,0);Label start=cb.newLabel(),end=cb.newLabel(),handler=cb.newLabel();cb.labelBinding(start).invokestatic(ClassDesc.of("Aerodynamics"),"runDelta",VOID).labelBinding(end);event(cb,DisplayProbe.DYNAMICS_OUT,0);cb.return_();cb.labelBinding(handler);event(cb,DisplayProbe.NATIVE_FAILURE,DisplayProbe.DYNAMICS_IN);event(cb,DisplayProbe.DYNAMICS_OUT,1);cb.athrow();cb.exceptionCatchAll(start,end,handler);});
                if(name.equals("IntegratedDisplaySystem"))for(int n=0;n<6;n++){
                    // Bridge names follow native call order; its metadata uses stable physical IDs.
                    final int physical=nativeOrder[n];final int index=n;
                    unitBridgeNamed(b,index,physical);
                }
            }));
            var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("display bytecode "+name+": "+errors.getFirst());result.put(name,bytes);
        }
        Map<String,byte[]> finalClasses=new TreeMap<>(existing);finalClasses.putAll(result);TerrainAccess.verifyAdded(existing,finalClasses,base.inputs);
        evidence.add("verified optional overlay/access; native render/dynamics invocation multiplicity retained");return result;
    }
    void unitBridgeNamed(ClassBuilder b,int nameIndex,int physical){
        // Generate with physical metadata, then rename the method when native-order names differ.
        if(nameIndex==physical){unitBridge(b,physical);return;}
        byte[] holder=base.cf.build(ClassDesc.of("DisplayBridgeHolder"),cb->unitBridge(cb,physical));
        MethodModel m=base.cf.parse(holder).methods().getFirst();b.withMethod(PREFIX+"unit"+nameIndex,MethodTypeDesc.ofDescriptor(m.methodType().stringValue()),m.flags().flagsMask(),mb->{for(MethodElement e:m)mb.with(e);});
    }
}
