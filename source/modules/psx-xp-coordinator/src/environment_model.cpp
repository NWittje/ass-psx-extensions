#include "coordinator/environment.hpp"
#include "coordinator/environment_time.hpp"
#include <algorithm>
#include <psx/non_normal/pipe.hpp>
namespace coordinator {
namespace w=environment_wire;
namespace v2=presentation_wire;
namespace {
bool domain(const w::Sample& a,const w::Sample& b) noexcept {return a.producer==b.producer&&a.lifecycle==b.lifecycle;}
bool identical(const w::Sample& a,const w::Sample& b) noexcept {
 return domain(a,b)&&a.configuration==b.configuration&&a.sequence==b.sequence&&a.writer_version==b.writer_version&&a.frame_sequence==b.frame_sequence&&a.sample_start_qpc==b.sample_start_qpc&&a.sample_end_qpc==b.sample_end_qpc&&a.received_qpc==b.received_qpc&&a.utc_millis==b.utc_millis&&a.configured_utc_millis==b.configured_utc_millis&&a.flags==b.flags&&a.valid_mask==b.valid_mask&&a.configured_mask==b.configured_mask&&a.values==b.values&&a.configured==b.configured;
}
}
bool encode_environment_dispatch(const EnvironmentDispatch& d,w::Role role,std::uint32_t sequence,w::Frame& f) noexcept {
 if(d.version2){
  switch(d.kind){
  case w::Kind::control:return v2::encode(d.control,sequence,f);
  case w::Kind::capture:return v2::encode(d.capture,sequence,f);
  case w::Kind::hello:return v2::encode(v2::Hello{d.hello,d.capabilities},sequence,f);
  case w::Kind::applied:if(d.weather_report.weather_target)return v2::encode(d.weather_report,sequence,f);return d.visibility.owner?v2::encode(v2::AppliedPresentation{d.applied,d.visibility},sequence,f):v2::encode(d.applied,sequence,f);
  case w::Kind::sample:if(role==w::Role::plugin&&!(d.capabilities&v2::visibility_presentation))return v2::encode(d.sample,sequence,f);return role==w::Role::exporter?v2::encode(v2::Source{d.sample,d.context,d.change,d.local_visibility},sequence,f):v2::encode(v2::Presentation{{d.sample,d.context,d.change,d.local_visibility},d.visibility,d.weather},sequence,f);
  default:return false;
  }
 }
 return d.kind==w::Kind::control?w::encode(d.control,sequence,f):d.kind==w::Kind::capture?w::encode(d.capture,sequence,f):d.kind==w::Kind::hello?w::encode(d.hello,sequence,f):d.kind==w::Kind::applied?w::encode(d.applied,sequence,f):w::encode(d.sample,sequence,f);
}
void EnvironmentModel::configure(std::uint64_t frequency,std::uint64_t owner) noexcept {frequency_=frequency;view_.owner=owner;visibility_.configure(frequency);}
void EnvironmentModel::associate_visibility(ProcessIdentity psx) noexcept {if(psx!=visibility_psx_)selection_floor_qpc_=0;visibility_psx_=psx;visibility_.associate(psx);view_.visibility_selections=visibility_.selections();view_.source_context={};dirty_=view_.source_available;}
void EnvironmentModel::visibility_ios(std::uint64_t epoch,std::uint64_t session,bool connected) noexcept {visibility_.ios(epoch,session,connected);view_.visibility_selections=visibility_.selections();dirty_=view_.source_available;}
void EnvironmentModel::invalidate_visibility(std::int64_t now) noexcept {selection_floor_qpc_=(std::max)(selection_floor_qpc_,now);visibility_.clear();view_.source_context={};view_.visibility_selections=visibility_.selections();dirty_=view_.source_available;}

v2::VisibilityResult EnvironmentModel::visibility_request(const v2::VisibilityRequest& request,std::int64_t now) noexcept {
 policy_now_=now;
 v2::VisibilityResult result;const auto self=psx::non_normal::current_process();result.server={self.pid,self.creation_filetime};
 result.session=request.session;result.request=request.request;result.target=request.target;result.observed_qpc=now;
 const auto reject=[&](v2::VisibilityReason reason){result.phase=v2::VisibilityPhase::rejected;result.reason=reason;return result;};
 if(!v2::valid(request)||request.server!=result.server||request.qpc_frequency!=frequency_||request.issued_qpc>now||!visibility_.session(request.session))return reject(v2::VisibilityReason::identity);
 if(now>=request.deadline_qpc)return reject(v2::VisibilityReason::expired);
 if(request.target!=v2::Target::planet&&request.target!=v2::Target::zone1&&request.target!=v2::Target::all)return reject(v2::VisibilityReason::malformed);
 const bool matching=request.psx.pid==visibility_psx_.pid&&request.psx.creation_filetime==visibility_psx_.creation_filetime;
 if(request.command!=v2::VisibilityCommand::query){
  if(!visibility_psx_.pid)return reject(v2::VisibilityReason::binding);
  if(!matching)return reject(v2::VisibilityReason::identity);
  if(request.command==v2::VisibilityCommand::select&&request.configured_readback_qpc<=selection_floor_qpc_)return reject(v2::VisibilityReason::lifecycle);
  if(request.target==v2::Target::all&&request.command==v2::VisibilityCommand::select&&!(view_.plugin_capabilities&v2::local_visibility_target))return reject(v2::VisibilityReason::unsupported);
  const auto metres=request.command==v2::VisibilityCommand::select?request.commanded_visibility_m:0;
  if(!visibility_.select(static_cast<VisibilityTarget>(request.target),metres,visibility_psx_,request.configured_readback_qpc,request.zone1_station,now))return reject(v2::VisibilityReason::malformed);
  view_.visibility_selections=visibility_.selections();dirty_=view_.source_available;weather_policy_pending_=true;
 }else if(request.psx.pid&&!matching)return reject(v2::VisibilityReason::identity);
 result.selection_revision=visibility_.revision();const auto index=static_cast<std::size_t>(request.target);
 const auto& selected=visibility_.selections()[index];if(selected.metres){result.valid_mask|=v2::commanded_valid;result.commanded_visibility_m=selected.metres;result.phase=v2::VisibilityPhase::selected;}else result.phase=v2::VisibilityPhase::released;
 const auto& g=view_.groups[1];const bool source_current=view_.source_available&&fresh(view_.source,now);
 result.weather_requested=g.requested&&view_.following&&source_current;
 result.weather_owned=g.owned&&result.weather_requested&&now<g.expires_qpc;
 result.presentation_available=view_.plugin_connected&&view_.plugin_v2&&(view_.plugin_capabilities&v2::visibility_presentation);
 const auto& context=view_.source_context;const auto configured=index==0?v2::planet_visibility_valid:v2::zone1_visibility_valid;
 const bool context_current=source_current&&context.observation_end_qpc>0&&now>=context.observation_end_qpc&&static_cast<long double>(now-context.observation_end_qpc)*1000<static_cast<long double>(frequency_)*w::source_stale_ms&&view_.exporter_v2&&(view_.exporter_capabilities&v2::source_context)&&context.source_sequence==view_.source.sequence;
 if(context_current&&(context.valid_mask&v2::active_target_valid))result.active_target=context.active_native_zone==0?v2::Target::planet:context.active_native_zone==1?v2::Target::zone1:v2::Target::other;
 if(context_current&&(context.valid_mask&configured)){result.valid_mask|=v2::configured_valid;result.configured_visibility_m=index==0?context.planet_visibility_m:context.zone1_visibility_m;}
 if(context_current&&request.target==v2::Target::all&&(context.valid_mask&v2::active_target_valid)&&(view_.source.configured_mask&w::bit(w::Field::visibility_m))){result.valid_mask|=v2::configured_valid;result.configured_visibility_m=view_.source.configured[18];}
 if(source_current)result.source_observation_qpc=context_current?context.observation_end_qpc:view_.source.sample_end_qpc;
 result.reason=v2::VisibilityReason::none;
 if(!visibility_psx_.pid)result.reason=v2::VisibilityReason::disconnected;
 else if(!source_current)result.reason=v2::VisibilityReason::source_stale;
 else if(!result.presentation_available)result.reason=v2::VisibilityReason::plugin_unavailable;
 else if(!view_.following)result.reason=v2::VisibilityReason::not_following;
 else if(selected.metres&&!(result.valid_mask&v2::configured_valid))result.reason=v2::VisibilityReason::context_unavailable;
 else if(selected.metres&&context.observation_start_qpc<=selected.configured_readback_qpc){result.phase=v2::VisibilityPhase::pending;result.reason=v2::VisibilityReason::pending_readback;}
 else if(selected.metres&&request.target!=v2::Target::all&&result.active_target!=request.target)result.reason=v2::VisibilityReason::inactive_target;
 const auto& policy=view_.applied_visibility_policy;const auto field=w::bit(w::Field::visibility_m);
 const auto recent=[&](std::int64_t observed){return observed>0&&now>=observed&&static_cast<std::uint64_t>(now-observed)<frequency_;};
 // PXW3 intentionally retains the target's original source time. Admission has
 // correlated its fresh current-source/SDK proof; query status must use that
 // proof rather than expire the unchanged weather target after one second.
 const bool report_current=g.weather_target?
  (g.weather_target==weather_target_.identity&&g.current_lifecycle==view_.source.lifecycle&&
   domain(g.applied.source,weather_target_.source.sample)&&g.current_sequence&&g.current_sequence<=view_.source.sequence&&
   recent(g.current_source_end_qpc)&&recent(g.weather_observed_qpc)&&recent(g.report_received_qpc)):
  fresh(g.applied.source,now);
 if(result.weather_owned&&g.applied_available&&view_.applied_visibility_available&&report_current&&
  g.applied.phase==w::Phase::owned&&policy.selection_revision==result.selection_revision&&
  (policy.target==request.target||request.target==v2::Target::all||
   (policy.kind!=v2::PolicyKind::extended&&(policy.target==v2::Target::other||policy.target==v2::Target::unknown)&&
    result.active_target==request.target&&identical(g.applied.source,view_.source)))){
  result.application_qpc=g.applied.applied_qpc;
  if(g.applied.accepted_mask&field){result.valid_mask|=v2::accepted_valid;result.accepted_visibility_m=g.applied.accepted[18];}
  // A different weather field can be unsupported without invalidating the
  // independently accepted visibility. Aircraft readback has its own age.
  if((g.applied.actual_mask&field)&&recent(g.applied.readback_qpc)){result.valid_mask|=v2::actual_valid;result.actual_visibility_m=g.applied.actual[18];}
 }
 return result;
}

bool EnvironmentModel::fresh(const w::Sample& s,std::int64_t now) const noexcept {return view_.exporter_connected&&frequency_&&s.sequence&&now>=s.received_qpc&&now>=s.sample_end_qpc&&static_cast<long double>(now-s.sample_end_qpc)*1000<static_cast<long double>(frequency_)*w::source_stale_ms;}
bool EnvironmentModel::command(std::size_t i,w::Action action,std::int64_t now) noexcept {
 auto& g=view_.groups[i];if(!view_.plugin_connected||!view_.source.sequence)return false;
 if(action==w::Action::release){std::array<w::Control,16> kept{};std::size_t count{};for(std::size_t n=0;n<size_;++n){const auto& c=commands_[(head_+n)%commands_.size()];if(!(c.groups&(1u<<i)))kept[count++]=c;}commands_=kept;head_=0;size_=count;}
 if(size_==commands_.size()){++view_.overflows;view_.plugin_connected=false;head_=size_=0;dirty_=false;for(auto& v:view_.groups){v.requested=v.owned=v.pending=false;v.reason=w::Reason::overflow;}return false;}
 w::Control c;const auto& s=view_.source;c.producer=s.producer;c.owner=view_.owner;c.operation=++operation_;c.lifecycle=s.lifecycle;c.configuration=s.configuration;c.action=action;c.groups=1u<<i;
 issued_controls_[(c.operation-1)%issued_controls_.size()]=c;commands_[(head_+size_)%commands_.size()]=c;++size_;++view_.controls;pending_[i]=c;g.pending=true;g.renewed_qpc=now;
 if(action==w::Action::release){g.requested=g.owned=false;g.phase=w::Phase::released;}else {g.requested=true;if(action==w::Action::acquire)g.phase=w::Phase::observing;}
 return true;
}
void EnvironmentModel::release(std::size_t i,w::Reason reason,std::int64_t now,bool baseline) noexcept {auto& g=view_.groups[i];if((g.requested||g.owned||g.pending)&&!(g.pending&&pending_[i].action==w::Action::release))command(i,w::Action::release,now);g.requested=g.owned=g.applied_available=false;g.phase=w::Phase::released;g.reason=reason;if(baseline)g.baseline_floor=now;}
void EnvironmentModel::following(bool enabled,std::int64_t now) noexcept {if(view_.following==enabled)return;view_.following=enabled;if(!enabled){weather_target_={};view_.weather_target=view_.weather_target_sequence=view_.weather_target_revision=0;view_.weather_target_source_qpc=0;weather_policy_pending_=false;for(std::size_t i=0;i<2;i++)release(i,w::Reason::stopped,now,false);dirty_=false;}else {for(auto& g:view_.groups)g.blocked=false;dirty_=view_.source_available;}}
void EnvironmentModel::lose(w::Role role,std::int64_t now) noexcept {
 if(role==w::Role::exporter){view_.source_context={};view_.visibility_selections=visibility_.selections();view_.exporter_v2=false;view_.exporter_capabilities=0;view_.exporter_connected=false;view_.source_available=false;needs_baseline_=true;source_floor_=now;announced_lifecycle_=announced_configuration_=0;dirty_=false;for(std::size_t i=0;i<2;i++){view_.groups[i].available=false;release(i,w::Reason::disconnected,now,true);}}
 else {view_.plugin_v2=false;view_.plugin_capabilities=0;view_.applied_visibility_available=false;view_.plugin_connected=false;head_=size_=0;dirty_=false;for(auto& g:view_.groups){g.requested=g.owned=g.pending=g.applied_available=g.blocked=false;g.phase=w::Phase::released;g.reason=w::Reason::disconnected;g.baseline_floor=now;}}
}
bool EnvironmentModel::issued(const w::Control& c) const noexcept {if(c.owner!=view_.owner||!c.operation||c.operation>operation_)return false;const auto& s=issued_controls_[(c.operation-1)%issued_controls_.size()];return s.operation==c.operation&&c.producer==s.producer&&c.lifecycle==s.lifecycle&&c.configuration==s.configuration&&c.action==s.action&&c.groups==s.groups&&c.duration_ms==s.duration_ms;}
bool EnvironmentModel::issued(const w::Sample& s) const noexcept {for(const auto& old:issued_samples_)if(old.sequence&&identical(old,s))return true;return false;}
bool EnvironmentModel::retained_report(const w::Applied& a,const v2::VisibilityPolicy& policy,bool presentation) const noexcept {
 if(!a.groups)return false;
 const auto& retained=a.phase==w::Phase::released||a.phase==w::Phase::unavailable?terminal_sources_:report_sources_;
 for(std::size_t i=0;i<retained.size();++i)if(a.groups&(1u<<i)){const auto& old=retained[i];
  if(!old.source.sequence||!identical(old.source,a.source)||old.provider!=a.provider||old.owner!=a.owner||
     old.provider_generation!=a.provider_generation||old.phase!=a.phase||old.presentation!=presentation||(presentation&&old.policy!=policy))return false;}
 return true;
}
EnvironmentAdmission EnvironmentModel::admit(const EnvironmentEvent& e) noexcept {diagnostic_input_={};const auto r=receive(e);if(r!=EnvironmentAdmission::accepted)++view_.rejected;if(r==EnvironmentAdmission::obsolete)++view_.obsolete;return r;}
EnvironmentAdmission EnvironmentModel::receive(const EnvironmentEvent& e) noexcept {
 if(e.role!=w::Role::plugin&&e.role!=w::Role::exporter)return EnvironmentAdmission::fault;const auto lane=e.role==w::Role::exporter?1u:0u;
 if(e.kind==EnvironmentEventKind::lost){if(!e.connection_generation||e.connection_generation==generations_[lane])lose(e.role,e.received_qpc);return EnvironmentAdmission::accepted;}
 w::Header h;v2::Header extended;const bool version2=v2::read_header(e.frame.bytes.data(),w::header_size,extended);
 if(version2){h.kind=static_cast<w::Kind>(extended.kind);h.payload_size=extended.payload_size;h.sequence=extended.sequence;if(extended.kind==v2::Kind::source)h.kind=w::Kind::sample;else if(extended.kind==v2::Kind::applied_presentation)h.kind=w::Kind::applied;}
 else if(!w::read_header(e.frame.bytes.data(),w::header_size,h))return EnvironmentAdmission::fault;
 if(!h.sequence||e.frame.size!=h.payload_size+w::header_size)return EnvironmentAdmission::fault;const auto* p=e.frame.bytes.data()+w::header_size;
 const auto whole=std::span<const std::uint8_t>(e.frame.bytes.data()+w::header_size,e.frame.size-w::header_size);
 v2::AppliedPresentation report_detail;const auto decode_applied=[&](w::Applied& a,v2::VisibilityPolicy& policy){if(version2){v2::AppliedPresentation envelope;if(extended.kind==v2::Kind::applied)return w::decode(p,h.payload_size,a);if(extended.kind!=v2::Kind::applied_presentation||!v2::decode(whole,envelope))return false;a=envelope.applied;policy=envelope.visibility;report_detail=envelope;return true;}return w::decode(p,h.payload_size,a);};
 const bool presentation=version2&&extended.kind==v2::Kind::applied_presentation;
 const auto report_clocks=[&](const w::Applied& a){return a.applied_qpc<=e.received_qpc&&a.readback_qpc<=e.received_qpc&&
  a.location_end_qpc<=e.received_qpc&&a.regeneration_end_qpc<=e.received_qpc&&a.settling_end_qpc<=e.received_qpc&&a.source.sample_end_qpc<=a.applied_qpc;};
 if(h.kind==w::Kind::hello){w::Hello v;v2::Hello envelope;const bool decoded=version2?v2::decode(whole,envelope):w::decode(p,h.payload_size,v);if(version2)v=envelope.base;if(!decoded||v.role!=e.role||v.pid!=e.process.pid||v.process_creation_filetime!=e.process.creation_filetime||v.qpc_frequency!=frequency_||e.connection_generation<=generations_[lane])return EnvironmentAdmission::fault;const bool initial=!generations_[lane];lose(e.role,e.received_qpc);generations_[lane]=e.connection_generation;if(lane){view_.exporter=v;view_.exporter_connected=true;if(initial){needs_baseline_=false;source_floor_=0;}}else{view_.plugin=v;view_.plugin_connected=true;if(initial)for(auto& g:view_.groups)g.baseline_floor=0;}if(lane){view_.exporter_v2=version2;view_.exporter_capabilities=version2?envelope.capabilities:0;}else{view_.plugin_v2=version2;view_.plugin_capabilities=version2?envelope.capabilities:0;}return EnvironmentAdmission::accepted;}
 const bool old_connection=e.connection_generation&&e.connection_generation<generations_[lane];
 if(!lane&&old_connection&&(h.kind==w::Kind::ack||h.kind==w::Kind::applied)){
  if(h.kind==w::Kind::ack){w::Ack a;if(w::decode(p,h.payload_size,a)){diagnostic_input_=environment_ack_input(a,e.received_qpc);if(issued(a.control))return EnvironmentAdmission::obsolete;}}
  else {w::Applied a;v2::VisibilityPolicy policy;if(decode_applied(a,policy)){diagnostic_input_=environment_applied_input(a,e.received_qpc);if(a.owner==view_.owner&&report_clocks(a)&&(issued(a.source)||retained_report(a,policy,presentation)))return EnvironmentAdmission::obsolete;}}
  return EnvironmentAdmission::fault;
 }
 if(e.connection_generation!=generations_[lane]||!(lane?view_.exporter_connected:view_.plugin_connected))return EnvironmentAdmission::fault;
 if(lane&&h.kind==w::Kind::sample){w::Sample s;v2::Source source;const bool has_context=version2&&extended.kind==v2::Kind::source;const bool decoded=has_context?v2::decode(whole,source):w::decode(p,h.payload_size,s);if(has_context)s=source.sample;if(!decoded||s.producer!=view_.exporter.instance||s.sample_end_qpc>e.received_qpc)return EnvironmentAdmission::fault;
  if(s.lifecycle<announced_lifecycle_||(needs_baseline_&&s.lifecycle==announced_lifecycle_&&s.configuration<announced_configuration_))return EnvironmentAdmission::obsolete;
  const auto& old=view_.source;if(old.producer==s.producer&&old.sequence){if(s.lifecycle<old.lifecycle||s.configuration<old.configuration||(s.lifecycle==old.lifecycle&&s.sequence<=old.sequence))return EnvironmentAdmission::obsolete;if(s.sample_end_qpc<old.sample_end_qpc||(s.configuration!=old.configuration&&s.sample_end_qpc==old.sample_end_qpc))return EnvironmentAdmission::fault;}
  const bool boundary=old.sequence&&!domain(old,s);if(boundary){selection_floor_qpc_=(std::max)(selection_floor_qpc_,s.sample_end_qpc);visibility_.clear();++view_.boundaries;for(std::size_t i=0;i<2;i++){release(i,w::Reason::lifecycle,e.received_qpc,false);view_.groups[i].baseline_floor=s.sample_end_qpc-1;view_.groups[i].blocked=false;}}
  if(old.sequence&&old.producer!=s.producer){weather_target_={};view_.weather_target=view_.weather_target_sequence=view_.weather_target_revision=0;view_.weather_target_source_qpc=0;}source_change_=has_context?source.change:v2::WeatherChange{};view_.source_change=source_change_;view_.local_visibility=has_context?source.local_visibility:v2::LocalVisibility{};s.received_qpc=e.received_qpc;view_.source=s;view_.source_available=!needs_baseline_||s.sample_end_qpc>source_floor_;if(view_.source_available){needs_baseline_=false;announced_lifecycle_=s.lifecycle;announced_configuration_=s.configuration;}view_.source_context=has_context?source.context:v2::Context{};
  if(has_context&&(view_.exporter_capabilities&v2::source_context)){
   VisibilityContext c;c.psx=visibility_psx_;c.source=s;c.configured_observation_start_qpc=source.context.observation_start_qpc;c.configured_observation_qpc=source.context.observation_end_qpc;const auto& ctx=source.context;
   if(ctx.valid_mask&v2::active_target_valid)c.active=ctx.active_native_zone==0?VisibilityTarget::planet:ctx.active_native_zone==1?VisibilityTarget::zone1:VisibilityTarget::other;
   c.configured_mask=((ctx.valid_mask&v2::planet_visibility_valid)?1u:0u)|((ctx.valid_mask&v2::zone1_visibility_valid)?2u:0u);
   c.configured_m={ctx.planet_visibility_m,ctx.zone1_visibility_m};if((ctx.valid_mask&v2::active_target_valid)&&(s.configured_mask&w::bit(w::Field::visibility_m))){c.configured_mask|=16u;c.configured_m[4]=static_cast<std::uint32_t>(s.configured[18]);}c.station_available=(ctx.valid_mask&v2::zone1_station_valid)!=0;c.zone1_station=ctx.zone1_station;visibility_.observe(c,e.received_qpc);
  }else if(boundary)visibility_.clear();
  view_.visibility_selections=visibility_.selections();dirty_=view_.source_available;++view_.accepted;return EnvironmentAdmission::accepted;
 }
 if(h.kind==w::Kind::status){w::Status s;if(!w::decode(p,h.payload_size,s)||s.producer!=(lane?view_.exporter.instance:view_.plugin.instance))return EnvironmentAdmission::fault;
  if(lane){if(s.lifecycle&&s.lifecycle<announced_lifecycle_)return EnvironmentAdmission::obsolete;const bool boundary=s.lifecycle>view_.source.lifecycle;view_.exporter.groups=s.groups;if(boundary||s.reason==w::Reason::loading){invalidate_visibility(e.received_qpc);}if(boundary||!s.groups){needs_baseline_=true;source_floor_=e.received_qpc;view_.source_available=false;dirty_=false;++view_.boundaries;if(s.lifecycle)announced_lifecycle_=s.lifecycle;if(s.configuration)announced_configuration_=s.configuration;else if(s.lifecycle>view_.source.lifecycle)announced_configuration_=0;}for(std::size_t i=0;i<2;i++)if(boundary||!(s.groups&(1u<<i))){view_.groups[i].available=false;release(i,s.reason,e.received_qpc,true);}else if(s.reason==w::Reason::none)view_.groups[i].blocked=false;}
  else {view_.plugin.groups=s.groups;for(std::size_t i=0;i<2;i++)if(!(s.groups&(1u<<i)))release(i,s.reason,e.received_qpc,true);else if(s.reason==w::Reason::none)view_.groups[i].blocked=false;}
  return EnvironmentAdmission::accepted;
 }
 if(!lane&&h.kind==w::Kind::ack){w::Ack a;if(!w::decode(p,h.payload_size,a))return EnvironmentAdmission::fault;diagnostic_input_=environment_ack_input(a,e.received_qpc);if(!issued(a.control))return EnvironmentAdmission::fault;bool matched=false;
  for(std::size_t i=0;i<2;i++)if(a.control.groups&(1u<<i)){auto& g=view_.groups[i];if(!g.pending||pending_[i].operation!=a.control.operation)continue;matched=true;g.pending=false;
   const bool accepted=(a.accepted_groups&(1u<<i))&&a.reason==w::Reason::none&&(a.phase==w::Phase::owned||a.phase==w::Phase::observing)&&e.received_qpc<g.renewed_qpc+static_cast<std::int64_t>(frequency_);
   // Observing acknowledges renewal intent; it does not revoke already applied native ownership.
   const bool retained=a.control.action==w::Action::renew&&g.owned&&e.received_qpc<g.expires_qpc;
   g.owned=g.requested&&view_.following&&accepted&&(a.phase==w::Phase::owned||retained);g.phase=g.owned?w::Phase::owned:a.phase;if(a.control.action!=w::Action::release||a.reason!=w::Reason::none)g.reason=a.reason;if(g.requested&&accepted)g.expires_qpc=g.renewed_qpc+static_cast<std::int64_t>(frequency_);else if(g.requested){release(i,a.reason==w::Reason::none?w::Reason::expired:a.reason,e.received_qpc,true);g.blocked=a.reason==w::Reason::unsupported||a.reason==w::Reason::binding||a.reason==w::Reason::owner;}}
  if(!matched)return EnvironmentAdmission::obsolete;++view_.acks;return EnvironmentAdmission::accepted;
 }
 if(!lane&&h.kind==w::Kind::applied){w::Applied a;v2::VisibilityPolicy policy;if(!decode_applied(a,policy))return EnvironmentAdmission::fault;
  diagnostic_input_=environment_applied_input(a,e.received_qpc);if(a.provider!=view_.plugin.instance||a.owner!=view_.owner||!a.groups||!report_clocks(a))return EnvironmentAdmission::fault;
  const bool held=report_detail.weather_target!=0;
  if(held&&report_detail.weather_target!=weather_target_.identity)return report_detail.weather_target<=next_weather_target_?EnvironmentAdmission::obsolete:EnvironmentAdmission::fault;
  bool live=false;if(held)for(const auto& current:issued_samples_)if(current.producer==a.source.producer&&current.producer==view_.source.producer&&current.sequence==report_detail.current_sequence&&current.lifecycle==report_detail.current_lifecycle&&current.sample_end_qpc==report_detail.current_source_end_qpc&&fresh(current,e.received_qpc)){live=true;break;}
  if(held&&(!live||report_detail.weather_observed_qpc>e.received_qpc||e.received_qpc-report_detail.weather_observed_qpc>=static_cast<std::int64_t>(frequency_)))return EnvironmentAdmission::obsolete;
  bool correlated=held&&identical(weather_target_.source.sample,a.source)&&weather_target_.visibility==policy;for(std::size_t i=0;i<issued_samples_.size();++i)if(issued_samples_[i].sequence&&identical(issued_samples_[i],a.source)&&(!presentation||issued_policies_[i]==policy)){correlated=true;break;}
  if(!correlated)return retained_report(a,policy,presentation)?EnvironmentAdmission::obsolete:EnvironmentAdmission::fault;
  if(held?report_detail.current_lifecycle!=view_.source.lifecycle:!domain(a.source,view_.source))return EnvironmentAdmission::obsolete;if(a.source.sequence>view_.source.sequence)return EnvironmentAdmission::fault;
  if(a.phase==w::Phase::released||a.phase==w::Phase::unavailable)for(std::size_t i=0;i<terminal_sources_.size();++i)if(a.groups&(1u<<i))
   terminal_sources_[i]={a.source,policy,a.provider,a.owner,a.provider_generation,presentation,a.phase};
  bool accepted=false;for(std::size_t i=0;i<2;i++)if(a.groups&(1u<<i)){auto& g=view_.groups[i];const auto fields=i?w::weather_fields:1u;
   // A retained target's original SDK application time is mapped again as the
   // remote clock estimate improves. Its converted value can move backwards;
   // current source proof, not that frozen timestamp, orders PXW3 reports.
   const bool older=g.applied_available&&(a.provider_generation<g.applied.provider_generation||
       (held?(g.weather_target==report_detail.weather_target&&report_detail.current_sequence<g.current_sequence):a.applied_qpc<g.applied.applied_qpc));
   if(!g.requested||!view_.following||e.received_qpc>=g.expires_qpc||(held?report_detail.current_source_end_qpc:a.source.sample_end_qpc)<=g.baseline_floor||older)continue;
   if(a.phase==w::Phase::released||a.phase==w::Phase::unavailable){release(i,a.reason,e.received_qpc,true);g.blocked=true;}else if(a.phase==w::Phase::owned&&(i?(a.accepted_mask&fields)!=0:true)&&(held?live:fresh(a.source,e.received_qpc)))g.owned=i?true:environment_time_report_qualified(a);
   if(a.phase==w::Phase::owned||a.phase==w::Phase::observing)report_sources_[i]={a.source,policy,a.provider,a.owner,a.provider_generation,presentation,a.phase};
   g.weather_target=report_detail.weather_target;g.current_lifecycle=report_detail.current_lifecycle;g.current_sequence=report_detail.current_sequence;g.current_source_end_qpc=report_detail.current_source_end_qpc;g.weather_observed_qpc=report_detail.weather_observed_qpc;g.phase=a.phase;g.applied=a;g.applied_available=true;g.applied_retained=true;g.report_received_qpc=e.received_qpc;if(i==1){view_.applied_visibility_available=version2&&extended.kind==v2::Kind::applied_presentation;view_.applied_visibility_policy=policy;}accepted=true;}
  return accepted?EnvironmentAdmission::accepted:EnvironmentAdmission::obsolete;
 }
 return EnvironmentAdmission::fault;
}
void EnvironmentModel::tick(std::int64_t now) noexcept {
 policy_now_=now;
 const auto& s=view_.source;const bool current=view_.source_available&&fresh(s,now);view_.source_available=current;
 for(std::size_t i=0;i<2;i++){auto& g=view_.groups[i];g.available=current&&(view_.exporter.groups&(1u<<i))&&(i?((s.valid_mask&(w::weather_fields&~w::reference_fields))!=0&&source_change_.qualified&&(view_.exporter_capabilities&v2::manual_weather)&&(view_.plugin_capabilities&v2::manual_weather)&&(!(view_.exporter_capabilities&v2::local_visibility_target)||view_.local_visibility.qualified||weather_target_.identity)):(s.flags&w::utc_valid));
  if(!g.available){release(i,current?w::Reason::unsupported:w::Reason::stale,now,true);continue;}
  // Release completion is an ownership boundary, not another expiring intent.
  // Keep it pending through a blocked SDK callback until Ack or link retirement.
  if(g.pending&&pending_[i].action!=w::Action::release&&now-g.renewed_qpc>=static_cast<std::int64_t>(frequency_)){g.pending=false;release(i,w::Reason::expired,now,true);}
  if(g.requested&&!g.pending&&now>=g.expires_qpc)release(i,w::Reason::expired,now,true);
  if(!view_.following||!view_.plugin_connected||g.blocked||!(view_.plugin.groups&(1u<<i))){if(g.requested)release(i,w::Reason::unsupported,now,false);continue;}
  if(!g.requested&&!g.pending&&s.sample_end_qpc>g.baseline_floor){if(command(i,w::Action::acquire,now))dirty_=true;}
  else if(g.requested&&!g.pending&&static_cast<long double>(now-g.renewed_qpc)*1000>=static_cast<long double>(frequency_)*w::renew_ms)command(i,w::Action::renew,now);
 }
}
bool EnvironmentModel::next(EnvironmentDispatch& d) noexcept {
 if(size_){d={};d.kind=w::Kind::control;d.control=commands_[head_];d.version2=view_.plugin_v2;head_=(head_+1)%commands_.size();--size_;return true;}
 if(dirty_){dirty_=false;if(view_.source_available&&view_.plugin_connected&&view_.following&&(view_.groups[0].requested||view_.groups[1].requested)){d={};d.sample=view_.source;d.version2=view_.plugin_v2;d.capabilities=view_.plugin_capabilities;d.context=view_.source_context;d.context.source_sequence=d.sample.sequence;d.change=source_change_;d.local_visibility=(view_.plugin_capabilities&v2::local_visibility_target)?view_.local_visibility:v2::LocalVisibility{};
   const auto chosen=visibility_.policy(d.sample,(std::max)(policy_now_,d.sample.received_qpc));auto& policy=d.visibility;
   policy.owner=view_.owner;policy.lifecycle=d.sample.lifecycle;policy.source_sequence=d.sample.sequence;policy.selection_revision=chosen.revision;
   policy.target=static_cast<v2::Target>(chosen.target);policy.kind=static_cast<v2::PolicyKind>(chosen.kind);policy.visibility_m=chosen.metres;
   if(policy.kind==v2::PolicyKind::extended&&!(view_.plugin_capabilities&v2::visibility_presentation)){policy.kind=((d.sample.flags&w::visibility_unlimited)&&(d.sample.valid_mask&w::bit(w::Field::visibility_m)))?v2::PolicyKind::sentinel_finite:v2::PolicyKind::native;policy.visibility_m=policy.kind==v2::PolicyKind::sentinel_finite?9999:0;}
   if(policy.kind!=v2::PolicyKind::extended&&d.local_visibility.qualified&&(view_.exporter_capabilities&v2::local_visibility_target)&&(view_.plugin_capabilities&v2::local_visibility_target)){
    policy.kind=v2::PolicyKind::local_target;policy.visibility_m=d.local_visibility.metres;
   }
   if(view_.groups[1].requested&&source_change_.qualified&&(view_.plugin_capabilities&v2::manual_weather)){
    const bool edited=source_change_.revision>weather_target_.source.change.revision&&source_change_.lifecycle==d.sample.lifecycle;
    bool policy_ready=weather_policy_pending_;
    for(const auto& selected:visibility_.selections())if(selected.metres&&d.context.observation_start_qpc<=selected.configured_readback_qpc)policy_ready=false;
    // A new v4 target requires the computed native local value. Do not freeze
    // the first sample of PSX's long visibility transition after an edit.
    const bool local_ready=!(view_.exporter_capabilities&v2::local_visibility_target)||d.local_visibility.qualified;
    if(local_ready&&(!weather_target_.identity||edited||policy_ready)){
     const auto identity=++next_weather_target_;
     if(!identity){++view_.overflows;release(1,w::Reason::overflow,policy_now_,false);}else{
      weather_target_={identity,{d.sample,d.context,source_change_,d.local_visibility},policy};if(policy_ready)weather_policy_pending_=false;
      view_.weather_target=identity;view_.weather_target_sequence=d.sample.sequence;view_.weather_target_revision=source_change_.revision;view_.weather_target_source_qpc=d.sample.sample_end_qpc;
     }
    }
    d.weather=weather_target_;
   }
   view_.visibility_policy=policy;const auto index=sample_cursor_++%issued_samples_.size();issued_samples_[index]=d.sample;issued_policies_[index]=policy;++view_.dispatched;return true;}}
 return false;
}
}
