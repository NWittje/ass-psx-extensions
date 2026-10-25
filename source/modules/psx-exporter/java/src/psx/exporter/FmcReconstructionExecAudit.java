package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.nio.file.*;
import java.util.*;

/** Potential native EXEC effects, including its operation-owned child jobs.
 * A field in this roster is not permission to overwrite physical state: the
 * caller must prove its conditional path harmless or preserve the current
 * preimage. Arrays and JDK buffers are explicit effects beyond field stores. */
final class FmcReconstructionExecAudit {
    record Effects(Set<FmcQualification.Member> writes,Map<FmcQualification.Member,Set<Prepare.MethodId>> writers,
                   Set<Prepare.MethodId> arrayStores,Map<Prepare.MethodId,Set<Prepare.MethodId>> externalCalls){}
    record Classified(Effects potential,Set<FmcQualification.Member> liveWrites,Set<Prepare.MethodId> liveArrayMethods,
                      FmcBackgroundAdmissionTransport.Profile transport,Set<FmcQualification.Member> constructorCandidates){}
    static Classified classify(Prepare data){var potential=inspect(data);var transport=FmcBackgroundAdmissionTransport.inspect(data,potential,FmcExecMutationPrepare.profile(data).methods());var live=new HashSet<>(potential.writes);live.removeAll(transport.fields());var arrays=new HashSet<>(potential.arrayStores);arrays.removeAll(transport.arrayMethods());var candidates=new HashSet<FmcQualification.Member>();potential.writers.forEach((field,methods)->{if(methods.stream().allMatch(m->m.name().equals("<init>")))candidates.add(field);});return new Classified(potential,Set.copyOf(live),Set.copyOf(arrays),transport,Set.copyOf(candidates));}
    static Effects inspect(Prepare data){
        var profile=FmcExecMutationPrepare.profile(data);var writers=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();var arrays=new HashSet<Prepare.MethodId>();var external=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();
        for(var id:profile.methods())for(var op:Prepare.instructions(data.methods.get(id))){
            if(op instanceof FieldInstruction f&&(op.opcode()==Opcode.PUTFIELD||op.opcode()==Opcode.PUTSTATIC)){
                var member=FmcQualification.resolveField(data.models,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),new HashSet<>());
                if(member==null)throw new IllegalArgumentException("FMC EXEC unresolved store "+id+" "+f);writers.computeIfAbsent(member,k->new HashSet<>()).add(id);
            }
            if(switch(op.opcode()){case IASTORE,LASTORE,FASTORE,DASTORE,AASTORE,BASTORE,CASTORE,SASTORE->true;default->false;})arrays.add(id);
            if(op instanceof InvokeInstruction call&&!data.models.containsKey(call.owner().asInternalName())){
                var target=new Prepare.MethodId(call.owner().asInternalName(),call.name().stringValue(),call.type().stringValue());external.computeIfAbsent(target,k->new HashSet<>()).add(id);
            }
        }
        var frozen=new TreeMap<FmcQualification.Member,Set<Prepare.MethodId>>();writers.forEach((field,methods)->frozen.put(field,Set.copyOf(methods)));var calls=new HashMap<Prepare.MethodId,Set<Prepare.MethodId>>();external.forEach((call,methods)->calls.put(call,Set.copyOf(methods)));
        return new Effects(Set.copyOf(writers.keySet()),Collections.unmodifiableMap(frozen),Set.copyOf(arrays),Map.copyOf(calls));
    }
    public static void main(String[] args)throws Exception{
        Path jar=Path.of(args[0]);if(!Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR))throw new IllegalArgumentException("native identity");var data=new Prepare(Prepare.readJar(jar));var effects=inspect(data);
        System.out.println("Native ordinary EXEC potential fields="+effects.writes.size()+" array-methods="+effects.arrayStores.size()+" external-call-shapes="+effects.externalCalls.size());
        for(var e:effects.writers.entrySet())System.out.println("FIELD "+e.getKey()+" <- "+e.getValue().stream().sorted(Comparator.comparing(Prepare.MethodId::toString)).toList());
        for(var m:effects.arrayStores.stream().sorted(Comparator.comparing(Prepare.MethodId::toString)).toList())System.out.println("ARRAY "+m);
        for(var e:effects.externalCalls.entrySet().stream().sorted(Comparator.comparing(e->e.getKey().toString())).toList())System.out.println("EXTERNAL "+e.getKey()+" <- "+e.getValue().stream().sorted(Comparator.comparing(Prepare.MethodId::toString)).toList());
    }
}
