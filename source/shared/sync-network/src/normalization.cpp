#include <psx/sync/normalization.hpp>
#include <psx/bridge/protocol.hpp>
#include <psx/aircraft_sync/protocol.hpp>
#include <psx/environment/protocol.hpp>
#include <psx/environment/presentation.hpp>
#include <psx/status/protocol.hpp>
#include <psx/traffic_visual/protocol.hpp>
#include <psx/non_normal/protocol.hpp>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <cstdio>

namespace psx::sync {
struct Normalizer::Memo {
 struct Entry {std::array<std::uint8_t,2048> original{},mapped{};std::size_t size{};};
 std::array<Entry,128> entries;unsigned cursor{};
 std::array<bridge::MotionRequest,64> motion{};unsigned motion_cursor{};
 std::array<non_normal::Request,64> actors{};unsigned actor_cursor{};
 std::uint64_t status_connection{};std::int64_t status_submitted{};
 struct TrafficOrigin {psx::motion::InstanceId plugin{},owner{},producer{};std::uint64_t connection{},lease{},source_connection{},selection{},lifecycle{},sequence{},frame{};std::int64_t observed{};};
 std::array<TrafficOrigin,128> traffic{};unsigned traffic_cursor{};
 std::array<traffic_visual::Claim,64> claims{};unsigned claim_cursor{};
 std::array<traffic_visual::Capture,16> captures{};unsigned capture_cursor{};
 struct EnvironmentOrigin {environment::Sample sample{};environment::presentation::VisibilityPolicy policy{};std::uint64_t owner{};bool presentation{};};
 struct WeatherOrigin {std::uint64_t identity{};EnvironmentOrigin source{};};std::array<WeatherOrigin,2> weather;
 std::array<EnvironmentOrigin,128> environment;unsigned environment_cursor{};std::uint64_t environment_owner{};
};
Normalizer::Normalizer():memo_(std::make_unique<Memo>()){}
Normalizer::~Normalizer()=default;
void Normalizer::reset()noexcept{queries_={};query_cursor_=sequence_=0;for(auto& e:memo_->entries)e.size=0;memo_->cursor=0;memo_->motion={};memo_->motion_cursor=0;memo_->actors={};memo_->actor_cursor=0;memo_->status_connection=0;memo_->status_submitted=0;memo_->traffic={};memo_->traffic_cursor=0;memo_->claims={};memo_->claim_cursor=0;memo_->captures={};memo_->capture_cursor=0;memo_->weather={};memo_->environment={};memo_->environment_cursor=0;memo_->environment_owner=0;}
namespace {
// The member list is an audited clock schema, not name-based runtime reflection.
// Counters, identities, UTC milliseconds and native integration milliseconds are
// deliberately absent. QPC durations scale without adding an epoch offset.
struct Convert {
 ClockMap clock;std::int64_t now{};std::uint64_t source_frequency{},destination_frequency{};bool good{true},presentation{};
 template<class T>void stamp(T& t){if(!t)return;const auto p=clock.to_local(static_cast<std::int64_t>(t),now);if(!p.valid){good=false;return;}t=static_cast<T>(presentation?p.value:p.earliest);}
 template<class T>void duration(T& t){const long double n=static_cast<long double>(t)*destination_frequency/source_frequency;if(n<0||n>static_cast<long double>((std::numeric_limits<T>::max)())){good=false;return;}t=static_cast<T>(n);}
 template<class T,std::size_t N>void visit(std::array<T,N>& a){for(auto& x:a)visit(x);}
 template<class T>void visit(T& v){
#define STAMP(x) if constexpr(requires{v.x;}) stamp(v.x)
 STAMP(published_qpc);STAMP(selected_qpc);STAMP(decoded_qpc);STAMP(received_qpc);STAMP(callback_qpc);STAMP(generated_qpc);STAMP(requested_qpc);
 STAMP(applied_qpc);STAMP(presentation_qpc);STAMP(target_qpc);STAMP(boost_received_qpc);STAMP(pitch_received_qpc);
 STAMP(sample_start_qpc);STAMP(sample_end_qpc);STAMP(sample_qpc);STAMP(query_start_qpc);STAMP(query_end_qpc);
 STAMP(consumed_qpc);STAMP(surface_qpc);STAMP(source_sample_qpc);STAMP(step_start_qpc);STAMP(step_end_qpc);
 STAMP(observed_qpc);STAMP(phase_start_qpc);STAMP(call_returned_qpc);STAMP(notification_qpc);
 STAMP(source_floor_qpc);STAMP(source_observed_qpc);STAMP(left_observed_qpc);STAMP(right_observed_qpc);
 STAMP(left_received_qpc);STAMP(right_received_qpc);STAMP(sent_qpc);STAMP(issued_qpc);STAMP(deadline_qpc);
 STAMP(expires_qpc);STAMP(lease_expires_qpc);STAMP(applied_sample_qpc);STAMP(readback_qpc);
 STAMP(regeneration_begin_qpc);STAMP(regeneration_end_qpc);STAMP(settling_begin_qpc);STAMP(settling_end_qpc);
 STAMP(location_begin_qpc);STAMP(location_end_qpc);STAMP(observation_start_qpc);STAMP(observation_end_qpc);
 STAMP(observation_qpc);STAMP(copy_finish_qpc);STAMP(observation_start);STAMP(observation_end);
 STAMP(source_observation_qpc);STAMP(application_qpc);STAMP(configured_readback_qpc);STAMP(completed_qpc);STAMP(current_source_end_qpc);STAMP(weather_observed_qpc);
#undef STAMP
#define STAMPS(x) if constexpr(requires{v.x;})for(auto& n:v.x)stamp(n)
 STAMPS(stage_qpc);STAMPS(component_qpc);
#undef STAMPS
#define DURATION(x) if constexpr(requires{v.x;})duration(v.x)
 DURATION(hook_ticks);DURATION(begin_ticks);DURATION(step_ticks);DURATION(helper_ticks);
#undef DURATION
 // PluginStatus::received_qpc is an array, handled by the overload below.
#define CHILD(x) if constexpr(requires{v.x;})visit(v.x)
 CHILD(result);CHILD(request);CHILD(observation);CHILD(stamp);CHILD(presentation);CHILD(provenance);CHILD(source);CHILD(samples);CHILD(boost);CHILD(pitch);CHILD(pitch_anchor);
 CHILD(groups);CHILD(control);CHILD(ownership);CHILD(lease);CHILD(query);CHILD(reference);CHILD(stations);CHILD(assemblies);
 CHILD(attempts);CHILD(sites);CHILD(snapshot);CHILD(header);CHILD(base);CHILD(sample);CHILD(context);CHILD(applied);
 CHILD(reconstruction_trace);CHILD(left);CHILD(right);CHILD(events);CHILD(change);CHILD(weather);CHILD(local_visibility);
 CHILD(receive_to_callback);CHILD(callback_duration);CHILD(frame_interval);CHILD(pre_callback_duration);CHILD(post_callback_duration);
 CHILD(boost_receive_to_apply);CHILD(pitch_age);CHILD(output_age);CHILD(hold_duration);CHILD(exporter_sample_to_apply);CHILD(sampling_width);
 CHILD(receive_to_callback_max);CHILD(callback_duration_max);CHILD(frame_interval_max);
#undef CHILD
 if constexpr(requires{v.qpc_frequency;})v.qpc_frequency=destination_frequency;
 if constexpr(requires{v.frequency;})v.frequency=destination_frequency;
 }
 template<class T,std::size_t N>void stamp(std::array<T,N>& a){for(auto& n:a)stamp(n);}
 void visit(bridge::Distribution& v){duration(v.minimum);duration(v.maximum);duration(v.p50);duration(v.p95);duration(v.p99);}
 void visit(traffic_visual::Claim& v){
  // This acquisition floor is an immutable part of the lease identity. A
  // refining uncertainty bound must not manufacture a new domain on Renew.
  if(v.source_floor_qpc){const auto p=clock.to_local(v.source_floor_qpc,now);if(!p.valid)good=false;else v.source_floor_qpc=p.value;}
  v.frequency=destination_frequency;
 }
 void visit(diagnostics::Maximum& v){duration(v.ticks);stamp(v.qpc);stamp(v.previous_qpc);}
 void visit(diagnostics::Event& v){stamp(v.qpc);
  // Event.values is intentionally heterogeneous (counts, IDs and occasional
  // raw timing details). Do not guess units; retain them with explicit origin.
  char suffix[64]{};const auto written=std::snprintf(suffix,sizeof(suffix)," [remote raw details; QPC=%lluHz]",static_cast<unsigned long long>(source_frequency));
  if(written>0&&static_cast<std::size_t>(written)<v.text.size()){const auto n=static_cast<std::size_t>(written);const auto used=(std::min)(static_cast<std::size_t>(std::find(v.text.begin(),v.text.end(),'\0')-v.text.begin()),v.text.size()-1-n);std::copy_n(suffix,n,v.text.begin()+used);v.text[used+n]=0;}
 }
 void visit(traffic::Header& v){stamp(v.observed);stamp(v.finished);v.frequency=destination_frequency;}
};
template<class T,class F,class Decode,class Encode>
bool mapped(Convert& cv,std::span<const std::uint8_t>b,std::uint32_t sequence,Frame& result,Decode decode,Encode encode){
 T value;if(!decode(b.data()+16,b.size()-16,value))return false;cv.visit(value);if(!cv.good)return false;
 F frame;if(!encode(value,sequence,frame))return false;std::copy_n(frame.bytes.data(),frame.size,result.bytes.data());result.size=frame.size;return true;
}
// A reply cannot precede the request that caused it. Intersect the mapped
// clock interval with that known local lower bound, never with receipt time.
// A disjoint interval is a clock/protocol error rather than permission to retime.
bool causal(std::int64_t& mapped,const ClockMap& clock,std::int64_t remote,std::int64_t now,std::int64_t floor)noexcept {
 if(!floor||mapped>=floor)return true;const auto p=clock.to_local(remote,now);
 if(!p.valid||floor>p.latest)return false;mapped=floor;return true;
}
}
void Normalizer::sent(Family family,std::span<const std::uint8_t>b,std::int64_t submitted)noexcept {
 if(b.size()<16)return;const auto kind=unsigned(b[6])|(unsigned(b[7])<<8);const auto* p=b.data()+16;const auto n=b.size()-16;
 if(family==Family::terrain&&kind==3){terrain::Query q;if(terrain::decode(p,n,q))queries_[query_cursor_++%queries_.size()]={q,true};}
 else if(family==Family::observation&&kind==6){bridge::MotionRequest v;if(bridge::decode(p,n,v))memo_->motion[memo_->motion_cursor++%memo_->motion.size()]=v;}
 else if(family==Family::actor&&kind==1){non_normal::Request v;std::uint32_t sequence{};if(non_normal::decode(b,v,sequence))memo_->actors[memo_->actor_cursor++%memo_->actors.size()]=v;}
 else if(family==Family::status&&kind==2){status::Subscribe v;if(status::decode(p,n,v)){memo_->status_connection=v.connection_id;memo_->status_submitted=submitted;}}
 else if(family==Family::environment){
  if(kind==2){environment::Subscribe v;if(environment::decode(p,n,v))memo_->environment_owner=v.owner;}
  else if(kind==3){environment::Sample v;if(environment::decode(p,n,v))memo_->environment[memo_->environment_cursor++%memo_->environment.size()]={v,{},memo_->environment_owner,false};}
  else if(kind==10&&(b[3]=='2'||b[3]=='3'||b[3]=='4')){environment::presentation::Presentation v;if(environment::presentation::decode({p,n},v)){
   memo_->environment[memo_->environment_cursor++%memo_->environment.size()]={v.source.sample,v.visibility,v.visibility.owner,true};
   if(v.weather.identity)memo_->weather[v.weather.identity%memo_->weather.size()]={v.weather.identity,{v.weather.source.sample,v.weather.visibility,v.weather.visibility.owner,true}};
  }}
 }
 else if(family==Family::traffic){
  if(kind==2){traffic_visual::Claim v;if(traffic_visual::decode(p,n,v))memo_->claims[memo_->claim_cursor++%memo_->claims.size()]=v;}
  else if(kind==7){traffic_visual::Capture v;if(traffic_visual::decode(p,n,v))memo_->captures[memo_->capture_cursor++%memo_->captures.size()]=v;}
  else if(kind==3){traffic_visual::Bundle v;if(traffic_visual::decode(p,n,v)){const auto& s=v.source;const auto& h=s.snapshot.header;memo_->traffic[memo_->traffic_cursor++%memo_->traffic.size()]={v.plugin,v.owner,s.producer,v.connection,v.lease,s.connection_id,s.selection_epoch,h.lifecycle,h.sequence,h.frame,static_cast<std::int64_t>(h.observed)};}}
 }
}
bool Normalizer::received(const Received& raw,Endpoint& endpoint,Frame& result)noexcept {
 const auto context=endpoint.context();const auto now=native_qpc();return received(raw,context,now,result);
}
bool Normalizer::received(const Received& raw,const LinkContext& context,std::int64_t now,Frame& result)noexcept {
 const auto& s=context.status;
 if(!s.connected||!s.clock_valid||raw.header.connection!=s.connection||raw.header.producer!=s.peer||raw.header.frequency!=s.remote_frequency||
    !raw.source_time.valid||raw.source_time.generation!=context.clock.generation())return false;
 const auto f=unsigned(raw.header.family);if(f>=s.local_services.size()||raw.header.source_service!=s.remote_services[f]||raw.header.target_service!=s.local_services[f])return false;
 return received(raw,context.clock,now,s.local_frequency,s.remote_frequency,result);
}
bool Normalizer::received(const Received& raw,const ClockMap& clock,std::int64_t now,std::uint64_t local_frequency,std::uint64_t remote_frequency,Frame& result)noexcept {
 if(!clock.current(now)||!remote_frequency||!local_frequency)return false;
 if(raw.size<16||raw.size>raw.payload.size())return false;const auto b=std::span(raw.payload).first(raw.size);
 // A clock adapter may not silently upgrade an incompatible inner protocol.
#define HEADER(NS) [&]{NS::Header h;return NS::read_header(b.data(),16,h)&&h.sequence&&b.size()==16+h.payload_size;}()
 bool framed=false;switch(raw.header.family){
 case Family::observation:framed=HEADER(bridge);break;case Family::terrain:framed=HEADER(terrain);break;
 case Family::aircraft:framed=HEADER(aircraft_sync);break;case Family::environment:framed=(b[3]=='2'||b[3]=='3'||b[3]=='4')?HEADER(environment::presentation):HEADER(environment);break;
 case Family::status:framed=HEADER(status);break;case Family::traffic:framed=HEADER(traffic_visual);break;
 case Family::actor:framed=true;break;default:break;}
#undef HEADER
 if(!framed)return false;
 const bool memo=(raw.header.flags&ordered)&&b.size()<=2048;
 if(memo)for(const auto& entry:memo_->entries)if(entry.size==b.size()&&std::equal(b.begin(),b.begin()+8,entry.original.begin())&&std::equal(b.begin()+16,b.end(),entry.original.begin()+16)){
  result.size=entry.size;std::copy_n(entry.mapped.data(),entry.size,result.bytes.data());const auto sequence=++sequence_;for(unsigned i=0;i<4;++i)result.bytes[12+i]=std::uint8_t(sequence>>(8*i));return true;
 }
 Convert cv{clock,now,remote_frequency,local_frequency};const auto kind=unsigned(b[6])|(unsigned(b[7])<<8);result.size=0;
 // Geometry/audio bracketing uses the fixed affine center, never a changing
 // uncertainty envelope. SDK freshness checks add the separately published bound.
 // Query references repeat the same immutable source identity at a different
 // query cadence. Their affine center must match the physical source timeline;
 // SDK source-age checks independently include the current uncertainty margin.
 cv.presentation=(raw.header.family==Family::observation&&(kind==3||kind==9||kind==11||kind==12))||
     (raw.header.family==Family::terrain&&kind==3)||
     (raw.header.family==Family::aircraft&&kind==3)||(raw.header.family==Family::traffic&&kind==3);
#define MAP(NS,T) mapped<NS::T,NS::Frame>(cv,b,++sequence_,result,[](const std::uint8_t*p,std::size_t n,NS::T&v){return NS::decode(p,n,v);},[](const NS::T&v,std::uint32_t s,NS::Frame&f){return NS::encode(v,s,f);})
#define MAP2(NS,T) mapped<NS::T,environment::Frame>(cv,b,++sequence_,result,[](const std::uint8_t*p,std::size_t n,NS::T&v){return environment::presentation::decode({p,n},v);},[](const NS::T&v,std::uint32_t s,environment::Frame&f){return environment::presentation::encode(v,s,f);})
 bool ok=false;
 switch(raw.header.family){
 case Family::observation:switch(kind){
 case 1:ok=MAP(bridge,Hello);break;case 2:ok=MAP(bridge,SessionStatus);break;case 3:ok=MAP(bridge,ObservationMessage);break;
 case 4:ok=MAP(bridge,PluginStatus);break;case 5:ok=MAP(bridge,DiagnosticBatch);break;case 6:ok=MAP(bridge,MotionRequest);break;
 case 7:ok=MAP(bridge,MotionAck);break;case 8:ok=MAP(bridge,MotionStatus);break;case 9:ok=MAP(bridge,MotionWindow);break;
 case 10:ok=MAP(bridge,SourceSelection);break;case 11:ok=MAP(bridge,CompletePoseWindow);break;case 12:ok=MAP(bridge,AudioState);break;
 case 13:ok=MAP(bridge,AudioTransition);break;case 14:ok=MAP(bridge,AudioControl);break;case 15:ok=MAP(bridge,AudioStatus);break;
 case 16:ok=MAP(bridge,ViewRequest);break;case 17:ok=MAP(bridge,ViewReply);break;case 18:ok=MAP(bridge,CoverageRequest);break;case 19:ok=MAP(bridge,CoverageReply);break;}break;
 case Family::terrain:switch(kind){
 case 1:ok=MAP(terrain,Hello);break;case 2:ok=MAP(terrain,Subscribe);break;case 3:ok=MAP(terrain,Query);break;
 case 4:ok=MAP(terrain,Surface);break;case 5:ok=MAP(terrain,Control);break;case 6:ok=MAP(terrain,Ack);break;
 case 7:ok=MAP(terrain,Consumed);break;case 8:ok=MAP(terrain,Diagnostics);break;case 9:ok=MAP(terrain,ProbeDetail);break;
 case 10:ok=MAP(terrain,SupportDetail);break;case 11:ok=MAP(terrain,RenderDetail);break;case 12:ok=MAP(terrain,Capture);break;
 case 13:ok=MAP(terrain,ConstraintDetail);break;case 14:ok=MAP(terrain,FrameDetail);break;}
 if(ok&&(kind==4||kind==8||kind==9||kind==11)){
  const auto restore=[&](auto& v){for(const auto& q:queries_)if(q.valid&&q.query.owner==v.query.owner&&q.query.terrain_epoch==v.query.terrain_epoch&&q.query.query_id==v.query.query_id){v.query=q.query;return true;}return false;};
  terrain::Frame f;
  if(kind==4){terrain::Surface v;ok=terrain::decode(result.bytes.data()+16,result.size-16,v)&&restore(v)&&terrain::encode(v,sequence_,f);}
  else if(kind==8){terrain::Diagnostics v;ok=terrain::decode(result.bytes.data()+16,result.size-16,v)&&restore(v)&&terrain::encode(v,sequence_,f);}
  else if(kind==9){terrain::ProbeDetail v;ok=terrain::decode(result.bytes.data()+16,result.size-16,v)&&restore(v)&&terrain::encode(v,sequence_,f);}
  else {terrain::RenderDetail v;ok=terrain::decode(result.bytes.data()+16,result.size-16,v)&&restore(v)&&terrain::encode(v,sequence_,f);}
  if(ok){result.size=f.size;std::copy_n(f.bytes.data(),f.size,result.bytes.data());}
 }break;
 case Family::aircraft:switch(kind){
 case 1:ok=MAP(aircraft_sync,Hello);break;case 2:ok=MAP(aircraft_sync,Subscribe);break;case 3:ok=MAP(aircraft_sync,Sample);break;
 case 4:ok=MAP(aircraft_sync,Control);break;case 5:ok=MAP(aircraft_sync,Ack);break;case 6:ok=MAP(aircraft_sync,Applied);break;
 case 7:ok=MAP(aircraft_sync,Status);break;case 8:ok=MAP(aircraft_sync,Capture);break;}break;
 case Family::environment:
 if((b[3]=='2'||b[3]=='3'||b[3]=='4')){namespace p=environment::presentation;switch(kind){
 case 1:ok=MAP2(p,Hello);break;case 2:ok=MAP2(environment,Subscribe);break;case 3:ok=MAP2(environment,Sample);break;case 4:ok=MAP2(environment,Control);break;
 case 5:ok=MAP2(environment,Ack);break;case 6:ok=MAP2(environment,Applied);break;case 7:ok=MAP2(environment,Status);break;case 8:ok=MAP2(environment,Capture);break;
 case 9:ok=MAP2(p,Source);break;case 10:ok=MAP2(p,Presentation);break;case 11:ok=MAP2(p,AppliedPresentation);break;}}
 else switch(kind){case 1:ok=MAP(environment,Hello);break;case 2:ok=MAP(environment,Subscribe);break;case 3:ok=MAP(environment,Sample);break;
 case 4:ok=MAP(environment,Control);break;case 5:ok=MAP(environment,Ack);break;case 6:ok=MAP(environment,Applied);break;
 case 7:ok=MAP(environment,Status);break;case 8:ok=MAP(environment,Capture);break;}break;
 case Family::status:switch(kind){case 1:ok=MAP(status,Hello);break;case 2:ok=MAP(status,Subscribe);break;case 3:ok=MAP(status,Snapshot);break;case 4:ok=MAP(status,Heartbeat);break;}break;
 case Family::traffic:switch(kind){case 1:ok=MAP(traffic_visual,Hello);break;case 2:ok=MAP(traffic_visual,Claim);break;case 3:ok=MAP(traffic_visual,Bundle);break;
 case 4:ok=MAP(traffic_visual,Ack);break;case 5:ok=MAP(traffic_visual,Heartbeat);break;case 6:ok=MAP(traffic_visual,Report);break;
 case 7:ok=MAP(traffic_visual,Capture);break;case 8:ok=MAP(traffic_visual,Detail);break;}break;
 case Family::actor:{non_normal::Frame f;std::uint32_t incoming{};if(kind==1){non_normal::Request v;if(non_normal::decode(b,v,incoming)){cv.visit(v);ok=cv.good&&non_normal::encode(v,++sequence_,f);}}
  else if(kind==2){non_normal::Result v;if(non_normal::decode(b,v,incoming)){cv.visit(v);ok=cv.good&&non_normal::encode(v,++sequence_,f);}}
  if(ok){result.size=f.size;std::copy_n(f.bytes.data(),f.size,result.bytes.data());}break;}
 default:break;
 }
#undef MAP
#undef MAP2
 const auto save=[&](const auto& frame){result.size=frame.size;std::copy_n(frame.bytes.data(),frame.size,result.bytes.data());};
 if(ok&&raw.header.family==Family::environment&&(kind==6||kind==11)){
  // Source is an echo of coordinator-owned evidence, not a new plugin clock.
  // Restore only an issued, byte-exact nonclock sample and its selected policy.
  // Double conservative projection otherwise changes identity on every return.
  namespace ep=environment::presentation;
  environment::Applied source,v;ep::AppliedPresentation envelope;environment::Frame frame;
  ok=kind==11?ep::decode(b.subspan(16),envelope):environment::decode(b.data()+16,b.size()-16,source);
  if(ok){if(kind==11)source=envelope.applied;v=source;Convert mapping{clock,now,remote_frequency,local_frequency};mapping.visit(v);ok=mapping.good;}
  bool found=false;
  const auto restore=[&](const Memo::EnvironmentOrigin& origin){
   if(!origin.sample.sequence||origin.owner!=source.owner||origin.presentation!=(kind==11)||
      (origin.presentation&&origin.policy!=envelope.visibility))return false;
   auto echo=source.source;echo.sample_start_qpc=origin.sample.sample_start_qpc;
   echo.sample_end_qpc=origin.sample.sample_end_qpc;echo.received_qpc=origin.sample.received_qpc;
   if(!ep::same_sample(echo,origin.sample))return false;v.source=origin.sample;return true;
  };
  if(ok&&envelope.weather_target){for(const auto& held:memo_->weather)if(held.identity==envelope.weather_target&&restore(held.source)){found=true;break;}}
  else if(ok)for(const auto& origin:memo_->environment)if(restore(origin)){found=true;break;}
  // Old target acknowledgments can outlive our two retained target records.
  // They cannot establish current ownership; the reducer classifies their ID
  // before inspecting source correlation. Do not tear down a healthy lane.
  if(ok&&!found&&envelope.weather_target&&source.owner==memo_->environment_owner){
   std::uint64_t newest{};for(const auto& held:memo_->weather)newest=(std::max)(newest,held.identity);
   if(envelope.weather_target<newest)return true;
  }
  ok=ok&&found&&causal(v.applied_qpc,clock,source.applied_qpc,now,v.source.sample_end_qpc);
  if(ok&&v.readback_qpc)ok=causal(v.readback_qpc,clock,source.readback_qpc,now,v.applied_qpc);
  if(ok&&envelope.weather_target){
   bool current=false;for(const auto& origin:memo_->environment)if(origin.owner==source.owner&&origin.sample.producer==source.source.producer&&origin.sample.sequence==envelope.current_sequence&&origin.sample.lifecycle==envelope.current_lifecycle){
    envelope.current_source_end_qpc=origin.sample.sample_end_qpc;current=true;break;}
   auto observed=envelope.weather_observed_qpc;Convert mapping{clock,now,remote_frequency,local_frequency};mapping.stamp(observed);
   ok=current&&mapping.good&&causal(observed,clock,envelope.weather_observed_qpc,now,envelope.current_source_end_qpc);envelope.weather_observed_qpc=observed;
  }
  if(ok){if(kind==11){envelope.applied=v;ok=ep::encode(envelope,sequence_,frame);}
   else ok=(b[3]=='2'||b[3]=='3'||b[3]=='4')?ep::encode(v,sequence_,frame):environment::encode(v,sequence_,frame);
   if(ok)save(frame);}
 }
 if(ok&&raw.header.family==Family::observation&&kind==7){
  bridge::MotionAck source,v;bridge::Frame frame;ok=bridge::decode(b.data()+16,b.size()-16,source)&&bridge::decode(result.bytes.data()+16,result.size-16,v);
  if(ok)for(const auto& request:memo_->motion)if(request.operation_id&&request.operation_id==v.operation_id&&request.connection_id==v.connection_id&&request.plugin_load_id==v.plugin_load_id&&request.enable_id==v.enable_id&&request.aircraft_generation==v.aircraft_generation){ok=causal(v.generated_qpc,clock,source.generated_qpc,now,request.requested_qpc);break;}
  if(ok){ok=bridge::encode(v,sequence_,frame);if(ok)save(frame);}
 }
 if(ok&&raw.header.family==Family::status&&(kind==3||kind==4)){
  status::Frame frame;
  if(kind==3){status::Snapshot source,v;ok=status::decode(b.data()+16,b.size()-16,source)&&status::decode(result.bytes.data()+16,result.size-16,v);
   if(ok&&v.connection_id==memo_->status_connection)ok=causal(v.observed_qpc,clock,source.observed_qpc,now,memo_->status_submitted);
   if(ok)ok=status::encode(v,sequence_,frame);
  }else{status::Heartbeat source,v;ok=status::decode(b.data()+16,b.size()-16,source)&&status::decode(result.bytes.data()+16,result.size-16,v);
   if(ok&&v.connection_id==memo_->status_connection)ok=causal(v.generated_qpc,clock,source.generated_qpc,now,memo_->status_submitted);
   if(ok)ok=status::encode(v,sequence_,frame);
  }if(ok)save(frame);
 }
 if(ok&&raw.header.family==Family::actor&&kind==2){
  non_normal::Result source,v;non_normal::Frame frame;std::uint32_t incoming{};
  ok=non_normal::decode(b,source,incoming)&&non_normal::decode({result.bytes.data(),result.size},v,incoming);
  if(ok)for(const auto& request:memo_->actors)if(request.request&&request.request==v.request&&request.session==v.session&&request.command==v.command&&request.target==v.server){ok=causal(v.observed_qpc,clock,source.observed_qpc,now,request.issued_qpc);break;}
  if(ok){ok=non_normal::encode(v,sequence_,frame);if(ok)save(frame);}
 }
 if(ok&&raw.header.family==Family::traffic&&(kind==4||kind==6||kind==8)){
  // These are echoes of coordinator-owned source samples. Mapping them twice
  // destroys exact identities and makes valid reports appear unsolicited.
  const auto restore=[&](const auto& report,std::uint64_t sequence,std::int64_t& observed){
   if(!sequence)return true;
   for(const auto& origin:memo_->traffic)if(origin.sequence==sequence&&origin.plugin==report.plugin&&origin.owner==report.owner&&origin.connection==report.connection&&origin.lease==report.lease&&origin.producer==report.source_producer&&origin.source_connection==report.source_connection&&origin.selection==report.source_selection&&origin.lifecycle==report.lifecycle){observed=origin.observed;return true;}
   return false;
  };
  const auto floor=[&](const auto& report){for(const auto& claim:memo_->claims)if(claim.request&&claim.request==report.request&&claim.plugin==report.plugin&&claim.owner==report.owner&&claim.connection==report.connection&&claim.lease==report.lease)return claim.source_floor_qpc;return std::int64_t{};};
  traffic_visual::Frame frame;
  if(kind==8){traffic_visual::Detail source,v;ok=traffic_visual::decode(b.data()+16,b.size()-16,source)&&traffic_visual::decode(result.bytes.data()+16,result.size-16,v);
   if(ok)ok=restore(v,v.source_sequence,v.source_observed_qpc)&&restore(v,v.left_sequence,v.left_observed_qpc)&&restore(v,v.right_sequence,v.right_observed_qpc);
   auto lower=ok?floor(v):0;std::int64_t capture_floor{};
   if(ok)for(const auto& capture:memo_->captures)if(capture.enabled&&capture.generation==v.capture_generation&&capture.plugin==v.plugin&&capture.connection==v.connection&&(!capture_floor||capture.sent_qpc<capture_floor))capture_floor=capture.sent_qpc;
   lower=(std::max)({lower,capture_floor,v.source_observed_qpc});
   if(ok&&(v.flags&traffic_visual::detail_pose_valid)){
    ok=causal(v.left_received_qpc,clock,source.left_received_qpc,now,v.left_observed_qpc)&&causal(v.right_received_qpc,clock,source.right_received_qpc,now,v.right_observed_qpc);
    // Presentation is a position within the echoed source bracket. Restore
    // that coordinate from its original endpoints and unchanged interpolation.
    v.presentation_qpc=v.left_observed_qpc+static_cast<std::int64_t>(static_cast<long double>(v.right_observed_qpc-v.left_observed_qpc)*v.alpha);
    lower=(std::max)({lower,v.left_received_qpc,v.right_received_qpc,v.presentation_qpc});
   }
   if(ok)ok=causal(v.generated_qpc,clock,source.generated_qpc,now,lower)&&traffic_visual::encode(v,sequence_,frame);
  }else{traffic_visual::Report source,v;ok=traffic_visual::decode(b.data()+16,b.size()-16,source)&&traffic_visual::decode(result.bytes.data()+16,result.size-16,v);
   if(ok)ok=restore(v,v.source_sequence,v.source_observed_qpc)&&causal(v.generated_qpc,clock,source.generated_qpc,now,(std::max)(floor(v),v.source_observed_qpc));
   if(ok&&kind==4){traffic_visual::Ack ack;static_cast<traffic_visual::Report&>(ack)=v;ok=traffic_visual::encode(ack,sequence_,frame);}
   else if(ok)ok=traffic_visual::encode(v,sequence_,frame);
  }if(ok)save(frame);
 }
 if(ok&&memo&&result.size==b.size()){auto& entry=memo_->entries[memo_->cursor++%memo_->entries.size()];entry.size=b.size();std::copy(b.begin(),b.end(),entry.original.begin());std::copy_n(result.bytes.data(),result.size,entry.mapped.data());}
 return ok;
}
}
