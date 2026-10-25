#include <psx/environment/protocol.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace psx::environment {
namespace {
struct Writer {std::uint8_t* data;std::size_t size,pos{};bool ok{true};
 void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(i*8));}
 void d(double v) noexcept {std::uint64_t b{};std::memcpy(&b,&v,8);u(b,8);}
};
struct Reader {const std::uint8_t* data;std::size_t size,pos{};bool ok{true};
 std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(i*8);return v;}
 std::int64_t i() noexcept {auto b=u(8);std::int64_t v{};std::memcpy(&v,&b,8);return v;}
 double d() noexcept {auto b=u(8);double v{};std::memcpy(&v,&b,8);return v;}
 bool end() const noexcept {return ok&&pos==size;}
};
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}
void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
bool groups(std::uint32_t v) noexcept {return (v&~all_groups)==0;}
bool reason(Reason v) noexcept {return static_cast<unsigned>(v)<=12;}
bool phase(Phase v) noexcept {return static_cast<unsigned>(v)<=3;}
bool scalar(unsigned i,double x) noexcept {
 if(!std::isfinite(x))return false;
 switch(static_cast<Field>(i)) {
 case Field::clock_rate:return x>=0&&x<=64;
 case Field::latitude_deg:return x>=-90&&x<=90;
 case Field::longitude_deg:return x>=-180&&x<=180;
 case Field::altitude_msl_m:case Field::weather_datum_msl_m:case Field::upper_base_msl_m:case Field::upper_top_msl_m:case Field::lower_base_msl_m:case Field::lower_top_msl_m:case Field::cb_base_msl_m:case Field::cb_top_msl_m:case Field::temperature_anchor_msl_m:case Field::pressure_altitude_m:return x>=-2000&&x<=100000;
 case Field::upper_coverage:case Field::lower_coverage:case Field::cb_coverage:return x>=0&&x<=1;
 case Field::wind_from_deg_true:return x>=0&&x<360;
 case Field::wind_speed_mps:return x>=0&&x<=300;
 case Field::temperature_c:return x>=-150&&x<=100;
 case Field::qnh_pa:return x>=10000&&x<=150000;
 case Field::pressure_at_aircraft_pa:return x>=0&&x<=150000;
 case Field::visibility_m:return x>=0&&x<=1000000;
 case Field::precipitation_level:return x>=0&&x<=3&&std::floor(x)==x;
 case Field::precipitation_ratio:return x>=0&&x<=1;
 default:return false;
 }
}
bool values(const std::array<double,field_capacity>& v,std::uint32_t mask) noexcept {if(mask&~all_fields)return false;for(unsigned i=0;i<field_capacity;++i)if(mask&(1u<<i)){if(!scalar(i,v[i]))return false;}else if(v[i]!=0)return false;return true;}
bool valid(const Hello& v) noexcept {return (v.role==Role::plugin||v.role==Role::exporter)&&motion::valid(v.instance)&&v.pid&&groups(v.groups)&&v.process_creation_filetime&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.instance)&&v.owner&&v.pid&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Control& v) noexcept {return motion::valid(v.producer)&&v.owner&&v.operation&&v.lifecycle&&v.configuration&&v.groups&&groups(v.groups)&&static_cast<unsigned>(v.action)>=1&&static_cast<unsigned>(v.action)<=3&&v.duration_ms==lease_ms;}
bool valid(const Ack& v) noexcept {return valid(v.control)&&phase(v.phase)&&reason(v.reason)&&groups(v.accepted_groups)&&(v.accepted_groups&~v.control.groups)==0;}
bool valid(const Applied& v) noexcept {return environment::valid(v.source)&&motion::valid(v.provider)&&v.provider_generation&&v.applied_qpc>0&&v.readback_qpc>=0&&v.regeneration_begin_qpc>=0&&v.regeneration_end_qpc>=v.regeneration_begin_qpc&&v.settling_begin_qpc>=0&&v.settling_end_qpc>=0&&(!v.settling_end_qpc||v.settling_end_qpc>=v.settling_begin_qpc)&&v.location_begin_qpc>=0&&v.location_end_qpc>=v.location_begin_qpc&&groups(v.groups)&&phase(v.phase)&&reason(v.reason)&&!(v.flags&~127u)&&!(v.approximation_mask&~v.accepted_mask)&&values(v.accepted,v.accepted_mask)&&values(v.actual,v.actual_mask)&&values(v.location,v.location_mask)&&(!v.location_mask||v.location_begin_qpc>0);}
bool valid(const Status& v) noexcept {return motion::valid(v.producer)&&groups(v.groups)&&reason(v.reason);}
bool valid(const Capture& v) noexcept {return v.owner&&v.generation&&v.enabled<=1;}
// Encoding is field-wise little endian. Native struct padding is never transported.
#define UPUT(x,n) w.u(static_cast<std::uint64_t>(v.x),n)
#define UGET(x,n) v.x=static_cast<decltype(v.x)>(r.u(n))
void put(Writer& w,const Hello& v) noexcept {UPUT(role,4);put(w,v.instance);UPUT(pid,4);UPUT(groups,4);UPUT(process_creation_filetime,8);UPUT(qpc_frequency,8);}
void get(Reader& r,Hello& v) noexcept {UGET(role,4);get(r,v.instance);UGET(pid,4);UGET(groups,4);UGET(process_creation_filetime,8);UGET(qpc_frequency,8);}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.instance);UPUT(owner,8);UPUT(qpc_frequency,8);UPUT(pid,4);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.instance);UGET(owner,8);UGET(qpc_frequency,8);UGET(pid,4);}
void put(Writer& w,const Sample& v) noexcept {put(w,v.producer);UPUT(lifecycle,8);UPUT(configuration,8);UPUT(sequence,8);UPUT(writer_version,8);UPUT(frame_sequence,8);UPUT(sample_start_qpc,8);UPUT(sample_end_qpc,8);UPUT(received_qpc,8);UPUT(utc_millis,8);UPUT(configured_utc_millis,8);UPUT(flags,4);UPUT(valid_mask,4);UPUT(configured_mask,4);for(auto x:v.values)w.d(x);for(auto x:v.configured)w.d(x);}
void get(Reader& r,Sample& v) noexcept {get(r,v.producer);UGET(lifecycle,8);UGET(configuration,8);UGET(sequence,8);UGET(writer_version,8);UGET(frame_sequence,8);v.sample_start_qpc=r.i();v.sample_end_qpc=r.i();v.received_qpc=r.i();v.utc_millis=r.i();v.configured_utc_millis=r.i();UGET(flags,4);UGET(valid_mask,4);UGET(configured_mask,4);for(auto& x:v.values)x=r.d();for(auto& x:v.configured)x=r.d();}
void put(Writer& w,const Control& v) noexcept {put(w,v.producer);UPUT(owner,8);UPUT(operation,8);UPUT(lifecycle,8);UPUT(configuration,8);UPUT(action,4);UPUT(groups,4);UPUT(duration_ms,4);}
void get(Reader& r,Control& v) noexcept {get(r,v.producer);UGET(owner,8);UGET(operation,8);UGET(lifecycle,8);UGET(configuration,8);UGET(action,4);UGET(groups,4);UGET(duration_ms,4);}
void put(Writer& w,const Ack& v) noexcept {put(w,v.control);UPUT(phase,4);UPUT(reason,4);UPUT(accepted_groups,4);}
void get(Reader& r,Ack& v) noexcept {get(r,v.control);UGET(phase,4);UGET(reason,4);UGET(accepted_groups,4);}
void put(Writer& w,const Applied& v) noexcept {put(w,v.source);put(w,v.provider);UPUT(owner,8);UPUT(provider_generation,8);UPUT(regeneration_sequence,8);UPUT(applied_qpc,8);UPUT(readback_qpc,8);UPUT(accepted_utc_millis,8);UPUT(actual_utc_millis,8);UPUT(regeneration_begin_qpc,8);UPUT(regeneration_end_qpc,8);UPUT(settling_begin_qpc,8);UPUT(settling_end_qpc,8);UPUT(location_begin_qpc,8);UPUT(location_end_qpc,8);UPUT(groups,4);UPUT(flags,4);UPUT(accepted_mask,4);UPUT(actual_mask,4);UPUT(approximation_mask,4);UPUT(location_mask,4);UPUT(phase,4);UPUT(reason,4);for(auto x:v.accepted)w.d(x);for(auto x:v.actual)w.d(x);for(auto x:v.location)w.d(x);}
void get(Reader& r,Applied& v) noexcept {get(r,v.source);get(r,v.provider);UGET(owner,8);UGET(provider_generation,8);UGET(regeneration_sequence,8);v.applied_qpc=r.i();v.readback_qpc=r.i();v.accepted_utc_millis=r.i();v.actual_utc_millis=r.i();v.regeneration_begin_qpc=r.i();v.regeneration_end_qpc=r.i();v.settling_begin_qpc=r.i();v.settling_end_qpc=r.i();v.location_begin_qpc=r.i();v.location_end_qpc=r.i();UGET(groups,4);UGET(flags,4);UGET(accepted_mask,4);UGET(actual_mask,4);UGET(approximation_mask,4);UGET(location_mask,4);UGET(phase,4);UGET(reason,4);for(auto& x:v.accepted)x=r.d();for(auto& x:v.actual)x=r.d();for(auto& x:v.location)x=r.d();}
void put(Writer& w,const Status& v) noexcept {put(w,v.producer);UPUT(lifecycle,8);UPUT(configuration,8);UPUT(groups,4);UPUT(reason,4);}
void get(Reader& r,Status& v) noexcept {get(r,v.producer);UGET(lifecycle,8);UGET(configuration,8);UGET(groups,4);UGET(reason,4);}
void put(Writer& w,const Capture& v) noexcept {UPUT(owner,8);UPUT(generation,8);UPUT(enabled,4);}
void get(Reader& r,Capture& v) noexcept {UGET(owner,8);UGET(generation,8);UGET(enabled,4);}
#undef UPUT
#undef UGET
template<class T> bool pack(const T& v,Kind kind,std::uint32_t seq,Frame& f) noexcept {
 f.size=0;if(!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.u(0x31575850u,4);w.u(protocol_version,2);w.u(static_cast<unsigned>(kind),2);w.u(0,4);w.u(seq,4);put(w,v);if(!w.ok||w.pos-header_size>max_payload)return false;const auto n=w.pos-header_size;for(std::size_t i=0;i<4;i++)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(8*i));f.size=w.pos;return true;
}
template<class T> bool unpack(const std::uint8_t* p,std::size_t n,T& v) noexcept {T out{};Reader r{p,n};get(r,out);if(!r.end()||!valid(out))return false;v=out;return true;}
}
bool valid(const Sample& v) noexcept {
 if(!motion::valid(v.producer)||!v.lifecycle||!v.configuration||!v.sequence||v.sample_start_qpc<=0||v.sample_end_qpc<v.sample_start_qpc||v.received_qpc<0||!(v.flags&initialized)||(v.flags&~63u)||!values(v.values,v.valid_mask)||!values(v.configured,v.configured_mask))return false;
 if((v.flags&utc_valid)&& (v.utc_millis<-62135596800000LL||v.utc_millis>253402300799999LL))return false;
 if((v.flags&configured_utc_valid)&& (v.configured_utc_millis<-62135596800000LL||v.configured_utc_millis>253402300799999LL))return false;
 if((v.valid_mask&1)&&!(v.flags&utc_valid))return false;
 if((v.flags&paused)&&(v.valid_mask&1)&&v.values[0]!=0)return false;
 if((v.flags&visibility_unlimited)&&!(v.valid_mask&bit(Field::visibility_m)))return false;
 for(unsigned i: {5u,8u,11u}){const auto m=7u<<i;if((v.valid_mask&m)&&((v.valid_mask&m)!=m||(v.valid_mask&reference_fields)!=reference_fields||v.values[i+1]>v.values[i+2]))return false;}
 return v.valid_mask||(v.flags&utc_valid);
}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out) noexcept {
 if(!p||n<header_size)return false;Reader r{p,header_size};if(r.u(4)!=0x31575850u||r.u(2)!=protocol_version)return false;Header h{};h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>8||h.payload_size>max_payload||!h.payload_size)return false;out=h;return true;
}
#define CODEC(T,K) bool encode(const T& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::K,s,f);} bool decode(const std::uint8_t* p,std::size_t n,T& v) noexcept {return unpack(p,n,v);}
CODEC(Hello,hello) CODEC(Subscribe,subscribe) CODEC(Sample,sample) CODEC(Control,control) CODEC(Ack,ack) CODEC(Applied,applied) CODEC(Status,status) CODEC(Capture,capture)
#undef CODEC
bool Framer::feed(const void* input,std::size_t size,FrameHandler handler,void* context) noexcept {
 if(failed_||(!input&&size)||!handler)return false;auto p=static_cast<const std::uint8_t*>(input);
 while(size){auto count=std::min(size,needed_-used_);std::memcpy(bytes_.data()+used_,p,count);used_+=count;p+=count;size-=count;if(used_!=needed_)continue;Header h{};if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}if(needed_==header_size){needed_=header_size+h.payload_size;continue;}if(!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}used_=0;needed_=header_size;}
 return true;
}
}
