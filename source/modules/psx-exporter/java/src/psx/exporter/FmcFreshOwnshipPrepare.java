package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Reuse the installed loader's synthetic-slot coordinate calculation only
 * inside the fresh route factory. The aircraft/FMC/IRS position is read-only.
 * This does not publish a route or establish a coherent live navigation input;
 * that witness and exclusion remain requirements of the complete adapter. */
public final class FmcFreshOwnshipPrepare {
    static final String BODY="Routes$RteBody",HEADER="FmcRte$Rte",POS="FmcGlobal$Pos";
    static final String NAME="$psxExporter$fmcFreshOwnship";
    private static final ClassDesc B=ClassDesc.of(BODY),H=ClassDesc.of(HEADER),W=ClassDesc.of("RouteWpt"),P=ClassDesc.of(POS),R=ClassDesc.of("Routes");
    private static final MethodTypeDesc ENTRY=MethodTypeDesc.of(CD_void,CD_Object);
    record Qualified(Map<String,byte[]> changed,int matchedInstructions){}
    private FmcFreshOwnshipPrepare(){}
    private static void require(boolean yes,String reason){if(!yes)throw new IllegalArgumentException("FMC fresh ownship "+reason);}
    private static MethodModel method(Map<String,ClassModel> models,String owner,String name,String descriptor){
        var m=models.get(owner);require(m!=null,"method owner");var all=m.methods().stream().filter(x->x.methodName().equalsString(name)&&x.methodType().equalsString(descriptor)).toList();
        require(all.size()==1&&all.getFirst().code().isPresent(),"method descriptor");return all.getFirst();
    }
    private static void field(Map<String,ClassModel> models,String owner,String name,String type,boolean stat){
        var m=models.get(owner);require(m!=null,"field owner");var all=m.fields().stream().filter(x->x.fieldName().equalsString(name)).toList();
        require(all.size()==1&&all.getFirst().fieldType().equalsString(type)&&((all.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==stat,"field descriptor/staticness");
        if(owner.equals("RouteWpt"))require((all.getFirst().flags().flagsMask()&ClassFile.ACC_FINAL)==0,"writable synthetic coordinate");
    }
    private static String meaning(Instruction i){
        if(i instanceof LoadInstruction l)return "load:"+l.typeKind()+":"+l.slot();
        if(i instanceof ConstantInstruction c){var v=c.constantValue();return "constant:"+(v==null?"null":v.getClass().getName()+":"+v);}
        if(i instanceof FieldInstruction f)return i.opcode()+":"+f.owner().asInternalName()+"."+f.name().stringValue()+":"+f.type().stringValue();
        return i.opcode().name();
    }
    // Radians are narrowed to the native float exactly once. Slots 0 and 1
    // are synthetic anchors; real waypoint coordinates are entered content.
    private static void anchors(CodeBuilder c){
        for(String coordinate:List.of("lat","lon")){
            c.aload(0).getfield(B,"leg",W.arrayType()).iconst_1().aaload().getstatic(R,"nrp",P)
                .getfield(P,coordinate,CD_double).d2f().putfield(W,coordinate,CD_float);
        }
        for(String coordinate:List.of("lat","lon")){
            c.aload(0).getfield(B,"leg",W.arrayType()).iconst_0().aaload()
                .aload(0).getfield(B,"leg",W.arrayType()).iconst_1().aaload()
                .getfield(W,coordinate,CD_float).putfield(W,coordinate,CD_float);
        }
    }
    private static List<String> expected(Prepare data){
        var bytes=data.cf.build(B,b->b.withMethodBody("anchorPattern",MethodTypeDesc.of(CD_void),ClassFile.ACC_PUBLIC,c->{anchors(c);c.return_();}));
        var m=data.cf.parse(bytes).methods().getFirst();var ins=Prepare.instructions(m);
        return ins.subList(0,ins.size()-1).stream().map(FmcFreshOwnshipPrepare::meaning).toList();
    }
    private static int occurrence(MethodModel method,List<String> pattern){
        require((method.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_ABSTRACT|ClassFile.ACC_NATIVE))==0,"native instance loader");
        var ins=Prepare.instructions(method);int matches=0;
        for(int i=0;i+pattern.size()<=ins.size();i++){
            boolean same=true;for(int n=0;n<pattern.size();n++)if(!meaning(ins.get(i+n)).equals(pattern.get(n))){same=false;break;}
            if(same)matches++;
        }
        require(matches==1,"unique native ownship expression");return pattern.size();
    }
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var models=FmcQualification.finalModels(data,lower);var pattern=expected(data);
        for(var map:List.of(data.models,models)){
            field(map,BODY,"leg","[LRouteWpt;",false);field(map,HEADER,"rteBody","LRoutes$RteBody;",false);
            field(map,"Routes","nrp","LFmcGlobal$Pos;",true);
            field(map,POS,"lat","D",false);field(map,POS,"lon","D",false);
            field(map,"RouteWpt","lat","F",false);field(map,"RouteWpt","lon","F",false);
            occurrence(method(map,BODY,"readGlob","(Ljava/lang/String;)V"),pattern);
        }
        var changed=new HashMap<>(FmcFreshSelectionPrepare.transform(data,lower).changed());
        var body=data.cf.parse(changed.getOrDefault(BODY,data.inputs.get(BODY)));
        require(body.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"entry collision");
        changed.put(BODY,data.cf.transformClass(body,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){b.withMethodBody(NAME,ENTRY,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                c.aload(0).checkcast(B).astore(0);anchors(c);c.return_();
            });}
        }));
        var header=data.cf.parse(changed.get(HEADER));var factory=method(Map.of(HEADER,header),HEADER,FmcFreshSelectionPrepare.NAME,"(IIIIIII)Ljava/lang/Object;");
        require(Prepare.instructions(factory).stream().filter(i->i.opcode()==Opcode.ARETURN).count()==1,"fresh factory return");
        changed.put(HEADER,data.cf.transformClass(header,(b,e)->{
            if(e instanceof MethodModel m&&m.methodName().equalsString(FmcFreshSelectionPrepare.NAME)&&m.methodType().equalsString("(IIIIIII)Ljava/lang/Object;"))b.transformMethod(m,MethodTransform.transformingCode((c,x)->{
                if(x instanceof Instruction i&&i.opcode()==Opcode.ARETURN)c.dup().getfield(H,"rteBody",B).invokestatic(B,NAME,ENTRY);
                c.with(x);
            }));else b.with(e);
        }));
        for(var bytes:changed.values())require(data.cf.verify(bytes).isEmpty(),"generated verification");
        TerrainAccess.verifyAdded(lower,changed,data.inputs);
        return new Qualified(Map.copyOf(changed),pattern.size());
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var result=transform(data,Map.of());
        Files.writeString(Path.of(args[1]),"{\"schema\":1,\"state\":\"fresh_ownship_component\",\"captureQualified\":false,\"recallQualified\":false,\"matchedNativeInstructions\":"+result.matchedInstructions()+",\"bodyInputSha256\":"+FmcQualification.quote(Identity.hash(data.inputs.get(BODY)))+",\"preparedBodySha256\":"+FmcQualification.quote(Identity.hash(result.changed().get(BODY)))+",\"limit\":\"Fresh factory coordinate calculation only; coherent live navigation input, full reconstruction/exclusion/publication/adapter remain unavailable. No installed class definition/initialization/invocation.\"}\n",StandardOpenOption.CREATE_NEW);
        System.out.println("Fresh synthetic anchors use current native FMC position; component only, capture/recall unavailable");
    }
}
