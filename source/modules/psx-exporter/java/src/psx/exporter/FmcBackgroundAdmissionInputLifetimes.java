package psx.exporter;
import java.util.*;
/** Quiet input excludes new user edits; an old callback may already be inside
 * a dialog or physical/UI prefix. Source-qualified relevant listener entries
 * retain their complete original lifetime before any selected read/write. */
final class FmcBackgroundAdmissionInputLifetimes {
 static Set<Prepare.MethodId> qualify(Prepare data,FmcBackgroundAdmissionProfile.Plan plan,Set<FmcQualification.Member> currentInputs){
  var observers=FmcBackgroundAdmissionObservers.inspect(data);var candidates=observers.quietInputs();
  var result=new HashSet<>(FmcBackgroundAdmissionClosure.relevantJobs(data,plan,candidates));
  if(!currentInputs.isEmpty()){var inputs=new FmcBackgroundAdmissionProfile.Plan(Set.copyOf(currentInputs),plan.access(),Set.of(),Set.of(),Map.of(),Set.of(),List.of(),Set.of(),Set.of(),plan.publishedReferences(),plan.ownedStatics(),plan.ownedChildren(),Set.of());result.addAll(FmcBackgroundAdmissionClosure.relevantWriters(data,inputs,candidates));}
  for(var id:result){var contracts=observers.roots().get(id);if(!candidates.contains(id)||contracts==null||contracts.isEmpty()||Set.of("paint","paintComponent","paintChildren","update","run","actionTimer").contains(id.name()))throw new IllegalArgumentException("FMC old input lifetime includes an autonomous render/timer root "+id);}
  return Set.copyOf(result);
 }
 private FmcBackgroundAdmissionInputLifetimes(){}
}