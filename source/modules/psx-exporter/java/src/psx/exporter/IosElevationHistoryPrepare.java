package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Unbound wrapper for the unchanged native altitude-rate history reset.
 * Destination support, all other elevation caches and owner/exclusion must be
 * qualified separately. This primitive does not constitute relocation proof. */
public final class IosElevationHistoryPrepare {
    public static final String RESET="psxExporterIosResetElevationHistory";
    private static final MethodTypeDesc VOID=MethodTypeDesc.of(CD_void);
    private static final Map<String,ClassDesc> FIELDS=Map.of("altRateDelta",CD_float,"altRate",CD_float,
        "targRealElev",CD_float,"targRealElevEx",CD_float,"antiBumpCount",CD_int,"tileReloadTimeLimit",CD_float);
    public record Selection(ClassDesc owner,List<Integer> nativeWrites){}
    private static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    private static MethodModel nativeMethod(ClassModel c){
        var candidates=c.methods().stream().filter(m->m.methodName().equalsString("resetAltRate")).toList();
        require(candidates.size()==1,"Missing/overloaded elevation reset");var m=candidates.getFirst();
        require(m.methodType().equalsString("()V")&&m.code().isPresent()&&
            (m.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_NATIVE|ClassFile.ACC_ABSTRACT|ClassFile.ACC_SYNCHRONIZED))==ClassFile.ACC_STATIC,"Changed elevation reset context");
        require(m.code().get().exceptionHandlers().isEmpty(),"Elevation reset exceptional effects");return m;
    }
    private static List<String> instructions(MethodModel m){
        var result=new ArrayList<String>();
        for(var e:m.code().orElseThrow())if(e instanceof Instruction i){
            if(i instanceof FieldInstruction f)result.add(i.opcode()+":"+f.owner().asInternalName()+":"+f.name()+":"+f.type());
            else if(i instanceof ConstantInstruction c)result.add("constant:"+c.constantValue().getClass().getName()+":"+c.constantValue());
            else {require(i.opcode()==Opcode.RETURN,"Unqualified elevation reset action "+i.opcode());result.add("return");}
        }
        return List.copyOf(result);
    }
    public static Selection select(ClassModel c){
        var owner=c.thisClass().asSymbol();
        for(var m:c.methods())require(!m.methodName().equalsString(RESET),"Existing IOS elevation reset binding");
        for(var entry:FIELDS.entrySet()){
            var fields=c.fields().stream().filter(f->f.fieldName().equalsString(entry.getKey())).toList();
            require(fields.size()==1,"Missing/duplicate elevation field "+entry.getKey());var f=fields.getFirst();
            require(f.fieldTypeSymbol().equals(entry.getValue())&&(f.flags().flagsMask()&(ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==ClassFile.ACC_STATIC,"Changed elevation field binding "+entry.getKey());
        }
        var m=nativeMethod(c);
        byte[] reference=ClassFile.of().build(owner,b->b.withMethodBody("resetAltRate",VOID,ClassFile.ACC_STATIC,code->
            code.loadConstant(1.0e9f).putstatic(owner,"altRateDelta",CD_float)
                .fconst_0().putstatic(owner,"altRate",CD_float)
                .getstatic(owner,"targRealElev",CD_float).putstatic(owner,"targRealElevEx",CD_float)
                .loadConstant(12).putstatic(owner,"antiBumpCount",CD_int)
                .loadConstant(9999.0f).putstatic(owner,"tileReloadTimeLimit",CD_float).return_()));
        require(instructions(m).equals(instructions(ClassFile.of().parse(reference).methods().getFirst())),"Changed native elevation reset ordering/values/effects");
        var writes=new ArrayList<Integer>();int offset=0;
        for(var e:m.code().orElseThrow())if(e instanceof Instruction i){if(i.opcode()==Opcode.PUTSTATIC)writes.add(offset);offset+=i.sizeInBytes();}
        return new Selection(owner,List.copyOf(writes));
    }
    public static byte[] prepare(ClassFile cf,ClassModel model,Selection s){
        require(select(model).equals(s),"Elevation reset input differs from qualification");
        return cf.transformClass(model,ClassTransform.endHandler(b->b.withMethodBody(RESET,VOID,
            ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,
            c->c.invokestatic(s.owner,"resetAltRate",VOID).return_())));
    }
    private IosElevationHistoryPrepare(){}
}
