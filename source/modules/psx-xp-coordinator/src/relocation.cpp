#include "coordinator/relocation.hpp"
#include <algorithm>
namespace coordinator::relocation {
namespace {
bool identity(const Identity& i) noexcept {return i.pid&&i.birth&&serial(i.instance);}
bool admission(const Admission& a) noexcept {return identity(a.scope.target)&&a.scope.issuer_pid&&a.scope.issuer_birth&&serial(a.scope.transaction)&&
 serial(a.token)&&serial(a.serial)&&serial(a.registry)&&serial(a.epoch)&&serial(a.edit)&&serial(a.deadline)&&
 serial(a.historical_intent)&&a.historical_intent==a.expected_intent&&serial(a.observation_sequence)&&serial(a.observed_qpc);}
bool domain(const Domain& d) noexcept {return (d.producer.high||d.producer.low)&&serial(d.native_connection)&&serial(d.coordinator_connection)&&
 serial(d.outbox_generation)&&serial(d.owner)&&serial(d.pose_epoch)&&serial(d.terrain_epoch);}
bool registered(const Registered& r) noexcept {return identity(r.coordinator)&&serial(r.association)&&serial(r.registration)&&domain(r.domain);}
bool counter_domain(const CounterDomain& d) noexcept {
 if(d.kind>CounterKind::transport||!identity(d.issuer)||!(d.incarnation.high||d.incarnation.low))return false;
 if(d.kind==CounterKind::transport)return serial(d.connection)&&(d.direction==Direction::to_native||d.direction==Direction::to_coordinator)&&d.producer==psx::motion::InstanceId{};
 return !d.connection&&d.direction==Direction::none&&(d.kind==CounterKind::source?(d.producer.high||d.producer.low):d.producer==psx::motion::InstanceId{});
}
bool counter_point(const CounterPoint& p) noexcept {
 if(!counter_domain(p.domain)||p.state>CounterState::inapplicable)return false;
 if(p.domain.kind==CounterKind::transport&&p.value>UINT32_MAX)return false;
 if(p.state==CounterState::known)return !p.reason;
 if(p.state==CounterState::no_issued)return !p.value&&!p.reason;
 return !p.value&&p.reason;
}
bool applicable(const CounterPoint& p) noexcept {return counter_point(p)&&(p.state==CounterState::known||p.state==CounterState::no_issued);}
}
bool serial(std::uint64_t n) noexcept {return n&&n<=serial_limit;}
bool retired(const IoReceipt& r) noexcept {
 if(r.kind>IoKind::write||r.outcome>IoOutcome::unknown||r.cancellation>Cancel::failed)return false;
 if(!r.issued)return r.outcome==IoOutcome::idle&&r.cancellation==Cancel::none&&!r.error&&!r.cancel_error&&!r.requested&&!r.completed&&!r.query_valid&&!r.terminal&&!r.bytes_valid&&!r.failure_qualified;
 if(r.cancellation==Cancel::not_found?r.cancel_error!=1168:r.cancellation==Cancel::failed?(!r.cancel_error||r.cancel_error==1168):r.cancel_error!=0)return false;
 if(!r.query_valid||!r.terminal||r.outcome==IoOutcome::pending||r.outcome==IoOutcome::unknown||r.outcome==IoOutcome::idle)return false;
 if(r.kind==IoKind::connect&&(r.requested||r.completed||r.bytes_valid))return false;
 if(r.completed>r.requested||(!r.bytes_valid&&r.completed))return false;
 if(r.outcome==IoOutcome::completed)return !r.error&&!r.failure_qualified&&(r.kind==IoKind::connect||r.bytes_valid);
 if(r.bytes_valid)return false;
 if(r.outcome==IoOutcome::cancelled)return r.error==995&&!r.failure_qualified;
 return r.failure_qualified&&r.error&&r.error!=6&&r.error!=995&&r.error!=996&&r.error!=997;
}
bool worker_fenced(const WorkerReceipt& r) noexcept {
 const auto* writer=std::get_if<Registered>(&r.prepare.writer);
 if(!writer||!admission(r.prepare.admission)||!registered(*writer)||writer->domain.pose_epoch!=r.prepare.admission.epoch)return false;
 const auto issued=(r.connect.issued?1u:0u)|(r.read.issued?2u:0u)|(r.write.issued?4u:0u);
 const auto completed=(r.connect.issued&&r.connect.terminal?1u:0u)|(r.read.issued&&r.read.terminal?2u:0u)|(r.write.issued&&r.write.terminal?4u:0u);
 return r.connect.kind==IoKind::connect&&r.read.kind==IoKind::read&&r.write.kind==IoKind::write&&retired(r.connect)&&retired(r.read)&&retired(r.write)&&
 serial(r.request_qpc)&&r.observed_qpc>=r.request_qpc&&r.observed_qpc<=serial_limit&&serial(r.model_generation)&&serial(r.producer_generation)&&
 r.retired_generation==writer->domain.outbox_generation&&r.issued_mask==issued&&r.completed_mask==completed&&completed==issued&&
 !r.pending_mask&&!r.queued&&!r.latest&&r.stream_closed&&r.storage_safe&&
 (r.staged_valid?(r.staged_kind&&r.staged_sequence&&r.staged_bytes&&r.staged_bytes<=4112&&r.staged_offset<=r.staged_bytes):
 !r.staged_kind&&!r.staged_sequence&&!r.staged_bytes&&!r.staged_offset)&&
 applicable(r.counters.points[1])&&applicable(r.counters.points[2])&&applicable(r.counters.points[3])&&
 r.counters.points[1].domain.kind==CounterKind::control&&r.counters.points[2].domain.kind==CounterKind::query&&r.counters.points[3].domain.kind==CounterKind::transport&&
 r.counters.points[1].domain.issuer==writer->coordinator&&r.counters.points[2].domain.issuer==writer->coordinator&&r.counters.points[3].domain.issuer==writer->coordinator&&
 r.counters.points[1].domain.incarnation==r.counters.points[2].domain.incarnation&&
 r.counters.points[3].domain.connection==writer->domain.coordinator_connection&&r.counters.points[3].domain.direction==Direction::to_native&&
 r.counters.points[1].value==r.control_floor&&r.counters.points[2].value==r.query_floor&&r.counters.points[3].value==r.transport_floor;
}
bool counters_correlated(const WorkerReceipt& w,const NativeFence& f,const Resolution& r,const Binding& b) noexcept {
 const auto* old=std::get_if<Registered>(&w.prepare.writer);
 if(!old||!worker_fenced(w)||f.prepare!=w.prepare||r.admission!=w.prepare.admission||b.admission!=r.admission||
 !f.ingress||!f.polled||!f.final_consumer||f.absence_proven||!serial(f.consumer_serial)||!r.retained_authority||
 r.claim!=ClaimDisposition::resolved||!b.native_accepted||!b.actual_hello_subscribe||!serial(b.serial)||!serial(b.observation_sequence)||
 b.writer.coordinator!=old->coordinator||!serial(b.observed_qpc)||!b.hello_sequence||!b.subscribe_sequence||
 !serial(r.observation_sequence)||!serial(r.observed_qpc)||b.observation_sequence<=r.observation_sequence||b.observed_qpc<r.observed_qpc)return false;
 const auto& source=r.source_counter;
 if(!counter_point(source)||source.domain.kind!=CounterKind::source||source.domain.issuer!=r.admission.scope.target||source.domain.producer!=old->domain.producer)return false;
 if(r.disposition==Disposition::applied&&(!applicable(source)||source.value!=r.reference_sequence))return false;
 const std::array<CounterPoint,4> retired{source,w.counters.points[1],w.counters.points[2],w.counters.points[3]};
 const std::array<std::uint64_t,4> actual{b.source_sequence_floor,b.control_floor,b.query_floor,b.transport_floor};
 for(std::size_t i=0;i<4;++i){
  const auto& before=retired[i];const auto& request=b.requested.points[i];const auto& accepted=b.accepted.points[i];
  const auto& issued=b.issued.points[i];const auto& excluded=b.excluded.points[i];const auto& proof=b.replacements[i];
  if(!applicable(request)||!applicable(accepted)||!applicable(issued)||!applicable(excluded)||request.domain.kind!=static_cast<CounterKind>(i)||
  request.domain!=accepted.domain||request.domain!=issued.domain||request.domain!=excluded.domain||accepted.value!=actual[i]||
  request.value>accepted.value||accepted.value>excluded.value||excluded.value>issued.value)return false;
  if(issued.state==CounterState::no_issued&&(request.value||accepted.value||excluded.value))return false;
  const auto expected_issuer=i==0?r.admission.scope.target:b.writer.coordinator;
  if(request.domain.issuer!=expected_issuer)return false;
  if(i==0&&request.domain.producer!=b.writer.domain.producer)return false;
  if(i==3&&(request.domain.connection!=b.writer.domain.coordinator_connection||request.domain.direction!=Direction::to_native||issued.value<b.subscribe_sequence))return false;
  if(request.domain==before.domain){
   if(!applicable(before)||request.value<before.value||proof!=CounterReplacement{})return false;
  }else{
   if(proof.retired!=before.domain||proof.current!=request.domain||proof.native_authority!=r.admission.scope.target||proof.scope!=r.admission.scope||
   proof.registry!=r.admission.registry||proof.registration!=old->registration||proof.association!=old->association||proof.binding!=b.serial||
   proof.consumer!=f.consumer_serial||proof.observation_sequence!=b.observation_sequence||proof.observed_qpc!=b.observed_qpc)return false;
  }
 }
 return b.requested.points[1].domain.incarnation==b.requested.points[2].domain.incarnation;
}
bool no_mutation(const Resolution& r) noexcept {return admission(r.admission)&&r.disposition==Disposition::no_mutation&&r.retained_authority&&!r.dispatch_unresolved&&r.claim==ClaimDisposition::resolved&&
 serial(r.observed_qpc)&&serial(r.observation_sequence)&&r.observation_sequence>r.admission.observation_sequence&&r.observed_qpc>=r.admission.observed_qpc&&
 serial(r.epoch)&&serial(r.current_edit)&&(r.effect_valid&~15u)==0&&(r.effects&~r.effect_valid)==0&&(r.effects&14u)==0&&
 (r.dispatch==DispatchDisposition::never_started?((r.effect_valid&1u)==1u&&(r.effects&1u)==0):
 r.dispatch==DispatchDisposition::finished&&(r.effect_valid&14u)==14u);}
bool Model::intent(bool value) noexcept {
 if(value==follow_)return !exhausted_;
 if(intent_==serial_limit){exhausted_=true;follow_=false;if(!release_)release_request_=pending_;release_=true;return false;}
 ++intent_;follow_=value;if(!value&&(stage_==Stage::active||stage_==Stage::resuming)){if(!release_)release_request_=pending_;release_=true;stage_=Stage::stopped;}return true;
}
bool Model::prepare(const Prepare& p,std::uint64_t now) noexcept {
 if(exhausted_||release_||!frequency_||!admission(p.admission)||!serial(now)||p.admission.observed_qpc>now||now>=p.admission.deadline||p.admission.historical_intent!=intent_)return false;
 if(std::holds_alternative<Absent>(p.writer))return false; // Native-only intent0 is inapplicable to coordinator Follow preparation.
 if(stage_!=Stage::idle&&stage_!=Stage::active&&stage_!=Stage::stopped)return p==prepare_; // Never replace an unresolved/persistent domain.
 if(stage_!=Stage::idle&&p.admission.scope.target==prepare_.admission.scope.target&&p.admission.scope.issuer_pid==prepare_.admission.scope.issuer_pid&&
 p.admission.scope.issuer_birth==prepare_.admission.scope.issuer_birth&&p.admission.scope.transaction<=prepare_.admission.scope.transaction)return false;
 if(const auto* r=std::get_if<Registered>(&p.writer);r&&(!registered(*r)||r->domain.pose_epoch!=p.admission.epoch))return false;
 prepare_=p;worker_ready_=native_ready_=false;control_floor_=query_floor_=native_observed_=native_sequence_=0;transport_floor_=0;stage_=Stage::retiring;return true;
}
void Model::fence() noexcept {if(native_ready_&&(std::holds_alternative<Absent>(prepare_.writer)||worker_ready_))stage_=Stage::fenced;}
bool Model::worker(const WorkerReceipt& r,std::uint64_t now) noexcept {
 if(stage_!=Stage::retiring||r.prepare!=prepare_||!worker_fenced(r)||!serial(now)||r.observed_qpc>now||now>=prepare_.admission.deadline)return false;
 worker_ready_=true;retired_worker_=r;control_floor_=r.control_floor;query_floor_=r.query_floor;transport_floor_=r.transport_floor;fence();return true;
}
bool Model::native(const NativeFence& r,std::uint64_t now) noexcept {
 if(stage_!=Stage::retiring||r.prepare!=prepare_||!serial(r.observed_qpc)||!serial(r.observation_sequence)||!serial(r.consumer_serial)||
 !serial(now)||r.observed_qpc>now||now>=prepare_.admission.deadline||!r.ingress||!r.polled||!r.final_consumer||r.absence_proven!=std::holds_alternative<Absent>(prepare_.writer))return false;
 native_ready_=true;native_fence_=r;native_observed_=r.observed_qpc;native_sequence_=r.observation_sequence;fence();return true;
}
bool Model::resolve(const Resolution& r,std::uint64_t now) noexcept {
 if((stage_!=Stage::fenced&&stage_!=Stage::uncertain)||r.admission!=prepare_.admission||!serial(now)||r.observed_qpc>now||r.observed_qpc<native_observed_||
 !serial(r.observation_sequence)||r.observation_sequence<=native_sequence_||r.observation_sequence<=r.admission.observation_sequence||
 r.observed_qpc<r.admission.observed_qpc||!serial(r.epoch))return false;
 const bool applied=r.disposition==Disposition::applied&&r.retained_authority&&!r.dispatch_unresolved&&r.complete_applied_proof&&
 r.dispatch==DispatchDisposition::finished&&r.claim==ClaimDisposition::resolved&&serial(r.current_edit)&&
 r.effect_valid==15&&r.effects==15&&r.epoch>r.admission.epoch&&serial(r.commit_qpc)&&r.commit_qpc<=r.observed_qpc&&
 r.reference_sequence!=0&&r.reference_qpc>=r.commit_qpc&&r.reference_qpc<=r.observed_qpc;
 if(!applied&&!no_mutation(r)){stage_=Stage::uncertain;return false;}
 resolution_=r;stage_=Stage::resolved;return true; // PSX result has no XP/status/Follow dependency.
}
bool Model::binding(const Binding& b,std::uint64_t now) noexcept {
 const auto* old=std::get_if<Registered>(&prepare_.writer);
 if(stage_!=Stage::resolved||!old||b.admission!=prepare_.admission||!registered(b.writer)||b.writer.coordinator!=old->coordinator||
 b.writer.association!=old->association||b.writer.registration!=old->registration||!serial(b.serial)||!serial(b.observation_sequence)||!b.native_accepted||!b.actual_hello_subscribe||
 !b.hello_sequence||!b.subscribe_sequence||!serial(now)||b.observation_sequence<=resolution_.observation_sequence||
 b.observed_qpc<resolution_.observed_qpc||b.observed_qpc>now||
 b.writer.domain.pose_epoch!=resolution_.epoch||b.writer.domain==old->domain||
 b.writer.domain.native_connection==old->domain.native_connection||b.writer.domain.coordinator_connection==old->domain.coordinator_connection||
 b.writer.domain.outbox_generation<=old->domain.outbox_generation||
 !serial(b.barrier_qpc)||b.barrier_qpc<resolution_.observed_qpc||b.barrier_qpc>b.observed_qpc||
 b.source_qpc_floor<(std::max)({resolution_.observed_qpc,resolution_.reference_qpc,b.barrier_qpc})||b.source_qpc_floor>b.observed_qpc||
 !counters_correlated(retired_worker_,native_fence_,resolution_,b))return false;
 binding_=b;stage_=Stage::bound;return true;
}
}
