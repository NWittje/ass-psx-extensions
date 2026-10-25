package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Legal member bridges for an explicit qualified profile, not a field-name
 * matcher. Bridge calls and object/array copying belong to workers or an
 * excluded command commit; they are never added to a native frame. */
final class FmcMemberPrepare {
    static final String PREFIX="$psxExporter$fmc";
    record Field(int id,String owner,String name,String descriptor,boolean writable){}
    private FmcMemberPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Field> fields) {
        var groups=new TreeMap<String,List<Field>>();var ids=new HashSet<Integer>();
        var finalModels=FmcQualification.finalModels(data,lower);
        for(var field:fields) {
            if(field.id<=0||!ids.add(field.id))throw new IllegalArgumentException("FMC duplicate/invalid field id");
            var model=finalModels.get(field.owner);if(model==null)throw new IllegalArgumentException("FMC field owner missing");
            var nativeField=model.fields().stream().filter(f->f.fieldName().equalsString(field.name)&&f.fieldType().equalsString(field.descriptor)).findFirst().orElseThrow(()->new IllegalArgumentException("FMC descriptor missing "+field));
            if(field.writable&&(nativeField.flags().flagsMask()&ClassFile.ACC_FINAL)!=0)throw new IllegalArgumentException("FMC final field write "+field);
            groups.computeIfAbsent(field.owner,k->new ArrayList<>()).add(field);
        }
        var changed=new HashMap<String,byte[]>();var before=new HashMap<String,byte[]>(lower);
        for(var group:groups.entrySet()) {
            String name=group.getKey();byte[] input=lower.getOrDefault(name,data.inputs.get(name));before.put(name,input);
            var source=data.cf.parse(input);if(source.methods().stream().anyMatch(m->m.methodName().stringValue().startsWith(PREFIX)))throw new IllegalArgumentException("FMC bridge collision");
            var sorted=new ArrayList<>(group.getValue());sorted.sort(Comparator.comparingInt(Field::id));ClassDesc owner=ClassDesc.of(name.replace('/','.'));
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder builder,ClassElement element){builder.with(element);}
                public void atEnd(ClassBuilder builder){
                    bridge(builder,source,owner,sorted,false,false);bridge(builder,source,owner,sorted,true,false);
                    bridge(builder,source,owner,sorted,false,true);bridge(builder,source,owner,sorted,true,true);
                }
            });
            var errors=data.cf.verify(output);if(!errors.isEmpty())throw new IllegalArgumentException("FMC member verification "+errors.getFirst());changed.put(name,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static boolean reference(Field field){return field.descriptor.startsWith("L")||field.descriptor.startsWith("[");}
    private static void bridge(ClassBuilder builder,ClassModel model,ClassDesc owner,List<Field> fields,boolean write,boolean references) {
        var selected=fields.stream().filter(f->reference(f)==references&&(!write||f.writable)).toList();
        String name=PREFIX+(write?"Write":"Read")+(references?"Ref":"Bits");
        ClassDesc value=references?CD_Object:CD_long;
        MethodTypeDesc type=write?MethodTypeDesc.of(CD_void,CD_Object,CD_int,value):MethodTypeDesc.of(value,CD_Object,CD_int);
        builder.withMethodBody(name,type,ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var bad=c.newLabel();var labels=new ArrayList<Label>();var cases=new ArrayList<SwitchCase>();
            for(var field:selected){var target=c.newLabel();labels.add(target);cases.add(SwitchCase.of(field.id,target));}
            c.iload(1).lookupswitch(bad,cases);
            for(int i=0;i<selected.size();i++) {
                var field=selected.get(i);ClassDesc descriptor=ClassDesc.ofDescriptor(field.descriptor);
                boolean stat=(model.fields().stream().filter(f->f.fieldName().equalsString(field.name)&&f.fieldType().equalsString(field.descriptor)).findFirst().orElseThrow().flags().flagsMask()&ClassFile.ACC_STATIC)!=0;
                c.labelBinding(labels.get(i));if(!stat)c.aload(0).checkcast(owner);
                if(write) {
                    if(references)c.aload(2).checkcast(descriptor);else{c.lload(2);fromBits(c,field.descriptor);}
                    if(stat)c.putstatic(owner,field.name,descriptor);else c.putfield(owner,field.name,descriptor);c.return_();
                }else{
                    if(stat)c.getstatic(owner,field.name,descriptor);else c.getfield(owner,field.name,descriptor);
                    if(references)c.areturn();else{toBits(c,field.descriptor);c.lreturn();}
                }
            }
            c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup()
                .loadConstant("unqualified FMC field identifier").invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
        });
    }
    private static void toBits(CodeBuilder c,String type) {
        switch(type) {
            case "Z"->c.i2l();case "B"->c.i2l().loadConstant(255L).land();
            case "C","S"->c.i2l().loadConstant(65535L).land();
            case "I"->c.i2l().loadConstant(0xffffffffL).land();case "J"->{}
            case "F"->c.invokestatic(ClassDesc.of("java.lang.Float"),"floatToRawIntBits",MethodTypeDesc.of(CD_int,CD_float)).i2l().loadConstant(0xffffffffL).land();
            case "D"->c.invokestatic(ClassDesc.of("java.lang.Double"),"doubleToRawLongBits",MethodTypeDesc.of(CD_long,CD_double));
            default->throw new IllegalArgumentException("unqualified FMC scalar descriptor");
        }
    }
    private static void fromBits(CodeBuilder c,String type) {
        switch(type) {
            case "Z","I"->c.l2i();case "B"->c.l2i().i2b();case "C"->c.l2i().i2c();case "S"->c.l2i().i2s();case "J"->{}
            case "F"->c.l2i().invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));
            case "D"->c.invokestatic(ClassDesc.of("java.lang.Double"),"longBitsToDouble",MethodTypeDesc.of(CD_double,CD_long));
            default->throw new IllegalArgumentException("unqualified FMC scalar descriptor");
        }
    }
}
