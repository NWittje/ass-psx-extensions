package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Explicit staging constructors for already initialized, qualified content
 * classes. Native constructors are not content parsers: they may initialize
 * navigation, radios or other preserved systems. These constructors call only
 * Object.<init> and assign the declared final cells of a new unpublished object.
 * A native profile must separately prove every remaining field, invariants,
 * external binding, consumer and publication frontier. This component alone
 * grants no native capture/recall capability. Preparation treats classes as data;
 * factory invocation is worker-only, after the initialized-class access gate. */
final class FmcOwnedFactoryPrepare {
    static final String NAME=FmcFactoryPrepare.NAME;
    static final String CONSTRUCTOR="([J[Ljava/lang/Object;)V";
    record Cell(int id,String name,String descriptor){}
    record Factory(String owner,List<Cell> finals){}
    private FmcOwnedFactoryPrepare(){}
    private static void require(boolean value,String why){
        if(!value)throw new IllegalArgumentException("FMC owned factory "+why);
    }
    private static boolean reference(Cell cell){
        return cell.descriptor.startsWith("L")||cell.descriptor.startsWith("[");
    }
    private static List<Cell> qualify(ClassModel source,Factory factory){
        int flags=source.flags().flagsMask();
        require((flags&(ClassFile.ACC_INTERFACE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_ENUM))==0,"non-concrete owner");
        require(source.superclass().isPresent()&&source.superclass().get().asInternalName().equals("java/lang/Object"),"unqualified superclass");
        require(source.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"factory collision");
        require(source.methods().stream().noneMatch(m->m.methodName().equalsString("<init>")&&m.methodType().equalsString(CONSTRUCTOR)),"constructor collision");
        var cells=new ArrayList<>(factory.finals);cells.sort(Comparator.comparingInt(Cell::id));
        var names=new HashSet<String>();int previous=0;
        for(Cell cell:cells){
            require(cell.id>previous,"duplicate/invalid cell id");previous=cell.id;
            require(names.add(cell.name),"duplicate final field");
            ClassDesc.ofDescriptor(cell.descriptor); // Descriptor syntax is checked before transformation.
            require(reference(cell)||Set.of("Z","B","C","S","I","J","F","D").contains(cell.descriptor),"field type");
            var selected=source.fields().stream().filter(f->f.fieldName().equalsString(cell.name)&&f.fieldType().equalsString(cell.descriptor)).toList();
            require(selected.size()==1,"missing final descriptor "+cell.name);
            int fieldFlags=selected.getFirst().flags().flagsMask();
            require((fieldFlags&ClassFile.ACC_FINAL)!=0&&(fieldFlags&ClassFile.ACC_STATIC)==0,"non-final or static cell "+cell.name);
        }
        long nativeFinals=source.fields().stream().filter(f->(f.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_FINAL).count();
        require(nativeFinals==cells.size(),"incomplete final field coverage");
        return List.copyOf(cells);
    }
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower,List<Factory> factories){
        var changed=new HashMap<String,byte[]>();var before=new HashMap<String,byte[]>(lower);
        for(Factory factory:factories){
            require(!changed.containsKey(factory.owner),"duplicate owner");
            byte[] input=lower.getOrDefault(factory.owner,data.inputs.get(factory.owner));
            require(input!=null,"missing owner");before.put(factory.owner,input);
            var source=data.cf.parse(input);var cells=qualify(source,factory);
            ClassDesc owner=ClassDesc.of(factory.owner.replace('/','.'));
            byte[] output=data.cf.transformClass(source,new ClassTransform(){
                public void accept(ClassBuilder b,ClassElement e){b.with(e);}
                public void atEnd(ClassBuilder b){
                    constructor(b,owner,cells);
                    factory(b,owner,cells);
                }
            });
            require(data.cf.verify(output).isEmpty(),"verification");
            // Audit generated work rather than accepting a broad constructor
            // allowlist. No native initialization or static-state write is legal.
            for(var method:data.cf.parse(output).methods()){
                boolean added=method.methodName().equalsString(NAME)||
                    method.methodName().equalsString("<init>")&&method.methodType().equalsString(CONSTRUCTOR);
                if(!added)continue;
                for(var ins:Prepare.instructions(method)){
                    require(ins.opcode()!=Opcode.PUTSTATIC,"static write");
                    if(ins instanceof FieldInstruction f)require(f.opcode()==Opcode.PUTFIELD&&f.owner().asInternalName().equals(factory.owner)&&
                        cells.stream().anyMatch(cell->cell.name.equals(f.name().stringValue())&&cell.descriptor.equals(f.type().stringValue())),"unqualified field effect");
                    if(ins instanceof InvokeInstruction call){
                        String called=call.owner().asInternalName()+"."+call.name().stringValue()+call.type().stringValue();
                        require(called.equals("java/lang/Object.<init>()V")||
                            called.equals(factory.owner+".<init>"+CONSTRUCTOR)||
                            called.equals("java/lang/Float.intBitsToFloat(I)F")||
                            called.equals("java/lang/Double.longBitsToDouble(J)D")||
                            called.equals("java/lang/IllegalArgumentException.<init>(Ljava/lang/String;)V"),"unqualified call "+called);
                    }
                }
            }
            changed.put(factory.owner,output);
        }
        TerrainAccess.verifyAdded(before,changed,data.inputs);
        return Map.copyOf(changed);
    }
    private static void constructor(ClassBuilder b,ClassDesc owner,List<Cell> cells){
        b.withMethodBody("<init>",MethodTypeDesc.ofDescriptor(CONSTRUCTOR),ClassFile.ACC_PRIVATE|ClassFile.ACC_SYNTHETIC,c->{
            c.aload(0).invokespecial(CD_Object,"<init>",MethodTypeDesc.of(CD_void));
            int scalar=0,ref=0;
            for(Cell cell:cells){
                ClassDesc type=ClassDesc.ofDescriptor(cell.descriptor);
                c.aload(0);
                if(reference(cell))c.aload(2).loadConstant(ref++).aaload().checkcast(type);
                else{
                    c.aload(1).loadConstant(scalar++).laload();
                    switch(cell.descriptor){
                        case "Z","I"->c.l2i();case "B"->c.l2i().i2b();case "C"->c.l2i().i2c();case "S"->c.l2i().i2s();case "J"->{}
                        case "F"->c.l2i().invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));
                        case "D"->c.invokestatic(ClassDesc.of("java.lang.Double"),"longBitsToDouble",MethodTypeDesc.of(CD_double,CD_long));
                        default->throw new IllegalArgumentException("FMC owned factory scalar");
                    }
                }
                c.putfield(owner,cell.name,type);
            }
            c.return_();
        });
    }
    private static void factory(ClassBuilder b,ClassDesc owner,List<Cell> cells){
        b.withMethodBody(NAME,MethodTypeDesc.of(CD_Object,CD_long.arrayType(),CD_Object.arrayType()),ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,c->{
            var bad=c.newLabel();int scalarCount=(int)cells.stream().filter(cell->!reference(cell)).count();
            c.aload(0).ifnull(bad).aload(1).ifnull(bad);
            c.aload(0).arraylength().loadConstant(scalarCount).if_icmpne(bad);
            c.aload(1).arraylength().loadConstant(cells.size()-scalarCount).if_icmpne(bad);
            int scalar=0,ref=0;
            for(Cell cell:cells){
                if(reference(cell)){c.aload(1).loadConstant(ref++).aaload().checkcast(ClassDesc.ofDescriptor(cell.descriptor)).pop();continue;}
                long mask=switch(cell.descriptor){case "Z"->1;case "B"->255;case "C","S"->65535;case "I","F"->0xffffffffL;default->-1;};
                if(mask!=-1)c.aload(0).loadConstant(scalar).laload().loadConstant(~mask).land().loadConstant(0L).lcmp().ifne(bad);
                scalar++;
            }
            c.new_(owner).dup().aload(0).aload(1).invokespecial(owner,"<init>",MethodTypeDesc.ofDescriptor(CONSTRUCTOR)).areturn();
            c.labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC owned factory cells unavailable")
                .invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow();
        });
    }
}
