package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Constructor bridges only after a separate pure-factory/initialization proof.
 * No constructor is selected by name prefix and none is run during inspection. */
final class FmcFactoryPrepare {
    static final String NAME="$psxExporter$fmcCreate";
    record Factory(String owner,String descriptor){}
    private FmcFactoryPrepare(){}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Factory> factories){
        var changed=new HashMap<String,byte[]>();var before=new HashMap<String,byte[]>(lower);
        for(Factory factory:factories){
            if(changed.containsKey(factory.owner))throw new IllegalArgumentException("duplicate FMC constructor owner");
            var ctor=data.methods.get(new Prepare.MethodId(factory.owner,"<init>",factory.descriptor));
            if(ctor==null||ctor.code().isEmpty()||(ctor.flags().flagsMask()&ClassFile.ACC_STATIC)!=0)throw new IllegalArgumentException("missing FMC constructor descriptor");
            var descriptor=MethodTypeDesc.ofDescriptor(factory.descriptor);if(!descriptor.returnType().equals(CD_void))throw new IllegalArgumentException("FMC constructor return type");
            byte[] input=lower.getOrDefault(factory.owner,data.inputs.get(factory.owner));before.put(factory.owner,input);var source=data.cf.parse(input);
            if(source.methods().stream().anyMatch(m->m.methodName().equalsString(NAME)))throw new IllegalArgumentException("FMC constructor bridge collision");
            ClassDesc owner=ClassDesc.of(factory.owner.replace('/','.'));
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){b.withMethodBody(NAME,MethodTypeDesc.of(CD_Object,CD_long.arrayType(),CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
                    c.new_(owner).dup();int scalar=0,reference=0;
                    for(ClassDesc parameter:descriptor.parameterList()){
                        String type=parameter.descriptorString();
                        if(type.startsWith("L")||type.startsWith("["))c.aload(1).loadConstant(reference++).aaload().checkcast(parameter);
                        else{c.aload(0).loadConstant(scalar++).laload();switch(type){
                            case "Z","I"->c.l2i();case "B"->c.l2i().i2b();case "C"->c.l2i().i2c();case "S"->c.l2i().i2s();case "J"->{}
                            case "F"->c.l2i().invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));
                            case "D"->c.invokestatic(ClassDesc.of("java.lang.Double"),"longBitsToDouble",MethodTypeDesc.of(CD_double,CD_long));
                            default->throw new IllegalArgumentException("FMC factory primitive");
                        }}
                    }
                    c.invokespecial(owner,"<init>",descriptor).areturn();
                });}
            });
            var errors=data.cf.verify(output);if(!errors.isEmpty())throw new IllegalArgumentException("FMC constructor verification "+errors.getFirst());changed.put(factory.owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
}
