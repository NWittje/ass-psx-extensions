#include "coordinator/traffic.hpp"
#include <limits>
#include <cstring>
namespace coordinator {
namespace w=traffic_wire;
namespace {
bool process(ProcessIdentity p) noexcept {return (p.pid!=0)==(p.creation_filetime!=0);}
bool invalidates(w::Reason r) noexcept {return r!=w::Reason::none&&r!=w::Reason::awaiting_source&&r!=w::Reason::loading&&r!=w::Reason::stale;}
}
const char* traffic_reason_name(w::Reason r) noexcept {
 constexpr const char* names[]{"Qualified source","Unsupported traffic","Awaiting fresh source","Loading","Source stale","Identity","Lifecycle","Native fault","Disconnected","Transport timeout","Protocol fault","Ingress overflow","Incompatible traffic"};
 const auto i=static_cast<unsigned>(r);return i<std::size(names)?names[i]:"Invalid reason";
}
void TrafficModel::configure(std::uint64_t frequency) noexcept {
 frequency_=frequency&&frequency<=1000000000000ULL?frequency:0;selected_={};peer_={};view_={};
}
void TrafficModel::tick(std::int64_t now) noexcept {
 const auto before_health=view_.communication_healthy,before_available=view_.source_available;
 const auto before_reason=view_.reason;
 auto& p=peer_;auto& v=view_;v.selected=selected_;v.process=p.process;v.producer=p.hello.instance;
 v.connection_id=p.connection;v.selection_epoch=p.selection;v.capabilities=p.capabilities;v.error=p.error;v.heartbeat_qpc=p.heartbeat;
 v.status_received_qpc=p.status_received;v.status_frame=p.status_frame;
 v.retained=p.have_snapshot;v.lifecycle=p.lifecycle;v.sequence=p.source_sequence;
 v.observed_qpc=p.have_snapshot?static_cast<std::int64_t>(p.snapshot.header.observed):0;
 v.received_qpc=p.have_snapshot?p.received:0;v.admitted_qpc=p.have_snapshot?p.admitted:0;
 v.source_age_qpc=p.have_snapshot&&now>=v.observed_qpc?now-v.observed_qpc:-1;
 v.communication_healthy=p.connected&&frequency_&&now>=p.heartbeat&&static_cast<std::uint64_t>(now-p.heartbeat)<frequency_*5;
 v.source_available=false;v.active_slots=v.pose_valid_slots=0;
 if(p.have_snapshot){v.source=p.snapshot;for(const auto& slot:p.snapshot.slots){if(slot.flags&16u)++v.active_slots;if((slot.valid&63u)==63u)++v.pose_valid_slots;}}else v.source={};
 if(!v.communication_healthy)v.reason=p.connected?w::Reason::timeout:p.reason;
 else if(p.process!=selected_)v.reason=w::Reason::identity;
 else if(invalidates(p.reason))v.reason=p.reason;
 else if(!(p.capabilities&1u))v.reason=w::Reason::unsupported;
 else if(p.reason==w::Reason::stale||(p.have_snapshot&&(v.source_age_qpc<0||static_cast<std::uint64_t>(v.source_age_qpc)>=frequency_)))v.reason=w::Reason::stale;
 else if(p.reason==w::Reason::loading||(p.have_snapshot&&(p.snapshot.header.flags&8u)))v.reason=w::Reason::loading;
 else if(!p.have_snapshot)v.reason=w::Reason::awaiting_source;
 else {v.reason=w::Reason::none;v.source_available=true;}
 if(before_health!=v.communication_healthy||before_available!=v.source_available||before_reason!=v.reason)++v.revision;
}
TrafficAdmission TrafficModel::event(const TrafficEvent& e,std::int64_t now) noexcept {
 auto& p=peer_;
 const auto fault=[&](){++view_.faults;p.connected=false;p.have_snapshot=false;p.reason=w::Reason::protocol;p.error=e.error;tick(now);return TrafficAdmission::fault;};
 const auto obsolete=[&](){++view_.obsolete;return TrafficAdmission::obsolete;};
 if(!frequency_||e.received_qpc<=0||now<e.received_qpc||!process(e.process)||static_cast<unsigned>(e.kind)>3)return fault();
 if(e.kind==TrafficEventKind::selection){
  if(selected_!=e.process){selected_=e.process;p.connected=false;p.have_snapshot=false;p.reason=selected_.pid?w::Reason::awaiting_source:w::Reason::disconnected;++view_.revision;}
 }else if(e.kind==TrafficEventKind::hello){
  w::Frame f;
  if(!e.process.pid||e.process!=selected_||!w::encode(e.hello,1,f)||e.hello.pid!=e.process.pid||
   e.hello.process_creation_filetime!=e.process.creation_filetime||e.hello.qpc_frequency!=frequency_||
   !e.connection_id||!e.selection_epoch||e.subscribe_floor_qpc<=0||e.subscribe_floor_qpc>e.received_qpc)return fault();
  if(p.connection&&e.connection_id<=p.connection)return obsolete();
  if(p.selection&&e.selection_epoch<p.selection)return obsolete();
  const bool same=p.process==e.process&&p.hello.instance==e.hello.instance;
  const auto old_sequence=same?p.source_sequence:0,old_lifecycle=same?p.lifecycle:0;
  p={};p.connected=true;p.process=e.process;p.hello=e.hello;p.connection=e.connection_id;p.selection=e.selection_epoch;
  p.floor=p.source_floor=e.subscribe_floor_qpc;p.heartbeat=e.received_qpc;p.capabilities=e.hello.capabilities;
  p.source_sequence=old_sequence;p.lifecycle=old_lifecycle;p.reason=w::Reason::awaiting_source;++view_.revision;
 }else if(e.kind==TrafficEventKind::lost){
  if(e.connection_id&&p.connection&&e.connection_id!=p.connection)return obsolete();
  if(e.process.pid&&p.process.pid&&e.process!=p.process)return obsolete();
  if(e.selection_epoch&&p.selection&&e.selection_epoch!=p.selection)return obsolete();
  if(static_cast<unsigned>(e.reason)>12)return fault();
  p.connected=false;p.have_snapshot=false;p.reason=e.reason;p.error=e.error;
 }else {
  if(!p.connected||e.process!=p.process||e.process!=selected_||e.connection_id!=p.connection||e.selection_epoch!=p.selection)return obsolete();
  w::Header h;if(e.frame.size<w::header_size||!w::read_header(e.frame.bytes.data(),w::header_size,h)||
   e.frame.size!=w::header_size+h.payload_size||h.sequence<=p.wire_sequence)return fault();
  const auto* bytes=e.frame.bytes.data()+w::header_size;
  if(h.kind==w::Kind::sample){
   w::Sample sample;if(!w::decode(bytes,h.payload_size,sample)||sample.producer!=p.hello.instance||sample.connection_id!=p.connection||sample.selection_epoch!=p.selection||
    sample.snapshot.header.process!=p.process.pid||sample.snapshot.header.process_created!=p.process.creation_filetime||sample.snapshot.header.frequency!=frequency_||
    sample.snapshot.header.finished>static_cast<std::uint64_t>(e.received_qpc))return fault();
   p.wire_sequence=h.sequence;p.heartbeat=e.received_qpc;
   const auto& source=sample.snapshot.header;
   if(!(p.capabilities&1u)||invalidates(p.reason)||source.observed<=static_cast<std::uint64_t>(p.source_floor)||
    source.sequence<=p.source_sequence||source.lifecycle<p.lifecycle||(p.have_snapshot&&source.observed<p.snapshot.header.observed))return obsolete();
   if(p.have_snapshot&&source.lifecycle==p.lifecycle){for(std::size_t i=0;i<7;++i)if(p.snapshot.slots[i].generation!=sample.snapshot.slots[i].generation)++view_.generation_changes;}
   if(source.lifecycle!=p.lifecycle)++view_.revision;
   p.snapshot=sample.snapshot;p.source_sequence=source.sequence;p.lifecycle=source.lifecycle;p.have_snapshot=true;
   p.received=e.received_qpc;p.admitted=now;p.reason=source.flags&8u?w::Reason::loading:w::Reason::none;
  }else if(h.kind==w::Kind::heartbeat||h.kind==w::Kind::status){
   w::Heartbeat value;
   if(h.kind==w::Kind::heartbeat){if(!w::decode(bytes,h.payload_size,value))return fault();}
   else {w::Status status;if(!w::decode(bytes,h.payload_size,status))return fault();value={status.producer,status.connection_id,status.selection_epoch,status.generated_qpc,status.capabilities,status.reason};}
   if(value.producer!=p.hello.instance||value.connection_id!=p.connection||value.selection_epoch!=p.selection||
    value.generated_qpc<p.floor||value.generated_qpc>e.received_qpc)return fault();
   if(h.kind==w::Kind::status){p.status_frame=e.frame;p.status_received=e.received_qpc;}
   if(!(value.capabilities&1u)||invalidates(value.reason)||(!(p.capabilities&1u)&&(value.capabilities&1u))){p.have_snapshot=false;p.source_floor=value.generated_qpc;}
   p.capabilities=value.capabilities;p.reason=value.reason;p.heartbeat=e.received_qpc;p.wire_sequence=h.sequence;
  }else return fault();
 }
 ++view_.accepted;tick(now);return TrafficAdmission::accepted;
}
void TrafficModel::disconnect(std::int64_t now) noexcept {peer_.connected=peer_.have_snapshot=false;peer_.reason=w::Reason::disconnected;tick(now);}
bool TrafficModel::restore(const Checkpoint& c,std::int64_t now) noexcept {
 if(!frequency_||now<=0||!process(c.selected)||!process(c.peer.process))return false;
 const auto& p=c.peer;
 if(p.connection){w::Frame f;if(!w::encode(p.hello,1,f)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=frequency_||
  !p.selection||p.floor<=0||p.source_floor<p.floor||p.source_floor>now||p.heartbeat<p.floor||p.heartbeat>now||p.capabilities&~1u||static_cast<unsigned>(p.reason)>12)return false;
 }else if(p.connected||p.have_snapshot||p.selection||p.source_sequence||p.lifecycle)return false;
 if(p.connected&&p.process!=c.selected)return false;
 if(p.status_frame.size){w::Header h;w::Status status;if(p.status_received<=0||p.status_received>now||!w::read_header(p.status_frame.bytes.data(),16,h)||h.kind!=w::Kind::status||p.status_frame.size!=16+h.payload_size||
  !w::decode(p.status_frame.bytes.data()+16,h.payload_size,status)||status.producer!=p.hello.instance||status.connection_id!=p.connection||status.selection_epoch!=p.selection||status.generated_qpc<p.floor||status.generated_qpc>p.status_received)return false;}
 else if(p.status_received)return false;
 if(p.have_snapshot&&(!w::valid(p.snapshot)||p.snapshot.header.sequence!=p.source_sequence||p.snapshot.header.lifecycle!=p.lifecycle||
  p.snapshot.header.process!=p.process.pid||p.snapshot.header.process_created!=p.process.creation_filetime||p.snapshot.header.frequency!=frequency_||
  p.snapshot.header.observed<=static_cast<std::uint64_t>(p.source_floor)||p.snapshot.header.finished>static_cast<std::uint64_t>(p.received)||p.received>p.admitted||p.admitted>now))return false;
 TrafficModel check;check.configure(frequency_);check.selected_=c.selected;check.peer_=p;check.view_=c.view;check.tick(now);
 const auto& v=check.view_;
 if(v.communication_healthy!=c.view.communication_healthy||v.source_available!=c.view.source_available||v.reason!=c.view.reason||v.retained!=c.view.retained||
  v.selected!=c.view.selected||v.process!=c.view.process||v.producer!=c.view.producer||v.connection_id!=c.view.connection_id||v.selection_epoch!=c.view.selection_epoch||
  v.capabilities!=c.view.capabilities||v.lifecycle!=c.view.lifecycle||v.sequence!=c.view.sequence||v.observed_qpc!=c.view.observed_qpc||v.received_qpc!=c.view.received_qpc||
  v.admitted_qpc!=c.view.admitted_qpc||v.source_age_qpc!=c.view.source_age_qpc||v.active_slots!=c.view.active_slots||v.pose_valid_slots!=c.view.pose_valid_slots||v.heartbeat_qpc!=c.view.heartbeat_qpc||v.error!=c.view.error||
  std::memcmp(&v.source,&c.view.source,sizeof(v.source))||v.status_received_qpc!=c.view.status_received_qpc||v.status_frame.size!=c.view.status_frame.size||
  v.status_frame.size>w::max_frame||!std::equal(v.status_frame.bytes.begin(),v.status_frame.bytes.begin()+v.status_frame.size,c.view.status_frame.bytes.begin()))return false;
 selected_=c.selected;peer_=p;view_=v;return true;
}
}
