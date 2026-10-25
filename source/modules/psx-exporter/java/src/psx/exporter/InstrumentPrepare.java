package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Optional drawing-only extension. Inspection never defines installed classes. */
final class InstrumentPrepare {
    static final String PREFIX="$psxInstrument$";
    static final ClassDesc REC=ClassDesc.of("psx.exporter.InstrumentRecorder");
    static final ClassDesc G=ClassDesc.of("java.awt.Graphics2D"), TRANS=ClassDesc.of("java.awt.geom.AffineTransform"), RECT=ClassDesc.of("java.awt.Rectangle");
    static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void), DRAW=MethodTypeDesc.of(CD_void,G), DYNAMIC=MethodTypeDesc.of(CD_void,G,CD_boolean);
    final Prepare base;
    final List<String> evidence=new ArrayList<>();
    boolean lowerQualified;
    private InstrumentLowerPrepare lowerWork;
    private List<Instruction> lettering,scale,colour,letterStroke,scaleStroke,green;
    InstrumentPrepare(Prepare base){this.base=base;}
    private MethodModel method(String owner,String name,String descriptor){
        MethodModel m=base.methods.get(new Prepare.MethodId(owner,name,descriptor));
        if(m==null||m.code().isEmpty()||(m.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))!=0)
            throw new IllegalArgumentException("instrument method "+owner+"."+name+descriptor);
        return m;
    }
    private static int unique(List<Instruction> code,java.util.function.Predicate<Instruction> test,String label){
        int found=-1;for(int i=0;i<code.size();i++)if(test.test(code.get(i))){if(found!=-1)throw new IllegalArgumentException("instrument ambiguous "+label);found=i;}
        if(found<0)throw new IllegalArgumentException("instrument absent "+label);return found;
    }
    private static boolean constant(Instruction i,Object value){return i instanceof ConstantInstruction c&&value.equals(c.constantValue());}
    private static boolean read(Instruction i,String owner,String field){return i.opcode()==Opcode.GETSTATIC&&DisplayPrepare.field(i,owner,field);}
    private static List<Instruction> linear(List<Instruction> code,int from,int to){
        if(from<0||to>code.size()||from>=to)throw new IllegalArgumentException("instrument block bounds");
        List<Instruction> result=List.copyOf(code.subList(from,to));
        for(Instruction i:result)if(i instanceof BranchInstruction||i instanceof LookupSwitchInstruction||i instanceof TableSwitchInstruction||i.opcode()==Opcode.PUTSTATIC||i.opcode()==Opcode.PUTFIELD||i instanceof ReturnInstruction)
            throw new IllegalArgumentException("instrument block is not drawing-only");
        return result;
    }
    private static List<Instruction> strokeBefore(List<Instruction> code,int before){
        int end=-1,start=-1;
        for(int n=before-1;n>=0;n--)if(DisplayPrepare.call(code.get(n),"java/awt/Graphics2D","setStroke")){end=n+1;break;}
        for(int n=end-2;n>=Math.max(0,end-20);n--)if(code.get(n) instanceof NewObjectInstruction v&&v.className().asInternalName().equals("java/awt/BasicStroke")){start=n-1;break;}
        if(start<0||code.get(start).opcode()!=Opcode.ALOAD_0)throw new IllegalArgumentException("instrument native lettering/scale stroke");
        return linear(code,start,end);
    }
    void qualify(){
        var nativeDraw=Prepare.instructions(method("ThrStand","drawIt",DYNAMIC.descriptorString()));
        var dynamic=method("ThrStand","drawStabTrimIndicators",DYNAMIC.descriptorString());
        var letters=method("ThrStand","drawStabTrLettering","(Ljava/awt/Graphics2D;I)V");
        int letter=-1;for(int n=2;n<nativeDraw.size();n++)if(DisplayPrepare.call(nativeDraw.get(n),"ThrStand","drawStabTrLettering")&&constant(nativeDraw.get(n-1),-786)){
            if(letter!=-1)throw new IllegalArgumentException("instrument captain lettering ambiguity");letter=n;
        }
        if(letter<0||nativeDraw.get(letter-2).opcode()!=Opcode.ALOAD_0)throw new IllegalArgumentException("instrument captain lettering block");
        lettering=linear(nativeDraw,letter-2,letter+1);
        int s=-1;for(int n=letter+1;n<nativeDraw.size();n++)if(read(nativeDraw.get(n),"ThrStand","pStabTrScale")){s=n;break;}
        if(s<5||!constant(nativeDraw.get(s-4),-786)||!DisplayPrepare.call(nativeDraw.get(s-2),"java/awt/Graphics2D","translate")||!DisplayPrepare.call(nativeDraw.get(s+1),"java/awt/Graphics2D","draw"))
            throw new IllegalArgumentException("instrument captain scale block");
        scale=linear(nativeDraw,s-5,s+2);letterStroke=strokeBefore(nativeDraw,letter);scaleStroke=strokeBefore(nativeDraw,s-5);
        int c=-1;for(int n=letter-1;n>=0;n--)if(DisplayPrepare.call(nativeDraw.get(n),"java/awt/Graphics2D","setColor")){c=n;break;}
        if(c<2||!read(nativeDraw.get(c-1),"Lights","aisleLettBrightT144"))throw new IllegalArgumentException("instrument native lettering colour");
        colour=linear(nativeDraw,c-2,c+1);
        int mid=unique(nativeDraw,i->read(i,"Lights","stabMidGreenColor"),"centre green band");
        int end=mid+1;while(end<nativeDraw.size()&&!DisplayPrepare.call(nativeDraw.get(end),"java/awt/Graphics2D","fillRect"))end++;
        if(end>=nativeDraw.size()||!constant(nativeDraw.get(mid+3),-755)||!constant(nativeDraw.get(mid+4),-186)||!constant(nativeDraw.get(mid+5),8)||!constant(nativeDraw.get(mid+6),58))
            throw new IllegalArgumentException("instrument captain green band geometry");
        green=linear(nativeDraw,mid-1,end+1);
        closure(letters);closure(dynamic);
        long origins=Prepare.instructions(dynamic).stream().filter(i->read(i,"ThrStand","x")||read(i,"ThrStand","y")).count();
        long transforms=Prepare.instructions(dynamic).stream().filter(i->read(i,"ThrStand","noTrans")).count();
        long clips=Prepare.instructions(dynamic).stream().filter(i->DisplayPrepare.field(i,"Visuals","mainClip")).count();
        if(origins!=8||transforms!=1||clips!=2)throw new IllegalArgumentException("instrument native origin/scale/clip coverage "+origins+"/"+transforms+"/"+clips);
        geometryWriters();
        lowerWork=new InstrumentLowerPrepare(base);
        try{lowerWork.qualify();lowerQualified=true;evidence.addAll(lowerWork.evidence);}
        catch(Throwable unavailable){lowerQualified=false;evidence.add("Lower eligibility unavailable: "+unavailable);}
        evidence.add("captain drawing: original lettering/strokes/scale/centre-band blocks; native dynamic helper; zero native field/array writes in drawing closure");
        evidence.add("export context only: native x/y reads -> zero, noTrans -> canonical transform, mainClip -> owned crop; ordinary contexts unchanged");
        for(String owner:List.of("ThrStand","PanelFont","Lights","Visuals"))evidence.add(owner+" original.sha256="+RecorderPrepare.hashUnchecked(base.inputs.get(owner)));
    }
    private void geometryWriters(){
        Set<String> fields=Set.of("x","y","noTrans","yStabTrimBarL","yStabTrimBarR","greenband","greenbandUpd","pStabTrScale","pStabTrimOffSlashes","pStabTrimOffWord");
        Set<String> allowed=Set.of("<clinit>","init","situLoadInit","runDelta","keyHolder","drawIt");Set<Prepare.MethodId> writers=new TreeSet<>(Comparator.comparing(Object::toString));
        for(var entry:base.methods.entrySet())if(entry.getValue().code().isPresent())for(Instruction i:Prepare.instructions(entry.getValue()))if(i instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("ThrStand")&&fields.contains(f.name().stringValue())){
            if(!entry.getKey().owner().equals("ThrStand")||!allowed.contains(entry.getKey().name()))throw new IllegalArgumentException("instrument unqualified geometry writer "+entry.getKey()+" "+f.name());writers.add(entry.getKey());
        }
        for(var writer:writers)evidence.add("native instrument writer "+writer+" retained unchanged");
        MethodModel presto=base.methods.get(new Prepare.MethodId("Core","runPresto","()V"));
        if(presto!=null&&Prepare.instructions(presto).stream().filter(i->DisplayPrepare.call(i,"ThrStand","runDelta")).count()!=1)throw new IllegalArgumentException("instrument ordinary animation update seam");
        evidence.add("native animation remains in ordinary Core.runPresto; hidden/minimized loaded execution is not established by static qualification");
    }
    private void closure(MethodModel root){
        Set<Prepare.MethodId> visited=new HashSet<>();ArrayDeque<MethodModel> pending=new ArrayDeque<>();pending.add(root);
        Set<String> graphics=Set.of("translate","scale","draw","fill","fillRect","setColor","setStroke","setRenderingHint","getTransform","setTransform","setClip","clipRect");
        while(!pending.isEmpty()){
            MethodModel m=pending.removeFirst();String owner=m.parent().orElseThrow().thisClass().asInternalName();
            var id=new Prepare.MethodId(owner,m.methodName().stringValue(),m.methodType().stringValue());if(!visited.add(id))continue;
            if(!m.code().orElseThrow().exceptionHandlers().isEmpty())throw new IllegalArgumentException("instrument drawing exceptional closure "+id);
            for(Instruction i:Prepare.instructions(m)){
                if(i instanceof FieldInstruction hint&&hint.owner().asInternalName().equals("java/awt/RenderingHints")&&!Set.of("KEY_ANTIALIASING","VALUE_ANTIALIAS_OFF","VALUE_ANTIALIAS_ON").contains(hint.name().stringValue()))
                    throw new IllegalArgumentException("instrument unqualified native rendering hint "+hint.name());
                if(i.opcode()==Opcode.PUTSTATIC||i.opcode()==Opcode.PUTFIELD||Set.of(Opcode.IASTORE,Opcode.LASTORE,Opcode.FASTORE,Opcode.DASTORE,Opcode.AASTORE,Opcode.BASTORE,Opcode.CASTORE,Opcode.SASTORE,Opcode.MONITORENTER,Opcode.MONITOREXIT).contains(i.opcode())||i instanceof InvokeDynamicInstruction)
                    throw new IllegalArgumentException("instrument drawing side effect "+id+" "+i.opcode());
                if(i instanceof InvokeInstruction call){String o=call.owner().asInternalName(),n=call.name().stringValue(),d=call.type().stringValue();
                    if(o.equals("PanelFont")&&n.equals("writeWord")&&d.equals("(Ljava/awt/Graphics2D;Ljava/lang/String;FFF)V")){pending.add(method(o,n,d));continue;}
                    boolean allowed=(o.equals("java/awt/Graphics2D")&&graphics.contains(n))||(o.equals("java/lang/String")&&Set.of("length","charAt").contains(n))||(o.equals("java/awt/geom/AffineTransform")&&Set.of("<init>","getScaleX").contains(n))||((o.equals("java/awt/BasicStroke")||o.equals("java/awt/geom/Rectangle2D$Float"))&&n.equals("<init>"));
                    if(!allowed)throw new IllegalArgumentException("instrument unqualified drawing call "+o+"."+n+d);
                }
            }
            evidence.add("read-only drawing closure "+id);
        }
    }
    private static void guard(CodeBuilder cb,java.util.function.Consumer<CodeBuilder> action){
        cb.trying(c->action.accept(c),h->h.catching(CD_Throwable,c->{c.pop();c.trying(n->n.invokestatic(REC,"failed",VOID),f->f.catching(CD_Throwable,n->n.pop()));}));
    }
    private static void valueBridge(ClassBuilder b,String name,ClassDesc value){
        var type=MethodTypeDesc.of(value,value,G);b.withMethodBody(PREFIX+name,type,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            int result=cb.allocateLocal(TypeKind.from(value).asLoadable());cb.loadLocal(TypeKind.from(value).asLoadable(),0).storeLocal(TypeKind.from(value).asLoadable(),result);
            guard(cb,c->c.loadLocal(TypeKind.from(value).asLoadable(),0).aload(1).invokestatic(REC,name,type).storeLocal(TypeKind.from(value).asLoadable(),result));
            cb.loadLocal(TypeKind.from(value).asLoadable(),result).return_(TypeKind.from(value).asLoadable());
        });
    }
    Map<String,byte[]> transform(Map<String,byte[]> lower){
        Map<String,byte[]> result=new TreeMap<>(),accessBase=lower;ClassDesc thr=ClassDesc.of("ThrStand");
        if(lowerQualified)try{Map<String,byte[]> eligibility=lowerWork.transform(lower);Map<String,byte[]> complete=new TreeMap<>(lower);complete.putAll(eligibility);TerrainAccess.verifyAdded(lower,complete,base.inputs);result.putAll(eligibility);accessBase=complete;}
        catch(Throwable unavailable){lowerQualified=false;evidence.add("Lower helper unavailable: "+unavailable);}
        byte[] before=lower.getOrDefault("ThrStand",base.inputs.get("ThrStand"));
        byte[] transformed=base.cf.transformClass(base.cf.parse(before),(b,e)->{
            if(e instanceof MethodModel m&&m.methodName().equalsString("drawStabTrimIndicators")&&m.methodType().equalsString(DYNAMIC.descriptorString()))
                b.transformMethod(m,(mb,me)->{if(me instanceof CodeModel code)mb.transformCode(code,(cb,i)->{
                    cb.with(i);if(readInstruction(i,"ThrStand","x")||readInstruction(i,"ThrStand","y"))cb.aload(0).invokestatic(thr,PREFIX+"origin",MethodTypeDesc.of(CD_int,CD_int,G));
                    else if(readInstruction(i,"ThrStand","noTrans"))cb.aload(0).invokestatic(thr,PREFIX+"transform",MethodTypeDesc.of(TRANS,TRANS,G));
                    else if(i instanceof FieldInstruction f&&f.opcode()==Opcode.GETFIELD&&DisplayPrepare.field(i,"Visuals","mainClip"))cb.aload(0).invokestatic(thr,PREFIX+"clip",MethodTypeDesc.of(RECT,RECT,G));
                });else mb.with(me);});else b.with(e);
        });
        transformed=base.cf.transformClass(base.cf.parse(transformed),ClassTransform.endHandler(b->{
            valueBridge(b,"origin",CD_int);valueBridge(b,"transform",TRANS);valueBridge(b,"clip",RECT);
            b.withMethodBody(PREFIX+"draw",DRAW,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
                cb.aload(0).invokevirtual(G,"getTransform",MethodTypeDesc.of(TRANS)).astore(1);
                for(Instruction i:colour)cb.with(i);for(Instruction i:letterStroke)cb.with(i);for(Instruction i:lettering)cb.with(i);
                cb.aload(0).aload(1).invokevirtual(G,"setTransform",MethodTypeDesc.of(CD_void,TRANS));
                for(Instruction i:scaleStroke)cb.with(i);for(Instruction i:scale)cb.with(i);
                cb.aload(0).aload(1).invokevirtual(G,"setTransform",MethodTypeDesc.of(CD_void,TRANS));for(Instruction i:green)cb.with(i);
                cb.aload(0).iconst_1().invokestatic(thr,"drawStabTrimIndicators",DYNAMIC).return_();
            });
        }));result.put("ThrStand",transformed);
        ClassDesc visuals=ClassDesc.of("Visuals");before=lower.getOrDefault("Visuals",base.inputs.get("Visuals"));
        int[] sites={0};transformed=base.cf.transformClass(base.cf.parse(before),(b,e)->{
            if(e instanceof MethodModel m&&m.methodName().equalsString(DisplayPrepare.PREFIX+"event")&&m.methodType().equalsString(DisplayPrepare.EVENT.descriptorString())){
                sites[0]++;b.transformMethod(m,(mb,me)->{if(me instanceof CodeModel code)mb.transformCode(code,(cb,i)->{
                    if(i instanceof Instruction instruction&&instruction.opcode()==Opcode.RETURN)guard(cb,c->c.iload(0).iload(1).invokestatic(visuals,PREFIX+"event",DisplayPrepare.EVENT));cb.with(i);
                });else mb.with(me);});
            }else b.with(e);
        });if(sites[0]!=1)throw new IllegalArgumentException("instrument successful paint seam");
        transformed=base.cf.transformClass(base.cf.parse(transformed),ClassTransform.endHandler(b->b.withMethodBody(PREFIX+"event",DisplayPrepare.EVENT,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
            guard(cb,c->{c.iload(0).iload(1).invokestatic(REC,"event",DisplayPrepare.EVENT);Label done=c.newLabel();
                c.iload(0).loadConstant(DisplayProbe.PAINT_OUT).if_icmpne(done).iload(1).ifne(done);
                int graphics=c.allocateLocal(TypeKind.REFERENCE);c.invokestatic(REC,"begin",MethodTypeDesc.of(G)).astore(graphics).aload(graphics).ifnull(done);
                if(lowerQualified)c.invokestatic(InstrumentLowerPrepare.IDS,InstrumentLowerPrepare.METHOD,MethodTypeDesc.of(CD_long)).invokestatic(REC,"lower",MethodTypeDesc.of(CD_void,CD_long));
                c.aload(graphics).invokestatic(thr,PREFIX+"draw",DRAW).invokestatic(REC,"complete",VOID).labelBinding(done);
            });cb.return_();
        })));result.put("Visuals",transformed);
        for(var entry:result.entrySet())if(!base.cf.verify(entry.getValue()).isEmpty())throw new IllegalArgumentException("instrument generated bytecode "+entry.getKey());
        Map<String,byte[]> finalClasses=new TreeMap<>(accessBase);finalClasses.putAll(result);
        if(accessBase!=lower){
            // Reuse only direct-member proofs whose caller and referenced owner
            // bytes remain identical in the final definitions. Inherited lookup
            // or an Upper replacement changing that context requires a full audit.
            boolean stable=true;Map<String,ClassModel> context=new HashMap<>();
            contextCheck:for(String caller:InstrumentLowerPrepare.OWNERS){
                if(!Arrays.equals(accessBase.get(caller),finalClasses.get(caller))){stable=false;break;}
                ClassModel type=base.cf.parse(accessBase.get(caller));
                for(MethodModel m:type.methods())if(m.methodName().equalsString(InstrumentLowerPrepare.METHOD))for(CodeElement i:m.code().orElseThrow()){
                    String owner,name,descriptor;boolean field;
                    if(i instanceof FieldInstruction f){owner=f.owner().asInternalName();name=f.name().stringValue();descriptor=f.type().stringValue();field=true;}
                    else if(i instanceof InvokeInstruction call){owner=call.owner().asInternalName();name=call.name().stringValue();descriptor=call.type().stringValue();field=false;}
                    else continue;
                    byte[] verified=accessBase.getOrDefault(owner,base.inputs.get(owner)),finished=finalClasses.getOrDefault(owner,base.inputs.get(owner));
                    if(!Arrays.equals(verified,finished)){stable=false;break contextCheck;}
                    if(verified==null){
                        if(!Set.of("psx/exporter/InstrumentRecorder","java/lang/invoke/VarHandle").contains(owner)){stable=false;break contextCheck;}
                        continue;
                    }
                    ClassModel target=context.get(owner);if(target==null){target=base.cf.parse(verified);context.put(owner,target);}
                    boolean declared=field?target.fields().stream().anyMatch(f->f.fieldName().equalsString(name)&&f.fieldType().equalsString(descriptor))
                        :target.methods().stream().anyMatch(method->method.methodName().equalsString(name)&&method.methodType().equalsString(descriptor));
                    if(!declared){stable=false;break contextCheck;}
                }
            }
            if(!stable)accessBase=lower;
            evidence.add(stable?"Lower access proof retained: identical final callers/direct-member contexts":"Lower access proof re-audited: inherited or changed final context");
        }
        TerrainAccess.verifyAdded(accessBase,finalClasses,base.inputs);
        evidence.add("complete helper/access verification; original paint, drawIt, Presto and dynamics invocation counts unchanged");return result;
    }
    private static boolean readInstruction(CodeElement i,String owner,String name){return i instanceof Instruction v&&read(v,owner,name);}
}
