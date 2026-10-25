#include <psx/traffic_source/protocol.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace psx::traffic_source {
namespace {
struct Writer {std::uint8_t* data;std::size_t size,pos{};bool ok{true};void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(i*8));}void d(double v) noexcept {std::uint64_t b;std::memcpy(&b,&v,8);u(b,8);}};
struct Reader {const std::uint8_t* data;std::size_t size,pos{};bool ok{true};std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(8*i);return v;}double d() noexcept {auto b=u(8);double v;std::memcpy(&v,&b,8);return v;}bool end() const noexcept {return ok&&pos==size;}};
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
bool frequency(std::uint64_t v) noexcept {return v&&v<=1000000000000ULL;}
bool reason(Reason v) noexcept {return static_cast<unsigned>(v)<=12;}
bool valid(const Hello& v) noexcept {return motion::valid(v.instance)&&v.pid&&v.process_creation_filetime&&frequency(v.qpc_frequency)&&!(v.capabilities&~1u);}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.instance)&&v.connection_id&&v.selection_epoch&&v.pid&&v.process_creation_filetime&&frequency(v.qpc_frequency);}
template<class T> bool state(const T& v) noexcept {return motion::valid(v.producer)&&v.connection_id&&v.selection_epoch&&v.generated_qpc>0&&!(v.capabilities&~1u)&&reason(v.reason);}
bool valid(const Heartbeat& v) noexcept {return state(v);}bool valid(const Status& v) noexcept {return state(v);}
#define P(x,n) w.u(static_cast<std::uint64_t>(v.x),n)
#define G(x,n) v.x=static_cast<decltype(v.x)>(r.u(n))
void put(Writer& w,const Hello& v) noexcept {put(w,v.instance);P(pid,4);P(capabilities,4);P(process_creation_filetime,8);P(qpc_frequency,8);}
void get(Reader& r,Hello& v) noexcept {get(r,v.instance);G(pid,4);G(capabilities,4);G(process_creation_filetime,8);G(qpc_frequency,8);}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.instance);P(connection_id,8);P(selection_epoch,8);P(qpc_frequency,8);P(pid,4);P(process_creation_filetime,8);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.instance);G(connection_id,8);G(selection_epoch,8);G(qpc_frequency,8);G(pid,4);G(process_creation_filetime,8);}
#define HEADER_FIELDS(M) M(abi,4);M(size,4);M(slots,4);M(anchor,4);M(sequence,8);M(lifecycle,8);M(frame,8);M(observed,8);M(finished,8);M(writer_version,8);M(flags,4);M(mode,4);M(process,4);M(producer,4);M(process_created,8);M(frequency,8);M(reserved_identity,8);M(native_delta,8)
void put(Writer& w,const traffic::Header& v) noexcept {HEADER_FIELDS(P);w.d(v.rate);P(metadata_valid,4);P(reserved,4);}
void get(Reader& r,traffic::Header& v) noexcept {HEADER_FIELDS(G);v.rate=r.d();G(metadata_valid,4);G(reserved,4);}
#undef HEADER_FIELDS
void put(Writer& w,const traffic::Slot& v) noexcept {P(generation,8);P(id,4);P(valid,4);P(flags,4);P(reserved,4);P(reserved_identity,8);for(auto value:v.values)w.d(value);}
void get(Reader& r,traffic::Slot& v) noexcept {G(generation,8);G(id,4);G(valid,4);G(flags,4);G(reserved,4);G(reserved_identity,8);for(auto& value:v.values)value=r.d();}
void put(Writer& w,const Sample& v) noexcept {put(w,v.producer);P(connection_id,8);P(selection_epoch,8);put(w,v.snapshot.header);for(const auto& slot:v.snapshot.slots)put(w,slot);}
void get(Reader& r,Sample& v) noexcept {get(r,v.producer);G(connection_id,8);G(selection_epoch,8);get(r,v.snapshot.header);for(auto& slot:v.snapshot.slots)get(r,slot);}
template<class T> void put_state(Writer& w,const T& v) noexcept {put(w,v.producer);P(connection_id,8);P(selection_epoch,8);P(generated_qpc,8);P(capabilities,4);P(reason,4);}
template<class T> void get_state(Reader& r,T& v) noexcept {get(r,v.producer);G(connection_id,8);G(selection_epoch,8);G(generated_qpc,8);G(capabilities,4);G(reason,4);}
void put(Writer& w,const Heartbeat& v) noexcept {put_state(w,v);}void get(Reader& r,Heartbeat& v) noexcept {get_state(r,v);}
void put(Writer& w,const Status& v) noexcept {put_state(w,v);P(drained,8);P(sent,8);P(sequence_skips,8);P(work_ticks,8);P(max_work_ticks,8);}void get(Reader& r,Status& v) noexcept {get_state(r,v);G(drained,8);G(sent,8);G(sequence_skips,8);G(work_ticks,8);G(max_work_ticks,8);}
#undef P
#undef G
template<class T> bool pack(const T& v,Kind kind,std::uint32_t seq,Frame& f) noexcept {f.size=0;if(!seq||!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.u(0x31465850u,4);w.u(protocol_version,2);w.u(static_cast<unsigned>(kind),2);w.u(0,4);w.u(seq,4);put(w,v);if(!w.ok||w.pos-header_size>max_payload)return false;const auto n=w.pos-header_size;for(unsigned i=0;i<4;++i)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(i*8));f.size=w.pos;return true;}
template<class T> bool unpack(const std::uint8_t* p,std::size_t n,T& v) noexcept {T out{};Reader r{p,n};get(r,out);if(!r.end()||!valid(out))return false;v=out;return true;}
}
bool valid(const traffic::Snapshot& v) noexcept {
 const auto& h=v.header;
 if(h.abi!=1||h.size!=912||h.slots!=7||h.anchor!=1||!h.sequence||!h.lifecycle||!h.frame||!h.process||h.producer!=1||!h.process_created||!frequency(h.frequency)||!h.observed||h.finished<h.observed||h.finished>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())||h.reserved_identity||h.reserved||h.metadata_valid>1||(h.flags&~15u)||(((h.flags&1u)!=0)==((h.flags&8u)!=0)))return false;
 if(h.metadata_valid&&(!std::isfinite(h.rate)||h.rate<0||h.native_delta<0))return false;
 for(std::size_t i=0;i<v.slots.size();++i){const auto& s=v.slots[i];if(s.id!=static_cast<std::int32_t>(i)||!s.generation||s.reserved||s.reserved_identity||(s.valid&~1023u)||(s.flags&~31u))return false;
  for(std::size_t j=0;j<10;++j)if((s.valid&(1u<<j))&&!std::isfinite(s.values[j]))return false;
  if((s.valid&1u)&&std::abs(s.values[0])>1.5707963267948966)return false;
  if((s.valid&2u)&&std::abs(s.values[1])>6.283185307179586)return false;
  if((s.valid&64u)&&s.values[6]<0)return false;
  if((s.flags&16u)&&(!(s.flags&1u)||h.mode==0||(h.flags&8u)))return false;
 }
 return true;
}
bool valid(const Sample& v) noexcept {return motion::valid(v.producer)&&v.connection_id&&v.selection_epoch&&valid(v.snapshot);}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out) noexcept {if(!p||n<header_size)return false;Reader r{p,header_size};if(r.u(4)!=0x31465850u||r.u(2)!=protocol_version)return false;Header h{};h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>5||!h.sequence||!h.payload_size||h.payload_size>max_payload)return false;out=h;return true;}
#define CODEC(T,K) bool encode(const T& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::K,s,f);}bool decode(const std::uint8_t* p,std::size_t n,T& v) noexcept {return unpack(p,n,v);}
CODEC(Hello,hello) CODEC(Subscribe,subscribe) CODEC(Sample,sample) CODEC(Heartbeat,heartbeat) CODEC(Status,status)
#undef CODEC
bool Framer::feed(const void* input,std::size_t n,FrameHandler handler,void* context) noexcept {if(failed_||(!input&&n)||!handler)return false;auto p=static_cast<const std::uint8_t*>(input);while(n){const auto count=std::min(n,needed_-used_);std::memcpy(bytes_.data()+used_,p,count);used_+=count;p+=count;n-=count;if(used_!=needed_)continue;Header h{};if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}if(needed_==header_size){needed_=header_size+h.payload_size;continue;}if(h.sequence<=last_sequence_||!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}last_sequence_=h.sequence;used_=0;needed_=header_size;}return true;}
}
