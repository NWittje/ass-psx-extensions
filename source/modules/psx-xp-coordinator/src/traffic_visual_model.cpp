#include "coordinator/traffic_visual.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace coordinator {
namespace w=visual_wire;
namespace {
bool same_source(const w::Claim& c,const TrafficVisualSource& v) noexcept {const auto& s=v.sample;const auto& h=s.snapshot.header;return c.source_producer==s.producer&&c.source_connection==s.connection_id&&c.source_selection==s.selection_epoch&&c.lifecycle==h.lifecycle&&c.source_pid==h.process&&c.source_birth==h.process_created&&c.frequency==h.frequency;}
bool same_control(const w::Claim& a,const w::Claim& b) noexcept {w::Frame x,y;return w::encode(a,1,x)&&w::encode(b,1,y)&&x.size==y.size&&std::equal(x.bytes.begin(),x.bytes.begin()+x.size,y.bytes.begin());}
bool same_frame(const w::Frame& a,const w::Frame& b) noexcept {return a.size==b.size&&a.size<=w::max_frame&&std::equal(a.bytes.begin(),a.bytes.begin()+a.size,b.bytes.begin());}
bool same_domain(const traffic_wire::Sample& a,const traffic_wire::Sample& b) noexcept {return a.producer==b.producer&&a.connection_id==b.connection_id&&a.selection_epoch==b.selection_epoch&&a.snapshot.header.lifecycle==b.snapshot.header.lifecycle&&a.snapshot.header.process==b.snapshot.header.process&&a.snapshot.header.process_created==b.snapshot.header.process_created&&a.snapshot.header.frequency==b.snapshot.header.frequency;}
std::uint32_t drawable(const psx::traffic::Snapshot& s) noexcept {std::uint32_t mask{};for(std::size_t i=0;i<7;++i)if((s.slots[i].flags&16u)&&(s.slots[i].valid&63u)==63u)mask|=1u<<i;return mask;}
}
TrafficVisualSource visual_source(const TrafficView& v) noexcept {return {{v.producer,v.connection_id,v.selection_epoch,v.source},v.communication_healthy,v.source_available,v.retained,v.reason,v.received_qpc,v.admitted_qpc};}
const char* traffic_visual_reason_name(w::Reason r) noexcept {constexpr const char* names[]{"Qualified SDK submission","Unsupported traffic protocol","Awaiting fresh source","Model loading","Source stale","Identity","Lifecycle","Native fault","Disconnected","Timeout","Protocol fault","Overflow","Incompatible model","Model unavailable","Model fault","Binding","Competing owner","Released"};const auto n=static_cast<unsigned>(r);return n<std::size(names)?names[n]:"Invalid reason";}
TrafficVisualModel::TrafficVisualModel():state_(std::make_unique<State>()){}
TrafficVisualModel::~TrafficVisualModel()=default;
void TrafficVisualModel::configure(std::uint64_t frequency,psx::motion::InstanceId owner){state_=std::make_unique<State>();state_->frequency=frequency;state_->view.owner=owner;}
const TrafficVisualView& TrafficVisualModel::view() const noexcept{return state_->view;}
const TrafficVisualModel::State& TrafficVisualModel::checkpoint() const noexcept{return *state_;}
bool TrafficVisualModel::fresh(std::int64_t now) const noexcept {const auto& s=state_->source;return s.healthy&&s.available&&s.retained&&s.reason==traffic_wire::Reason::none&&traffic_wire::valid(s.sample)&&s.sample.snapshot.header.frequency==state_->frequency&&now>=static_cast<std::int64_t>(s.sample.snapshot.header.observed)&&static_cast<std::uint64_t>(now-s.sample.snapshot.header.observed)<state_->frequency;}
void TrafficVisualModel::lose(w::Reason reason,std::uint32_t error,std::int64_t now) noexcept {auto& s=*state_;auto& v=s.view;s.connected=false;s.head=s.size=0;s.source_dirty=false;s.binding={};v.communication_healthy=v.requested=v.pending=v.owned=v.report_current=false;v.phase=w::Phase::released;v.reason=reason;v.error=error;v.drawable_mask=v.submitted_mask=v.held_mask=v.hidden_mask=0;v.lease=v.request=0;s.cooldown_qpc=now;++v.revision;}
bool TrafficVisualModel::command(w::Action action,std::int64_t now) noexcept {
 auto& s=*state_;auto& v=s.view;if(!s.connected)return false;
 if(action==w::Action::release){std::array<w::Claim,16> keep{};std::size_t count{};for(std::size_t i=0;i<s.size;++i){const auto& c=s.commands[(s.head+i)%s.commands.size()];if(c.lease!=s.binding.lease)keep[count++]=c;}s.commands=keep;s.head=0;s.size=count;}
 if(s.size==s.commands.size()||s.request_counter==UINT64_MAX||(action==w::Action::acquire&&s.lease_counter==UINT64_MAX)){++v.overflows;lose(w::Reason::overflow,0,now);return false;}
 if(action==w::Action::acquire){const auto& source=s.source.sample;const auto& h=source.snapshot.header;w::Claim c;c.plugin=v.plugin.instance;c.owner=v.owner;c.connection=v.connection;c.lease=++s.lease_counter;c.source_producer=source.producer;c.source_connection=source.connection_id;c.source_selection=source.selection_epoch;c.lifecycle=h.lifecycle;c.source_pid=h.process;c.source_birth=h.process_created;c.frequency=s.frequency;c.source_floor_qpc=now;c.presentation_mode=v.policy.mode==psx::motion::ReconstructionMode::direct?1u:0u;c.delay_ms=c.presentation_mode?0u:v.policy.delay_ms;s.binding=c;v.lease=c.lease;v.source_floor_qpc=now;v.report_current=false;v.drawable_mask=v.submitted_mask=v.held_mask=v.hidden_mask=0;++v.boundaries;}
 auto c=s.binding;c.action=action;c.request=++s.request_counter;if(!w::valid(c)){lose(w::Reason::protocol,0,now);return false;}s.binding=c;s.planned_controls[(c.request-1)%s.planned_controls.size()]=c;s.commands[(s.head+s.size)%s.commands.size()]=c;++s.size;v.request=c.request;v.renewed_qpc=now;
 if(action==w::Action::release){v.requested=v.pending=v.owned=v.report_current=false;v.phase=w::Phase::released;s.source_dirty=false;v.drawable_mask=v.submitted_mask=v.held_mask=v.hidden_mask=0;}
 else {v.requested=true;if(action==w::Action::acquire){v.pending=true;v.owned=false;v.phase=w::Phase::pending;v.reason=(v.plugin.capabilities&w::loaded_model)?w::Reason::awaiting_source:w::Reason::loading;}}
 ++v.revision;return true;
}
void TrafficVisualModel::release(w::Reason reason,std::int64_t now) noexcept {auto& s=*state_;if(s.binding.lease&&s.binding.action!=w::Action::release)command(w::Action::release,now);auto& v=s.view;v.requested=v.pending=v.owned=v.report_current=false;v.drawable_mask=v.submitted_mask=v.held_mask=v.hidden_mask=0;v.phase=w::Phase::released;v.reason=reason;s.source_dirty=false;}
void TrafficVisualModel::tick(std::int64_t now) noexcept {
 auto& s=*state_;auto& v=s.view;const auto before_healthy=v.communication_healthy,before_current=v.report_current;const auto before_reason=v.reason;
 v.communication_healthy=s.connected&&s.frequency&&now>=v.heartbeat_qpc&&static_cast<std::uint64_t>(now-v.heartbeat_qpc)<s.frequency*5;
 if(s.connected&&!v.communication_healthy)lose(w::Reason::timeout,0,now);
 if(s.connected&&v.following){
  if((v.plugin.capabilities&(w::supported|w::qualified_model))!=(w::supported|w::qualified_model)){release((v.plugin.capabilities&w::supported)?w::Reason::model_unavailable:w::Reason::unsupported,now);}
  else if(!fresh(now)){release(s.source.reason==traffic_wire::Reason::loading?w::Reason::loading:s.source.retained?w::Reason::stale:w::Reason::awaiting_source,now);}
  else if((!v.requested||s.binding.action==w::Action::release)&&now>=s.cooldown_qpc){command(w::Action::acquire,now);}
  else if(v.requested&&now-v.renewed_qpc>=static_cast<std::int64_t>(s.frequency/4)){command(w::Action::renew,now);}
 }
 v.presentation_age_qpc=v.report_retained&&v.report.source_observed_qpc>0&&now>=v.report.source_observed_qpc?now-v.report.source_observed_qpc:-1;
 v.report_current=v.following&&v.communication_healthy&&v.owned&&v.report_retained&&v.report.lease==v.lease&&v.report.connection==v.connection&&fresh(now)&&v.presentation_age_qpc>=0&&static_cast<std::uint64_t>(v.presentation_age_qpc)<s.frequency&&v.report.generated_qpc<=now&&static_cast<std::uint64_t>(now-v.report.generated_qpc)<s.frequency&&v.report.applied_qpc>0&&v.report.resource_generation>0;
 if(!v.report_current)v.drawable_mask=v.submitted_mask=v.held_mask=v.hidden_mask=0;
 else {std::uint32_t current_generations{};const auto* b=issued(v.report.lease,v.report.source_sequence,v.report.source_observed_qpc);if(b)for(std::size_t i=0;i<7;++i)if(b->source.snapshot.slots[i].generation==s.source.sample.snapshot.slots[i].generation)current_generations|=1u<<i;v.drawable_mask=v.report.drawable_mask&current_generations;v.submitted_mask=v.report.submitted_mask&current_generations;v.held_mask=v.report.held_mask&current_generations;v.hidden_mask=v.report.hidden_mask&current_generations;}
 if(s.capture_enabled&&s.connected&&now-s.last_capture_qpc>=static_cast<std::int64_t>(s.frequency/2))s.capture_dirty=true;
 if(before_healthy!=v.communication_healthy||before_current!=v.report_current||before_reason!=v.reason)++v.revision;
}
const w::Claim* TrafficVisualModel::issued(const w::Report& r) const noexcept {const auto& c=state_->issued_controls[(r.request?r.request-1:0)%state_->issued_controls.size()];return r.request&&c.request==r.request&&c.plugin==r.plugin&&c.owner==r.owner&&c.connection==r.connection&&c.lease==r.lease&&c.source_producer==r.source_producer&&c.source_connection==r.source_connection&&c.source_selection==r.source_selection&&c.lifecycle==r.lifecycle&&c.model==r.model&&c.profile==r.profile?&c:nullptr;}
const w::Bundle* TrafficVisualModel::issued(std::uint64_t lease,std::uint64_t sequence,std::int64_t observed) const noexcept {const auto& s=*state_;const auto count=(std::min)(s.source_cursor,s.issued_sources.size());for(std::size_t i=0;i<count;++i){const auto& b=s.issued_sources[(s.source_cursor-1-i)%s.issued_sources.size()];if(b.lease==lease&&b.source.snapshot.header.sequence==sequence&&b.source.snapshot.header.observed==static_cast<std::uint64_t>(observed))return &b;}return nullptr;}
TrafficVisualAdmission TrafficVisualModel::event(const TrafficVisualEvent& e,std::int64_t now) noexcept {const auto result=receive(e,now);if(result==TrafficVisualAdmission::accepted){++state_->view.accepted;tick(now);}else if(result==TrafficVisualAdmission::obsolete)++state_->view.obsolete;else {++state_->view.faults;lose(w::Reason::protocol,e.error,now);}return result;}
TrafficVisualAdmission TrafficVisualModel::receive(const TrafficVisualEvent& e,std::int64_t now) noexcept {
 auto& s=*state_;auto& v=s.view;const auto fault=TrafficVisualAdmission::fault,old=TrafficVisualAdmission::obsolete,ok=TrafficVisualAdmission::accepted;
 if(!s.frequency||s.frequency>1000000000000ULL||!psx::motion::valid(v.owner)||e.received_qpc<=0||now<e.received_qpc||static_cast<unsigned>(e.kind)>9)return fault;
 if(e.kind==TrafficVisualEventKind::selection){if((e.process.pid!=0)!=(e.process.creation_filetime!=0))return fault;if(v.selected!=e.process){lose(w::Reason::disconnected,0,now);v.selected=e.process;}return ok;}
 if(e.kind==TrafficVisualEventKind::source){if(e.source.retained&&(!traffic_wire::valid(e.source.sample)||e.source.received_qpc>e.source.admitted_qpc||e.source.admitted_qpc>now||e.source.sample.snapshot.header.finished>static_cast<std::uint64_t>(e.source.received_qpc)))return fault;if(e.source.available&&(!e.source.retained||!e.source.healthy||e.source.reason!=traffic_wire::Reason::none))return fault;
  if(s.binding.lease&&e.source.retained&&!same_source(s.binding,e.source))release(w::Reason::lifecycle,now);
  const bool changed=s.source.sample.snapshot.header.sequence!=e.source.sample.snapshot.header.sequence||s.source.sample.producer!=e.source.sample.producer||s.source.sample.connection_id!=e.source.sample.connection_id;s.source=e.source;if(changed)s.source_dirty=true;return ok;}
 if(e.kind==TrafficVisualEventKind::following||e.kind==TrafficVisualEventKind::policy){if(!psx::motion::valid(e.policy))return fault;const bool changed=v.policy.mode!=e.policy.mode||v.policy.delay_ms!=e.policy.delay_ms;if(changed&&v.requested)release(w::Reason::binding,now);v.policy=e.policy;if(e.kind==TrafficVisualEventKind::following){v.following=e.enabled;if(!e.enabled)release(w::Reason::released,now);}return ok;}
 if(e.kind==TrafficVisualEventKind::capture){if(!e.capture_generation||e.capture_generation<s.capture_generation)return old;s.capture_generation=e.capture_generation;s.capture_enabled=e.enabled;s.capture_start_qpc=e.received_qpc;s.capture_floor_qpc=0;s.capture_dirty=true;v.capture_generation=e.capture_generation;return ok;}
 if(e.kind==TrafficVisualEventKind::dispatch){w::Frame expected;if(!next(expected,now)||!same_frame(expected,e.frame))return fault;return ok;}
 if(e.kind==TrafficVisualEventKind::hello){w::Frame f;if(e.process!=v.selected||!e.process.pid||e.hello.pid!=e.process.pid||e.hello.process_creation_filetime!=e.process.creation_filetime||e.hello.qpc_frequency!=s.frequency||!w::encode(e.hello,1,f)||!e.connection)return fault;if(e.connection<=v.connection)return old;lose(w::Reason::awaiting_source,0,now);v.process=e.process;v.plugin=e.hello;v.connection=e.connection;v.heartbeat_qpc=e.received_qpc;s.wire_sequence=0;s.connected=true;s.capture_dirty=true;return ok;}
 if(e.kind==TrafficVisualEventKind::lost){if(e.connection&&e.connection!=v.connection)return old;if(e.process.pid&&e.process!=v.process)return old;lose(e.reason,e.error,now);return ok;}
 w::Header h;if(e.frame.size<w::header_size||!w::read_header(e.frame.bytes.data(),w::header_size,h)||e.frame.size!=w::header_size+h.payload_size)return fault;
 if(e.connection!=v.connection||e.process!=v.process||!s.connected)return old;const auto* p=e.frame.bytes.data()+w::header_size;
 if(e.kind==TrafficVisualEventKind::sent){
  if(h.kind==w::Kind::claim){w::Claim c;if(!w::decode(p,h.payload_size,c)||c.owner!=v.owner||c.plugin!=v.plugin.instance||c.connection!=v.connection||c.request>s.request_counter||!same_control(c,s.planned_controls[(c.request-1)%s.planned_controls.size()]))return fault;s.issued_controls[(c.request-1)%s.issued_controls.size()]=c;return ok;}
  if(h.kind==w::Kind::bundle){w::Bundle b;if(!w::decode(p,h.payload_size,b)||b.owner!=v.owner||b.plugin!=v.plugin.instance||b.connection!=v.connection)return fault;bool planned=false;const auto count=(std::min)(s.planned_source_cursor,s.planned_sources.size());for(std::size_t i=0;i<count&&!planned;++i){const auto& f=s.planned_sources[(s.planned_source_cursor-1-i)%s.planned_sources.size()];planned=f.size==e.frame.size&&std::equal(f.bytes.begin()+w::header_size,f.bytes.begin()+f.size,p);}if(!planned)return fault;s.issued_sources[s.source_cursor++%s.issued_sources.size()]=b;if(b.lease==v.lease)v.sent_sequence=b.source.snapshot.header.sequence;return ok;}
  if(h.kind==w::Kind::capture){w::Capture c;if(!w::decode(p,h.payload_size,c)||c.plugin!=v.plugin.instance||c.connection!=v.connection||c.sent_qpc>e.received_qpc)return fault;bool planned=false;for(const auto& candidate:s.planned_captures)if(candidate.generation){w::Frame f;if(w::encode(candidate,1,f)&&f.size==e.frame.size&&std::equal(f.bytes.begin()+w::header_size,f.bytes.begin()+f.size,p)){planned=true;break;}}if(!planned)return fault;s.issued_captures[s.capture_cursor++%s.issued_captures.size()]=c;if(c.generation==s.capture_generation&&c.enabled&&s.capture_floor_qpc==0)s.capture_floor_qpc=c.sent_qpc;return ok;}
  return fault;
 }
 if(e.kind!=TrafficVisualEventKind::frame||h.sequence<=s.wire_sequence)return fault;s.wire_sequence=h.sequence;
 if(h.kind==w::Kind::heartbeat){w::Heartbeat b;if(!w::decode(p,h.payload_size,b)||b.sender!=v.plugin.instance||b.connection!=v.connection||b.generated_qpc>e.received_qpc)return fault;v.heartbeat_qpc=e.received_qpc;v.plugin.capabilities=b.capabilities;if(b.reason==w::Reason::model_fault||b.reason==w::Reason::native_fault||b.reason==w::Reason::binding)release(b.reason,now);return ok;}
 if(h.kind==w::Kind::ack||h.kind==w::Kind::report){w::Report r;if(h.kind==w::Kind::ack){w::Ack a;if(!w::decode(p,h.payload_size,a))return fault;r=a;}else if(!w::decode(p,h.payload_size,r))return fault;
  if(r.plugin!=v.plugin.instance||r.connection!=v.connection||r.generated_qpc>e.received_qpc)return fault;v.heartbeat_qpc=e.received_qpc;
  if(r.phase==w::Phase::observing&&!r.request&&!r.lease&&!psx::motion::valid(r.owner)&&!r.submitted_mask){if(v.requested)return old;v.phase=r.phase;v.reason=r.reason;return ok;}
  const auto* c=issued(r);if(!c||(r.source_sequence&&r.source_observed_qpc<=c->source_floor_qpc))return fault;
  if(r.source_sequence){const auto* b=issued(r.lease,r.source_sequence,r.source_observed_qpc);if(!b||b->source.producer!=r.source_producer||b->source.connection_id!=r.source_connection||b->source.selection_epoch!=r.source_selection||b->source.snapshot.header.lifecycle!=r.lifecycle||(r.drawable_mask&~drawable(b->source.snapshot)))return fault;}
  if(r.lease!=v.lease||r.owner!=v.owner||((!v.following||s.binding.action==w::Action::release)&&c->action!=w::Action::release))return old;
  if(c->action==w::Action::release&&r.phase!=w::Phase::released&&r.phase!=w::Phase::rejected&&r.phase!=w::Phase::observing)return fault;
  if(h.kind==w::Kind::ack){if(c->action==w::Action::release){release(w::Reason::released,now);return ok;}if(r.phase==w::Phase::rejected||r.phase==w::Phase::released){release(r.reason,now);s.cooldown_qpc=now+static_cast<std::int64_t>(s.frequency/4);return ok;}v.pending=r.phase!=w::Phase::owned;v.owned=r.phase==w::Phase::owned;v.phase=r.phase;v.reason=r.reason;return ok;}
  const bool same_lease=v.report_retained&&r.connection==v.report.connection&&r.lease==v.report.lease;
  if(same_lease&&r.generated_qpc<=v.report.generated_qpc)return old;
  if(same_lease&&r.resource_generation==v.report.resource_generation&&r.sdk_sequence<v.report.sdk_sequence)return old;
  for(const auto& retired:s.retired_resources)if(r.resource_generation&&retired.connection==r.connection&&retired.lease==r.lease&&retired.generation==r.resource_generation)return old;
  if(same_lease&&v.report.resource_generation&&r.resource_generation!=v.report.resource_generation)s.retired_resources[s.resource_cursor++%s.retired_resources.size()]={r.connection,r.lease,v.report.resource_generation};
  v.report=r;v.report_retained=true;v.report_received_qpc=e.received_qpc;v.report_admitted_qpc=now;v.phase=r.phase;v.reason=r.reason;v.owned=r.phase==w::Phase::owned;v.pending=r.phase==w::Phase::pending;v.drawable_mask=r.drawable_mask;v.submitted_mask=r.submitted_mask;v.held_mask=r.held_mask;v.hidden_mask=r.hidden_mask;
  if(r.phase==w::Phase::rejected||r.phase==w::Phase::released){release(r.reason,now);s.cooldown_qpc=now+static_cast<std::int64_t>(s.frequency/4);}return ok;
 }
 if(h.kind==w::Kind::detail){w::Detail d;if(!w::decode(p,h.payload_size,d)||d.plugin!=v.plugin.instance||d.connection!=v.connection||d.generated_qpc>e.received_qpc||d.applied_qpc>e.received_qpc)return fault;
  if(!s.capture_enabled||d.capture_generation!=s.capture_generation||d.generated_qpc<s.capture_start_qpc||!s.capture_floor_qpc||d.generated_qpc<s.capture_floor_qpc)return old;
  w::Report scope;scope.plugin=d.plugin;scope.owner=d.owner;scope.connection=d.connection;scope.lease=d.lease;scope.request=d.request;scope.source_producer=d.source_producer;scope.source_connection=d.source_connection;scope.source_selection=d.source_selection;scope.lifecycle=d.lifecycle;scope.model=d.model;scope.profile=d.profile;
  const auto* c=issued(scope);if(!c)return fault;if(d.lease!=v.lease||d.owner!=v.owner||((!v.following||s.binding.action==w::Action::release)&&c->action!=w::Action::release))return old;
  if(c->action==w::Action::release&&(d.flags&(w::detail_pose_valid|w::detail_submitted|w::detail_visible)))return fault;
  if(d.delay_ms!=c->delay_ms||d.presentation_mode!=c->presentation_mode)return fault;
  if(d.flags&w::detail_pose_valid){const auto* b=issued(d.lease,d.source_sequence,d.source_observed_qpc);const auto* l=issued(d.lease,d.left_sequence,d.left_observed_qpc);const auto* r=issued(d.lease,d.right_sequence,d.right_observed_qpc);if(!b||!l||!r||d.slot>=7||!same_domain(b->source,l->source)||!same_domain(b->source,r->source)||b->source.snapshot.header.frame!=d.source_frame||l->source.snapshot.header.frame!=d.left_frame||r->source.snapshot.header.frame!=d.right_frame||b->source.snapshot.slots[d.slot].generation!=d.generation||l->source.snapshot.slots[d.slot].generation!=d.generation||r->source.snapshot.slots[d.slot].generation!=d.generation)return fault;}
  for(const auto& retired:s.retired_resources)if(d.resource_generation&&retired.connection==d.connection&&retired.lease==d.lease&&retired.generation==d.resource_generation)return old;
  const auto& last=v.details[d.slot];if(d.sdk_sequence<=last.sdk_sequence&&last.capture_generation==d.capture_generation&&last.connection==d.connection&&last.lease==d.lease&&last.resource_generation==d.resource_generation)return old;v.details[d.slot]=d;v.detail_received_qpc[d.slot]=e.received_qpc;v.detail_dropped=(std::max)(v.detail_dropped,d.dropped);if(d.flags&w::detail_incomplete)v.detail_dropped=(std::max)(v.detail_dropped,std::uint64_t{1});return ok;
 }
 return fault;
}
bool TrafficVisualModel::next(w::Frame& frame,std::int64_t now) noexcept {
 auto& s=*state_;auto& v=s.view;if(!s.connected)return false;tick(now);
 if(s.capture_dirty){w::Capture c{v.plugin.instance,v.connection,s.capture_generation?s.capture_generation:1,now,s.capture_enabled?1u:0u,2000};if(!w::encode(c,1,frame))return false;s.planned_captures[s.planned_capture_cursor++%s.planned_captures.size()]=c;s.capture_dirty=false;s.last_capture_qpc=now;return true;}
 if(s.size){const auto c=s.commands[s.head];if(!w::encode(c,1,frame))return false;s.head=(s.head+1)%s.commands.size();--s.size;return true;}
 if(s.source_dirty&&v.following&&v.requested&&s.binding.action!=w::Action::release&&fresh(now)&&s.source.sample.snapshot.header.observed>static_cast<std::uint64_t>(s.binding.source_floor_qpc)){w::Bundle b{v.plugin.instance,v.owner,v.connection,v.lease,s.source.sample};if(!w::encode(b,1,frame))return false;s.planned_sources[s.planned_source_cursor++%s.planned_sources.size()]=frame;s.source_dirty=false;return true;}return false;
}
bool TrafficVisualModel::restore(const State& state,std::int64_t now) noexcept {
 if(state.frequency!=state_->frequency||!state.frequency||state.frequency>1000000000000ULL||state.view.owner!=state_->view.owner||!psx::motion::valid(state.view.owner)||now<=0||state.size>state.commands.size()||state.head>=state.commands.size()||state.source_cursor>UINT64_MAX-state.issued_sources.size()||state.planned_source_cursor>UINT64_MAX-state.planned_sources.size()||!psx::motion::valid(state.view.policy)||state.view.lease>state.lease_counter||state.view.request>state.request_counter||state.wire_sequence>UINT32_MAX)return false;
 if(state.connected&&(state.view.process!=state.view.selected||state.view.plugin.pid!=state.view.process.pid||state.view.plugin.process_creation_filetime!=state.view.process.creation_filetime||state.view.plugin.qpc_frequency!=state.frequency||state.view.heartbeat_qpc>now))return false;
 if(state.source.retained&&(!traffic_wire::valid(state.source.sample)||state.source.received_qpc<=0||state.source.received_qpc>state.source.admitted_qpc||state.source.admitted_qpc>now||state.source.sample.snapshot.header.finished>static_cast<std::uint64_t>(state.source.received_qpc)))return false;
 if(state.source.available&&(!state.source.retained||!state.source.healthy||state.source.reason!=traffic_wire::Reason::none))return false;
 const auto& v=state.view;for(const auto clock:{v.source_floor_qpc,v.heartbeat_qpc,v.renewed_qpc,v.report_received_qpc,v.report_admitted_qpc,state.last_capture_qpc,state.capture_start_qpc,state.capture_floor_qpc})if(clock<0||clock>now)return false;
 if(v.report_received_qpc>v.report_admitted_qpc||state.capture_floor_qpc&&(state.capture_floor_qpc<state.capture_start_qpc||!state.capture_enabled))return false;
 const auto control_ok=[&](const w::Claim& c){return !c.request||(w::valid(c)&&c.owner==v.owner&&c.frequency==state.frequency&&c.request<=state.request_counter&&c.lease<=state.lease_counter&&c.source_floor_qpc<=now);};
 if(!control_ok(state.binding)||(state.binding.request&&(state.binding.connection!=v.connection||state.binding.plugin!=v.plugin.instance||state.binding.lease!=v.lease||state.binding.request!=v.request||state.binding.source_floor_qpc!=v.source_floor_qpc)))return false;
 for(const auto& c:state.issued_controls)if(!control_ok(c))return false;for(const auto& c:state.planned_controls)if(!control_ok(c))return false;
 for(std::size_t i=0;i<state.size;++i){const auto& c=state.commands[(state.head+i)%state.commands.size()];if(!c.request||!control_ok(c)||!same_control(c,state.planned_controls[(c.request-1)%state.planned_controls.size()]))return false;}
 for(const auto& b:state.issued_sources)if(b.lease&&(!w::valid(b)||b.owner!=v.owner||b.lease>state.lease_counter||b.source.snapshot.header.frequency!=state.frequency||b.source.snapshot.header.finished>static_cast<std::uint64_t>(now)))return false;
 for(const auto& f:state.planned_sources)if(f.size){w::Header h;w::Bundle b;if(f.size>w::max_frame||!w::read_header(f.bytes.data(),f.size,h)||h.kind!=w::Kind::bundle||f.size!=w::header_size+h.payload_size||!w::decode(f.bytes.data()+w::header_size,h.payload_size,b)||b.owner!=v.owner||b.lease>state.lease_counter||b.source.snapshot.header.finished>static_cast<std::uint64_t>(now))return false;}
 for(const auto& c:state.issued_captures)if(c.generation){w::Frame f;if(!w::encode(c,1,f)||c.sent_qpc>now)return false;}for(const auto& c:state.planned_captures)if(c.generation){w::Frame f;if(!w::encode(c,1,f)||c.sent_qpc>now)return false;}
 if(v.report_retained&&(!w::valid(v.report)||v.report.generated_qpc>v.report_received_qpc||v.report.applied_qpc>v.report_received_qpc))return false;
 for(std::size_t i=0;i<7;++i)if(v.details[i].sdk_sequence){w::Frame f;if(!w::encode(v.details[i],1,f)||v.detail_received_qpc[i]>now||v.details[i].generated_qpc>v.detail_received_qpc[i])return false;}
 if(!state.connected&&(v.communication_healthy||v.requested||v.pending||v.owned||v.report_current||state.size))return false;
 if(v.report_current&&(!v.following||!v.communication_healthy||!v.owned||!v.report_retained||v.report.connection!=v.connection||v.report.lease!=v.lease||!v.report.applied_qpc||!v.report.resource_generation||v.report.source_observed_qpc<=0||now-v.report.source_observed_qpc>=static_cast<std::int64_t>(state.frequency)||v.presentation_age_qpc!=now-v.report.source_observed_qpc))return false;
 *state_=state;return true;
}
}
