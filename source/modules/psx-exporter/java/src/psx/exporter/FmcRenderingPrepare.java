package psx.exporter;

import java.lang.classfile.*;import java.lang.classfile.instruction.*;import java.lang.constant.*;import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** The exact native ND image-cache boundary. Native clip, power and blanking
 * still execute; an armed completed image replaces only the subsequent ND
 * drawing body. Other instrument renderers and the complete frame stay native. */
final class FmcRenderingPrepare {
    static final String ROOTS="$psxExporter$fmcRenderingRoots",BOX_ROOTS="$psxExporter$fmcRenderingBoxes",VISIBLE="$psxExporter$fmcRenderingVisible",POWERED="$psxExporter$fmcRenderingPowered",CACHED="$psxExporter$fmcRenderingCached",FAULT="$psxExporter$fmcRenderingFailure",SHADOW="$psxExporter$fmcRenderingShadow",SHADOW_EPOCH="$psxExporter$fmcRenderingShadowEpoch",SHADOW_PUBLISH="$psxExporter$fmcRenderingShadowPublish",SHADOW_CLEAR="$psxExporter$fmcRenderingShadowClear",SHADOW_READ="$psxExporter$fmcRenderingShadowRead",FALLBACK="$psxExporter$fmcRenderingFallback",ENTRY="$psxExporter$fmcRenderingEntry",LEAVE="$psxExporter$fmcRenderingLeave";
    static final Prepare.MethodId ROOT=new Prepare.MethodId("NavigationDisplay","drawIt","(Ljava/awt/Graphics2D;Ljava/awt/Point;Z)V");
    static final List<String> NAMES=List.of("graphNdCp","graphNdFo","graphNdCp2","graphNdFo2");
    private static final ClassDesc ND=ClassDesc.of("NavigationDisplay"),IDS=ClassDesc.of("IntegratedDisplaySystem"),BOX=ClassDesc.of("IntegratedDisplaySystem$Box"),G=ClassDesc.of("java.awt.Graphics2D"),POINT=ClassDesc.of("java.awt.Point"),HELPER=ClassDesc.of("psx.exporter.FmcRenderingCache");
    static Set<FmcQualification.Member> inputs(){var fields=new HashSet<FmcQualification.Member>();for(String name:NAMES)fields.add(new FmcQualification.Member("IntegratedDisplaySystem",name,"LNavigationDisplay;"));for(String name:DisplayPrepare.BOXES)fields.add(new FmcQualification.Member("IntegratedDisplaySystem",name,"LIntegratedDisplaySystem$Box;"));fields.add(new FmcQualification.Member("IntegratedDisplaySystem$Box","idu","LIdu;"));fields.add(new FmcQualification.Member("NavigationDisplay","eiuIsPowered","Z"));return Set.copyOf(fields);}
    private FmcRenderingPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC rendering preparation "+why);}
    static void qualify(Prepare data){
        for(var field:inputs())require(field.equals(FmcQualification.resolveField(data.models,field.owner(),field.name(),field.descriptor(),new HashSet<>())),"exact renderer/root member");
        var allocations=new HashMap<String,Integer>();int made=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            var ops=Prepare.instructions(entry.getValue());for(int at=0;at<ops.size();at++){
                if(ops.get(at) instanceof NewObjectInstruction n&&n.className().asInternalName().equals("NavigationDisplay")){
                    made++;require(at+3<ops.size()&&ops.get(at+1).opcode()==Opcode.DUP&&Prepare.call(ops.get(at+2),"NavigationDisplay","<init>","()V")&&ops.get(at+3) instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("IntegratedDisplaySystem")&&NAMES.contains(f.name().stringValue()),"all native ND instances are the four maintained roots");
                }
                if(ops.get(at) instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("IntegratedDisplaySystem")&&NAMES.contains(f.name().stringValue())){
                    require(at>=3&&ops.get(at-3) instanceof NewObjectInstruction n&&n.className().asInternalName().equals("NavigationDisplay")&&ops.get(at-2).opcode()==Opcode.DUP&&Prepare.call(ops.get(at-1),"NavigationDisplay","<init>","()V"),"sole native root constructor stores");allocations.merge(f.name().stringValue(),1,Integer::sum);
                }
            }
        }
        require(made==4&&allocations.size()==4&&allocations.values().stream().allMatch(n->n==1),"finite exact renderer population");
        var method=data.methods.get(ROOT);require(method!=null&&method.code().isPresent(),"native ND drawing root");
        var prefix=new ArrayList<Instruction>();int bci=0;for(var op:Prepare.instructions(method)){if(bci>=74)break;prefix.add(op);bci+=op.sizeInBytes();}require(bci==74,"exact native drawing prefix boundary");
        var fields=new ArrayList<String>();var calls=new ArrayList<String>();var branches=new ArrayList<Integer>();var labels=new HashMap<Label,Integer>();int offset=0;for(var e:method.code().orElseThrow()){if(e instanceof LabelTarget l)labels.put(l.label(),offset);if(e instanceof Instruction i)offset+=i.sizeInBytes();}
        for(var op:prefix){if(op instanceof FieldInstruction f)fields.add(f.opcode()+" "+f.owner().asInternalName()+"."+f.name().stringValue()+":"+f.type().stringValue());if(op instanceof InvokeInstruction c)calls.add(c.owner().asInternalName()+"."+c.name().stringValue()+c.type().stringValue());if(op instanceof BranchInstruction b)branches.add(labels.get(b.target()));}
        require(fields.equals(List.of("PUTFIELD NavigationDisplay.noTrans:Ljava/awt/geom/AffineTransform;","GETFIELD java/awt/Point.x:I","GETFIELD java/awt/Point.y:I","GETFIELD NavigationDisplay.eiuIsPowered:Z","GETFIELD NavigationDisplay.blankAll:Z","GETSTATIC java/awt/Color.BLACK:Ljava/awt/Color;")),"clip/power/blank prefix fields");
        require(calls.equals(List.of("java/awt/Graphics2D.getTransform()Ljava/awt/geom/AffineTransform;","java/awt/Graphics2D.hitClip(IIII)Z","java/awt/Graphics2D.setColor(Ljava/awt/Color;)V","java/awt/Graphics2D.fillRect(IIII)V"))&&branches.equals(List.of(38,74,73)),"unaltered native drawing predicates");
        var ops=Prepare.instructions(method);int target=-1,b=0;for(int at=0;at<ops.size();at++){if(b==76)target=at;b+=ops.get(at).sizeInBytes();}
        require(target>=2&&Prepare.field(ops.get(target),Opcode.PUTFIELD,"NavigationDisplay","incompleteRender")&&ops.get(target-1) instanceof ConstantInstruction c&&c.constantValue().equals(1)&&ops.get(target-2) instanceof LoadInstruction l&&l.slot()==0,"exact native drawing-body start");
        // Assignment indices are the existing collector's installed identity
        // mapping; the cache never infers a renderer from a nearby class name.
        for(int i=0;i<4;i++)require(DisplayPrepare.ASSIGNMENTS[i+2].equals(NAMES.get(i)),"existing ND collector assignment mapping");
        var caller=new Prepare.MethodId("IntegratedDisplaySystem$Box","drawIdu","(Ljava/awt/Graphics2D;Z)V");int sites=0;
        for(var entry:data.methods.entrySet())if(entry.getValue().code().isPresent()){
            int at=0;var code=Prepare.instructions(entry.getValue());for(int n=0;n<code.size();n++){var op=code.get(n);if(op instanceof InvokeInstruction call&&call.name().equalsString(ROOT.name())&&call.type().equalsString(ROOT.descriptor())){
                sites++;require(entry.getKey().equals(caller)&&at==90&&call.owner().asInternalName().equals("Idu")&&call.opcode()==Opcode.INVOKEVIRTUAL,"sole native ND renderer caller");
                require(n>=13&&code.get(n-13) instanceof LoadInstruction receiver&&receiver.slot()==0&&Prepare.field(code.get(n-12),Opcode.GETFIELD,"IntegratedDisplaySystem$Box","idu"),"renderer receiver is exact current Box.idu");
            }at+=op.sizeInBytes();}
        }
        require(sites==1,"no alternate native/observer ND drawing producers");
    }
    static Map<String,byte[]> bindings(Prepare data,Map<String,byte[]> lower){
        qualify(data);byte[] input=lower.getOrDefault("IntegratedDisplaySystem",data.inputs.get("IntegratedDisplaySystem"));var model=data.cf.parse(input);require(model.methods().stream().noneMatch(m->Set.of(ROOTS,BOX_ROOTS,VISIBLE).contains(m.methodName().stringValue())),"binding collision");int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
        byte[] output=data.cf.transformClass(model,ClassTransform.endHandler(b->{
            b.withMethodBody(ROOTS,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.iconst_4().anewarray(CD_Object).astore(0);for(int i=0;i<4;i++)c.aload(0).loadConstant(i).getstatic(IDS,NAMES.get(i),ND).aastore();c.aload(0).areturn();});
            b.withMethodBody(BOX_ROOTS,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.loadConstant(6).anewarray(CD_Object).astore(0);for(int i=0;i<6;i++)c.aload(0).loadConstant(i).getstatic(IDS,DisplayPrepare.BOXES[i],BOX).aastore();c.aload(0).areturn();});
            b.withMethodBody(VISIBLE,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.loadConstant(6).anewarray(CD_Object).astore(0);for(int i=0;i<6;i++)c.aload(0).loadConstant(i).getstatic(IDS,DisplayPrepare.BOXES[i],BOX).getfield(BOX,"idu",ClassDesc.of("Idu")).aastore();c.aload(0).areturn();});
        }));require(data.cf.verify(output).isEmpty(),"binding verifier");var result=new HashMap<>(lower);result.put("IntegratedDisplaySystem",output);byte[] ndInput=lower.getOrDefault("NavigationDisplay",data.inputs.get("NavigationDisplay"));var ndModel=data.cf.parse(ndInput);require(ndModel.methods().stream().noneMatch(m->m.methodName().equalsString(POWERED)),"power binding collision");byte[] ndOutput=data.cf.transformClass(ndModel,ClassTransform.endHandler(b->b.withMethodBody(POWERED,MethodTypeDesc.of(CD_boolean,CD_Object),flags,c->c.aload(0).checkcast(ND).getfield(ND,"eiuIsPowered",CD_boolean).ireturn())));require(data.cf.verify(ndOutput).isEmpty(),"power binding verifier");result.put("NavigationDisplay",ndOutput);TerrainAccess.verifyAdded(lower,result,data.inputs);return FmcRenderingCapturePrepare.transform(data,Map.copyOf(result));
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        qualify(data);byte[] input=lower.getOrDefault("NavigationDisplay",data.inputs.get("NavigationDisplay"));var model=data.cf.parse(input);require(model.fields().stream().noneMatch(f->Set.of(FAULT,SHADOW,SHADOW_EPOCH).contains(f.fieldName().stringValue()))&&model.methods().stream().noneMatch(m->Set.of(CACHED,FALLBACK,SHADOW_PUBLISH,SHADOW_CLEAR,SHADOW_READ,ENTRY,LEAVE).contains(m.methodName().stringValue())),"cache collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){
            if(!(e instanceof MethodModel m)||!m.methodName().equalsString(ROOT.name())||!m.methodType().equalsString(ROOT.descriptor())){b.with(e);return;}
            b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){boolean done;Instruction previous;int ticket,error;Label begin,end,handler;
                public void atStart(CodeBuilder c){ticket=c.allocateLocal(TypeKind.INT);error=c.allocateLocal(TypeKind.REFERENCE);begin=c.newLabel();end=c.newLabel();handler=c.newLabel();c.invokestatic(ND,ENTRY,MethodTypeDesc.of(CD_int)).istore(ticket).labelBinding(begin);}
                private void retire(CodeBuilder c){c.iload(ticket).invokestatic(ND,LEAVE,MethodTypeDesc.of(CD_void,CD_int));}
                public void accept(CodeBuilder c,CodeElement e){
                    if(!done&&e instanceof FieldInstruction f&&Prepare.field(f,Opcode.PUTFIELD,"NavigationDisplay","incompleteRender")&&previous instanceof ConstantInstruction k&&k.constantValue().equals(1)){
                        done=true;int value=c.allocateLocal(TypeKind.INT),receiver=c.allocateLocal(TypeKind.REFERENCE);var nativeBody=c.newLabel();c.istore(value).astore(receiver).aload(0).aload(1).aload(2).invokestatic(ND,CACHED,MethodTypeDesc.of(CD_boolean,CD_Object,G,POINT)).ifeq(nativeBody)
                            .aload(0).iconst_0().putfield(ND,"incompleteRender",CD_boolean);retire(c);c.return_().labelBinding(nativeBody).aload(receiver).iload(value);
                    }
                    if(e instanceof Instruction i&&i.opcode()==Opcode.RETURN)retire(c);c.with(e);if(e instanceof Instruction i)previous=i;
                }
                public void atEnd(CodeBuilder c){require(done,"final native drawing-body boundary");c.labelBinding(end).labelBinding(handler).astore(error);retire(c);c.aload(error).athrow().exceptionCatch(begin,end,handler,CD_Throwable);}
            }));
        }public void atEnd(ClassBuilder b){
            b.withField(FAULT,CD_boolean,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC);
b.withMethodBody(ENTRY,MethodTypeDesc.of(CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{int ticket=c.allocateLocal(TypeKind.INT);c.iconst_m1().istore(ticket).trying(t->t.invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"enter",MethodTypeDesc.of(CD_int)).istore(ticket),h->h.catching(CD_Throwable,t->t.pop().iconst_1().putstatic(ND,FAULT,CD_boolean)));c.iload(ticket).ireturn();});
            b.withMethodBody(LEAVE,MethodTypeDesc.of(CD_void,CD_int),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var done=c.newLabel();c.iload(0).ifle(done).trying(t->t.iload(0).invokestatic(ClassDesc.of("psx.exporter.FmcExclusion"),"leave",MethodTypeDesc.of(CD_void,CD_int)),h->h.catching(CD_Throwable,t->t.pop().iconst_1().putstatic(ND,FAULT,CD_boolean))).labelBinding(done).return_();});
            var image=ClassDesc.of("java.awt.image.BufferedImage");int shadowFlags=ClassFile.ACC_PRIVATE|ClassFile.ACC_VOLATILE|ClassFile.ACC_SYNTHETIC;int bridgeFlags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            b.withField(SHADOW,image.arrayType(),shadowFlags);b.withField(SHADOW_EPOCH,CD_long,shadowFlags);
            b.withMethodBody(SHADOW_PUBLISH,MethodTypeDesc.of(CD_void,CD_Object,CD_long,image.arrayType()),bridgeFlags,c->c.aload(0).checkcast(ND).lload(1).putfield(ND,SHADOW_EPOCH,CD_long).aload(0).checkcast(ND).aload(3).putfield(ND,SHADOW,image.arrayType()).return_());
            b.withMethodBody(SHADOW_CLEAR,MethodTypeDesc.of(CD_void,CD_Object),bridgeFlags,c->c.aload(0).checkcast(ND).aconst_null().putfield(ND,SHADOW,image.arrayType()).return_());
            b.withMethodBody(SHADOW_READ,MethodTypeDesc.of(image.arrayType(),CD_Object),bridgeFlags,c->c.aload(0).checkcast(ND).getfield(ND,SHADOW,image.arrayType()).areturn());
            b.withMethodBody(FALLBACK,MethodTypeDesc.of(CD_boolean,CD_Object,G,POINT),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{var absent=c.newLabel();c.aload(0).checkcast(ND).getfield(ND,SHADOW,image.arrayType()).astore(3).aload(3).ifnull(absent).aload(0).checkcast(ND).getfield(ND,SHADOW_EPOCH,CD_long).invokestatic(ClassDesc.of("psx.exporter.Observer"),"terrainEpoch",MethodTypeDesc.of(CD_long)).lcmp().ifne(absent);
                for(int i=0;i<2;i++){c.aload(1).aload(3).loadConstant(i).aaload().aload(2).getfield(POINT,"x",CD_int).aload(2).getfield(POINT,"y",CD_int);if(i==1)c.aload(3).iconst_0().aaload().invokevirtual(image,"getHeight",MethodTypeDesc.of(CD_int)).iadd();c.aconst_null().invokevirtual(G,"drawImage",MethodTypeDesc.of(CD_boolean,ClassDesc.of("java.awt.Image"),CD_int,CD_int,ClassDesc.of("java.awt.image.ImageObserver"))).pop();}c.iconst_1().ireturn().labelBinding(absent).iconst_0().ireturn();});
            b.withMethodBody(CACHED,MethodTypeDesc.of(CD_boolean,CD_Object,G,POINT),ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{int result=c.allocateLocal(TypeKind.INT);c.iconst_0().istore(result).trying(t->t.aload(0).aload(1).aload(2).invokestatic(HELPER,"draw",MethodTypeDesc.of(CD_boolean,CD_Object,G,POINT)).istore(result),h->h.catching(CD_Throwable,t->{t.pop().iconst_1().putstatic(ND,FAULT,CD_boolean).trying(x->x.invokestatic(HELPER,"failed",MethodTypeDesc.of(CD_void)),ignore->ignore.catching(CD_Throwable,x->x.pop()));t.aload(0).aload(1).aload(2).invokestatic(ND,FALLBACK,MethodTypeDesc.of(CD_boolean,CD_Object,G,POINT)).istore(result);}));c.iload(result).ireturn();});
        }});require(data.cf.verify(output).isEmpty(),"native cache branch verifier");var result=new HashMap<>(lower);result.put("NavigationDisplay",output);TerrainAccess.verifyAdded(lower,result,data.inputs);return Map.copyOf(result);
    }
}
