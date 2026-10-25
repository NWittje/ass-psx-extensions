package psx.exporter;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Qualify the computed local target at its actual native smoother input. */
final class EnvironmentVisibilityPrepare {
 static final String METHOD="$psxEnvironment$visibilityTarget";
 static final ClassDesc OBS=ClassDesc.of("psx.exporter.EnvironmentVisibilityObserver");
 static final MethodTypeDesc TYPE=MethodTypeDesc.of(CD_void,CD_int);
 static int targetIndex(MethodModel method){
  var code=Prepare.instructions(method);int found=-1;
  for(int i=0;i+9<code.size();i++)if(code.get(i) instanceof LoadInstruction load&&load.typeKind()==TypeKind.INT&&
    code.get(i+1).opcode()==Opcode.I2F&&field(code.get(i+2),Opcode.GETSTATIC,"fVisibMtrs")&&field(code.get(i+3),Opcode.GETSTATIC,"dampFacVisib")&&
    code.get(i+4) instanceof FieldInstruction delta&&delta.opcode()==Opcode.GETSTATIC&&delta.owner().asInternalName().equals("Core")&&delta.name().equalsString("deltaAllegro")&&delta.typeSymbol().equals(CD_long)&&
    code.get(i+5).opcode()==Opcode.L2F&&code.get(i+6).opcode()==Opcode.FMUL&&code.get(i+7).opcode()==Opcode.FCONST_1&&
    Prepare.call(code.get(i+8),"Calc","getSmoothExpo","(FFFF)F")&&field(code.get(i+9),Opcode.PUTSTATIC,"fVisibMtrs")){
   if(found>=0)throw new IllegalArgumentException("ambiguous native local visibility smoother");found=i;
  }
  if(found<0)throw new IllegalArgumentException("native local visibility smoother input missing");return found;
 }
 private static boolean field(Instruction i,Opcode op,String name){return i instanceof FieldInstruction f&&f.opcode()==op&&f.owner().asInternalName().equals("Weather")&&f.name().equalsString(name)&&f.typeSymbol().equals(CD_float);}
 static Map<String,byte[]> transform(Prepare base,Map<String,byte[]> input){
  var model=base.cf.parse(input.getOrDefault("Weather",base.inputs.get("Weather")));
  var nativeMethod=base.requireMethod("Weather","interpolate","()V");targetIndex(nativeMethod);
  var output=new TreeMap<>(input);var owner=ClassDesc.of("Weather");
  byte[] bytes=base.cf.transformClass(model,new ClassTransform(){
   public void accept(ClassBuilder b,ClassElement e){
    if(!(e instanceof MethodModel m)||!m.methodName().equalsString("interpolate")||!m.methodType().equalsString("()V")){b.with(e);return;}
    final int at=targetIndex(m);final var load=(LoadInstruction)Prepare.instructions(m).get(at);
    b.transformMethod(m,(mb,me)->{if(!(me instanceof CodeModel c)){mb.with(me);return;}mb.transformCode(c,new CodeTransform(){int index;
     public void accept(CodeBuilder cb,CodeElement ce){if(ce instanceof Instruction){if(index++==at)cb.iload(load.slot()).invokestatic(owner,METHOD,TYPE);}cb.with(ce);}
    });});
   }
   public void atEnd(ClassBuilder b){b.withMethodBody(METHOD,TYPE,ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC,cb->{
    cb.trying(x->x.iload(0).invokestatic(OBS,"record",TYPE),c->c.catching(CD_Throwable,h->{h.pop();h.trying(x->x.invokestatic(OBS,"disable",MethodTypeDesc.of(CD_void)),d->d.catching(CD_Throwable,x->x.pop()));}));cb.return_();
   });}
  });
  var errors=base.cf.verify(bytes);if(!errors.isEmpty())throw new IllegalArgumentException("local visibility verify "+errors.getFirst());output.put("Weather",bytes);
  var before=new TreeMap<>(input);before.putIfAbsent("Weather",base.inputs.get("Weather"));TerrainAccess.verifyAdded(before,output,base.inputs);return output;
 }
}
