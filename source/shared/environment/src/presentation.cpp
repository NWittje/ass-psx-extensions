#include <psx/environment/presentation.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
namespace psx::environment::presentation {
namespace {
struct Writer {
 std::uint8_t* p;std::size_t size,pos{};bool ok{true},third{},fourth{};
 void u(std::uint64_t v,std::size_t n)noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)p[pos++]=static_cast<std::uint8_t>(v>>(8*i));}
 void bytes(const void* v,std::size_t n)noexcept {if(pos>size||n>size-pos){ok=false;return;}std::memcpy(p+pos,v,n);pos+=n;}
 void d(double v)noexcept {std::uint64_t bits{};std::memcpy(&bits,&v,8);u(bits,8);}
};
struct Reader {
 const std::uint8_t* p;std::size_t size,pos{};bool ok{true},third{},fourth{};
 std::uint64_t u(std::size_t n)noexcept {if(!p||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(p[pos++])<<(8*i);return v;}
 std::int64_t i()noexcept {auto bits=u(8);std::int64_t v{};std::memcpy(&v,&bits,8);return v;}
 double d()noexcept {auto bits=u(8);double v{};std::memcpy(&v,&bits,8);return v;}
 void bytes(void* v,std::size_t n)noexcept {if(!p||pos>size||n>size-pos){ok=false;return;}std::memcpy(v,p+pos,n);pos+=n;}
 bool end()const noexcept{return ok&&pos==size;}
};
bool station(const std::array<char,8>& a)noexcept {bool end=false;for(char c:a){if(!c){end=true;continue;}if(end||c<32||c>126)return false;}return end;}
bool process(ProcessIdentity p)noexcept{return p.pid&&p.creation_filetime;}
bool target(Target t)noexcept{return static_cast<unsigned>(t)<=4;}
bool policy(const VisibilityPolicy& p,const environment::Sample& s)noexcept {
 if(!target(p.target)||!p.owner||p.lifecycle!=s.lifecycle||p.source_sequence!=s.sequence||!std::isfinite(p.visibility_m))return false;
 if(p.kind==PolicyKind::native)return p.visibility_m==0;
 if(p.kind==PolicyKind::sentinel_finite)return p.visibility_m==9999&&(s.flags&environment::visibility_unlimited)&&(s.valid_mask&bit(Field::visibility_m));
 if(p.kind==PolicyKind::local_target)return (s.valid_mask&bit(Field::visibility_m))&&p.visibility_m>=0&&p.visibility_m<=9999;
 return p.kind==PolicyKind::extended&&(p.target==Target::planet||p.target==Target::zone1||p.target==Target::all)&&p.selection_revision&&p.visibility_m>=10000&&p.visibility_m<=40000;
}
template<class T> void base(Writer& w,const T& v)noexcept {environment::Frame f;if(!environment::encode(v,0,f)){w.ok=false;return;}w.u(f.size-environment::header_size,4);w.bytes(f.bytes.data()+environment::header_size,f.size-environment::header_size);}
template<class T> void base(Reader& r,T& v)noexcept {auto n=r.u(4);if(!r.ok||n>r.size-r.pos||!environment::decode(r.p+r.pos,static_cast<std::size_t>(n),v)){r.ok=false;return;}r.pos+=static_cast<std::size_t>(n);}
#define PUT(x,n) w.u(static_cast<std::uint64_t>(v.x),n)
#define GET(x,n) v.x=static_cast<decltype(v.x)>(r.u(n))
void put(Writer& w,ProcessIdentity v)noexcept{PUT(pid,4);PUT(creation_filetime,8);}
void get(Reader& r,ProcessIdentity& v)noexcept{GET(pid,4);GET(creation_filetime,8);}
void put(Writer& w,const Context& v)noexcept{PUT(source_sequence,8);PUT(observation_start_qpc,8);PUT(observation_end_qpc,8);PUT(valid_mask,4);PUT(active_native_zone,4);PUT(planet_visibility_m,4);PUT(zone1_visibility_m,4);w.bytes(v.zone1_station.data(),8);}
void get(Reader& r,Context& v)noexcept{GET(source_sequence,8);v.observation_start_qpc=r.i();v.observation_end_qpc=r.i();GET(valid_mask,4);GET(active_native_zone,4);GET(planet_visibility_m,4);GET(zone1_visibility_m,4);r.bytes(v.zone1_station.data(),8);}
void put(Writer& w,const Hello& v)noexcept{base(w,v.base);PUT(capabilities,4);}
void get(Reader& r,Hello& v)noexcept{base(r,v.base);GET(capabilities,4);}
bool valid(const Hello& v)noexcept{environment::Frame f;return !(v.capabilities&~15u)&&environment::encode(v.base,0,f);}
void put(Writer& w,const Source& v)noexcept{base(w,v.sample);put(w,v.context);if(w.third){w.u(v.change.revision,8);w.u(v.change.lifecycle,8);w.u(static_cast<std::uint64_t>(v.change.completed_qpc),8);w.u(v.change.qualified,4);w.u(v.change.reason,4);}if(w.fourth){w.u(v.local_visibility.lifecycle,8);w.u(static_cast<std::uint64_t>(v.local_visibility.observed_qpc),8);w.d(v.local_visibility.metres);w.u(v.local_visibility.qualified,4);w.u(0,4);}}
void get(Reader& r,Source& v)noexcept{base(r,v.sample);get(r,v.context);if(r.third){v.change.revision=r.u(8);v.change.lifecycle=r.u(8);v.change.completed_qpc=r.i();v.change.qualified=static_cast<std::uint32_t>(r.u(4));v.change.reason=static_cast<std::uint32_t>(r.u(4));}if(r.fourth){v.local_visibility.lifecycle=r.u(8);v.local_visibility.observed_qpc=r.i();v.local_visibility.metres=r.d();v.local_visibility.qualified=static_cast<std::uint32_t>(r.u(4));if(r.u(4))r.ok=false;}}
void put(Writer& w,const VisibilityPolicy& v)noexcept{PUT(kind,4);PUT(target,4);PUT(owner,8);PUT(lifecycle,8);PUT(source_sequence,8);PUT(selection_revision,8);w.d(v.visibility_m);}
void get(Reader& r,VisibilityPolicy& v)noexcept{GET(kind,4);GET(target,4);GET(owner,8);GET(lifecycle,8);GET(source_sequence,8);GET(selection_revision,8);v.visibility_m=r.d();}
void put(Writer& w,const Presentation& v)noexcept{put(w,v.source);put(w,v.visibility);if(w.third){w.u(v.weather.identity,8);put(w,v.weather.identity?v.weather.source:v.source);put(w,v.weather.identity?v.weather.visibility:v.visibility);}}
void get(Reader& r,Presentation& v)noexcept{get(r,v.source);get(r,v.visibility);if(r.third){v.weather.identity=r.u(8);get(r,v.weather.source);get(r,v.weather.visibility);}}
void put(Writer& w,const AppliedPresentation& v)noexcept{base(w,v.applied);put(w,v.visibility);if(w.third){w.u(v.weather_target,8);w.u(v.current_lifecycle,8);w.u(v.current_sequence,8);w.u(static_cast<std::uint64_t>(v.current_source_end_qpc),8);w.u(static_cast<std::uint64_t>(v.weather_observed_qpc),8);}}
void get(Reader& r,AppliedPresentation& v)noexcept{base(r,v.applied);get(r,v.visibility);if(r.third){v.weather_target=r.u(8);v.current_lifecycle=r.u(8);v.current_sequence=r.u(8);v.current_source_end_qpc=r.i();v.weather_observed_qpc=r.i();}}
bool valid(const AppliedPresentation& v)noexcept{environment::Frame f;return environment::encode(v.applied,0,f)&&v.visibility.owner==v.applied.owner&&policy(v.visibility,v.applied.source)&&
 (v.weather_target?(v.applied.groups==2&&v.current_lifecycle&&v.current_sequence&&v.current_source_end_qpc>0&&v.current_source_end_qpc<=v.weather_observed_qpc&&v.weather_observed_qpc>0):(!v.current_lifecycle&&!v.current_sequence&&!v.current_source_end_qpc&&!v.weather_observed_qpc));}
void put(Writer& w,const VisibilityRequest& v)noexcept{PUT(command,4);put(w,v.client);put(w,v.server);put(w,v.psx);PUT(session,8);PUT(request,8);PUT(qpc_frequency,8);PUT(issued_qpc,8);PUT(deadline_qpc,8);PUT(configured_readback_qpc,8);PUT(target,4);w.d(v.commanded_visibility_m);w.bytes(v.zone1_station.data(),8);for(unsigned i=0;i<52;++i)w.u(0,1);}
void get(Reader& r,VisibilityRequest& v)noexcept{GET(command,4);get(r,v.client);get(r,v.server);get(r,v.psx);GET(session,8);GET(request,8);GET(qpc_frequency,8);v.issued_qpc=r.i();v.deadline_qpc=r.i();v.configured_readback_qpc=r.i();GET(target,4);v.commanded_visibility_m=r.d();r.bytes(v.zone1_station.data(),8);for(unsigned i=0;i<52;++i)if(r.u(1))r.ok=false;}
void put(Writer& w,const VisibilityResult& v)noexcept{put(w,v.server);PUT(session,8);PUT(request,8);PUT(selection_revision,8);PUT(observed_qpc,8);PUT(source_observation_qpc,8);PUT(application_qpc,8);PUT(target,4);PUT(active_target,4);PUT(phase,4);PUT(reason,4);PUT(valid_mask,4);PUT(weather_requested,4);PUT(weather_owned,4);PUT(presentation_available,4);w.d(v.commanded_visibility_m);w.d(v.configured_visibility_m);w.d(v.accepted_visibility_m);w.d(v.actual_visibility_m);w.u(0,4);}
void get(Reader& r,VisibilityResult& v)noexcept{get(r,v.server);GET(session,8);GET(request,8);GET(selection_revision,8);v.observed_qpc=r.i();v.source_observation_qpc=r.i();v.application_qpc=r.i();GET(target,4);GET(active_target,4);GET(phase,4);GET(reason,4);GET(valid_mask,4);GET(weather_requested,4);GET(weather_owned,4);GET(presentation_available,4);v.commanded_visibility_m=r.d();v.configured_visibility_m=r.d();v.accepted_visibility_m=r.d();v.actual_visibility_m=r.d();if(r.u(4))r.ok=false;}
#undef PUT
#undef GET
template<class T> bool fourth(const T& v)noexcept {
 if constexpr(std::is_same_v<T,Hello>)return (v.capabilities&local_visibility_target)!=0;
 else if constexpr(std::is_same_v<T,Source>)return v.local_visibility.qualified!=0;
 else if constexpr(std::is_same_v<T,Presentation>)return fourth(v.source)||fourth(v.weather.source)||v.visibility.target==Target::all||v.weather.visibility.target==Target::all||v.visibility.kind==PolicyKind::local_target||v.weather.visibility.kind==PolicyKind::local_target;
 else if constexpr(std::is_same_v<T,AppliedPresentation>)return v.visibility.target==Target::all||v.visibility.kind==PolicyKind::local_target;
 else if constexpr(std::is_same_v<T,VisibilityRequest>||std::is_same_v<T,VisibilityResult>)return v.target==Target::all;
 else return false;
}
template<class T> bool third(const T& v)noexcept {
 if constexpr(std::is_same_v<T,Hello>)return (v.capabilities&manual_weather)!=0;
 else if constexpr(std::is_same_v<T,Source>)return v.change.qualified!=0;
 else if constexpr(std::is_same_v<T,Presentation>)return v.weather.identity!=0;
 else if constexpr(std::is_same_v<T,AppliedPresentation>)return v.weather_target!=0;
 else return false;
}
template<class T> bool pack(const T& v,Kind k,std::uint32_t seq,environment::Frame& f)noexcept {f.size=0;if(!valid(v))return false;Writer w{f.bytes.data(),f.bytes.size()};w.fourth=fourth(v);w.third=w.fourth||third(v);w.u(w.fourth?0x34575850u:w.third?0x33575850u:magic,4);w.u(w.fourth?4:w.third?3:version,2);w.u(static_cast<unsigned>(k),2);w.u(0,4);w.u(seq,4);put(w,v);if(!w.ok||w.pos-environment::header_size>environment::max_payload)return false;auto n=w.pos-environment::header_size;for(unsigned i=0;i<4;++i)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(8*i));f.size=w.pos;return true;}
template<class T> bool unpack(std::span<const std::uint8_t> b,T& v)noexcept{T out{};Reader r{b.data(),b.size()};
 if constexpr(std::is_same_v<T,Source>){r.fourth=b.size()==736;r.third=r.fourth||b.size()==704;}
 else if constexpr(std::is_same_v<T,Presentation>){r.fourth=b.size()==1576;r.third=r.fourth||b.size()==1512;}
 else if constexpr(std::is_same_v<T,AppliedPresentation>)r.third=b.size()==1632;
 get(r,out);if(!r.end()||!valid(out))return false;v=out;return true;}
}
bool same_sample(const environment::Sample& a,const environment::Sample& b)noexcept{return a.producer.high==b.producer.high&&a.producer.low==b.producer.low&&a.lifecycle==b.lifecycle&&a.configuration==b.configuration&&a.sequence==b.sequence&&a.writer_version==b.writer_version&&a.frame_sequence==b.frame_sequence&&a.sample_start_qpc==b.sample_start_qpc&&a.sample_end_qpc==b.sample_end_qpc&&a.received_qpc==b.received_qpc&&a.utc_millis==b.utc_millis&&a.configured_utc_millis==b.configured_utc_millis&&a.flags==b.flags&&a.valid_mask==b.valid_mask&&a.configured_mask==b.configured_mask&&a.values==b.values&&a.configured==b.configured;}
bool valid(const Source& v)noexcept{
 const auto& c=v.context;const auto& s=v.sample;
 const auto& local=v.local_visibility;
 if(local.qualified>1||!std::isfinite(local.metres))return false;
 if(local.qualified){if(local.lifecycle!=s.lifecycle||local.observed_qpc<=0||local.observed_qpc>s.sample_end_qpc||local.metres<0||local.metres>9999||!(s.valid_mask&bit(Field::visibility_m)))return false;
  if(v.change.revision&&(v.change.lifecycle!=local.lifecycle||v.change.completed_qpc>local.observed_qpc))return false;
 }else if(local.lifecycle||local.observed_qpc||local.metres)return false;
 const auto& change=v.change;
 if(change.qualified>1||change.reason>3)return false;
 if(!change.qualified){if(change.revision||change.lifecycle||change.completed_qpc||change.reason)return false;}
 else if(change.revision){if(!change.lifecycle||!change.reason||change.completed_qpc<=0||change.completed_qpc>s.sample_start_qpc)return false;}
 else if(change.lifecycle||change.completed_qpc||change.reason)return false;
 if(!environment::valid(s)||c.source_sequence!=s.sequence||(c.valid_mask&~15u))return false;
 if(c.valid_mask){if(c.observation_start_qpc<s.sample_start_qpc||c.observation_end_qpc<c.observation_start_qpc||c.observation_end_qpc>s.sample_end_qpc)return false;}
 else if(c.observation_start_qpc||c.observation_end_qpc)return false;
 if(c.valid_mask&active_target_valid){if(c.active_native_zone<0||c.active_native_zone>7)return false;}else if(c.active_native_zone!=-1)return false;
 if(c.valid_mask&planet_visibility_valid){if(c.planet_visibility_m>9999)return false;}else if(c.planet_visibility_m)return false;
 if(c.valid_mask&zone1_visibility_valid){if(c.zone1_visibility_m>9999)return false;}else if(c.zone1_visibility_m)return false;
 if(!station(c.zone1_station))return false;
 if(!(c.valid_mask&zone1_station_valid)&&std::any_of(c.zone1_station.begin(),c.zone1_station.end(),[](char x){return x!=0;}))return false;
 return true;
}
bool valid(const Presentation& v)noexcept{
 if(!valid(v.source)||!policy(v.visibility,v.source.sample))return false;
 if(v.weather.identity&&(!v.source.change.qualified||!valid(v.weather.source)||!v.weather.source.change.qualified||v.weather.source.sample.producer!=v.source.sample.producer||v.weather.source.sample.sample_end_qpc>v.source.sample.sample_end_qpc||v.weather.visibility.owner!=v.visibility.owner||!policy(v.weather.visibility,v.weather.source.sample)))return false;
 if(v.visibility.kind==PolicyKind::native&&(v.source.sample.flags&environment::visibility_unlimited)&&(v.source.sample.valid_mask&bit(Field::visibility_m)))return false;
 if(v.visibility.kind==PolicyKind::local_target)return v.source.local_visibility.qualified&&v.visibility.visibility_m==v.source.local_visibility.metres;
 if(v.visibility.kind!=PolicyKind::extended)return true;
 if(v.visibility.target==Target::all)return (v.source.context.valid_mask&active_target_valid)&&(v.source.sample.configured_mask&bit(Field::visibility_m))&&v.source.sample.configured[18]==9999;
 const auto& c=v.source.context;const auto t=v.visibility.target;
 const auto mask=active_target_valid|(t==Target::planet?planet_visibility_valid:(zone1_visibility_valid|zone1_station_valid));
 return (c.valid_mask&mask)==mask&&c.active_native_zone==static_cast<int>(t)&&(t==Target::planet?c.planet_visibility_m:c.zone1_visibility_m)==9999;
}
bool valid(const VisibilityRequest& v)noexcept{
 if(static_cast<unsigned>(v.command)>2||!process(v.client)||!process(v.server)||!v.session||!v.request||!v.qpc_frequency||v.qpc_frequency>1000000000000ULL||v.issued_qpc<=0||v.deadline_qpc<=v.issued_qpc||static_cast<std::uint64_t>(v.deadline_qpc-v.issued_qpc)>v.qpc_frequency*5||v.configured_readback_qpc<0||v.configured_readback_qpc>v.issued_qpc||(v.target!=Target::planet&&v.target!=Target::zone1&&v.target!=Target::all)||!station(v.zone1_station)||!std::isfinite(v.commanded_visibility_m))return false;
 if(!process(v.psx)&&!(v.command==VisibilityCommand::query&&v.psx.pid==0&&v.psx.creation_filetime==0))return false;
 if(v.command==VisibilityCommand::select)return v.configured_readback_qpc>0&&v.commanded_visibility_m>=10000&&v.commanded_visibility_m<=40000;
 return v.commanded_visibility_m>=0&&v.commanded_visibility_m<=9999;
}
bool valid(const VisibilityResult& v)noexcept{
 if(!process(v.server)||!v.session||!v.request||v.observed_qpc<=0||v.source_observation_qpc<0||v.application_qpc<0||(v.target!=Target::planet&&v.target!=Target::zone1&&v.target!=Target::all)||!target(v.active_target)||static_cast<unsigned>(v.phase)>4||static_cast<unsigned>(v.reason)>16||(v.valid_mask&~15u)||v.weather_requested>1||v.weather_owned>1||v.presentation_available>1)return false;
 const double values[]={v.commanded_visibility_m,v.configured_visibility_m,v.accepted_visibility_m,v.actual_visibility_m};
 for(unsigned i=0;i<4;++i){const auto x=values[i];if(!std::isfinite(x)||(v.valid_mask&(1u<<i)?(x<0||x>(i==0?40000:1000000)):x!=0))return false;}
 return true;
}
bool read_header(const std::uint8_t* p,std::size_t n,Header& out)noexcept {if(!p||n<environment::header_size)return false;Reader r{p,environment::header_size};const auto tag=r.u(4),wire=r.u(2);if(!((tag==magic&&wire==version)||(tag==0x33575850u&&wire==3)||(tag==0x34575850u&&wire==4)))return false;Header h;h.wire_version=static_cast<std::uint16_t>(wire);h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));if(static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>13||!h.payload_size||h.payload_size>environment::max_payload)return false;if((h.kind==Kind::source&&h.payload_size!=(wire==4?736u:wire==3?704u:672u))||(h.kind==Kind::presentation&&h.payload_size!=(wire==4?1576u:wire==3?1512u:720u))||(h.kind==Kind::applied_presentation&&h.payload_size!=(wire>=3?1632u:1592u)))return false;out=h;return true;}
#define CODEC(T,K) bool encode(const T& v,std::uint32_t s,environment::Frame& f)noexcept{return pack(v,Kind::K,s,f);}bool decode(std::span<const std::uint8_t> b,T& v)noexcept{return unpack(b,v);}
CODEC(Hello,hello) CODEC(Source,source) CODEC(Presentation,presentation) CODEC(AppliedPresentation,applied_presentation) CODEC(VisibilityRequest,visibility_request) CODEC(VisibilityResult,visibility_result)
#undef CODEC
#define BASE(T) bool encode(const environment::T& v,std::uint32_t s,environment::Frame& f)noexcept{if(!environment::encode(v,s,f))return false;f.bytes[3]='2';f.bytes[4]=2;return true;}bool decode(std::span<const std::uint8_t> b,environment::T& v)noexcept{return environment::decode(b.data(),b.size(),v);}
BASE(Subscribe) BASE(Sample) BASE(Control) BASE(Ack) BASE(Applied) BASE(Status) BASE(Capture)
#undef BASE
bool Framer::feed(const void* data,std::size_t size,Handler handler,void* context)noexcept {
 if(failed_||(!data&&size)||!handler)return false;auto p=static_cast<const std::uint8_t*>(data);
 while(size){auto n=std::min(size,needed_-used_);std::memcpy(bytes_.data()+used_,p,n);used_+=n;p+=n;size-=n;if(used_!=needed_)continue;Header h;if(!read_header(bytes_.data(),used_,h)){failed_=true;return false;}if(needed_==environment::header_size){needed_+=h.payload_size;continue;}if(!handler(context,h,bytes_.data()+environment::header_size,h.payload_size)){failed_=true;return false;}used_=0;needed_=environment::header_size;}
 return true;
}
}
