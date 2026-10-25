#include <psx/status/protocol.hpp>
#include <algorithm>
#include <cstring>
namespace psx::status {
namespace {
struct Writer {std::uint8_t* data;std::size_t size,pos{};bool ok{true};void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(i*8));}};
struct Reader {const std::uint8_t* data;std::size_t size,pos{};bool ok{true};std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(i*8);return v;}bool end() const noexcept {return ok&&pos==size;}};
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
bool role(Role r) noexcept {return r==Role::plugin||r==Role::exporter;}bool reason(Reason r) noexcept {return static_cast<unsigned>(r)<=9;}
bool valid(const Hello& v) noexcept {return role(v.role)&&motion::valid(v.instance)&&v.pid&&!(v.capabilities&~(v.role==Role::plugin?2u:1u))&&v.process_creation_filetime&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.instance)&&v.connection_id&&v.pid&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Heartbeat& v) noexcept {return motion::valid(v.producer)&&v.connection_id&&v.generated_qpc>0&&!(v.capabilities&~3u)&&reason(v.reason);}
#define P(x,n) w.u(static_cast<std::uint64_t>(v.x),n)
#define G(x,n) v.x=static_cast<decltype(v.x)>(r.u(n))
void put(Writer& w,const Hello& v) noexcept {P(role,4);put(w,v.instance);P(pid,4);P(capabilities,4);P(process_creation_filetime,8);P(qpc_frequency,8);}
void get(Reader& r,Hello& v) noexcept {G(role,4);get(r,v.instance);G(pid,4);G(capabilities,4);G(process_creation_filetime,8);G(qpc_frequency,8);}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.instance);P(connection_id,8);P(qpc_frequency,8);P(pid,4);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.instance);G(connection_id,8);G(qpc_frequency,8);G(pid,4);}
void put(Writer& w,const Heartbeat& v) noexcept {put(w,v.producer);P(connection_id,8);P(generated_qpc,8);P(capabilities,4);P(reason,4);}
void get(Reader& r,Heartbeat& v) noexcept {get(r,v.producer);G(connection_id,8);G(generated_qpc,8);G(capabilities,4);G(reason,4);}
#define SNAP_FIELDS(M) M(role,4);M(connection_id,8);M(sequence,8);M(lifecycle,8);M(enable_generation,8);M(aircraft_generation,8);M(scenery_generation,8);M(local_frame_generation,8);M(command_id,8);M(placement_attempt,8);M(target_epoch,8);M(target_sequence,8);M(callback_count,8);M(observed_qpc,8);M(phase_start_qpc,8);M(call_returned_qpc,8);M(notification_qpc,8);M(flags,4);M(notification_kind,4);M(target_source,4);M(reason,4)
void put(Writer& w,const Snapshot& v) noexcept {put(w,v.producer);SNAP_FIELDS(P);put(w,v.target_producer);w.u(v.selection_epoch,8);}
void get(Reader& r,Snapshot& v) noexcept {get(r,v.producer);SNAP_FIELDS(G);get(r,v.target_producer);v.selection_epoch=r.u(8);}
#undef SNAP_FIELDS
#undef P
#undef G
template<class T> bool pack(const T& v,Kind kind,std::uint32_t seq,Frame& f) noexcept {f.size=0;if(!seq||!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.u(0x31535850u,4);w.u(protocol_version,2);w.u(static_cast<unsigned>(kind),2);w.u(0,4);w.u(seq,4);put(w,v);if(!w.ok)return false;const auto n=w.pos-header_size;for(unsigned i=0;i<4;++i)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(i*8));f.size=w.pos;return true;}
template<class T> bool unpack(const std::uint8_t* p,std::size_t n,T& v) noexcept {T out{};Reader r{p,n};get(r,out);if(!r.end()||!valid(out))return false;v=out;return true;}
}
bool valid(const Snapshot& v) noexcept {
 if(!motion::valid(v.producer)||!role(v.role)||!v.connection_id||!v.sequence||!v.lifecycle||v.observed_qpc<=0||v.phase_start_qpc<0||v.call_returned_qpc<0||v.notification_qpc<0||!reason(v.reason)||v.target_source>2||(v.flags&~all_flags))return false;
 if((v.flags&async_busy)&&!(v.flags&async_available))return false;
 if((v.flags&(fatal_active|fatal_requested))&&!(v.flags&fatal_valid))return false;
 if((v.flags&(paused|motion_frozen))&&!(v.flags&pause_valid))return false;
 constexpr unsigned plugin_flags=qualified|aircraft_loaded|bindings_ready|readback_ready|placement_active|placement_returned|reconciling|async_available|async_busy|plugin_enabled|readback_required|reference_ready;
 constexpr unsigned exporter_flags=qualified|fatal_valid|fatal_active|fatal_requested|pause_valid|paused|motion_frozen;
 return v.role==Role::plugin ? !(v.flags&~plugin_flags) : !(v.flags&~exporter_flags);
}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out) noexcept {if(!p||n<header_size)return false;Reader r{p,header_size};if(r.u(4)!=0x31535850u||r.u(2)!=protocol_version)return false;Header h{};h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>4||!h.sequence||!h.payload_size||h.payload_size>max_payload)return false;out=h;return true;}
#define CODEC(T,K) bool encode(const T& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::K,s,f);}bool decode(const std::uint8_t* p,std::size_t n,T& v) noexcept {return unpack(p,n,v);}
CODEC(Hello,hello) CODEC(Subscribe,subscribe) CODEC(Snapshot,snapshot) CODEC(Heartbeat,heartbeat)
#undef CODEC
bool Framer::feed(const void* input,std::size_t n,FrameHandler handler,void* context) noexcept {if(failed_||(!input&&n)||!handler)return false;auto p=static_cast<const std::uint8_t*>(input);while(n){auto count=std::min(n,needed_-used_);std::memcpy(bytes_.data()+used_,p,count);used_+=count;p+=count;n-=count;if(used_!=needed_)continue;Header h{};if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}if(needed_==header_size){needed_=header_size+h.payload_size;continue;}if(h.sequence<=last_sequence_||!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}last_sequence_=h.sequence;used_=0;needed_=header_size;}return true;}
}
