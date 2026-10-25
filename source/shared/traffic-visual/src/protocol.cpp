#include <psx/traffic_visual/protocol.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>
namespace psx::traffic_visual {
namespace {
struct Writer {std::uint8_t* p;std::size_t n,i{};bool ok{true};void u(std::uint64_t v,std::size_t c) noexcept {if(i>n||c>n-i){ok=false;return;}for(std::size_t k=0;k<c;++k)p[i++]=static_cast<std::uint8_t>(v>>(8*k));}void d(double v) noexcept {std::uint64_t b{};std::memcpy(&b,&v,8);u(b,8);}void bytes(const void* b,std::size_t c) noexcept {if(i>n||c>n-i){ok=false;return;}std::memcpy(p+i,b,c);i+=c;}};
struct Reader {const std::uint8_t* p;std::size_t n,i{};bool ok{true};std::uint64_t u(std::size_t c) noexcept {if(!p||i>n||c>n-i){ok=false;return 0;}std::uint64_t v{};for(std::size_t k=0;k<c;++k)v|=std::uint64_t(p[i++])<<(8*k);return v;}double d() noexcept {auto b=u(8);double v{};std::memcpy(&v,&b,8);return v;}bool end() const noexcept {return ok&&i==n;}};
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
bool frequency(std::uint64_t v) noexcept {return v&&v<=1000000000000ULL;}
bool valid(const Hello& v) noexcept {return motion::valid(v.instance)&&v.pid&&v.process_creation_filetime&&frequency(v.qpc_frequency)&&!(v.capabilities&~7u)&&(!(v.capabilities&loaded_model)||(v.capabilities&qualified_model))&&v.model==model_revision&&v.profile==profile_revision;}
bool reason(Reason v) noexcept {return static_cast<std::uint32_t>(v)<=17;}
bool valid(const Heartbeat& v) noexcept {return motion::valid(v.sender)&&v.connection&&v.generated_qpc>0&&!(v.capabilities&~7u)&&reason(v.reason);}
bool settings(std::uint32_t delay,std::uint32_t mode) noexcept {return mode<=1&&(mode?delay==0:(delay>=10&&delay<=50));}
bool valid(const Capture& v) noexcept {return motion::valid(v.plugin)&&v.connection&&v.generation&&v.sent_qpc>0&&v.enabled<=1&&v.duration_ms==2000;}
bool valid(const Detail& v) noexcept {
 if(!motion::valid(v.plugin)||!motion::valid(v.owner)||!v.connection||!v.lease||!v.request||!v.capture_generation||!v.sdk_sequence||v.slot>=7||(v.flags&~15u)||!motion::valid(v.source_producer)||!v.source_connection||!v.source_selection||!v.lifecycle||v.generated_qpc<=0||v.source_observed_qpc<0||v.source_observed_qpc>v.generated_qpc||v.applied_qpc<0||v.applied_qpc>v.generated_qpc||v.state>8||(v.validity&~1023u)||!settings(v.delay_ms,v.presentation_mode)||v.model!=model_revision||v.profile!=profile_revision)return false;
 const double values[]={v.alpha,v.latitude_deg,v.longitude_deg,v.altitude_msl_m,v.heading_deg,v.pitch_deg,v.bank_deg,v.local_x,v.local_y,v.local_z,v.local_heading_deg,v.local_pitch_deg,v.local_bank_deg,v.strobe_phase,v.nav_brightness,v.strobe_brightness};
 for(auto x:values)if(!std::isfinite(x))return false;
 if(v.alpha<0||v.alpha>1||v.strobe_phase<0||v.strobe_phase>=1||v.nav_brightness<0||v.nav_brightness>1||v.strobe_brightness<0||v.strobe_brightness>1)return false;
 if(v.flags&detail_pose_valid){if(std::abs(v.latitude_deg)>89||std::abs(v.longitude_deg)>180||!v.generation||!v.source_sequence||!v.source_frame||!v.left_sequence||!v.right_sequence||!v.left_frame||!v.right_frame||v.left_observed_qpc<=0||v.right_observed_qpc<v.left_observed_qpc||v.right_observed_qpc>v.source_observed_qpc||v.left_received_qpc<v.left_observed_qpc||v.right_received_qpc<v.right_observed_qpc||v.left_received_qpc>v.generated_qpc||v.right_received_qpc>v.generated_qpc||v.target_qpc<0||v.target_qpc>v.generated_qpc||v.presentation_qpc<v.left_observed_qpc||v.presentation_qpc>v.right_observed_qpc)return false;}
 if((v.flags&(detail_submitted|detail_visible))&&(!(v.flags&detail_pose_valid)||!v.applied_qpc||!v.resource_generation))return false;
 return true;
}
#define P(x,n) w.u(static_cast<std::uint64_t>(v.x),n)
#define G(x,n) v.x=static_cast<decltype(v.x)>(r.u(n))
void put(Writer& w,const Hello& v) noexcept {put(w,v.instance);P(pid,4);P(capabilities,4);P(process_creation_filetime,8);P(qpc_frequency,8);P(model,4);P(profile,4);}
void get(Reader& r,Hello& v) noexcept {get(r,v.instance);G(pid,4);G(capabilities,4);G(process_creation_filetime,8);G(qpc_frequency,8);G(model,4);G(profile,4);}
void put(Writer& w,const Claim& v) noexcept {put(w,v.plugin);put(w,v.owner);P(connection,8);P(lease,8);P(request,8);put(w,v.source_producer);P(source_connection,8);P(source_selection,8);P(lifecycle,8);P(source_pid,4);P(source_birth,8);P(frequency,8);P(source_floor_qpc,8);P(action,4);P(delay_ms,4);P(presentation_mode,4);P(model,4);P(profile,4);}
void get(Reader& r,Claim& v) noexcept {get(r,v.plugin);get(r,v.owner);G(connection,8);G(lease,8);G(request,8);get(r,v.source_producer);G(source_connection,8);G(source_selection,8);G(lifecycle,8);G(source_pid,4);G(source_birth,8);G(frequency,8);G(source_floor_qpc,8);G(action,4);G(delay_ms,4);G(presentation_mode,4);G(model,4);G(profile,4);}
void put(Writer& w,const Bundle& v) noexcept {put(w,v.plugin);put(w,v.owner);P(connection,8);P(lease,8);traffic_source::Frame f{};if(!traffic_source::encode(v.source,1,f)){w.ok=false;return;}w.bytes(f.bytes.data()+traffic_source::header_size,f.size-traffic_source::header_size);}
void get(Reader& r,Bundle& v) noexcept {get(r,v.plugin);get(r,v.owner);G(connection,8);G(lease,8);if(!r.ok||r.i>r.n||!traffic_source::decode(r.p+r.i,r.n-r.i,v.source)){r.ok=false;return;}r.i=r.n;}
void put(Writer& w,const Heartbeat& v) noexcept {put(w,v.sender);P(connection,8);P(generated_qpc,8);P(capabilities,4);P(reason,4);}
void get(Reader& r,Heartbeat& v) noexcept {get(r,v.sender);G(connection,8);G(generated_qpc,8);G(capabilities,4);G(reason,4);}
void put(Writer& w,const Report& v) noexcept {put(w,v.plugin);put(w,v.owner);P(connection,8);P(lease,8);P(request,8);P(generated_qpc,8);put(w,v.source_producer);P(source_connection,8);P(source_selection,8);P(lifecycle,8);P(source_sequence,8);P(source_observed_qpc,8);P(phase,4);P(reason,4);P(drawable_mask,4);P(submitted_mask,4);P(held_mask,4);P(hidden_mask,4);P(model,4);P(profile,4);P(sdk_sequence,8);P(scenery_generation,8);P(origin_generation,8);P(applied_qpc,8);P(resource_generation,8);}
void get(Reader& r,Report& v) noexcept {get(r,v.plugin);get(r,v.owner);G(connection,8);G(lease,8);G(request,8);G(generated_qpc,8);get(r,v.source_producer);G(source_connection,8);G(source_selection,8);G(lifecycle,8);G(source_sequence,8);G(source_observed_qpc,8);G(phase,4);G(reason,4);G(drawable_mask,4);G(submitted_mask,4);G(held_mask,4);G(hidden_mask,4);G(model,4);G(profile,4);G(sdk_sequence,8);G(scenery_generation,8);G(origin_generation,8);G(applied_qpc,8);G(resource_generation,8);}
void put(Writer& w,const Capture& v) noexcept {put(w,v.plugin);P(connection,8);P(generation,8);P(sent_qpc,8);P(enabled,4);P(duration_ms,4);}
void get(Reader& r,Capture& v) noexcept {get(r,v.plugin);G(connection,8);G(generation,8);G(sent_qpc,8);G(enabled,4);G(duration_ms,4);}
#define DETAIL_FIELDS(M) M(connection,8);M(lease,8);M(request,8);M(capture_generation,8);M(sdk_sequence,8);M(slot,4);M(flags,4)
#define DETAIL_SOURCE(M) M(source_connection,8);M(source_selection,8);M(lifecycle,8);M(source_sequence,8);M(source_frame,8);M(generation,8);M(source_observed_qpc,8);M(generated_qpc,8);M(target_qpc,8);M(presentation_qpc,8);M(left_observed_qpc,8);M(right_observed_qpc,8);M(left_received_qpc,8);M(right_received_qpc,8);M(applied_qpc,8);M(left_sequence,8);M(right_sequence,8);M(left_frame,8);M(right_frame,8);M(scenery_generation,8);M(origin_generation,8);M(resource_generation,8);M(dropped,8);M(state,4);M(validity,4);M(delay_ms,4);M(presentation_mode,4);M(model,4);M(profile,4)
#define DETAIL_DOUBLES(M) M(alpha);M(latitude_deg);M(longitude_deg);M(altitude_msl_m);M(heading_deg);M(pitch_deg);M(bank_deg);M(local_x);M(local_y);M(local_z);M(local_heading_deg);M(local_pitch_deg);M(local_bank_deg);M(strobe_phase);M(nav_brightness);M(strobe_brightness)
#define PD(x) w.d(v.x)
#define GD(x) v.x=r.d()
void put(Writer& w,const Detail& v) noexcept {put(w,v.plugin);put(w,v.owner);DETAIL_FIELDS(P);put(w,v.source_producer);DETAIL_SOURCE(P);DETAIL_DOUBLES(PD);}
void get(Reader& r,Detail& v) noexcept {get(r,v.plugin);get(r,v.owner);DETAIL_FIELDS(G);get(r,v.source_producer);DETAIL_SOURCE(G);DETAIL_DOUBLES(GD);}
#undef PD
#undef GD
#undef DETAIL_FIELDS
#undef DETAIL_SOURCE
#undef DETAIL_DOUBLES
#undef P
#undef G
std::uint32_t length(Kind k) noexcept {switch(k){case Kind::hello:return 48;case Kind::claim:return 144;case Kind::bundle:return 992;case Kind::ack:case Kind::report:return 192;case Kind::heartbeat:return 40;case Kind::capture:return 48;case Kind::detail:return 432;}return 0;}
template<class T> bool pack(const T& v,Kind k,std::uint32_t s,Frame& f) noexcept {f.size=0;if(!s||!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.u(0x31565850u,4);w.u(protocol_version,2);w.u(static_cast<unsigned>(k),2);w.u(length(k),4);w.u(s,4);put(w,v);if(!w.ok||w.i!=header_size+length(k))return false;f.size=w.i;return true;}
template<class T> bool unpack(const std::uint8_t* p,std::size_t n,T& v) noexcept {T t{};Reader r{p,n};get(r,t);if(!r.end()||!valid(t))return false;v=t;return true;}
}
bool valid(const Claim& v) noexcept {return motion::valid(v.plugin)&&motion::valid(v.owner)&&v.connection&&v.lease&&v.request&&motion::valid(v.source_producer)&&v.source_connection&&v.source_selection&&v.lifecycle&&v.source_pid&&v.source_birth&&frequency(v.frequency)&&v.source_floor_qpc>0&&static_cast<unsigned>(v.action)>=1&&static_cast<unsigned>(v.action)<=3&&settings(v.delay_ms,v.presentation_mode)&&v.model==model_revision&&v.profile==profile_revision;}
bool valid(const Bundle& v) noexcept {return motion::valid(v.plugin)&&motion::valid(v.owner)&&v.connection&&v.lease&&traffic_source::valid(v.source);}
bool valid(const Report& v) noexcept {
 if(!motion::valid(v.plugin)||!v.connection||v.generated_qpc<=0||v.source_observed_qpc<0||v.applied_qpc<0||v.applied_qpc>v.generated_qpc||v.source_observed_qpc>v.generated_qpc||static_cast<unsigned>(v.phase)>4||!reason(v.reason)||((v.drawable_mask|v.submitted_mask|v.held_mask|v.hidden_mask)&~127u)||v.model!=model_revision||v.profile!=profile_revision)return false;
 if((v.submitted_mask&~v.drawable_mask)||(v.held_mask&~v.submitted_mask)||(v.hidden_mask&v.submitted_mask))return false;
 if(v.phase!=Phase::observing&&(!motion::valid(v.owner)||!v.lease||!v.request))return false;
 if(v.submitted_mask&&(!v.sdk_sequence||!v.resource_generation||!v.applied_qpc||!v.source_sequence||!v.source_observed_qpc||!motion::valid(v.source_producer)||!v.source_connection||!v.source_selection||!v.lifecycle))return false;
 return true;
}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out) noexcept {if(!p||n<header_size)return false;Reader r{p,header_size};if(r.u(4)!=0x31565850u||r.u(2)!=protocol_version)return false;Header h{};h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(!h.sequence||!length(h.kind)||h.payload_size!=length(h.kind))return false;out=h;return true;}
#define CODEC(T,K) bool encode(const T& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::K,s,f);}bool decode(const std::uint8_t* p,std::size_t n,T& v) noexcept {return unpack(p,n,v);}
CODEC(Hello,hello) CODEC(Claim,claim) CODEC(Bundle,bundle) CODEC(Ack,ack) CODEC(Heartbeat,heartbeat) CODEC(Report,report) CODEC(Capture,capture) CODEC(Detail,detail)
#undef CODEC
bool Framer::feed(const void* input,std::size_t n,FrameHandler handler,void* context) noexcept {if(failed_||(!input&&n)||!handler)return false;auto p=static_cast<const std::uint8_t*>(input);while(n){auto c=std::min(n,needed_-used_);std::memcpy(bytes_.data()+used_,p,c);used_+=c;p+=c;n-=c;if(used_!=needed_)continue;Header h{};if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}if(needed_==header_size){needed_=header_size+h.payload_size;continue;}if(h.sequence<=last_sequence_||!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}last_sequence_=h.sequence;used_=0;needed_=header_size;}return true;}
}
