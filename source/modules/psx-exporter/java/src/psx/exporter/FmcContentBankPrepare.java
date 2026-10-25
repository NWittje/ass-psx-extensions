package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Preparation-only neutral access to route/performance bank candidates.
 * These are not restore targets or an applied-memory profile. Freshness,
 * native caches, writers and exclusion must qualify independently before an
 * optional FMC capability can be advertised. Installed classes remain data.
 * Generated access is worker-only; it never calls transfer/isUpdated or clears
 * native input flags. The returned bank objects are live external bindings. */
public final class FmcContentBankPrepare {
    static final String BANK_METHOD="$psxExporter$fmcContentBanks";
    static final List<String> CONTENT=List.of(
        "qsDepArrSel0","qsDepArrSel1","qsDepArrSel2","qsDepArr",
        "qsFmcRte0","qsFmcRte1","qsFmcRte2",
        "qsFmcRteApts0","qsFmcRteApts1","qsFmcRteApts2",
        "qsFmcDepRwys0","qsFmcDepRwys1","qsFmcDepRwys2",
        "qsFmcArrRwys0","qsFmcArrRwys1","qsFmcArrRwys2",
        "qsFmcRteViAcMo","qsFmcPerfInit");
    // These banks remain live context. Narrow route/PERF recall must neither
    // select them as owned content nor consume their native freshness flags.
    static final List<String> PRESERVED=List.of("qsFmcPosInit","qsFmcPosBias",
        "qsFmcTkofRef","qsFmcAppRef","qsFmcThrSet","qsFmcFltNo");
    static final List<FmcMemberPrepare.Field> READS=List.of(
        new FmcMemberPrepare.Field(1,"Qs","sVar","Ljava/lang/String;",false),
        new FmcMemberPrepare.Field(2,"Q","fresh","Z",false),
        new FmcMemberPrepare.Field(3,"Q","updated","Z",false),
        new FmcMemberPrepare.Field(4,"Qs","lastComeFrom","I",false));
    record Qualified(Map<String,byte[]> changed,Map<String,String> inputHashes,
                     Map<String,List<Prepare.MethodId>> indexWriters){}
    private FmcContentBankPrepare(){}
    private static void require(boolean value,String why){
        if(!value)throw new IllegalArgumentException("FMC content banks "+why);
    }
    private static void field(Prepare data,String owner,String name,String type,boolean stat){
        var model=data.models.get(owner);require(model!=null,"missing owner "+owner);
        var matches=model.fields().stream().filter(f->f.fieldName().equalsString(name)&&f.fieldType().equalsString(type)).toList();
        require(matches.size()==1,"missing descriptor "+owner+"."+name+type);
        require(((matches.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0)==stat,"staticness "+owner+"."+name);
    }
    static Qualified qualify(Prepare data,Map<String,byte[]> lower)throws Exception{
        field(data,"Glob","qs","[LQs;",true);
        var selected=new TreeSet<FmcQualification.Member>();
        for(String name:CONTENT){field(data,"Glob",name,"I",true);selected.add(new FmcQualification.Member("Glob",name,"I"));}
        for(String name:PRESERVED){field(data,"Glob",name,"I",true);selected.add(new FmcQualification.Member("Glob",name,"I"));}
        selected.add(new FmcQualification.Member("Glob","qs","[LQs;"));
        for(var f:READS)field(data,f.owner(),f.name(),f.descriptor(),false);
        var writers=new TreeMap<String,List<Prepare.MethodId>>();
        for(var f:selected)writers.put(f.name(),new ArrayList<>());
        // Resolve inherited owner spellings as well as direct Glob references.
        for(var entry:data.methods.entrySet()){
            if(entry.getValue().code().isEmpty())continue;
            for(var instruction:Prepare.instructions(entry.getValue())){
            if(instruction instanceof FieldInstruction f&&f.opcode()==Opcode.PUTSTATIC){
                var actual=FmcQualification.resolve(data,selected,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue());
                if(actual!=null)writers.get(actual.name()).add(entry.getKey());
            }
            }
        }
        var init=new Prepare.MethodId("Glob","initAll","()V");
        for(var entry:writers.entrySet())require(entry.getValue().equals(List.of(init)),"index/array writer coverage "+entry.getKey()+" "+entry.getValue());
        var changed=new HashMap<>(FmcMemberPrepare.transform(data,lower,READS));
        byte[] input=lower.getOrDefault("Glob",data.inputs.get("Glob"));
        var source=data.cf.parse(input);
        require(source.methods().stream().noneMatch(m->m.methodName().equalsString(BANK_METHOD)),"bank bridge collision");
        ClassDesc glob=ClassDesc.of("Glob"),qs=ClassDesc.of("Qs"),array=qs.arrayType();
        byte[] output=data.cf.transformClass(source,new ClassTransform(){
            public void accept(ClassBuilder b,ClassElement e){b.with(e);}
            public void atEnd(ClassBuilder b){
                b.withMethodBody(BANK_METHOD,MethodTypeDesc.of(CD_void,CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    var bad=c.newLabel();
                    c.aload(0).arraylength().loadConstant(CONTENT.size()).if_icmpne(bad);
                    c.getstatic(glob,"qs",array).astore(1);
                    // Capture every excluded binding without consuming it.
                    for(int i=0;i<PRESERVED.size();i++)c.aload(1).getstatic(glob,PRESERVED.get(i),CD_int).aaload().astore(4+i);
                    for(int i=0;i<CONTENT.size();i++){
                        c.getstatic(glob,CONTENT.get(i),CD_int).istore(2);
                        c.aload(1).iload(2).aaload().astore(3);
                        c.aload(3).ifnull(bad);
                        for(int p=0;p<PRESERVED.size();p++)c.aload(3).aload(4+p).if_acmpeq(bad);
                        for(int prior=0;prior<i;prior++)c.aload(3).aload(0).loadConstant(prior).aaload().if_acmpeq(bad);
                        c.aload(0).loadConstant(i).aload(3).aastore();
                    }
                    c.getstatic(glob,"qs",array).aload(1).if_acmpne(bad);
                    c.return_();
                    c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup()
                        .loadConstant("FMC content bank binding unavailable")
                        .invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
                });
            }
        });
        require(data.cf.verify(output).isEmpty(),"bank bridge verifier");changed.put("Glob",output);
        var before=new HashMap<>(lower);for(String name:changed.keySet())before.put(name,lower.getOrDefault(name,data.inputs.get(name)));
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        // A component failure discards the local map. No native setter is added.
        for(var item:changed.entrySet())for(var method:data.cf.parse(item.getValue()).methods()){
            String name=method.methodName().stringValue();
            if(!name.startsWith(FmcMemberPrepare.PREFIX)&&!name.equals(BANK_METHOD))continue;
            for(var ins:Prepare.instructions(method))require(ins.opcode()!=Opcode.PUTFIELD&&ins.opcode()!=Opcode.PUTSTATIC,"native write in read-only bridge");
        }
        var hashes=new TreeMap<String,String>();
        for(String name:List.of("Glob","Q","Qs"))hashes.put(name,Identity.hash(data.inputs.get(name)));
        var frozen=new TreeMap<String,List<Prepare.MethodId>>();writers.forEach((k,v)->frozen.put(k,List.copyOf(v)));
        return new Qualified(Map.copyOf(changed),Map.copyOf(hashes),Map.copyOf(frozen));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report required");Path jar=Path.of(args[0]);
        require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var data=new Prepare(Prepare.readJar(jar));var q=qualify(data,Map.of());
        var out=new StringBuilder("{\"schema\":1,\"state\":\"neutral_bank_access_component_only\",\"captureQualified\":false,\"recallQualified\":false,\"candidateCount\":").append(CONTENT.size()).append(",\"classes\":[");
        boolean comma=false;for(var e:new TreeMap<>(q.inputHashes).entrySet()){
            if(comma)out.append(',');comma=true;out.append("{\"name\":").append(FmcQualification.quote(e.getKey())).append(",\"inputSha256\":").append(FmcQualification.quote(e.getValue())).append(",\"preparedSha256\":").append(FmcQualification.quote(Identity.hash(q.changed.get(e.getKey())))).append('}');
        }
        out.append("],\"candidates\":[");comma=false;for(String name:CONTENT){if(comma)out.append(',');comma=true;out.append(FmcQualification.quote(name));}
        out.append("],\"preserved\":[");comma=false;for(String name:PRESERVED){if(comma)out.append(',');comma=true;out.append(FmcQualification.quote(name));}
        out.append("],\"indexWriters\":{");comma=false;for(var e:new TreeMap<>(q.indexWriters).entrySet()){
            if(comma)out.append(',');comma=true;out.append(FmcQualification.quote(e.getKey())).append(':').append(FmcQualification.quote(e.getValue().getFirst().toString()));
        }
        out.append("},\"limit\":\"No owned-memory/cache/content decoding, whole-writer exclusion, coherence, native adapter/JNI or restore capability. Bank objects/metadata remain live bindings; worker monitor and lifecycle/writer witnesses required.\"}");
        Files.writeString(Path.of(args[1]),out,StandardOpenOption.CREATE_NEW);
        System.out.println("Neutral read-only FMC content bank component qualified as class data; no capture/recall capability");
    }
}
