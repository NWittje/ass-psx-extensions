package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.util.*;

/** Installed current active-leg calculation with current forecasts preserved.
 * The reused qualified arithmetic layer removes its direct bank/message
 * outputs. This successor additionally redirects every native waypoint blank
 * to the exact forecast-preserving clone. */
final class FmcReconstructionGeometryPrepare {
    record Qualified(Map<String,byte[]> changed,Set<FmcQualification.Member> writes,Set<FmcQualification.Member> inputs){}
    static Set<Prepare.MethodId> roots(){var result=new HashSet<Prepare.MethodId>();for(var clone:FmcGeometryPrepare.CLONES)result.add(new Prepare.MethodId(clone.original().owner(),clone.generated(),clone.original().descriptor()));return Set.copyOf(result);}
    static Qualified transform(Prepare data,Map<String,byte[]> lower){
        var effects=FmcGeometryPrepare.qualify(data,lower);var changed=new HashMap<>(FmcGeometryPrepare.transform(data,lower));var before=new HashMap<>(changed);
        var generated=FmcGeometryPrepare.CLONES.stream().map(FmcGeometryPrepare.Clone::generated).collect(java.util.stream.Collectors.toSet());
        var model=data.cf.parse(changed.get("Routes$RteBody"));int[] redirected={0};
        byte[] output=data.cf.transformClass(model,ClassTransform.transformingMethodBodies(m->generated.contains(m.methodName().stringValue()),(c,e)->{
            if(e instanceof InvokeInstruction call&&call.owner().asInternalName().equals("RouteWpt")&&call.name().equalsString("blank")&&call.type().equalsString("()V")){
                redirected[0]++;c.invokevirtual(ClassDesc.of("RouteWpt"),"$psxExporter$fmcBlankPreserveForecast",MethodTypeDesc.ofDescriptor("()V"));return;
            }c.with(e);
        }));if(redirected[0]!=2||!data.cf.verify(output).isEmpty())throw new IllegalArgumentException("FMC reconstruction current-geometry blank/verifier frontier");
        changed.put("Routes$RteBody",output);TerrainAccess.verifyAdded(before,changed,data.inputs);return new Qualified(Map.copyOf(changed),effects.writes(),effects.staticInputs());
    }
}
