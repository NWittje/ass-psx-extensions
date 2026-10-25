#include "coordinator/aircraft_sync.hpp"
#include <charconv>
#include <algorithm>
namespace coordinator {
namespace w=aircraft_wire;namespace a=psx::aircraft;
namespace {bool same(const w::Sample& a,const w::Sample& b){return a.producer==b.producer&&a.lifecycle==b.lifecycle&&a.configuration==b.configuration&&a.source==b.source;}}
void AircraftModel::configure(std::uint64_t frequency,std::uint64_t owner) noexcept {frequency_=frequency;view_.owner=owner;public_id_={owner,1};}
bool AircraftModel::fresh(const w::Sample& s,std::int64_t now) const noexcept {
 if(!s.valid_mask||!(s.flags&w::initialized)||s.received_qpc>now)return false;
 if(s.source==w::Source::public_record)return (public_.flags&(psx::bridge::main_connected|psx::bridge::synchronized))==(psx::bridge::main_connected|psx::bridge::synchronized)&&s.lifecycle==public_.session_epoch&&s.configuration==public_.configuration_epoch;
 return view_.exporter_connected&&now>=s.sample_end_qpc&&static_cast<long double>(now-s.sample_end_qpc)*1000<static_cast<long double>(frequency_)*w::native_stale_ms;
}
bool AircraftModel::command(std::size_t i,w::Action action,std::int64_t now) noexcept {
 auto& g=view_.groups[i];if(!view_.plugin_connected)return false;
 if(action==w::Action::release){std::array<Output,16> retained{};std::size_t count{};for(std::size_t n=0;n<size_;n++){const auto& entry=commands_[(head_+n)%commands_.size()];if(!(entry.value.groups&(1u<<i)))retained[count++]=entry;}commands_=retained;head_=0;size_=count;}
 if(size_==commands_.size()){++view_.rejected;++view_.overflows;view_.plugin_connected=false;head_=size_=0;for(auto& v:view_.groups){v.requested=v.owned=v.pending=false;v.reason=w::Reason::overflow;}dirty_={};return false;}
 w::Control c;c.producer=g.sample.producer;c.owner=view_.owner;c.operation=++operation_;c.lifecycle=g.sample.lifecycle;c.configuration=g.sample.configuration;c.action=action;c.groups=1u<<i;
 issued_[(c.operation-1)%issued_.size()]=c;commands_[(head_+size_)%commands_.size()].value=c;++size_;++view_.controls;g.operation=c.operation;g.pending=true;pending_[i]=c;g.renewed_qpc=now;
 if(action==w::Action::acquire){acquire_source_floor_[i]=g.sample.sequence;acquire_queue_floor_[i]=now;}
 if(action==w::Action::release){g.requested=g.owned=false;dirty_[i]=false;}else g.requested=true;
 return true;
}
void AircraftModel::release(std::size_t i,w::Reason reason,std::int64_t now,bool baseline) noexcept {auto& g=view_.groups[i];if((g.requested||g.owned||g.pending)&&!(g.pending&&pending_[i].action==w::Action::release))command(i,w::Action::release,now);g.requested=g.owned=false;g.applied_available=false;g.reason=reason;dirty_[i]=false;if(baseline)g.baseline_floor=now;}
void AircraftModel::following(bool enabled,std::int64_t now) noexcept {if(view_.following==enabled)return;view_.following=enabled;if(!enabled)for(std::size_t i=0;i<6;i++)release(i,w::Reason::stopped,now,false);}
void AircraftModel::provider_boundary(std::int64_t now) noexcept {++view_.boundaries;for(std::size_t i=0;i<6;i++){release(i,w::Reason::lifecycle,now,true);view_.groups[i].blocked=false;}}
void AircraftModel::public_session(const psx::bridge::SessionStatus& s,std::int64_t now) noexcept {
 if((public_.flags&psx::bridge::synchronized)&&(s.flags&psx::bridge::main_connected)&&!(s.flags&psx::bridge::synchronized)){native_valid_={};fallback_valid_={};++view_.boundaries;for(std::size_t i=0;i<6;i++)release(i,w::Reason::loading,now,true);}
 if(s.session_epoch!=public_.session_epoch||s.configuration_epoch!=public_.configuration_epoch){fallback_={};fallback_valid_={};++view_.boundaries;}
 public_=s;
 if(!(s.flags&psx::bridge::main_connected)) {fallback_valid_={};for(std::size_t i=0;i<6;i++)if(view_.groups[i].sample.source==w::Source::public_record)release(i,w::Reason::disconnected,now,true);}
}
bool AircraftModel::patch(w::Group group,std::size_t index,const a::PropertyValue* value,std::int64_t qpc,std::uint64_t sequence,bool approximate,bool endpoint) noexcept {
 const auto i=static_cast<std::size_t>(group);if(i>=6||index>=w::component_counts[i]||qpc<=0||!sequence||!public_.session_epoch||!public_.configuration_epoch)return false;
 auto& s=fallback_[i];s.producer=public_id_;s.lifecycle=public_.session_epoch;s.configuration=public_.configuration_epoch;s.group=group;s.source=w::Source::public_record;s.flags=w::initialized;s.sequence=++public_sequence_;s.received_qpc=qpc;s.sample_start_qpc=s.sample_end_qpc=0;
 const auto bit=1u<<index;s.valid_mask&=~bit;s.approximation_mask&=~bit;s.values[index]=0;s.component_qpc[index]=0;s.component_sequence[index]=0;
 if(value&&value->validity==a::ValueValidity::valid){s.valid_mask|=bit;if(approximate)s.approximation_mask|=bit;s.values[index]=static_cast<float>(endpoint?(value->value>0?1:0):value->value);s.component_qpc[index]=qpc;s.component_sequence[index]=sequence;}
 fallback_valid_[i]=s.valid_mask!=0;return true;
}
bool AircraftModel::public_observation(const a::Observation& o) noexcept {
 if(!a::well_formed(o)||o.stamp.session_epoch!=public_.session_epoch||o.stamp.configuration_epoch!=public_.configuration_epoch)return false;
 using P=a::PropertyId;using G=w::Group;bool changed=false;
 const auto update=[&](G g,std::size_t slot,P id,std::size_t element,bool approx=false,bool endpoint=false){const auto* v=o.find(id,static_cast<std::uint8_t>(element));if(v)changed|=patch(g,slot,v,o.stamp.received_qpc,o.stamp.sequence,approx,endpoint);};
 for(std::size_t n=0;n<4;n++){update(G::controls,n,P::aileron_angle_deg,n);update(G::controls,4+n,P::elevator_angle_deg,n);update(G::high_lift,6+n,P::flap_deploy_ratio,n);update(G::reversers,n,P::engine_reverser_deploy_ratio,n,true,true);}
 for(std::size_t n=0;n<2;n++)update(G::controls,8+n,P::rudder_angle_deg,n);
 for(std::size_t n=0;n<11;n++)update(G::spoilers,n,P::spoiler_angle_deg,n);
 for(std::size_t n=0;n<6;n++)update(G::high_lift,n,P::leading_edge_deploy_ratio,n);
 for(std::size_t n=0;n<5;n++){update(G::gear,n,P::gear_deploy_ratio,n);update(G::gear,5+n,P::gear_door_open,n,true);}
 for(std::size_t n=0;n<14;n++)update(G::lights,n,P::exterior_light_level,n);
 return changed;
}
bool AircraftModel::public_stabilizer(std::string_view line,std::int64_t time,std::uint64_t sequence) noexcept {
 if(line.substr(0,6)!="Qi119=")return false;line.remove_prefix(6);int raw{};const auto p=std::from_chars(line.data(),line.data()+line.size(),raw);
 if(line.empty()||p.ec!=std::errc{}||p.ptr!=line.data()+line.size()||raw<0||raw>15000)return false;
 a::PropertyValue v;v.validity=a::ValueValidity::valid;v.value=raw/1000.0;return patch(w::Group::controls,10,&v,time,sequence,false);
}
void AircraftModel::lose(w::Role role,std::int64_t now) noexcept {
 if(role==w::Role::exporter){view_.exporter_connected=false;native_valid_={};native_={};native_lifecycle_=native_configuration_=0;native_identity_qpc_=0;for(std::size_t i=0;i<6;i++)if(view_.groups[i].sample.source==w::Source::native)release(i,w::Reason::disconnected,now,true);}
 else {view_.plugin_connected=false;head_=size_=0;dirty_={};for(auto& g:view_.groups){g.requested=g.owned=g.pending=g.applied_available=g.blocked=false;g.baseline_floor=now;g.reason=w::Reason::disconnected;}}
}
bool AircraftModel::issued(const w::Control& c) const noexcept {
 if(c.owner!=view_.owner||!c.operation||c.operation>operation_)return false;
 const auto& saved=issued_[(c.operation-1)%issued_.size()];
 if(saved.operation!=c.operation)return operation_-c.operation>=issued_.size();
 return c.producer==saved.producer&&c.lifecycle==saved.lifecycle&&c.configuration==saved.configuration&&c.action==saved.action&&c.groups==saved.groups&&c.duration_ms==saved.duration_ms;
}
bool AircraftModel::issued(const w::Applied& v) const noexcept {
 if(v.owner!=view_.owner)return false;
 const auto& last=pending_[static_cast<std::size_t>(v.group)];
 if(last.operation&&v.producer==last.producer&&v.lifecycle==last.lifecycle&&v.configuration==last.configuration)return true;
 for(const auto& c:issued_)if(c.operation&&c.action!=w::Action::release&&(c.groups&(1u<<static_cast<unsigned>(v.group)))&&v.producer==c.producer&&v.lifecycle==c.lifecycle&&v.configuration==c.configuration)return true;
 return false;
}
AircraftAdmission AircraftModel::admit(const AircraftEvent& e) noexcept {const auto result=receive(e);if(result!=AircraftAdmission::accepted)++view_.rejected;if(result==AircraftAdmission::obsolete)++view_.obsolete;return result;}
bool AircraftModel::event(const AircraftEvent& e) noexcept {return admit(e)==AircraftAdmission::accepted;}
AircraftAdmission AircraftModel::receive(const AircraftEvent& e) noexcept {
 if(e.role!=w::Role::plugin&&e.role!=w::Role::exporter)return AircraftAdmission::fault;
 const auto lane=e.role==w::Role::exporter?1u:0u;
 if(e.kind==AircraftEventKind::lost){if(e.connection_generation&&e.connection_generation!=generations_[lane])return AircraftAdmission::accepted;lose(e.role,e.received_qpc);return AircraftAdmission::accepted;}
 w::Header h;if(!w::read_header(e.frame.bytes.data(),w::header_size,h)||!h.sequence||e.frame.size!=w::header_size+h.payload_size)return AircraftAdmission::fault;
 const auto* bytes=e.frame.bytes.data()+w::header_size;
 if(h.kind==w::Kind::hello){w::Hello hello;if(!w::decode(bytes,h.payload_size,hello)||hello.role!=e.role||hello.pid!=e.process.pid||hello.process_creation_filetime!=e.process.creation_filetime||hello.qpc_frequency!=frequency_||e.connection_generation<=generations_[lane])return AircraftAdmission::fault;
 const bool initial=generations_[lane]==0;lose(e.role,e.received_qpc);if(initial&&!lane)for(auto& g:view_.groups)g.baseline_floor=0;generations_[lane]=e.connection_generation;if(lane){view_.exporter=hello;view_.exporter_connected=true;}else{view_.plugin=hello;view_.plugin_connected=true;}return AircraftAdmission::accepted;}
 if(!lane&&(h.kind==w::Kind::ack||h.kind==w::Kind::applied)&&e.connection_generation&&e.connection_generation<generations_[lane]){
  if(h.kind==w::Kind::ack){w::Ack v;if(w::decode(bytes,h.payload_size,v)&&issued(v.control))return AircraftAdmission::obsolete;}
  else {w::Applied v;if(w::decode(bytes,h.payload_size,v)&&v.owner==view_.owner&&v.applied_qpc<=e.received_qpc&&issued(v))return AircraftAdmission::obsolete;}
  return AircraftAdmission::fault;
 }
 if(e.connection_generation!=generations_[lane]||!(lane?view_.exporter_connected:view_.plugin_connected))return AircraftAdmission::fault;
 if(lane&&h.kind==w::Kind::sample){w::Sample s;if(!w::decode(bytes,h.payload_size,s)||s.source!=w::Source::native||s.producer!=view_.exporter.instance||!(view_.exporter.groups&(1u<<static_cast<unsigned>(s.group)))||s.sample_end_qpc>e.received_qpc)return AircraftAdmission::fault;
 const auto i=static_cast<std::size_t>(s.group);auto& old=native_[i];if(s.lifecycle<native_lifecycle_||(old.sequence&&(s.lifecycle<old.lifecycle||(s.lifecycle==old.lifecycle&&s.sequence<=old.sequence))))return AircraftAdmission::fault;
 if(s.lifecycle==native_lifecycle_&&native_configuration_&&s.configuration!=native_configuration_&&s.sample_end_qpc<=native_identity_qpc_)return AircraftAdmission::fault;
 if((native_lifecycle_&&s.lifecycle!=native_lifecycle_)||(native_configuration_&&s.configuration!=native_configuration_)){native_valid_={};++view_.boundaries;for(std::size_t n=0;n<6;n++)if(view_.groups[n].sample.source==w::Source::native)release(n,w::Reason::configuration,e.received_qpc,true);}
 native_lifecycle_=s.lifecycle;native_configuration_=s.configuration;
 native_identity_qpc_=(std::max)(native_identity_qpc_,s.sample_end_qpc);
 s.received_qpc=e.received_qpc;old=s;native_valid_[i]=s.valid_mask&&(s.flags&w::initialized);++view_.accepted;return AircraftAdmission::accepted;}
 if(h.kind==w::Kind::status){w::Status s;if(!w::decode(bytes,h.payload_size,s)||s.producer!=(lane?view_.exporter.instance:view_.plugin.instance))return AircraftAdmission::fault;
 if(lane){if(s.lifecycle&&s.lifecycle<native_lifecycle_)return AircraftAdmission::fault;const bool boundary=s.lifecycle>native_lifecycle_;if(boundary){native_lifecycle_=s.lifecycle;native_configuration_=s.configuration;++view_.boundaries;native_valid_={};}for(std::size_t i=0;i<6;i++)if(boundary||!(s.groups&(1u<<i))){native_valid_[i]=false;if(view_.groups[i].sample.source==w::Source::native)release(i,s.reason,e.received_qpc,true);}}
 else {view_.plugin.groups=s.groups;for(std::size_t i=0;i<6;i++)if(!(s.groups&(1u<<i)))release(i,s.reason,e.received_qpc,true);else if(s.reason==w::Reason::none)view_.groups[i].blocked=false;}return AircraftAdmission::accepted;}
 if(!lane&&h.kind==w::Kind::ack){w::Ack ack;if(!w::decode(bytes,h.payload_size,ack)||!issued(ack.control))return AircraftAdmission::fault;bool matched=false;
 for(std::size_t i=0;i<6;i++)if(ack.control.groups&(1u<<i)){auto& g=view_.groups[i];const auto& c=pending_[i];if(!g.pending||ack.control.operation!=c.operation||ack.control.action!=c.action||ack.control.groups!=c.groups||ack.control.producer!=c.producer||ack.control.lifecycle!=c.lifecycle||ack.control.configuration!=c.configuration)continue;
 matched=true;g.pending=false;const bool accepted=(ack.accepted_groups&(1u<<i))&&ack.reason==w::Reason::none&&(ack.phase==w::Phase::owned||ack.phase==w::Phase::observing);
 // A pending/observing Renew acknowledges the continuing claim, not a loss
 // of its confirmed output. Retain that evidence only inside the old lease;
 // Acquire, rejection and every explicit boundary still require new evidence.
 const bool retained_owned=g.owned&&ack.control.action==w::Action::renew&&e.received_qpc<g.expires_qpc;
 g.owned=g.requested&&view_.following&&accepted&&(ack.phase==w::Phase::owned||retained_owned);g.reason=ack.reason;if(g.requested&&accepted)g.expires_qpc=g.renewed_qpc+static_cast<std::int64_t>(frequency_);else if(g.requested){g.requested=false;g.baseline_floor=e.received_qpc;g.blocked=ack.reason==w::Reason::binding||ack.reason==w::Reason::unsupported||ack.reason==w::Reason::owner;}}
 if(!matched)return AircraftAdmission::obsolete;++view_.acks;return AircraftAdmission::accepted;}
 if(!lane&&h.kind==w::Kind::applied){w::Applied v;if(!w::decode(bytes,h.payload_size,v)||v.provider!=view_.plugin.instance||v.owner!=view_.owner||v.applied_qpc>e.received_qpc)return AircraftAdmission::fault;
 auto& g=view_.groups[static_cast<std::size_t>(v.group)];
 const bool current=v.producer==g.sample.producer&&v.lifecycle==g.sample.lifecycle&&v.configuration==g.sample.configuration;
 if(!current&&v.producer==g.sample.producer&&(v.lifecycle<g.sample.lifecycle||v.applied_qpc<=g.baseline_floor))return AircraftAdmission::obsolete;
 if(!issued(v))return AircraftAdmission::fault;
 // Applied has no operation echo. A delayed prior-claim report must not retire
 // a new claim whose selected source baseline is already newer. Keep this
 // floor fixed across Renew; public on-change observations may retain exactly
 // the same sequence across a legitimate Stop/Follow boundary.
 if(current&&v.sequence<acquire_source_floor_[static_cast<std::size_t>(v.group)])return AircraftAdmission::obsolete;
 // Network normalization supplies the mapped earliest application time. Its
 // lower bound must qualify this Acquire before any phase can establish or
 // retire the claim. A pre-floor bound can be ambiguous, not proven old; a
 // later coherent SDK report can qualify without extending any lease.
 if(current&&v.applied_qpc<acquire_queue_floor_[static_cast<std::size_t>(v.group)])return AircraftAdmission::obsolete;
 if(current&&v.sequence>g.sample.sequence)return AircraftAdmission::fault;
 if(!current||!g.requested||e.received_qpc>=g.expires_qpc||(g.applied_available&&v.sequence<g.applied.sequence))return AircraftAdmission::obsolete;if(!v.valid_mask&&(v.phase==w::Phase::released||v.phase==w::Phase::unavailable)){release(static_cast<std::size_t>(v.group),v.reason,e.received_qpc,true);g.blocked=true;}g.applied=v;g.applied_available=true;if(v.phase==w::Phase::owned&&v.valid_mask)g.owned=true;return AircraftAdmission::accepted;}
 return AircraftAdmission::fault;
}
bool AircraftModel::select(std::size_t i,std::int64_t now) noexcept {
 auto& g=view_.groups[i];const w::Sample* s=native_valid_[i]&&fresh(native_[i],now)?&native_[i]:fallback_valid_[i]&&fresh(fallback_[i],now)?&fallback_[i]:nullptr;
 if(!s){g.available=false;release(i,w::Reason::stale,now,true);return false;}
 const bool boundary=g.sample.sequence&&!same(*s,g.sample);if(boundary){release(i,w::Reason::lifecycle,now,false);g.blocked=false;}
 const bool changed=!g.available||!same(*s,g.sample)||s->sequence!=g.sample.sequence;g.available=true;g.sample=*s;
 if(changed)dirty_[i]=true;return true;
}
void AircraftModel::tick(std::int64_t now) noexcept {
 for(std::size_t i=0;i<6;i++){auto& g=view_.groups[i];if(!select(i,now))continue;
 // Renew confirms a continuing claim, not continuing physical application.
 // Keep the original mapped-earliest report clock: a renewable pending claim
 // can remain requested while its physical output evidence becomes unknown.
 if(g.applied_available&&now>=g.applied.applied_qpc&&now-g.applied.applied_qpc>=static_cast<std::int64_t>(frequency_)){g.owned=false;g.applied_available=false;}
 if(!view_.following||!view_.plugin_connected||g.blocked||!(view_.plugin.groups&(1u<<i))){if(g.requested)release(i,w::Reason::unsupported,now,false);continue;}
 // A timed-out Acquire/Renew is replaced by one ordered Release. That Release
 // remains pending until its Ack or transport retirement, even if SDK callbacks
 // are blocked by placement; fresh source copies cannot replenish the SDK FIFO.
 if(g.pending&&pending_[i].action!=w::Action::release&&now-g.renewed_qpc>=static_cast<std::int64_t>(frequency_)){g.pending=false;release(i,w::Reason::expired,now,true);}
 if(g.requested&&!g.pending&&now>=g.expires_qpc)release(i,w::Reason::expired,now,true);
 const auto baseline=g.sample.source==w::Source::native?g.sample.sample_end_qpc:g.sample.received_qpc;
 if(!g.requested&&!g.pending&&baseline>g.baseline_floor){if(command(i,w::Action::acquire,now))dirty_[i]=true;}
 else if(g.requested&&!g.pending&&static_cast<long double>(now-g.renewed_qpc)*1000>=static_cast<long double>(frequency_)*w::renew_ms)command(i,w::Action::renew,now);
 }
}
bool AircraftModel::next(AircraftDispatch& f) noexcept {
 if(size_){f={};f.kind=w::Kind::control;f.control=commands_[head_].value;head_=(head_+1)%commands_.size();--size_;return true;}
 for(std::size_t n=0;n<6;n++){const auto i=cursor_++%6;if(dirty_[i]){dirty_[i]=false;const auto& g=view_.groups[i];if(g.available&&view_.plugin_connected){f={};f.kind=w::Kind::sample;f.sample=g.sample;return true;}}}
 return false;
}
}
