package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Independent transaction above the qualified display/aircraft layers. */
final class RecorderPrepare {
    static final String PREFIX="$psxRecorder$";
    static final ClassDesc REC=ClassDesc.of("psx.exporter.PfdRecorder"),G=ClassDesc.of("java.awt.Graphics2D"),POINT=ClassDesc.of("java.awt.Point"),RECT=ClassDesc.of("java.awt.Rectangle"),TRANS=ClassDesc.of("java.awt.geom.AffineTransform"),TCAS=ClassDesc.of("Tcas");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void),EVENT=MethodTypeDesc.of(CD_void,CD_int,CD_int),UNIT=MethodTypeDesc.of(CD_void,CD_int,CD_int,CD_boolean),BEGIN=MethodTypeDesc.of(G,G,POINT,CD_boolean),END=MethodTypeDesc.of(CD_void,G),REFRESH=MethodTypeDesc.of(CD_boolean,G,CD_boolean),CLIP=MethodTypeDesc.of(RECT,RECT,G);
    final Prepare base;final Set<String> required=new TreeSet<>();final List<String> evidence=new ArrayList<>();
    RecorderPrepare(Prepare p){base=p;}
    void drawingClosure(){
        Set<String> allowed=Set.of("draw","fill","drawImage","drawLine","drawOval","drawRect","drawArc","drawRoundRect","fillOval","fillRect","getTransform","getClipBounds","getColor","clip","hitClip","rotate","setClip","clipRect","setColor","setRenderingHint","setStroke","setTransform","translate","scale","dispose");
        ArrayDeque<Prepare.MethodId> pending=new ArrayDeque<>();Set<Prepare.MethodId> seen=new HashSet<>();Set<String> writers=new TreeSet<>(),graphics=new TreeSet<>();for(String root:DisplayPrepare.ROOTS)pending.add(new Prepare.MethodId(root,"drawIt","(Ljava/awt/Graphics2D;Ljava/awt/Point;Z)V"));
        while(!pending.isEmpty()){Prepare.MethodId id=resolve(pending.remove());if(!seen.add(id))continue;MethodModel method=base.methods.get(id);writers.add(id.owner());
            for(String owner:base.models.keySet())if(descendant(owner,id.owner())){var override=new Prepare.MethodId(owner,id.name(),id.descriptor());if(base.methods.containsKey(override))pending.add(override);}
            if(method.code().isEmpty())continue;
            evidence.add("drawing.method "+id+" exception.handlers="+method.code().orElseThrow().exceptionHandlers().size());
            for(Instruction instruction:Prepare.instructions(method))if(instruction instanceof InvokeDynamicInstruction)throw new IllegalArgumentException("unqualified invokedynamic drawing writer "+id);
            for(Instruction instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&Set.of("updAltitude","updModes","updBaroSet","updAirspeed").contains(f.name().stringValue()))throw new IllegalArgumentException("drawing mutates sensor update flag "+id+" "+f.name());
            for(Instruction instruction:Prepare.instructions(method))if(instruction instanceof InvokeInstruction call){String owner=call.owner().asInternalName(),name=call.name().stringValue(),type=call.type().stringValue();
                if(owner.equals("java/awt/Graphics2D")||owner.equals("java/awt/Graphics")){if(!allowed.contains(name))throw new IllegalArgumentException("unqualified graphics operation "+owner+"."+name+type);graphics.add(owner+"."+name+type);}
                if(base.models.containsKey(owner)&&(type.contains("Ljava/awt/Graphics2D;")||type.contains("Ljava/awt/Graphics;")||owner.startsWith("Tcas$")&&name.startsWith("draw")))pending.add(new Prepare.MethodId(owner,name,type));
            }
        }
        required.addAll(writers);for(String owner:writers)evidence.add("drawing.writer "+owner+" sha256="+hashUnchecked(base.inputs.get(owner)));
        for(String operation:graphics)evidence.add("drawing.operation "+operation);
        evidence.add("drawing.closure methods="+seen.size()+" writers="+writers.size()+" graphics.descriptors="+graphics.size());
    }
    Prepare.MethodId resolve(Prepare.MethodId id){for(int n=0;n<100;n++){if(base.methods.containsKey(id))return id;var m=base.models.get(id.owner());if(m==null||m.superclass().isEmpty())break;id=new Prepare.MethodId(m.superclass().get().asInternalName(),id.name(),id.descriptor());}throw new IllegalArgumentException("unresolved drawing closure "+id);}
    boolean descendant(String child,String owner){for(int n=0;n<100;n++){var m=base.models.get(child);if(m==null||m.superclass().isEmpty())return false;child=m.superclass().get().asInternalName();if(child.equals(owner))return true;}throw new IllegalArgumentException("hierarchy cycle");}
    static String hashUnchecked(byte[] bytes){try{return Identity.hash(bytes);}catch(Exception failure){throw new IllegalArgumentException(failure);}}
    void qualifyRetained(){
        int graphicsWriters=0,transformWriters=0,bitmapWriters=0,allocations=0,uses=0;
        for(var entry:base.methods.entrySet()){var id=entry.getKey();var method=entry.getValue();if(method.code().isEmpty())continue;var code=Prepare.instructions(method);
            for(int at=0;at<code.size();at++){var instruction=code.get(at);
                if(instruction instanceof FieldInstruction f&&f.owner().asInternalName().equals("Tcas")&&Set.of("g","origTrans").contains(f.name().stringValue())){
                    if(!required.contains(id.owner()))throw new IllegalArgumentException("retained TCAS context outside qualified closure "+id);
                    if(f.opcode()==Opcode.PUTSTATIC){if(!id.owner().equals("Tcas")||!id.name().equals("setGeom"))throw new IllegalArgumentException("TCAS context replacement "+id);if(f.name().equalsString("g"))graphicsWriters++;else transformWriters++;}
                }
                if(instruction instanceof FieldInstruction f&&f.owner().asInternalName().equals("WxRadarBasics$NdImage")&&f.name().equalsString("buff")&&f.opcode()==Opcode.PUTFIELD){if(!id.name().equals("<init>"))throw new IllegalArgumentException("ND bitmap replacement "+id);bitmapWriters++;}
                if(id.owner().equals("WxRadarBasics$NdImage")&&DisplayPrepare.call(instruction,"java/awt/GraphicsConfiguration","createCompatibleImage")){if(!id.name().equals("<init>")||at<2||!(code.get(at-2) instanceof ConstantInstruction w)||!Integer.valueOf(120).equals(w.constantValue())||!(code.get(at-1) instanceof ConstantInstruction h)||!Integer.valueOf(80).equals(h.constantValue()))throw new IllegalArgumentException("ND bitmap allocation");allocations++;}
                if(id.owner().equals("NavigationDisplay")&&DisplayPrepare.call(instruction,"java/awt/Graphics2D","drawImage"))uses++;
            }
        }
        if(graphicsWriters!=1||transformWriters!=1||bitmapWriters!=1||allocations!=1||uses!=2)throw new IllegalArgumentException("retained context/ND bitmap lifetime changed");
        evidence.add("TCAS:one native context/transform writer setGeom; all readers qualified; retained fields store ordinary context/native transform; bounded invocation mapping at readers preserves centered test drawing and post-return lifetime");
        evidence.add("ND weather bitmap120x80 allocated once; sole replacement constructor; two drawing use sites freeze pixels; mutable blankIt/drawNdImage writers stay native; image.owner.sha256="+hashUnchecked(base.inputs.get("WxRadarBasics$NdImage")));
    }
    void qualify(DisplayPrepare display)throws Exception{required.addAll(display.required);ClassModel pfd=base.models.get("PrimaryFlightDisplay");if(pfd==null)throw new IllegalArgumentException("PFD absent");int clips=0,scales=0,images=0,raAllocation=0,raAssignments=0;Set<Double> thresholds=new HashSet<>();
        for(MethodModel m:pfd.methods())if(m.code().isPresent()){var code=Prepare.instructions(m);for(int i=0;i<code.size();i++){Instruction instruction=code.get(i);
            if(DisplayPrepare.field(instruction,"Visuals","mainClip")){var type=MethodTypeDesc.ofDescriptor(m.methodType().stringValue());if(type.parameterCount()==0||!type.parameterType(0).equals(G))throw new IllegalArgumentException("PFD clip context "+m.methodName());clips++;}
            if(DisplayPrepare.call(instruction,"java/awt/geom/AffineTransform","getScaleX")){if(i+3>=code.size()||!(code.get(i+1) instanceof ConstantInstruction c)||!(c.constantValue() instanceof Double d))throw new IllegalArgumentException("PFD source scale branch");boolean small=d==0.2;Opcode compare=small?Opcode.DCMPG:Opcode.DCMPL,branch=small?Opcode.IFGE:Opcode.IFLE;if(code.get(i+2).opcode()!=compare||code.get(i+3).opcode()!=branch)throw new IllegalArgumentException("PFD source scale branch direction");thresholds.add(d);scales++;}
            if(DisplayPrepare.call(instruction,"java/awt/Graphics2D","drawImage"))images++;
            if(DisplayPrepare.call(instruction,"java/awt/GraphicsConfiguration","createCompatibleImage")){if(i<2||!m.methodName().equalsString("<init>")||!(code.get(i-2) instanceof ConstantInstruction w)||!Integer.valueOf(79).equals(w.constantValue())||!(code.get(i-1) instanceof ConstantInstruction h)||!Integer.valueOf(30).equals(h.constantValue()))throw new IllegalArgumentException("RA bitmap dimensions/lifetime");raAllocation++;}
            if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.PUTFIELD&&f.name().equalsString("raBoxBuff")){if(!m.methodName().equalsString("<init>"))throw new IllegalArgumentException("RA bitmap replacement");raAssignments++;}
            if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD&&(f.name().equalsString("noTrans")||f.name().equalsString("transAttitude"))&&!m.methodName().stringValue().startsWith("draw"))throw new IllegalArgumentException("PFD retained drawing transform consumer");
        }}
        if(clips!=11||scales!=3||!thresholds.equals(Set.of(0.4,0.2,0.175))||images!=1||raAllocation!=1||raAssignments!=1)throw new IllegalArgumentException("PFD clip/LOD/image closure changed "+clips+"/"+thresholds+"/"+images+"/"+raAllocation+"/"+raAssignments);
        drawingClosure();qualifyRetained();evidence.add("PFD:11 installed-bytecode mainClip reads;3 source-scale branches at0.4/0.2/0.175;1 cached bitmap site; positive uniform source scale must exceed0.4 without shear and quarter clip must be rectangular");
        evidence.add("canonical logical662x685; six physical Boxes; PFD/ND/primary/secondary roots; no extra renderer or dynamics calls");
        evidence.add("RA bitmap79x30 allocated once in constructor; one native replacement writer; completed bitmap frozen at sole drawing use; drawing closure does not clear four sensor update flags");
        for(String name:required)evidence.add(name+" original.sha256="+Identity.hash(base.inputs.get(name)));
    }
    static void guarded(CodeBuilder cb,java.util.function.Consumer<CodeBuilder> body){cb.trying(c->body.accept(c),handlers->handlers.catching(CD_Throwable,c->{c.pop();c.trying(n->n.invokestatic(REC,"failed",VOID),h->h.catching(CD_Throwable,n->n.pop()));}));}
    static void valueCallback(ClassBuilder b,String name,boolean transform){MethodTypeDesc type=transform?MethodTypeDesc.of(TRANS,TRANS,G):MethodTypeDesc.of(G,G);int result=transform?2:1;
        b.withMethodBody(PREFIX+name,type,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{cb.aload(0).astore(result);guarded(cb,c->{c.aload(0);if(transform)c.aload(1);c.invokestatic(REC,name,type).astore(result);});cb.aload(result).areturn();});}
    void callbacks(ClassBuilder b,ClassDesc self){for(String name:new String[]{"event","gate","unit"}){MethodTypeDesc type=name.equals("unit")?UNIT:EVENT;
        b.withMethodBody(PREFIX+name,type,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{cb.iload(0).iload(1);if(name.equals("unit"))cb.iload(2);cb.invokestatic(DisplayPrepare.PROBE,name.equals("unit")?"beginUnit":name,type);
            guarded(cb,c->{c.iload(0).iload(1);if(name.equals("unit"))c.iload(2);c.invokestatic(REC,name,type);});cb.return_();});}
        b.withMethodBody(PREFIX+"clip",CLIP,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{Label a=cb.newLabel(),z=cb.newLabel(),h=cb.newLabel();cb.labelBinding(a).aload(0).aload(1).invokestatic(REC,"clip",CLIP).labelBinding(z).areturn().labelBinding(h).pop();guarded(cb,c->c.invokestatic(REC,"failed",VOID));cb.aload(0).areturn();cb.exceptionCatchAll(a,z,h);});
        b.withMethodBody(PREFIX+"draw",VOID,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{guarded(cb,c->c.invokestatic(REC,"drawCount",VOID));cb.return_();});
        valueCallback(b,"nativeGraphics",false);valueCallback(b,"replayGraphics",false);valueCallback(b,"nativeTransform",true);valueCallback(b,"replayTransform",true);
    }
    Map<String,byte[]> transform(Map<String,byte[]> existing){Map<String,byte[]> result=new TreeMap<>();for(String name:required){byte[] before=existing.getOrDefault(name,base.inputs.get(name));if(before==null)throw new IllegalArgumentException("recorder source missing "+name);ClassModel model=base.cf.parse(before);ClassDesc self=ClassDesc.ofInternalName(name);
        byte[] bytes=base.cf.transformClass(model,(b,e)->{if(e instanceof MethodModel m&&m.code().isPresent()){
            boolean root=Arrays.asList(DisplayPrepare.ROOTS).contains(name)&&m.methodName().equalsString("drawIt");int slot=(m.flags().flagsMask()&ClassFile.ACC_STATIC)!=0?0:1;
            CodeTransform code=(cb,i)->{if(i instanceof InvokeInstruction call&&call.opcode()==Opcode.INVOKESTATIC&&call.owner().asInternalName().equals("psx/exporter/DisplayProbe")&&(call.name().equalsString("event")||call.name().equalsString("gate")||call.name().equalsString("beginUnit"))){String helper=call.name().equalsString("beginUnit")?"unit":call.name().stringValue();cb.invokestatic(self,PREFIX+helper,helper.equals("unit")?UNIT:EVENT);return;}
                if(i instanceof FieldInstruction f&&f.owner().asInternalName().equals("Tcas")&&f.opcode()==Opcode.PUTSTATIC){if(f.name().equalsString("g"))cb.invokestatic(self,PREFIX+"nativeGraphics",MethodTypeDesc.of(G,G));if(f.name().equalsString("origTrans"))cb.getstatic(TCAS,"g",G).invokestatic(self,PREFIX+"nativeTransform",MethodTypeDesc.of(TRANS,TRANS,G));}
                cb.with(i);
                if(i instanceof FieldInstruction f&&f.owner().asInternalName().equals("Tcas")&&f.opcode()==Opcode.GETSTATIC){if(f.name().equalsString("g"))cb.invokestatic(self,PREFIX+"replayGraphics",MethodTypeDesc.of(G,G));if(f.name().equalsString("origTrans"))cb.getstatic(TCAS,"g",G).invokestatic(self,PREFIX+"replayTransform",MethodTypeDesc.of(TRANS,TRANS,G));}
                if(i instanceof FieldInstruction clipRead&&clipRead.opcode()==Opcode.GETFIELD&&DisplayPrepare.field(i,"Visuals","mainClip")&&MethodTypeDesc.ofDescriptor(m.methodType().stringValue()).parameterCount()>0&&MethodTypeDesc.ofDescriptor(m.methodType().stringValue()).parameterType(0).equals(G))cb.aload(slot).invokestatic(self,PREFIX+"clip",CLIP);if(i instanceof InvokeInstruction call&&(call.owner().asInternalName().equals("java/awt/Graphics2D")||call.owner().asInternalName().equals("java/awt/Graphics"))&&Set.of("draw","fill","drawImage","drawLine","drawOval","drawRect","drawArc","drawRoundRect","fillOval","fillRect").contains(call.name().stringValue()))cb.invokestatic(self,PREFIX+"draw",VOID);};
            if(root){b.withMethod(PREFIX+"native",MethodTypeDesc.ofDescriptor(m.methodType().stringValue()),ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC,mb->{for(MethodElement element:m)if(element instanceof CodeModel c)mb.transformCode(c,code);else if(!(element instanceof AccessFlags))mb.with(element);});wrapper(b,self,m);}
            else b.transformMethod(m,(mb,me)->{if(me instanceof CodeModel c)mb.transformCode(c,code);else mb.with(me);});
        }else b.with(e);});
        bytes=base.cf.transformClass(base.cf.parse(bytes),ClassTransform.endHandler(b->callbacks(b,self)));if(!base.cf.verify(bytes).isEmpty())throw new IllegalArgumentException("recorder bytecode "+name);result.put(name,bytes);
    }Map<String,byte[]> complete=new TreeMap<>(existing);complete.putAll(result);TerrainAccess.verifyAdded(existing,complete,base.inputs);evidence.add("whole overlay verified including every added helper member; all lower overlays retained");return result;}
    void wrapper(ClassBuilder b,ClassDesc self,MethodModel method){b.withMethodBody("drawIt",MethodTypeDesc.ofDescriptor(method.methodType().stringValue()),method.flags().flagsMask(),cb->{int graphics=cb.allocateLocal(TypeKind.REFERENCE),refresh=cb.allocateLocal(TypeKind.INT),exception=cb.allocateLocal(TypeKind.REFERENCE);
        cb.aload(1).astore(graphics).iload(3).istore(refresh);guarded(cb,c->c.aload(1).aload(2).iload(3).invokestatic(REC,"begin",BEGIN).astore(graphics));guarded(cb,c->c.aload(graphics).iload(3).invokestatic(REC,"refresh",REFRESH).istore(refresh));
        Label start=cb.newLabel(),end=cb.newLabel(),handler=cb.newLabel();cb.labelBinding(start).aload(0).aload(graphics).aload(2).iload(refresh).invokespecial(self,PREFIX+"native",MethodTypeDesc.of(CD_void,G,POINT,CD_boolean)).labelBinding(end);
        guarded(cb,c->c.aload(graphics).invokestatic(REC,"end",END));cb.return_().labelBinding(handler).astore(exception);guarded(cb,c->c.aload(graphics).invokestatic(REC,"end",END));cb.aload(exception).athrow();cb.exceptionCatchAll(start,end,handler);
    });}
}


