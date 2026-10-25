#include "coordinator/readiness.hpp"
#include <algorithm>
#include <limits>
namespace coordinator {
namespace s=status_wire;
namespace {
std::size_t lane(s::Role r){return r==s::Role::exporter?1:0;}
bool role(s::Role r){return r==s::Role::plugin||r==s::Role::exporter;}
std::uint32_t capability(std::size_t i){return i?s::fatal_state:s::xplane_readiness;}
bool broken(s::Reason r){return r==s::Reason::native_fault||r==s::Reason::disconnected||r==s::Reason::timeout;}
bool invalidates(s::Reason r){return r!=s::Reason::none&&r!=s::Reason::loading;}
bool target(const s::Snapshot& v){return v.selection_epoch&&v.target_epoch&&v.target_sequence&&
 ((v.target_source==static_cast<unsigned>(s::TargetSource::public_main_boost)&&v.target_producer==psx::motion::InstanceId{})||
 (v.target_source==static_cast<unsigned>(s::TargetSource::exporter)&&psx::motion::valid(v.target_producer)));}
}
const char* xplane_state_name(XPlaneState v) noexcept {switch(v){case XPlaneState::Ready:return "Ready";case XPlaneState::Loading:return "Loading";case XPlaneState::Unavailable:return "Unavailable";default:return "Unknown";}}
const char* psx_state_name(PsxState v) noexcept {switch(v){case PsxState::Running:return "Running";case PsxState::Crashed:return "Crashed";case PsxState::Unavailable:return "Unavailable";default:return "Unknown";}}
const char* readiness_reason_name(ReadinessReason v) noexcept {
 constexpr const char* names[]={"Qualified source","Disconnected","Heartbeat timeout","Awaiting fresh source","Unsupported status","Unqualified source","Source stale","Loading/reconciling","Plugin disabled","Binding unavailable","Reference/readback unavailable","Status producer failed","Identity changed","Lifecycle changed","Invalid status"};
 const auto i=static_cast<std::size_t>(v);return i<std::size(names)?names[i]:"Unknown reason";
}
void ReadinessModel::configure(std::uint64_t frequency) noexcept {frequency_=frequency;peers_={};view_={};selected_plugin_={};}
void ReadinessModel::evaluate(std::size_t i,std::int64_t now) noexcept {
 auto& p=peers_[i];auto& d=i?view_.exporter:view_.plugin;const auto previous=i?static_cast<unsigned>(view_.psx):static_cast<unsigned>(view_.xplane);
 const auto previous_reason=d.reason;const auto previous_health=d.communication_healthy;
 unsigned state=3;d.communication_healthy=p.connected&&frequency_&&now>=p.heartbeat&&
  static_cast<std::uint64_t>(now-p.heartbeat)<frequency_*s::peer_timeout_ms/1000;
 d.process=p.process;d.producer=p.hello.instance;d.connection_id=p.connection;d.lifecycle=p.lifecycle;
 d.observation_available=p.have_snapshot;d.observed_qpc=p.have_snapshot?p.snapshot.observed_qpc:0;
 d.observation_age_qpc=p.have_snapshot&&now>=p.snapshot.observed_qpc?now-p.snapshot.observed_qpc:-1;
 if(p.have_snapshot)d.source=p.snapshot;
 if(!d.communication_healthy){state=2;d.reason=p.connected?ReadinessReason::timeout:ReadinessReason::disconnected;}
 else if(broken(p.reason))d.reason=ReadinessReason::producer_fault;
 else if(!i&&p.process!=selected_plugin_)d.reason=ReadinessReason::identity;
 else if(!(p.capabilities&capability(i))||p.reason==s::Reason::unsupported)d.reason=ReadinessReason::unsupported;
 else if(p.reason==s::Reason::identity)d.reason=ReadinessReason::identity;
 else if(p.reason==s::Reason::lifecycle)d.reason=ReadinessReason::lifecycle;
 else if(p.reason==s::Reason::stale)d.reason=ReadinessReason::stale;
 else if(p.reason==s::Reason::binding)d.reason=ReadinessReason::binding;
 else if(!p.have_snapshot)d.reason=ReadinessReason::awaiting_source;
 else if(d.observation_age_qpc<0||static_cast<std::uint64_t>(d.observation_age_qpc)>frequency_*s::source_stale_ms/1000)d.reason=ReadinessReason::stale;
 else if(broken(p.snapshot.reason))d.reason=ReadinessReason::producer_fault;
 else if(p.snapshot.reason==s::Reason::unsupported)d.reason=ReadinessReason::unsupported;
 else if(p.snapshot.reason==s::Reason::identity)d.reason=ReadinessReason::identity;
 else if(p.snapshot.reason==s::Reason::lifecycle)d.reason=ReadinessReason::lifecycle;
 else if(p.snapshot.reason==s::Reason::stale)d.reason=ReadinessReason::stale;
 else if(!(p.snapshot.flags&s::qualified))d.reason=ReadinessReason::unqualified;
 else if(p.snapshot.reason==s::Reason::binding||p.reason==s::Reason::binding)d.reason=ReadinessReason::binding;
 else if(i){if(!(p.snapshot.flags&s::fatal_valid))d.reason=ReadinessReason::unqualified;
  else {state=(p.snapshot.flags&(s::fatal_active|s::fatal_requested))?1:0;d.reason=ReadinessReason::none;}}
 else if(!(p.snapshot.flags&s::plugin_enabled))d.reason=ReadinessReason::disabled;
 else if(p.snapshot.flags&(s::placement_active|s::reconciling|s::async_busy)){state=1;d.reason=ReadinessReason::loading;}
 else if((p.snapshot.flags&(s::aircraft_loaded|s::bindings_ready))!=(s::aircraft_loaded|s::bindings_ready))d.reason=ReadinessReason::binding;
 else if((p.snapshot.flags&s::readback_required)&&
  ((p.snapshot.flags&(s::reference_ready|s::readback_ready))!=(s::reference_ready|s::readback_ready)||!target(p.snapshot)))d.reason=ReadinessReason::reference;
 else {state=0;d.reason=ReadinessReason::none;}
 if(state<2){d.last_known_available=true;d.last_known_state=state;d.last_known_qpc=p.snapshot.observed_qpc;
  d.last_known_phase_qpc=p.snapshot.phase_start_qpc;d.last_known_producer=p.hello.instance;}
 if(i)view_.psx=static_cast<PsxState>(state);else view_.xplane=static_cast<XPlaneState>(state);
 if(previous!=state||previous_reason!=d.reason||previous_health!=d.communication_healthy){++d.revision;++view_.revision;}
}
ReadinessAdmission ReadinessModel::event(const ReadinessEvent& e) noexcept {
 if(!role(e.role)||e.received_qpc<=0){++view_.faults;return ReadinessAdmission::fault;}
 const auto i=lane(e.role);auto& p=peers_[i];
 const auto fault=[&](){++view_.faults;p.connected=false;p.have_snapshot=false;evaluate(i,e.received_qpc);return ReadinessAdmission::fault;};
 const auto obsolete=[&](){++view_.obsolete;return ReadinessAdmission::obsolete;};
 if(e.kind==ReadinessEventKind::selection){
  if(i||(!e.process.pid&&e.process.creation_filetime)|| (e.process.pid&&!e.process.creation_filetime))return fault();
  if(selected_plugin_.pid&&selected_plugin_!=e.process)p.have_snapshot=false;
  selected_plugin_=e.process;
 }else if(e.kind==ReadinessEventKind::hello){
  s::Frame f;if(!s::encode(e.hello,1,f)||e.hello.role!=e.role||e.hello.pid!=e.process.pid||
   e.hello.process_creation_filetime!=e.process.creation_filetime||e.hello.qpc_frequency!=frequency_||
   !e.connection_id||e.subscribe_floor_qpc<=0||e.subscribe_floor_qpc>e.received_qpc)return fault();
  if(p.connection&&e.connection_id<=p.connection)return obsolete();
  p={};p.connected=true;p.process=e.process;p.hello=e.hello;p.connection=e.connection_id;
  p.floor=e.subscribe_floor_qpc;p.heartbeat=e.received_qpc;p.capabilities=e.hello.capabilities;
  auto& d=i?view_.exporter:view_.plugin;d.observation_available=false;d.source={};
 }else if(e.kind==ReadinessEventKind::lost){
  if(p.connection&&e.connection_id&&e.connection_id!=p.connection)return obsolete();
  if(p.process.pid&&e.process.pid&&e.process!=p.process)return obsolete();
  p.connected=false;p.have_snapshot=false;
 }else{
  if(!p.connected||e.process!=p.process||e.connection_id!=p.connection)return obsolete();
  s::Header h;if(e.frame.size<s::header_size||!s::read_header(e.frame.bytes.data(),s::header_size,h)||
   e.frame.size!=s::header_size+h.payload_size)return fault();
  if(h.sequence<=p.wire_sequence)return fault();
  const auto* bytes=e.frame.bytes.data()+s::header_size;
  if(h.kind==s::Kind::heartbeat){s::Heartbeat v;if(!s::decode(bytes,h.payload_size,v)||v.producer!=p.hello.instance||
    v.connection_id!=p.connection||v.generated_qpc>e.received_qpc||v.generated_qpc<p.floor||
    (v.capabilities&~capability(i)))return fault();
   if(invalidates(v.reason)||!(v.capabilities&capability(i)))p.have_snapshot=false;
   p.capabilities=v.capabilities;p.reason=v.reason;p.heartbeat=e.received_qpc;
  }else if(h.kind==s::Kind::snapshot){s::Snapshot v;if(!s::decode(bytes,h.payload_size,v)||v.role!=e.role||
    v.producer!=p.hello.instance||v.connection_id!=p.connection||v.observed_qpc>e.received_qpc||
    v.phase_start_qpc>v.observed_qpc||v.call_returned_qpc>v.observed_qpc||v.notification_qpc>v.observed_qpc)return fault();
   if(i&&(v.enable_generation||v.aircraft_generation||v.scenery_generation||v.local_frame_generation||v.command_id||v.placement_attempt||v.target_epoch||v.target_sequence||v.callback_count||v.phase_start_qpc||v.call_returned_qpc||v.notification_qpc||v.notification_kind||v.target_source||v.selection_epoch||v.target_producer!=psx::motion::InstanceId{}))return fault();
   p.wire_sequence=h.sequence;p.heartbeat=e.received_qpc;
   if(v.observed_qpc<=p.floor||v.sequence<=p.source_sequence||v.lifecycle<p.lifecycle||
    (p.source_sequence&&v.observed_qpc<p.snapshot.observed_qpc))return obsolete();
   if(!i&&p.source_sequence&&(v.enable_generation<p.snapshot.enable_generation||v.aircraft_generation<p.snapshot.aircraft_generation||
    v.scenery_generation<p.snapshot.scenery_generation||(v.local_frame_generation&&p.snapshot.local_frame_generation&&v.local_frame_generation<p.snapshot.local_frame_generation)))return obsolete();
   if(!i&&p.source_sequence&&((v.selection_epoch&&p.snapshot.selection_epoch&&v.selection_epoch<p.snapshot.selection_epoch)||
    (v.command_id&&v.command_id==p.snapshot.command_id&&v.placement_attempt<p.snapshot.placement_attempt)))return obsolete();
   if(!i&&p.source_sequence&&v.selection_epoch==p.snapshot.selection_epoch&&v.target_source==p.snapshot.target_source&&
    v.target_producer==p.snapshot.target_producer&&v.target_epoch==p.snapshot.target_epoch&&v.target_sequence&&p.snapshot.target_sequence&&v.target_sequence<p.snapshot.target_sequence)return obsolete();
   p.snapshot=v;p.have_snapshot=(p.capabilities&capability(i))&&!invalidates(p.reason);p.source_sequence=v.sequence;p.lifecycle=v.lifecycle;
  }else return fault();
  p.wire_sequence=h.sequence;
 }
 ++view_.accepted;evaluate(i,e.received_qpc);return ReadinessAdmission::accepted;
}
void ReadinessModel::tick(std::int64_t now) noexcept {for(std::size_t i=0;i<2;i++)evaluate(i,now);}
void ReadinessModel::disconnect(std::int64_t now) noexcept {for(auto& p:peers_){p.connected=false;p.have_snapshot=false;}tick(now);}
bool ReadinessModel::restore(const Checkpoint& c,std::int64_t now) noexcept {
 if(!frequency_||now<=0)return false;
 ReadinessModel check;check.configure(frequency_);
 for(std::size_t i=0;i<2;++i){const auto& p=c.peers[i];if(!p.connection){if(p.connected||p.have_snapshot||p.source_sequence)return false;continue;}
  s::Frame f;if(!s::encode(p.hello,1,f)||p.hello.role!=(i?s::Role::exporter:s::Role::plugin)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=frequency_||p.floor<=0||p.heartbeat<p.floor||p.heartbeat>now||p.capabilities&~capability(i))return false;
  if(p.source_sequence&&(!s::valid(p.snapshot)||p.snapshot.producer!=p.hello.instance||p.snapshot.connection_id!=p.connection||p.snapshot.role!=p.hello.role||p.snapshot.sequence!=p.source_sequence||p.snapshot.lifecycle!=p.lifecycle||p.snapshot.observed_qpc<=p.floor||p.snapshot.observed_qpc>now))return false;
  if(p.have_snapshot&&!p.source_sequence)return false;
 }
 check.peers_=c.peers;check.view_=c.view;check.selected_plugin_=c.selected_plugin;check.tick(now);
 if(check.view_.xplane!=c.view.xplane||check.view_.psx!=c.view.psx||check.view_.plugin.reason!=c.view.plugin.reason||check.view_.exporter.reason!=c.view.exporter.reason||check.view_.plugin.communication_healthy!=c.view.plugin.communication_healthy||check.view_.exporter.communication_healthy!=c.view.exporter.communication_healthy)return false;
 peers_=c.peers;view_=check.view_;selected_plugin_=c.selected_plugin;return true;
}
}
