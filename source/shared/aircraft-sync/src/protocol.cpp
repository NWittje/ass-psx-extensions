#include <psx/aircraft_sync/protocol.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace psx::aircraft_sync {
namespace {
struct Writer {std::uint8_t* data;std::size_t size,pos{};bool ok{true};
 void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(i*8));}
 void f(float v) noexcept {std::uint32_t b{};std::memcpy(&b,&v,4);u(b,4);}
};
struct Reader {const std::uint8_t* data;std::size_t size,pos{};bool ok{true};
 std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(i*8);return v;}
 std::int64_t i() noexcept {auto b=u(8);std::int64_t v{};std::memcpy(&v,&b,8);return v;}
 float f() noexcept {auto b=static_cast<std::uint32_t>(u(4));float v{};std::memcpy(&v,&b,4);return v;}
 bool end() const noexcept {return ok&&pos==size;}
};
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}
void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
bool groups(std::uint32_t v) noexcept {return (v&~all_groups)==0;}
bool reason(Reason v) noexcept {return static_cast<unsigned>(v)<=12;}
bool phase(Phase v) noexcept {return static_cast<unsigned>(v)<=3;}
bool valid(const Hello& v) noexcept {return (v.role==Role::plugin||v.role==Role::exporter)&&motion::valid(v.instance)&&v.pid&&groups(v.groups)&&v.process_creation_filetime&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.instance)&&v.owner&&v.pid&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Control& v) noexcept {return motion::valid(v.producer)&&v.owner&&v.operation&&v.lifecycle&&v.configuration&&v.groups&&groups(v.groups)&&static_cast<unsigned>(v.action)>=1&&static_cast<unsigned>(v.action)<=3&&v.duration_ms==lease_ms;}
bool valid(const Ack& v) noexcept {return valid(v.control)&&phase(v.phase)&&reason(v.reason)&&groups(v.accepted_groups)&&(v.accepted_groups&~v.control.groups)==0;}
bool valid(const Applied& v) noexcept {if(!motion::valid(v.producer)||!motion::valid(v.provider)||!v.lifecycle||!v.configuration||!v.sequence||v.applied_qpc<=0||!phase(v.phase)||!reason(v.reason))return false;auto mask=component_mask(v.group);if(!mask||((v.valid_mask|v.approximation_mask|v.readback_mask)&~mask)||(v.approximation_mask&~v.valid_mask)||(v.readback_mask&~v.valid_mask))return false;for(std::size_t i=0;i<32;i++){if((v.valid_mask&(1u<<i))&&!std::isfinite(v.requested[i]))return false;if((v.readback_mask&(1u<<i))&&!std::isfinite(v.readback[i]))return false;}return true;}
bool valid(const Status& v) noexcept {return motion::valid(v.producer)&&groups(v.groups)&&reason(v.reason);}
bool valid(const Capture& v) noexcept {return v.owner&&v.generation&&v.enabled<=1;}
void put(Writer& w,const Hello& v) noexcept {w.u(static_cast<std::uint32_t>(v.role),4);put(w,v.instance);w.u(static_cast<std::uint64_t>(v.pid),4);w.u(static_cast<std::uint64_t>(v.groups),4);w.u(static_cast<std::uint64_t>(v.process_creation_filetime),8);w.u(static_cast<std::uint64_t>(v.qpc_frequency),8);}
void get(Reader& r,Hello& v) noexcept {v.role=static_cast<decltype(v.role)>(r.u(4));get(r,v.instance);v.pid=static_cast<decltype(v.pid)>(r.u(4));v.groups=static_cast<decltype(v.groups)>(r.u(4));v.process_creation_filetime=static_cast<decltype(v.process_creation_filetime)>(r.u(8));v.qpc_frequency=static_cast<decltype(v.qpc_frequency)>(r.u(8));}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.instance);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.qpc_frequency),8);w.u(static_cast<std::uint64_t>(v.pid),4);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.instance);v.owner=static_cast<decltype(v.owner)>(r.u(8));v.qpc_frequency=static_cast<decltype(v.qpc_frequency)>(r.u(8));v.pid=static_cast<decltype(v.pid)>(r.u(4));}
void put(Writer& w,const Sample& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.lifecycle),8);w.u(static_cast<std::uint64_t>(v.configuration),8);w.u(static_cast<std::uint64_t>(v.sequence),8);w.u(static_cast<std::uint64_t>(v.writer_version),8);w.u(static_cast<std::uint64_t>(v.frame_sequence),8);w.u(static_cast<std::uint64_t>(v.sample_start_qpc),8);w.u(static_cast<std::uint64_t>(v.sample_end_qpc),8);w.u(static_cast<std::uint64_t>(v.received_qpc),8);w.u(static_cast<std::uint32_t>(v.group),4);w.u(static_cast<std::uint32_t>(v.source),4);w.u(static_cast<std::uint64_t>(v.flags),4);w.u(static_cast<std::uint64_t>(v.valid_mask),4);w.u(static_cast<std::uint64_t>(v.approximation_mask),4);for(auto x:v.values)w.f(x); for(auto x:v.component_qpc)w.u(static_cast<std::uint64_t>(x),8); for(auto x:v.component_sequence)w.u(static_cast<std::uint64_t>(x),8); }
void get(Reader& r,Sample& v) noexcept {get(r,v.producer);v.lifecycle=static_cast<decltype(v.lifecycle)>(r.u(8));v.configuration=static_cast<decltype(v.configuration)>(r.u(8));v.sequence=static_cast<decltype(v.sequence)>(r.u(8));v.writer_version=static_cast<decltype(v.writer_version)>(r.u(8));v.frame_sequence=static_cast<decltype(v.frame_sequence)>(r.u(8));v.sample_start_qpc=r.i();v.sample_end_qpc=r.i();v.received_qpc=r.i();v.group=static_cast<decltype(v.group)>(r.u(4));v.source=static_cast<decltype(v.source)>(r.u(4));v.flags=static_cast<decltype(v.flags)>(r.u(4));v.valid_mask=static_cast<decltype(v.valid_mask)>(r.u(4));v.approximation_mask=static_cast<decltype(v.approximation_mask)>(r.u(4));for(auto& x:v.values)x=r.f();for(auto& x:v.component_qpc)x=r.i();for(auto& x:v.component_sequence)x=r.u(8);}
void put(Writer& w,const Control& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.operation),8);w.u(static_cast<std::uint64_t>(v.lifecycle),8);w.u(static_cast<std::uint64_t>(v.configuration),8);w.u(static_cast<std::uint32_t>(v.action),4);w.u(static_cast<std::uint64_t>(v.groups),4);w.u(static_cast<std::uint64_t>(v.duration_ms),4);}
void get(Reader& r,Control& v) noexcept {get(r,v.producer);v.owner=static_cast<decltype(v.owner)>(r.u(8));v.operation=static_cast<decltype(v.operation)>(r.u(8));v.lifecycle=static_cast<decltype(v.lifecycle)>(r.u(8));v.configuration=static_cast<decltype(v.configuration)>(r.u(8));v.action=static_cast<decltype(v.action)>(r.u(4));v.groups=static_cast<decltype(v.groups)>(r.u(4));v.duration_ms=static_cast<decltype(v.duration_ms)>(r.u(4));}
void put(Writer& w,const Ack& v) noexcept {put(w,v.control);w.u(static_cast<std::uint32_t>(v.phase),4);w.u(static_cast<std::uint32_t>(v.reason),4);w.u(static_cast<std::uint64_t>(v.accepted_groups),4);}
void get(Reader& r,Ack& v) noexcept {get(r,v.control);v.phase=static_cast<decltype(v.phase)>(r.u(4));v.reason=static_cast<decltype(v.reason)>(r.u(4));v.accepted_groups=static_cast<decltype(v.accepted_groups)>(r.u(4));}
void put(Writer& w,const Applied& v) noexcept {put(w,v.producer);put(w,v.provider);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.lifecycle),8);w.u(static_cast<std::uint64_t>(v.configuration),8);w.u(static_cast<std::uint64_t>(v.sequence),8);w.u(static_cast<std::uint64_t>(v.aircraft_generation),8);w.u(static_cast<std::uint64_t>(v.applied_qpc),8);w.u(static_cast<std::uint32_t>(v.group),4);w.u(static_cast<std::uint32_t>(v.phase),4);w.u(static_cast<std::uint32_t>(v.reason),4);w.u(static_cast<std::uint64_t>(v.valid_mask),4);w.u(static_cast<std::uint64_t>(v.approximation_mask),4);w.u(static_cast<std::uint64_t>(v.readback_mask),4);for(auto x:v.requested)w.f(x); for(auto x:v.readback)w.f(x); }
void get(Reader& r,Applied& v) noexcept {get(r,v.producer);get(r,v.provider);v.owner=static_cast<decltype(v.owner)>(r.u(8));v.lifecycle=static_cast<decltype(v.lifecycle)>(r.u(8));v.configuration=static_cast<decltype(v.configuration)>(r.u(8));v.sequence=static_cast<decltype(v.sequence)>(r.u(8));v.aircraft_generation=static_cast<decltype(v.aircraft_generation)>(r.u(8));v.applied_qpc=r.i();v.group=static_cast<decltype(v.group)>(r.u(4));v.phase=static_cast<decltype(v.phase)>(r.u(4));v.reason=static_cast<decltype(v.reason)>(r.u(4));v.valid_mask=static_cast<decltype(v.valid_mask)>(r.u(4));v.approximation_mask=static_cast<decltype(v.approximation_mask)>(r.u(4));v.readback_mask=static_cast<decltype(v.readback_mask)>(r.u(4));for(auto& x:v.requested)x=r.f();for(auto& x:v.readback)x=r.f();}
void put(Writer& w,const Status& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.lifecycle),8);w.u(static_cast<std::uint64_t>(v.configuration),8);w.u(static_cast<std::uint64_t>(v.groups),4);w.u(static_cast<std::uint32_t>(v.reason),4);}
void get(Reader& r,Status& v) noexcept {get(r,v.producer);v.lifecycle=static_cast<decltype(v.lifecycle)>(r.u(8));v.configuration=static_cast<decltype(v.configuration)>(r.u(8));v.groups=static_cast<decltype(v.groups)>(r.u(4));v.reason=static_cast<decltype(v.reason)>(r.u(4));}
void put(Writer& w,const Capture& v) noexcept {w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.generation),8);w.u(static_cast<std::uint64_t>(v.enabled),4);}
void get(Reader& r,Capture& v) noexcept {v.owner=static_cast<decltype(v.owner)>(r.u(8));v.generation=static_cast<decltype(v.generation)>(r.u(8));v.enabled=static_cast<decltype(v.enabled)>(r.u(4));}
template<class T> bool pack(const T& v,Kind kind,std::uint32_t seq,Frame& f) noexcept {
 f.size=0;if(!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.u(0x31415850u,4);w.u(protocol_version,2);w.u(static_cast<unsigned>(kind),2);w.u(0,4);w.u(seq,4);put(w,v);if(!w.ok||w.pos-header_size>max_payload)return false;const auto n=w.pos-header_size;for(std::size_t i=0;i<4;i++)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(8*i));f.size=w.pos;return true;
}
template<class T> bool unpack(const std::uint8_t* p,std::size_t n,T& v) noexcept {T out{};Reader r{p,n};get(r,out);if(!r.end()||!valid(out))return false;v=out;return true;}
}
bool valid(const Sample& v) noexcept {
 auto mask=component_mask(v.group);
 if(!motion::valid(v.producer)||!v.lifecycle||!v.configuration||!v.sequence||!mask||!v.valid_mask||(v.valid_mask&~mask)||(v.approximation_mask&~v.valid_mask)||(v.flags&~7u)||(v.flags&initialized)==0||v.received_qpc<0)return false;
 if(v.source==Source::native){if(v.sample_start_qpc<=0||v.sample_end_qpc<v.sample_start_qpc)return false;}
 else if(v.source==Source::public_record){if(v.sample_start_qpc||v.sample_end_qpc||v.writer_version||v.frame_sequence||(v.flags&frame_known))return false;}
 else return false;
 for(std::size_t i=0;i<32;i++)if(v.valid_mask&(1u<<i)){
  const float x=v.values[i];if(!std::isfinite(x)||v.component_qpc[i]<=0||!v.component_sequence[i])return false;
  if(v.source==Source::native&&(v.component_qpc[i]!=v.sample_end_qpc||v.component_sequence[i]!=v.sequence))return false;
  if(v.group==Group::controls&&(x<-180||x>180))return false;
  if(v.group==Group::spoilers&&(x<-1||x>100))return false;
  if((v.group==Group::high_lift||v.group==Group::gear)&&(x<0||x>1))return false;
  if(v.group==Group::reversers&&(x!=0&&x!=1))return false;
  if(v.group==Group::lights&&(x<0||x>9||std::floor(x)!=x))return false;
 }else if(v.component_qpc[i]||v.component_sequence[i])return false;
 return true;
}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out) noexcept {
 if(!p||n<header_size)return false;Reader r{p,header_size};if(r.u(4)!=0x31415850u||r.u(2)!=protocol_version)return false;Header h{};h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>8||h.payload_size>max_payload||!h.payload_size)return false;out=h;return true;
}
bool encode(const Hello& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::hello,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Hello& v) noexcept {return unpack(p,n,v);}
bool encode(const Subscribe& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::subscribe,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Subscribe& v) noexcept {return unpack(p,n,v);}
bool encode(const Sample& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::sample,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Sample& v) noexcept {return unpack(p,n,v);}
bool encode(const Control& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::control,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Control& v) noexcept {return unpack(p,n,v);}
bool encode(const Ack& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::ack,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Ack& v) noexcept {return unpack(p,n,v);}
bool encode(const Applied& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::applied,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Applied& v) noexcept {return unpack(p,n,v);}
bool encode(const Status& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::status,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Status& v) noexcept {return unpack(p,n,v);}
bool encode(const Capture& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::capture,s,f);} bool decode(const std::uint8_t* p,std::size_t n,Capture& v) noexcept {return unpack(p,n,v);}
bool Framer::feed(const void* input,std::size_t size,FrameHandler handler,void* context) noexcept {
 if(failed_||(!input&&size)||!handler)return false;auto p=static_cast<const std::uint8_t*>(input);
 while(size){auto count=std::min(size,needed_-used_);std::memcpy(bytes_.data()+used_,p,count);used_+=count;p+=count;size-=count;if(used_!=needed_)continue;
 Header h{};if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}
 if(needed_==header_size){needed_=header_size+h.payload_size;continue;}
 if(!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}used_=0;needed_=header_size;
 }return true;
}
}

