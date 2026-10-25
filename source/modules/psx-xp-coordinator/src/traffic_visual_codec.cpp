#include "traffic_visual_codec.hpp"
#include <algorithm>
#include <type_traits>
namespace coordinator::traffic_visual_capture {
namespace {
constexpr std::size_t max_bytes=1024*1024;
struct Codec {
 std::vector<std::uint8_t>& bytes;bool reading{},ok{true};std::size_t pos{};
 template<class T> void number(T& value){
  std::uint64_t raw=static_cast<std::uint64_t>(value);
  if(reading){if(bytes.size()-pos<8){ok=false;return;}raw=0;for(unsigned i=0;i<8;++i)raw|=std::uint64_t(bytes[pos++])<<(8*i);
   if constexpr(!std::is_const_v<T>){using V=std::remove_cv_t<T>;const auto parsed=static_cast<V>(raw);if(static_cast<std::uint64_t>(parsed)!=raw)ok=false;value=parsed;}
  }else{if(bytes.size()>max_bytes-8){ok=false;return;}for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<std::uint8_t>(raw>>(i*8)));}
 }
 template<class T> void instance(T& v){number(v.high);number(v.low);}
 template<class T> void process(T& v){number(v.pid);number(v.creation_filetime);}
 template<class T> void policy(T& v){number(v.mode);number(v.delay_ms);if(!psx::motion::valid(v))ok=false;}
 template<class T> void frame(T& f){
  std::uint64_t count=f.size;number(count);if(count>visual_wire::max_frame){ok=false;return;}
  if(reading){if(count>bytes.size()-pos){ok=false;return;}if constexpr(!std::is_const_v<T>){f={};f.size=static_cast<std::size_t>(count);std::copy(bytes.begin()+pos,bytes.begin()+pos+f.size,f.bytes.begin());}pos+=static_cast<std::size_t>(count);}
  else bytes.insert(bytes.end(),f.bytes.begin(),f.bytes.begin()+f.size);
 }
 template<class T> void native_frame(T& f){
  std::uint64_t count=f.size;number(count);if(count>traffic_wire::max_frame){ok=false;return;}
  if(reading){if(count>bytes.size()-pos){ok=false;return;}if constexpr(!std::is_const_v<T>){f={};f.size=static_cast<std::size_t>(count);std::copy(bytes.begin()+pos,bytes.begin()+pos+f.size,f.bytes.begin());}pos+=static_cast<std::size_t>(count);}
  else bytes.insert(bytes.end(),f.bytes.begin(),f.bytes.begin()+f.size);
 }
 template<class T> void typed(T& v,bool present,visual_wire::Kind kind){
  number(present);visual_wire::Frame f;
  if(!reading&&present&&!visual_wire::encode(v,1,f))ok=false;
  frame(f);if(reading){if constexpr(!std::is_const_v<T>){v={};if(present){visual_wire::Header h;if(!visual_wire::read_header(f.bytes.data(),f.size,h)||h.kind!=kind||f.size!=visual_wire::header_size+h.payload_size||!visual_wire::decode(f.bytes.data()+visual_wire::header_size,h.payload_size,v))ok=false;}else if(f.size)ok=false;}}
 }
 template<class T> void source(T& v){
  number(v.healthy);number(v.available);number(v.retained);number(v.reason);number(v.received_qpc);number(v.admitted_qpc);
  bool present=v.sample.snapshot.header.sequence!=0;number(present);traffic_wire::Frame f;
  if(!reading&&present&&!traffic_wire::encode(v.sample,1,f))ok=false;native_frame(f);
  if(reading){if constexpr(!std::is_const_v<T>){v.sample={};if(present){traffic_wire::Header h;if(!traffic_wire::read_header(f.bytes.data(),f.size,h)||h.kind!=traffic_wire::Kind::sample||f.size!=traffic_wire::header_size+h.payload_size||!traffic_wire::decode(f.bytes.data()+traffic_wire::header_size,h.payload_size,v.sample))ok=false;}else if(f.size)ok=false;}}
  if(static_cast<unsigned>(v.reason)>12)ok=false;
 }
 template<class T> void view(T& v){
  number(v.following);number(v.communication_healthy);number(v.requested);number(v.pending);number(v.owned);number(v.report_retained);number(v.report_current);
  process(v.selected);process(v.process);typed(v.plugin,psx::motion::valid(v.plugin.instance),visual_wire::Kind::hello);instance(v.owner);
  number(v.connection);number(v.lease);number(v.request);number(v.sent_sequence);number(v.accepted);number(v.obsolete);number(v.faults);number(v.overflows);number(v.boundaries);number(v.revision);
  number(v.phase);number(v.reason);policy(v.policy);number(v.error);number(v.drawable_mask);number(v.submitted_mask);number(v.held_mask);number(v.hidden_mask);
  number(v.source_floor_qpc);number(v.heartbeat_qpc);number(v.renewed_qpc);number(v.report_received_qpc);number(v.report_admitted_qpc);number(v.presentation_age_qpc);
  typed(v.report,v.report_retained,visual_wire::Kind::report);number(v.capture_generation);number(v.detail_dropped);
  for(auto& d:v.details)typed(d,d.sdk_sequence!=0,visual_wire::Kind::detail);
  for(auto& receipt:v.detail_received_qpc)number(receipt);
  if(static_cast<unsigned>(v.phase)>4||static_cast<unsigned>(v.reason)>17||(v.drawable_mask|v.submitted_mask|v.held_mask|v.hidden_mask)&~127u)ok=false;
 }
 template<class T> void event(T& e){
  number(e.kind);process(e.process);number(e.connection);number(e.received_qpc);number(e.error);number(e.reason);
  typed(e.hello,psx::motion::valid(e.hello.instance),visual_wire::Kind::hello);frame(e.frame);source(e.source);number(e.enabled);policy(e.policy);number(e.capture_generation);
  if(static_cast<unsigned>(e.kind)>9||static_cast<unsigned>(e.reason)>17)ok=false;
 }
 template<class T> void state(T& s){
  number(s.frequency);view(s.view);source(s.source);number(s.connected);number(s.source_dirty);number(s.capture_dirty);number(s.capture_enabled);
  number(s.wire_sequence);number(s.request_counter);number(s.lease_counter);number(s.capture_generation);
  number(s.cooldown_qpc);number(s.last_capture_qpc);number(s.capture_start_qpc);number(s.capture_floor_qpc);
  typed(s.binding,s.binding.request!=0,visual_wire::Kind::claim);for(auto& c:s.commands)typed(c,c.request!=0,visual_wire::Kind::claim);number(s.head);number(s.size);
  for(auto& c:s.issued_controls)typed(c,c.request!=0,visual_wire::Kind::claim);
  for(auto& c:s.planned_controls)typed(c,c.request!=0,visual_wire::Kind::claim);
  for(auto& f:s.planned_sources)frame(f);number(s.planned_source_cursor);
  for(auto& b:s.issued_sources)typed(b,b.lease!=0,visual_wire::Kind::bundle);number(s.source_cursor);
  for(auto& c:s.issued_captures)typed(c,c.generation!=0,visual_wire::Kind::capture);number(s.capture_cursor);
  for(auto& c:s.planned_captures)typed(c,c.generation!=0,visual_wire::Kind::capture);number(s.planned_capture_cursor);
  for(auto& r:s.retired_resources){number(r.connection);number(r.lease);number(r.generation);}number(s.resource_cursor);
 }
};
template<class T> bool write(const T& v,std::vector<std::uint8_t>& bytes){bytes.clear();Codec c{bytes};if constexpr(std::is_same_v<T,TrafficVisualView>)c.view(v);else if constexpr(std::is_same_v<T,TrafficVisualEvent>)c.event(v);else c.state(v);return c.ok&&bytes.size()<=max_bytes;}
template<class T> bool read(const std::vector<std::uint8_t>& bytes,T& value){if(bytes.size()>max_bytes)return false;auto copy=bytes;Codec c{copy,true};if constexpr(std::is_same_v<T,TrafficVisualView>)c.view(value);else if constexpr(std::is_same_v<T,TrafficVisualEvent>)c.event(value);else c.state(value);return c.ok&&c.pos==bytes.size();}
}
bool encode(const TrafficVisualModel::State& v,std::vector<std::uint8_t>& b){return write(v,b);}
bool encode(const TrafficVisualView& v,std::vector<std::uint8_t>& b){return write(v,b);}
bool encode(const TrafficVisualEvent& v,std::vector<std::uint8_t>& b){return write(v,b);}
bool decode(const std::vector<std::uint8_t>& b,TrafficVisualModel::State& v){return read(b,v);}
bool decode(const std::vector<std::uint8_t>& b,TrafficVisualView& v){return read(b,v);}
bool decode(const std::vector<std::uint8_t>& b,TrafficVisualEvent& v){return read(b,v);}
bool equal(const TrafficVisualView& a,const TrafficVisualView& b){std::vector<std::uint8_t> x,y;return encode(a,x)&&encode(b,y)&&x==y;}
}
