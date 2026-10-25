package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.attribute.CodeAttribute;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Separate the instructor terrain map's final FMC-route overlay. Terrain,
 * plotter history and graphics before that overlay execute on their original
 * path. Only this route reader participates in FMC admission. The installed
 * renderer is byte data during preparation; no renderer is executed here. */
final class FmcTerrainRoutePrepare {
    static final String OWNER="TerrainReader",NAME="$psxExporter$fmcTerrainRoute";
    static final Prepare.MethodId ORIGINAL=new Prepare.MethodId(OWNER,"drawLand","(Ljava/awt/Graphics2D;)V");
    static final Prepare.MethodId REGION=new Prepare.MethodId(OWNER,NAME,"(Ljava/awt/Graphics2D;LRoutes$RteBody;LTerrainReader$FloatXY;)V");
    private static final ClassDesc BODY=ClassDesc.of("Routes$RteBody"),TERRAIN=ClassDesc.of(OWNER);
    private static final Set<String> FMC=Set.of("RouteWpt","Routes$RteBody","FmcRte$Rte","FmcRte$Rwy","FmcPerf");
    private static final Set<String> GRAPHICS=Set.of("java/awt/Color.<init>(III)V","java/awt/BasicStroke.<init>(FII)V",
        "java/awt/geom/Line2D$Float.<init>(FFFF)V","java/awt/Graphics2D.setColor(Ljava/awt/Color;)V",
        "java/awt/Graphics2D.setStroke(Ljava/awt/Stroke;)V","java/awt/Graphics2D.draw(Ljava/awt/Shape;)V");
    record Result(Map<String,byte[]> changed,int nativeReads,int splitInstruction){}
    private FmcTerrainRoutePrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC terrain route region "+why);}
    private static boolean active(Instruction i){return i instanceof FieldInstruction f&&f.opcode()==Opcode.GETSTATIC
        &&f.owner().asInternalName().equals("Routes")&&f.name().equalsString("actBody")&&f.type().equalsString("LRoutes$RteBody;");}
    private static Prepare.MethodId target(InvokeInstruction call){return new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());}
    private static boolean coordinate(Prepare.MethodId id){return id.owner().equals(OWNER)&&Set.of("calcXY","calcArcticXY","calcAntarcticXY").contains(id.name())&&id.descriptor().equals("(FF)LTerrainReader$FloatXY;")
        ||id.owner().equals("Calc$LambertConformalProj")&&id.name().equals("calc")&&id.descriptor().equals("(DD)V");}
    private static String bodyIdentity(Prepare data,Prepare.MethodId id,MethodModel method)throws Exception{
        return Identity.hash(data.cf.build(ClassDesc.of(id.owner()),b->b.withMethod(id.name(),method.methodTypeSymbol(),method.flags().flagsMask(),
            m->m.transformCode(method.code().orElseThrow(),(c,e)->c.with(e)))));
    }
    private static void coordinates(Prepare data,Map<String,ClassModel> models,Set<Prepare.MethodId> entries)throws Exception{
        var pending=new ArrayDeque<>(entries);var seen=new HashSet<Prepare.MethodId>();
        while(!pending.isEmpty()){
            var id=pending.removeFirst();if(!seen.add(id))continue;require(seen.size()<=5,"coordinate effect closure");
            var owner=models.get(id.owner());require(owner!=null,"coordinate owner");var method=owner.methods().stream()
                .filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).findFirst().orElseThrow();
            require(method.code().isPresent()&&(method.flags().flagsMask()&(ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_STATIC))==0,"coordinate method flags");
            var original=data.methods.get(id);require(original!=null&&bodyIdentity(data,id,method).equals(bodyIdentity(data,id,original)),"coordinate lower body drift");
            for(var op:Prepare.instructions(method)){
                require(!(op instanceof InvokeDynamicInstruction),"coordinate dynamic call");
                if(op instanceof FieldInstruction f){require((f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.PUTFIELD)
                    &&Set.of(OWNER,"TerrainReader$FloatXY","Calc$LambertConformalProj","Calc$Projection").contains(f.owner().asInternalName())&&Set.of("Z","I","F","D").contains(f.type().stringValue()),"coordinate external/reference/static field effect");}
                if(op instanceof InvokeInstruction call){var to=target(call);String name=to.toString();
                    if(name.equals("java/lang/Object.<init>()V")||Set.of("java/lang/Math.sin(D)D","java/lang/Math.sqrt(D)D","java/lang/Math.cos(D)D","java/lang/Math.abs(D)D","java/lang/Math.pow(DD)D","java/lang/Double.isNaN(D)Z").contains(name))continue;
                    require(name.equals("TerrainReader$FloatXY.<init>()V"),"coordinate external call "+to);pending.add(to);
                }
            }
        }
        // The ordinary receiver is the installed TerrainReader. Reject any
        // installed override that could replace the qualified coordinate body.
        for(var entry:models.entrySet())if(!entry.getKey().equals(OWNER)){
            String at=entry.getKey();var visited=new HashSet<String>();boolean child=false;
            while(visited.add(at)){var model=models.get(at);if(model==null||model.superclass().isEmpty())break;at=model.superclass().get().asInternalName();if(at.equals(OWNER)){child=true;break;}}
            if(child)for(var id:entries)require(entry.getValue().methods().stream().noneMatch(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())),"coordinate virtual override");
        }
    }
    private static void local(CodeBuilder b,CodeElement e){
        if(e instanceof LoadInstruction l){b.loadLocal(l.typeKind(),l.slot()>=2?l.slot()+2:l.slot());return;}
        if(e instanceof StoreInstruction s){b.storeLocal(s.typeKind(),s.slot()>=2?s.slot()+2:s.slot());return;}
        if(e instanceof IncrementInstruction n){b.iinc(n.slot()>=2?n.slot()+2:n.slot(),n.constant());return;}
        // Debug locals refer to the original host's scope; frame/max-local
        // information is regenerated by ClassFile for the extracted method.
        if(e instanceof LocalVariable||e instanceof LocalVariableType)return;
        b.with(e);
    }
    static Result transform(Prepare data,Map<String,byte[]> lower)throws Exception{
        byte[] input=lower.getOrDefault(OWNER,data.inputs.get(OWNER));require(input!=null,"owner");var model=data.cf.parse(input);
        var method=model.methods().stream().filter(m->m.methodName().equalsString(ORIGINAL.name())&&m.methodType().equalsString(ORIGINAL.descriptor())).findFirst().orElseThrow();
        require(method.code().isPresent()&&(method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==0,"ordinary instance renderer");
        var code=method.code().get();require(code instanceof CodeAttribute&&code.exceptionHandlers().isEmpty(),"unqualified exception region");
        require(model.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"region collision");
        var ops=Prepare.instructions(method);var locations=new ArrayList<Integer>();int bci=0,split=-1,reads=0;
        for(int i=0;i<ops.size();i++){locations.add(bci);bci+=ops.get(i).sizeInBytes();if(active(ops.get(i))){require(split<0,"ambiguous active-body boundary");split=i;}}
        require(split>=0&&split+1<ops.size()&&ops.get(split+1) instanceof StoreInstruction s&&s.typeKind()==TypeKind.REFERENCE&&s.slot()>=2,"body local at region start");
        int begin=locations.get(split),terminal=locations.getLast();var parsed=(CodeAttribute)code;var calcs=new HashSet<Prepare.MethodId>();
        // Every branch from the retained terrain prefix must land in that
        // prefix or at the single region entry. No partial overlay entry is
        // silently converted into a whole-region invocation.
        for(int i=0;i<ops.size();i++){
            var op=ops.get(i);var targets=new ArrayList<Label>();
            if(op instanceof BranchInstruction branch)targets.add(branch.target());
            if(op instanceof LookupSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}
            if(op instanceof TableSwitchInstruction sw){targets.add(sw.defaultTarget());for(var c:sw.cases())targets.add(c.target());}
            for(var target:targets){int to=parsed.labelToBci(target);require(i<split?to<=begin||to==terminal:to>=begin,"cross-region branch at "+i+" to "+Collections.binarySearch(locations,to));}
            if(op instanceof FieldInstruction f&&FMC.contains(f.owner().asInternalName())){
                require(i>=split,"FMC read before region");
                require(f.opcode()==Opcode.GETFIELD,"FMC mutation/static read in render region");
                if(f.owner().asInternalName().equals("RouteWpt")){
                    require(Set.of("lat","lon").contains(f.name().stringValue())&&f.type().equalsString("F"),"unqualified waypoint render field");reads++;
                }else require(f.owner().asInternalName().equals("Routes$RteBody")&&
                    (f.name().equalsString("leg")&&f.type().equalsString("[LRouteWpt;")||f.name().equalsString("legsAmo")&&f.type().equalsString("I")),"unqualified route render field");
            }
            if(i>=split){
                require(!(op instanceof InvokeDynamicInstruction),"dynamic overlay call");
                if(op instanceof InvokeInstruction call){var to=target(call);
                    if(coordinate(to))calcs.add(to);else require(GRAPHICS.contains(to.toString()),"unqualified overlay call "+to);
                }
                if(op instanceof FieldInstruction f&&(f.opcode()==Opcode.PUTFIELD||f.opcode()==Opcode.PUTSTATIC))
                    require(f.opcode()==Opcode.PUTFIELD&&Set.of(OWNER,"TerrainReader$FloatXY").contains(f.owner().asInternalName())&&Set.of("I","F","D").contains(f.type().stringValue()),"overlay external/reference/static mutation");
            }
            require(op.opcode()!=Opcode.JSR&&op.opcode()!=Opcode.JSR_W&&op.opcode()!=Opcode.RET,"subroutine");
        }
        require(reads>0&&ops.getLast().opcode()==Opcode.RETURN,"complete coordinate overlay");
        coordinates(data,FmcQualification.finalModels(data,lower),calcs);
        final int cut=split;
        byte[] extracted=data.cf.transformClass(model,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){
                if(!(e instanceof MethodModel m)||!m.methodName().equalsString(ORIGINAL.name())||!m.methodType().equalsString(ORIGINAL.descriptor())){b.with(e);return;}
                b.transformMethod(m,MethodTransform.transformingCode(new CodeTransform(){
                    int ordinal;
                    public void accept(CodeBuilder c,CodeElement x){
                        if(x instanceof Instruction){
                            if(ordinal==cut)c.aload(0).aload(1).getstatic(ClassDesc.of("Routes"),"actBody",BODY).aload(2)
                                .invokespecial(TERRAIN,NAME,MethodTypeDesc.ofDescriptor(REGION.descriptor())).return_();
                            if(ordinal<cut||ordinal==ops.size()-1)c.with(x);ordinal++;
                        }else if((ordinal<=cut||x instanceof LabelTarget label&&parsed.labelToBci(label.label())==terminal)
                            &&!(x instanceof LocalVariable)&&!(x instanceof LocalVariableType))c.with(x);
                    }
                }));
            }
            public void atEnd(ClassBuilder b){
                b.withMethodBody(NAME,MethodTypeDesc.ofDescriptor(REGION.descriptor()),ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC,c->{
                    // Lambert projection reuses the XY scratch object created
                    // in the retained terrain prefix. Preserve that exact
                    // reference; do not create a different native scratch.
                    c.aload(3).astore(4);
                    int ordinal=0;
                    for(var e:code){
                        if(e instanceof Instruction){if(ordinal>=cut){if(ordinal==cut)c.aload(2);else local(c,e);}ordinal++;}
                        else if(ordinal>=cut)local(c,e);
                    }
                });
            }
        });
        require(data.cf.verify(extracted).isEmpty(),"extraction bytecode verification");
        // Admission sees the original active body as argument2. Native locals
        // are shifted above it, so callback identity cannot be overwritten by
        // a renderer's XY scratch local. Non-owned bodies continue normally.
        var base=new HashMap<>(lower);base.put(OWNER,extracted);
        var admitted=FmcExclusionPrepare.terrainRegion(data,base);
        var changed=new HashMap<>(lower);changed.putAll(admitted);var before=new HashMap<>(lower);before.put(OWNER,input);
        TerrainAccess.verifyAdded(before,changed,data.inputs);return new Result(Map.copyOf(changed),reads,cut);
    }
}
