#include "psx_plugin/motion_controller.hpp"
#include <psx/sync/runtime.hpp>
#include <XPLMGraphics.h>
#include <XPLMPlanes.h>
#include <bit>
#include <cmath>
#include <limits>

namespace psx::xp {
namespace m=motion;namespace b=bridge;namespace d=diagnostics;
namespace {
struct PsxPause {
    std::uint64_t flags{},sequence{},qpc{},session{},configuration{};
    bool held() const noexcept {return (flags&(4|16))!=0;}
};
PsxPause psx_pause(const b::LatestState* state,bool connected) noexcept {
    PsxPause result;if(!connected || !state)return result;
    const auto slot=static_cast<std::size_t>(aircraft::SourceRecord::qi129);
    if(!state->available()[slot])return result;
    const auto& observation=state->records()[slot].observation;double value{};
    if(m::value(observation,aircraft::PropertyId::paused_all,value))result.flags|=2|(value!=0?4:0);
    if(m::value(observation,aircraft::PropertyId::motion_frozen,value))result.flags|=8|(value!=0?16:0);
    result.sequence=observation.stamp.sequence;result.qpc=static_cast<std::uint64_t>(observation.stamp.received_qpc);
    result.session=observation.stamp.session_epoch;result.configuration=observation.stamp.configuration_epoch;return result;
}
bool distant(const m::GeoPose& a,const m::GeoPose& z) noexcept {return m::distance_m(a,z)>1000 || std::abs(a.altitude_msl_m-z.altitude_msl_m)>500;}
double position_error(const m::LocalPose& a,const m::LocalPose& z) noexcept {const auto v=a.origin-z.origin;return std::sqrt(m::dot(v,v));}
double attitude_error(const m::LocalPose& a,const m::LocalPose& z) noexcept {
    // Rotation columns avoid false conflicts for equivalent near-gimbal Euler triples.
    const auto ar=m::body_rotation(a.heading_deg,a.pitch_deg,a.bank_deg),br=m::body_rotation(z.heading_deg,z.pitch_deg,z.bank_deg);
    const auto c=(m::dot(ar.x,br.x)+m::dot(ar.y,br.y)+m::dot(ar.z,br.z)-1)*0.5;
    return std::acos((std::max)(-1.0,(std::min)(1.0,c)))*180/m::pi;
}
}
void MotionController::begin(SdkApi& sdk,std::uint64_t load,std::uint64_t enable,d::DiagnosticProducer events,
    std::int64_t (*clock)() noexcept,std::uint64_t frequency,std::uint32_t timeout,std::uint32_t callback_delayed) noexcept {
    // An incomplete cleanup must never be hidden by enable/reinitialization.
    if(owned_)return;
    retire_resources();*this=MotionController{};sdk_=&sdk;events_=events;clock_=clock;frequency_=frequency;boost_timeout_ms_=timeout;
    callback_delayed_ms_=callback_delayed;
    report_.status.plugin_load_id=load;report_.status.enable_id=enable;report_.status.reference_revision=1;
    report_.status.revision=1;report_.status.generated_qpc=clock_();
}
void MotionController::prepare_bindings() noexcept {
    bound_=false;if(!sdk_ || sdk_->ready()!=Status::ok)return;
    auto bind=[&](const char* name,int type,bool writable){auto ref=XPLMFindDataRef(name);return sdk_->inspect(ref,type,writable)==Status::ok ? ref : nullptr;};
    override_=bind("sim/operation/override/override_planepath",xplmType_IntArray,true);
    paused_=bind("sim/time/paused",xplmType_Int,false);
    lat_ref_=bind("sim/flightmodel/position/lat_ref",xplmType_Float,false);lon_ref_=bind("sim/flightmodel/position/lon_ref",xplmType_Float,false);
    constexpr const char* xyz[]={"sim/flightmodel/position/local_x","sim/flightmodel/position/local_y","sim/flightmodel/position/local_z"};
    constexpr const char* angle[]={"sim/flightmodel/position/psi","sim/flightmodel/position/theta","sim/flightmodel/position/phi"};
    constexpr const char* head[]={"sim/graphics/view/pilots_head_x","sim/graphics/view/pilots_head_y","sim/graphics/view/pilots_head_z"};
    for(std::size_t i=0;i<3;++i){xyz_[i]=bind(xyz[i],xplmType_Double,true);euler_[i]=bind(angle[i],xplmType_Float,true);head_[i]=bind(head[i],xplmType_Float,false);}
    cg_y_=bind("sim/aircraft/weight/acf_cgY_original",xplmType_Float,false);cg_z_=bind("sim/aircraft/weight/acf_cgZ_original",xplmType_Float,false);
    pause_off_=XPLMFindCommand("sim/operation/pause_off");
    bound_=override_&&lat_ref_&&lon_ref_&&cg_y_&&cg_z_;
    for(std::size_t i=0;i<3;++i)bound_=bound_&&xyz_[i]&&euler_[i];
    if(bound_)bound_=XPLMGetDatavi(override_,nullptr,0,0)>=1;
    if(!probe_)probe_=XPLMCreateProbe(xplm_ProbeY);
    bound_=bound_&&probe_;reference_diagnostic_pending_=true;
}
void MotionController::retire_resources() noexcept {if(probe_&&sdk_&&sdk_->on_thread()){XPLMDestroyProbe(probe_);probe_=nullptr;}}
void MotionController::lifecycle(std::uint64_t aircraft,std::uint64_t scenery,bool replacement,bool unloaded) noexcept {
    reset_reconstruction();
    if(follow_&&have_pose_)readiness_destination_=true;
    if(replacement && !in_place_call_) {
        stop(m::Reason::aircraft_changed);profile_={};have_pose_=have_written_=false;
    }
    report_.status.aircraft_generation=aircraft;report_.status.scenery_generation=scenery;
    ++report_.status.revision;
    if(replacement){profile_.qualified=false;if(unloaded)bound_=false;}
    have_written_=false;reconcile_=true;++report_.status.discontinuity_generation;
    flags();
}
void MotionController::profile(const MotionProfile& value) noexcept {
    if(value.generation!=report_.status.aircraft_generation)return;
    if(value.generation!=profile_.generation||value.qualified!=profile_.qualified||value.sha256!=profile_.sha256||
       value.cg_y_ft!=profile_.cg_y_ft||value.cg_z_ft!=profile_.cg_z_ft||value.eye.x!=profile_.eye.x||value.eye.y!=profile_.eye.y||value.eye.z!=profile_.eye.z)reset_reconstruction();
    profile_=value;report_.status.reference_revision=value.sparky?2:1;reference_diagnostic_pending_=true;++report_.status.revision;flags();
    d::Context context;context.plugin_load_id=report_.status.plugin_load_id;context.enable_id=report_.status.enable_id;context.aircraft_generation=value.generation;
    events_.emit(d::Code::motion_reference,d::Severity::info,context,d::ErrorDomain::none,0,value.sha256.data(),{value.generation,value.qualified?1u:0u,1,0,0,0});
}
bool MotionController::reference_valid() noexcept {
    if(!bound_ || !profile_.qualified || profile_.generation!=report_.status.aircraft_generation)return false;
    // Aircraft geometry is fixed by the verified ACF and its original CG.
    // pilots_head_xyz is mutable view state: seat/head movement must not reject
    // Follow or move the aircraft to compensate for the user's camera.
    const double cg_y=XPLMGetDataf(cg_y_),cg_z=XPLMGetDataf(cg_z_);
    const bool valid=std::isfinite(cg_y)&&std::isfinite(cg_z)&&
        std::abs(cg_y-profile_.cg_y_ft)<0.01 && std::abs(cg_z-profile_.cg_z_ft)<0.01;
    if(reference_diagnostic_pending_ || !reference_sampled_ || valid!=previous_reference_valid_) {
        reference_diagnostic_pending_=false;reference_sampled_=true;previous_reference_valid_=valid;
        d::Context context;context.plugin_load_id=report_.status.plugin_load_id;context.enable_id=report_.status.enable_id;
        context.connection_id=report_.status.connection_id;context.aircraft_generation=report_.status.aircraft_generation;
        context.session_epoch=seen_session_;context.scenery_generation=report_.status.scenery_generation;
        const auto bits=[](double value){return std::bit_cast<std::uint64_t>(value);};
        events_.emit(d::Code::motion_reference,d::Severity::info,context,d::ErrorDomain::none,0,
            "fixed CG: f64 actual Y/Z ft, expected Y/Z ft; valid in value4",
            {bits(cg_y),bits(cg_z),bits(profile_.cg_y_ft),bits(profile_.cg_z_ft),valid?1u:0u,0});
        const auto read_head=[](XPLMDataRef ref)->double{return ref?XPLMGetDataf(ref):std::numeric_limits<double>::quiet_NaN();};
        const m::Vec3 head{read_head(head_[0]),read_head(head_[1]),read_head(head_[2])};const auto delta=head-profile_.eye;
        events_.emit(d::Code::motion_reference,d::Severity::info,context,d::ErrorDomain::none,0,
            "view head (informational): f64 XYZ m, delta from fixed ACF eye XYZ m",
            {bits(head.x),bits(head.y),bits(head.z),bits(delta.x),bits(delta.y),bits(delta.z)});
    }
    return valid;
}
void MotionController::flags() noexcept {
    auto& s=report_.status;s.flags=s.flags&m::placement_starts_engines;
    if((have_pose_?s.presentation.provenance.kind:s.selected_source)==m::PoseSource::public_main_boost)s.flags|=m::approximate_pitch;
    if(follow_)s.flags|=m::follow_requested;if(owned_)s.flags|=m::override_owned;if(have_pose_)s.flags|=m::pose_valid;
    if(have_written_)s.flags|=m::readback_valid;if(profile_.qualified)s.flags|=m::profile_valid;
    if(paused_ && XPLMGetDatai(paused_))s.flags|=m::xp_paused;
    if(clock_)s.generated_qpc=clock_();
}
void MotionController::transition(m::Phase phase,m::Reason reason) noexcept {
    auto& s=report_.status;if(s.phase==phase && s.reason==reason){flags();return;}
    s.phase=phase;s.reason=reason;++s.revision;flags();
    d::Context c;c.plugin_load_id=s.plugin_load_id;c.enable_id=s.enable_id;c.connection_id=s.connection_id;
    c.aircraft_generation=s.aircraft_generation;c.scenery_generation=s.scenery_generation;c.session_epoch=s.presentation.source.session_epoch;
    events_.emit(d::Code::motion_state,d::Severity::info,c,d::ErrorDomain::none,0,{},
        {static_cast<std::uint64_t>(phase),static_cast<std::uint64_t>(reason),s.operation_id,s.discontinuity_generation,s.applied_count,s.flags});
}
void MotionController::ack(const b::MotionRequest& request,m::AckResult result,m::Reason reason) noexcept {
    if(ack_write_-ack_read_==acks_.size()){++report_.status.control_dropped;faulted_=true;follow_=false;return;}
    acks_[ack_write_++%acks_.size()]={request.connection_id,request.operation_id,request.plugin_load_id,request.enable_id,request.aircraft_generation,clock_(),result,reason};
    d::Context context;context.connection_id=request.connection_id;context.plugin_load_id=request.plugin_load_id;context.enable_id=request.enable_id;
    context.aircraft_generation=request.aircraft_generation;context.session_epoch=request.session_epoch;
    events_.emit(result==m::AckResult::accepted?d::Code::motion_requested:result==m::AckResult::rejected?d::Code::motion_rejected:d::Code::motion_state,
        d::Severity::info,context,d::ErrorDomain::none,0,"motion acknowledgement",
        {request.operation_id,static_cast<std::uint64_t>(request.action),static_cast<std::uint64_t>(result),static_cast<std::uint64_t>(reason),request.motion_epoch,0});
}
bool MotionController::pop_ack(b::MotionAck& ack_value) noexcept {if(ack_read_==ack_write_)return false;ack_value=acks_[ack_read_++%acks_.size()];return true;}
void MotionController::complete(m::AckResult result,m::Reason reason) noexcept {if(operation_.operation_id){ack(operation_,result,reason);operation_={};}}
void MotionController::connection(std::uint64_t id) noexcept {
    if(report_.status.connection_id==id)return;
    reset_reconstruction();seen_domain_={};
    selection_known_=false;selection_={};exporter_floor_=0;exporter_branch_=0;
    report_.status.selected_source=m::PoseSource::public_main_boost;report_.status.selected_domain={};report_.status.selection_epoch=1;
    complete(m::AckResult::cancelled,m::Reason::disconnected);last_operation_=0;
    unpause_attempted_=false;unpause_result_=0;frozen_pose_changed_=false;frozen_pose_baseline_valid_=false;
    report_.status.connection_id=id;++report_.status.revision;reconcile_=true;
    if(follow_)transition(m::Phase::holding,m::Reason::disconnected);
}
void MotionController::boundary(const b::SessionStatus& session) noexcept {
    if(session.connection_id!=report_.status.connection_id)return;
    if(selection_known_&&selection_.source==m::PoseSource::exporter)return; // Main/Qi122 are not exporter lifecycle authority.
    const auto domain=b::history_domain(session);
    if(!m::same_motion_domain(domain,seen_domain_)) {
        reset_reconstruction();seen_domain_=domain;
        seen_session_=session.session_epoch;seen_epoch_=session.motion_epoch;reference_diagnostic_pending_=true;
        reconcile_=true;++report_.status.discontinuity_generation;++report_.status.revision;have_written_=false;
        if(follow_&&have_pose_)readiness_destination_=true;
        if(follow_)transition(m::Phase::holding,m::Reason::epoch_changed);
    }
}
void MotionController::selection(const b::SourceSelection& selected) noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    if(selected.connection_id!=report_.status.connection_id||!selected.revision||!selected.selection_epoch)return;
    if(selected.source==m::PoseSource::exporter){if(!m::current_reference(selected.domain))return;}
    else if(selected.source!=m::PoseSource::public_main_boost||m::valid(selected.domain.producer)||selected.domain.pose_epoch||
            selected.domain.reference_revision!=m::instant_reference_revision)return;
    if(selection_known_&&(selected.revision<selection_.revision||selected.selection_epoch<selection_.selection_epoch))return;
    const bool changed=!selection_known_||selected.selection_epoch!=selection_.selection_epoch;
    if(!changed&&(selected.source!=selection_.source||selected.domain!=selection_.domain))return;
    selection_=selected;selection_known_=true;
    auto& s=report_.status;s.selected_source=selected.source;s.selection_epoch=selected.selection_epoch;s.selected_domain=selected.domain;
    if(changed) {
        complete(m::AckResult::cancelled,m::Reason::epoch_changed);reset_reconstruction();seen_domain_={};exporter_floor_=0;exporter_branch_=0;
        reconcile_=true;placement_applied_=false;have_written_=false;handoff_pending_=have_pose_;
        if(follow_&&have_pose_)readiness_destination_=true;
        ++s.discontinuity_generation;++s.revision;
        if(follow_)transition(m::Phase::holding,m::Reason::epoch_changed);
        d::Context context;context.connection_id=s.connection_id;context.plugin_load_id=s.plugin_load_id;context.enable_id=s.enable_id;
        events_.emit(d::Code::motion_state,d::Severity::info,context,d::ErrorDomain::none,0,"selected pose source/epoch/domain; Follow authority unchanged",
            {static_cast<std::uint64_t>(selected.source),selected.selection_epoch,selected.domain.producer.high,selected.domain.producer.low,selected.domain.pose_epoch,selected.domain.reference_revision});
    }
    flags();
}
bool MotionController::complete_matches(const b::CompletePoseWindow* w) const noexcept {
    return selection_known_&&selection_.source==m::PoseSource::exporter&&m::current_reference(selection_.domain)&&w&&
        m::current_reference(w->samples.domain)&&w->connection_id==report_.status.connection_id&&
        w->selection_epoch==selection_.selection_epoch&&w->samples.domain==selection_.domain&&w->samples.count&&w->samples.count<=m::history_capacity;
}
void MotionController::exporter_boundary(const b::CompletePoseWindow* w) noexcept {
    if(!complete_matches(w))return;
    const auto branch=w->samples.samples[w->samples.count-1].flags&(m::raw_ordinary_altitude|m::raw_paused_altitude);
    if(exporter_branch_&&(exporter_floor_!=w->samples.sequence_floor||exporter_branch_!=branch)) {
        reset_reconstruction();reconcile_=true;placement_applied_=false;handoff_pending_=have_pose_;have_written_=false;
        if(follow_&&have_pose_)readiness_destination_=true;
        ++report_.status.discontinuity_generation;++report_.status.revision;
    }
    exporter_floor_=w->samples.sequence_floor;exporter_branch_=branch;
}
bool MotionController::current_pose(const m::PresentationPose& p) const noexcept {
    if(!selection_known_||p.provenance.kind!=selection_.source||p.provenance.selection_epoch!=selection_.selection_epoch)return false;
    return selection_.source==m::PoseSource::exporter?p.provenance.domain==selection_.domain:
        p.source.session_epoch==seen_session_&&p.source.motion_epoch==seen_epoch_&&p.source.boost_generation==seen_domain_.boost_generation;
}
m::Reason MotionController::source(const b::LatestState* state,bool connected,m::PresentationPose& pose,bool& paused,
    const b::CompletePoseWindow* complete,m::SourcePreference preference) const noexcept {
    paused=false;if(!connected || !state)return m::Reason::disconnected;
    if(!selection_known_)return m::Reason::source_unavailable;
    if(selection_.source==m::PoseSource::exporter) {
        if(preference==m::SourcePreference::public_only||!complete_matches(complete))return m::Reason::source_unavailable;
        const auto& newest=complete->samples.samples[complete->samples.count-1];const auto now=clock_();
        if(newest.sample_end_qpc>psx::sync::freshness_now(now)||newest.received_qpc>psx::sync::freshness_now(now))return m::Reason::invalid_pose;
        if(d::freshness(true,psx::sync::freshness_now(now),newest.sample_end_qpc,frequency_,m::exporter_timeout_ms)!=d::Freshness::current||
           d::freshness(true,psx::sync::freshness_now(now),newest.received_qpc,frequency_,m::exporter_timeout_ms)!=d::Freshness::current)return m::Reason::source_unavailable;
        // Whole-window admission is worker work. Preflight needs only the
        // newest original sample; post performs the bounded reconstruction.
        pose={};if(!m::geographic_pose(newest,pose.geo))return m::Reason::invalid_pose;
        pose.on_ground=(newest.flags&m::raw_on_ground)!=0;
        pose.provenance={m::PoseSource::exporter,selection_.selection_epoch,complete->samples.domain,
            newest.sequence,newest.sequence,newest.frame_sequence,newest.sample_start_qpc,newest.sample_end_qpc,newest.received_qpc};
        if(std::abs(pose.geo.altitude_msl_m)>(std::numeric_limits<float>::max)())return m::Reason::invalid_pose;
        paused=psx_pause(state,connected).held();return m::Reason::none;
    }
    const auto& s=state->session();constexpr auto required=b::main_connected|b::synchronized|b::boost_connected|b::boost_available;
    if((s.flags&required)!=required || !s.motion_epoch)return m::Reason::source_unavailable;
    const auto boost=static_cast<std::size_t>(aircraft::SourceRecord::boost),pitch=static_cast<std::size_t>(aircraft::SourceRecord::qs121);
    if(!state->available()[boost] || !state->available()[pitch])return m::Reason::source_unavailable;
    const auto& bo=state->records()[boost];const auto& po=state->records()[pitch];
    if(bo.observation.stamp.sequence<s.motion_sequence_floor || po.observation.stamp.sequence<s.motion_sequence_floor || bo.boost_generation!=s.boost_generation)return m::Reason::epoch_changed;
    auto reason=m::assemble(bo.observation,po.observation,s.session_epoch,s.boost_generation,s.motion_epoch,pose);if(reason!=m::Reason::none)return reason;
    pose.provenance={};pose.provenance.selection_epoch=selection_.selection_epoch;
    if(std::abs(pose.geo.altitude_msl_m)>(std::numeric_limits<float>::max)())return m::Reason::invalid_pose;
    const auto now=clock_();
    if(pose.source.pitch_received_qpc>psx::sync::freshness_now(now) || pose.source.boost_received_qpc>psx::sync::freshness_now(now))return m::Reason::invalid_pose;
    if(d::freshness(true,psx::sync::freshness_now(now),pose.source.boost_received_qpc,frequency_,boost_timeout_ms_)!=d::Freshness::current)return m::Reason::boost_delayed;
    paused=psx_pause(state,connected).held();
    return m::Reason::none;
}
int MotionController::override_value() noexcept {int value=-1;return override_ && XPLMGetDatavi(override_,&value,0,1)==1 ? value : -1;}
void MotionController::pause_diagnostics(const b::LatestState* state,bool connected) noexcept {
    const auto psx=psx_pause(state,connected);
    const std::uint64_t xp=paused_?static_cast<std::uint64_t>(XPLMGetDatai(paused_)!=0):2;
    const auto connection=connected&&state?state->session().connection_id:0;
    const auto flags=psx.flags|(connected?1u:0u);
    const std::array<std::uint64_t,5> inputs{xp,flags,psx.session,psx.configuration,connection};
    if(pause_inputs_sampled_ && inputs==previous_pause_inputs_)return;
    pause_inputs_sampled_=true;previous_pause_inputs_=inputs;
    d::Context context;context.plugin_load_id=report_.status.plugin_load_id;context.enable_id=report_.status.enable_id;
    context.connection_id=connection;context.session_epoch=psx.session;context.configuration_epoch=psx.configuration;
    context.aircraft_generation=report_.status.aircraft_generation;context.scenery_generation=report_.status.scenery_generation;
    events_.emit(d::Code::motion_state,d::Severity::info,context,d::ErrorDomain::none,0,
        "pause inputs: XP(0/1,2 unknown), flags(bridge1,pausedKnown2,paused4,frozenKnown8,frozen16), Qi129 sequence/QPC/session/connection",
        {xp,flags,psx.sequence,psx.qpc,psx.session,connection});
}
void MotionController::unpause_connected(bool connected) noexcept {
    if(!connected){unpause_attempted_=false;unpause_result_=0;return;}
    auto report_result=[&](unsigned result,int actual) {
        if(result==unpause_result_)return;
        const auto previous=unpause_result_;unpause_result_=result;
        if(result==1 && previous==0)return;
        d::Context context;context.plugin_load_id=report_.status.plugin_load_id;context.enable_id=report_.status.enable_id;
        context.connection_id=report_.status.connection_id;context.aircraft_generation=report_.status.aircraft_generation;
        events_.emit(d::Code::sdk_failure,result==1?d::Severity::info:d::Severity::warning,context,
            result==1?d::ErrorDomain::none:d::ErrorDomain::sdk,result==1?0u:1u,
            result==1?"X-Plane unpause recovered":"X-Plane pause_off unavailable or did not clear pause; bounded retry",
            {static_cast<std::uint64_t>(actual),unpause_attempts_,250,paused_?1u:0u,pause_off_?1u:0u,0});
    };
    if(paused_ && XPLMGetDatai(paused_)==0){unpause_attempted_=false;report_result(1,0);return;}
    const auto now=clock_();const auto retry=static_cast<std::int64_t>((std::max)(std::uint64_t{1},frequency_/4));
    if(unpause_attempted_ && now>=last_unpause_attempt_ && now-last_unpause_attempt_<retry)return;
    unpause_attempted_=true;last_unpause_attempt_=now;++unpause_attempts_;
    if(pause_off_)XPLMCommandOnce(pause_off_);
    const int actual=paused_?XPLMGetDatai(paused_):2;
    report_result(paused_&&actual==0?1:2,actual);
    if(paused_&&actual==0)unpause_attempted_=false;
}
void MotionController::frozen_pose_diagnostics(const b::LatestState* state,const m::PresentationPose& target,bool frozen) noexcept {
    if(frozen&&!frozen_pose_baseline_valid_){frozen_pose_baseline_=target.geo;frozen_pose_baseline_valid_=true;}
    const auto& a=target.geo;const auto& z=frozen_pose_baseline_;
    const bool changed=frozen && (frozen_pose_changed_ ||
        a.latitude_deg!=z.latitude_deg||a.longitude_deg!=z.longitude_deg||a.altitude_msl_m!=z.altitude_msl_m||
        a.heading_deg!=z.heading_deg||a.pitch_deg!=z.pitch_deg||a.bank_deg!=z.bank_deg);
    if(!frozen)frozen_pose_baseline_valid_=false;
    if(changed==frozen_pose_changed_)return;frozen_pose_changed_=changed;
    const auto psx=psx_pause(state,true);d::Context context;context.plugin_load_id=report_.status.plugin_load_id;
    context.enable_id=report_.status.enable_id;context.connection_id=report_.status.connection_id;
    context.session_epoch=psx.session;context.configuration_epoch=psx.configuration;
    context.aircraft_generation=report_.status.aircraft_generation;
    events_.emit(d::Code::motion_state,d::Severity::info,context,d::ErrorDomain::none,0,
        "PSX pause/freeze with changing source pose; following source: active, Qi129 sequence/QPC, Boost/pitch sequence, motion epoch",
        {changed?1u:0u,psx.sequence,psx.qpc,target.source.boost_sequence,target.source.pitch_sequence,target.source.motion_epoch});
}
bool MotionController::stop(m::Reason why) noexcept {
    if(!sdk_ || !sdk_->on_thread())return false;
    reset_reconstruction();
    follow_=false;relocating_=placement_ready_=placement_applied_=false;need_initial_=true;
    readiness_destination_=false;
    complete(m::AckResult::cancelled,why);
    if(!owned_){transition(m::Phase::observing,why);return true;}
    if(override_value()==1){int zero=0;XPLMSetDatavi(override_,&zero,0,1);if(override_value()!=0){transition(m::Phase::fault,m::Reason::override_lost);faulted_=true;return false;}}
    owned_=false;have_written_=false;transition(m::Phase::stopped,why);return true;
}
void MotionController::fault(m::Reason reason) noexcept {
    reset_reconstruction();
    complete(m::AckResult::rejected,reason);faulted_=true;follow_=false;relocating_=placement_ready_=false;
    // Stop competing writes. Retain owned override until Stop/disable; never pause X-Plane.
    transition(m::Phase::fault,reason);
}
void MotionController::transport_overflow() noexcept {++report_.status.control_dropped;fault(m::Reason::transport_overflow);}
void MotionController::request(const b::MotionRequest& r,const b::LatestState* state,bool connected,const b::CompletePoseWindow* complete_window) noexcept {
    if(r.connection_id!=report_.status.connection_id || r.plugin_load_id!=report_.status.plugin_load_id || r.enable_id!=report_.status.enable_id ||
       r.aircraft_generation!=report_.status.aircraft_generation || !r.operation_id || r.operation_id<=last_operation_) {ack(r,m::AckResult::rejected,m::Reason::stale_request);return;}
    last_operation_=r.operation_id;
    if(r.action==m::Action::stop) {
        ack(r,m::AckResult::accepted,m::Reason::none);const bool ok=stop(m::Reason::none);
        if(ok){faulted_=false;transition(m::Phase::stopped,m::Reason::none);}
        ack(r,ok?m::AckResult::applied:m::AckResult::rejected,ok?m::Reason::none:report_.status.reason);return;
    }
    reference_diagnostic_pending_=true;
    m::PresentationPose target;bool frozen{};auto reason=source(state,connected,target,frozen,complete_window,r.source_preference);
    const bool pending_window=reason==m::Reason::source_unavailable&&connected&&selection_known_&&
        selection_.source==m::PoseSource::exporter&&r.source_preference==m::SourcePreference::automatic&&!complete_matches(complete_window);
    // A completed placement failure belongs to its operation. A fresh Follow
    // requalifies source, identity, profile and ownership and may try again.
    // Writer/override faults still require their existing explicit recovery.
    const bool retry_placement=faulted_&&(report_.status.reason==m::Reason::relocation_timeout||report_.status.reason==m::Reason::relocation_failed);
    if(pending_window)reason=m::Reason::none;
    if(!m::valid(r.reconstruction)||static_cast<unsigned>(r.source_preference)>1)reason=m::Reason::invalid_pose;
    else if(!selection_known_||r.expected_selection_epoch!=selection_.selection_epoch||!state)reason=m::Reason::stale_request;
    else if(selection_.source==m::PoseSource::public_main_boost&&
        (r.session_epoch!=state->session().session_epoch || r.motion_epoch!=state->session().motion_epoch))reason=m::Reason::stale_request;
    else if(!profile_.qualified)reason=m::Reason::profile_unqualified;
    else if(!bound_)reason=m::Reason::binding_unavailable;
    else if(!reference_valid())reason=m::Reason::reference_unqualified;
    else if(!owned_ && override_value()!=0)reason=m::Reason::override_conflict;
    else if(faulted_&&!retry_placement)reason=report_.status.reason;
    m::LocalPose projected;if(reason==m::Reason::none&&!pending_window&&!project(target,projected))reason=m::Reason::invalid_pose;
    if(reason!=m::Reason::none){ack(r,m::AckResult::rejected,reason);return;}
    const auto previous_trace=applied_trace_;
    const bool keep_anchor=owned_&&have_pose_&&reconstruction_ready_&&current_pose(report_.status.presentation)&&
        report_.status.presentation.source.session_epoch==target.source.session_epoch&&
        report_.status.presentation.source.boost_generation==target.source.boost_generation&&
        report_.status.presentation.source.motion_epoch==target.source.motion_epoch;
    reset_reconstruction();auto& s=report_.status;s.reconstruction=r.reconstruction;
    s.source_preference=r.source_preference;report_.exporter_age={};report_.sampling_width={};
    s.bracketed_count=s.oldest_hold_count=s.newest_hold_count=s.no_pitch_count=s.hold_transitions=0;s.history_reset_count=1;
    report_.output_age={};report_.hold_duration={};
    if(keep_anchor)seed_anchor(s.presentation,&previous_trace);
    complete(m::AckResult::cancelled,m::Reason::cancelled);operation_=r;report_.status.operation_id=r.operation_id;
    if(retry_placement)faulted_=false;
    follow_=true;need_initial_=!owned_||retry_placement;reconcile_=true;ack(r,m::AckResult::accepted,m::Reason::none);transition(m::Phase::waiting,m::Reason::none);
    readiness_destination_=!pending_window&&(need_initial_||(have_pose_&&distant(report_.status.presentation.geo,target.geo)));
    readiness_target_=pending_window?m::PresentationPose{}:target;
}
bool MotionController::project(const m::PresentationPose& pose,m::LocalPose& local) noexcept {
    const auto& g=pose.geo;m::Vec3 center,north,up;
    XPLMWorldToLocal(g.latitude_deg,g.longitude_deg,g.altitude_msl_m,&center.x,&center.y,&center.z);
    XPLMWorldToLocal(g.latitude_deg+0.0001,g.longitude_deg,g.altitude_msl_m,&north.x,&north.y,&north.z);
    XPLMWorldToLocal(g.latitude_deg,g.longitude_deg,g.altitude_msl_m+10,&up.x,&up.y,&up.z);
    m::Rotation tangent;return m::tangent_rotation(north-center,up-center,tangent)&&m::local_pose(g,center,tangent,profile_.eye,local);
}
bool MotionController::read(m::LocalPose& p) noexcept {
    p.origin={XPLMGetDatad(xyz_[0]),XPLMGetDatad(xyz_[1]),XPLMGetDatad(xyz_[2])};
    p.heading_deg=XPLMGetDataf(euler_[0]);p.pitch_deg=XPLMGetDataf(euler_[1]);p.bank_deg=XPLMGetDataf(euler_[2]);
    return m::finite(p.origin)&&std::isfinite(p.heading_deg)&&std::isfinite(p.pitch_deg)&&std::isfinite(p.bank_deg);
}
bool MotionController::competing() noexcept {
    if(!have_written_)return false;
    const auto lat=XPLMGetDataf(lat_ref_),lon=XPLMGetDataf(lon_ref_);
    if(lat!=previous_lat_ref_ || lon!=previous_lon_ref_){have_written_=false;return false;}
    m::LocalPose actual;if(!read(actual))return true;
    return position_error(actual,report_.status.written)>0.05 || attitude_error(actual,report_.status.written)>0.01;
}
bool MotionController::apply(const m::PresentationPose& pose) noexcept {
    if(!bound_ || !owned_ || override_value()!=1){owned_=false;fault(m::Reason::override_lost);return false;}
    m::LocalPose local;if(!project(pose,local)){fault(m::Reason::invalid_pose);return false;}
    // All six values validated before the first write. Never write q, velocities or other systems.
    XPLMSetDatad(xyz_[0],local.origin.x);XPLMSetDatad(xyz_[1],local.origin.y);XPLMSetDatad(xyz_[2],local.origin.z);
    XPLMSetDataf(euler_[0],static_cast<float>(local.heading_deg));XPLMSetDataf(euler_[1],static_cast<float>(local.pitch_deg));XPLMSetDataf(euler_[2],static_cast<float>(local.bank_deg));
    auto& s=report_.status;s.written=local;
    if(!read(s.readback)){fault(m::Reason::competing_writer);return false;}
    s.position_error_m=position_error(local,s.readback);s.attitude_error_deg=attitude_error(local,s.readback);
    if(s.position_error_m>0.05 || s.attitude_error_deg>0.01){fault(m::Reason::competing_writer);return false;}
    s.presentation=pose;have_pose_=have_written_=true;previous_lat_ref_=XPLMGetDataf(lat_ref_);previous_lon_ref_=XPLMGetDataf(lon_ref_);
    s.applied_qpc=clock_();++s.applied_count;++s.revision;
    if(pose.provenance.kind==m::PoseSource::exporter) {
        if(s.applied_qpc>=pose.provenance.sample_end_qpc)report_.exporter_age.add(static_cast<std::uint64_t>(s.applied_qpc-pose.provenance.sample_end_qpc));
        report_.sampling_width.add(static_cast<std::uint64_t>(pose.provenance.sample_end_qpc-pose.provenance.sample_start_qpc));
    } else {
        report_.age.add(static_cast<std::uint64_t>(s.applied_qpc-pose.source.boost_received_qpc));report_.pitch.add(static_cast<std::uint64_t>(s.applied_qpc-pose.source.pitch_received_qpc));
    }
    flags();return true;
}
void MotionController::place(const m::PresentationPose& target) noexcept {
    reset_reconstruction();
    placed_=target;readiness_target_=target;placement_started_=clock_();++placement_attempt_;placement_returned_=0;
    relocating_=true;placement_ready_=placement_applied_=false;have_written_=false;
    readiness_destination_=true;
    ++report_.status.discontinuity_generation;++report_.status.revision;
    report_.status.flags|=m::placement_starts_engines;transition(m::Phase::relocating,m::Reason::scenery_loading);
    d::Context context;context.plugin_load_id=report_.status.plugin_load_id;context.enable_id=report_.status.enable_id;
    context.connection_id=report_.status.connection_id;context.session_epoch=target.source.session_epoch;
    events_.emit(d::Code::motion_placement,d::Severity::info,context,d::ErrorDomain::none,0,"Zero-speed initialization; SDK starts engines",
        {report_.status.operation_id,target.source.motion_epoch,target.source.boost_sequence,target.source.pitch_sequence,static_cast<std::uint64_t>(placement_started_),0});
    in_place_call_=true;
    if(readiness_sink_)readiness_sink_(readiness_context_);
    XPLMPlaceUserAtLocation(target.geo.latitude_deg,target.geo.longitude_deg,static_cast<float>(target.geo.altitude_msl_m),static_cast<float>(target.geo.heading_deg),0);
    in_place_call_=false;
    placement_returned_=clock_();
    if(readiness_sink_)readiness_sink_(readiness_context_);
    // Initialization may reset our bit. Reacquire only through the guarded pre path.
    if(override_value()==0)owned_=false;
}
MotionReadiness MotionController::readiness() const noexcept {
    return {bound_,reference_sampled_&&previous_reference_valid_&&profile_.qualified,
        follow_,follow_&&(relocating_||readiness_destination_),in_place_call_,
        placement_attempt_,placement_started_,placement_returned_,readiness_target_};
}
bool MotionController::readiness_bindings() const {
    if(!bound_||!sdk_||sdk_->ready()!=Status::ok)return false;
    const auto valid=[&](XPLMDataRef ref,int type,bool writing){return sdk_->inspect(ref,type,writing)==Status::ok;};
    if(!valid(override_,xplmType_IntArray,true)||!valid(lat_ref_,xplmType_Float,false)||
       !valid(lon_ref_,xplmType_Float,false)||!valid(cg_y_,xplmType_Float,false)||!valid(cg_z_,xplmType_Float,false))return false;
    for(unsigned i=0;i<3;++i)if(!valid(xyz_[i],xplmType_Double,true)||!valid(euler_[i],xplmType_Float,true))return false;
    return XPLMGetDatavi(override_,nullptr,0,0)>=1;
}
bool MotionController::readiness_reference() const {
    if(!profile_.qualified||profile_.generation!=report_.status.aircraft_generation)return false;
    const auto y=XPLMGetDataf(cg_y_),z=XPLMGetDataf(cg_z_);
    return std::isfinite(y)&&std::isfinite(z)&&std::abs(y-profile_.cg_y_ft)<0.01&&std::abs(z-profile_.cg_z_ft)<0.01;
}
bool MotionController::readiness_readback() const {
    if(!have_written_||!current_pose(report_.status.presentation))return false;
    m::LocalPose actual;
    actual.origin={XPLMGetDatad(xyz_[0]),XPLMGetDatad(xyz_[1]),XPLMGetDatad(xyz_[2])};
    actual.heading_deg=XPLMGetDataf(euler_[0]);actual.pitch_deg=XPLMGetDataf(euler_[1]);actual.bank_deg=XPLMGetDataf(euler_[2]);
    return m::finite(actual.origin)&&std::isfinite(actual.heading_deg)&&std::isfinite(actual.pitch_deg)&&std::isfinite(actual.bank_deg)&&
        position_error(actual,report_.status.written)<=0.05&&attitude_error(actual,report_.status.written)<=0.01;
}
void MotionController::pre(const b::LatestState* state,bool connected,const b::CompletePoseWindow* complete_window) noexcept {
    if(!sdk_ || sdk_->ready()!=Status::ok)return;
    auto finish=[&]{flags();};
    connection(connected&&state?state->session().connection_id:0);if(state&&connected)boundary(state->session());
    pause_diagnostics(state,connected);unpause_connected(connected);pause_diagnostics(state,connected);
    exporter_boundary(complete_window);
    if(!follow_ || faulted_){finish();return;}
    if(owned_ && override_value()!=1){owned_=false;fault(m::Reason::override_lost);finish();return;}
    m::PresentationPose target;bool frozen{};const auto reason=source(state,connected,target,frozen,complete_window,report_.status.source_preference);
    if(relocating_ && clock_()-placement_started_>static_cast<std::int64_t>(frequency_*m::relocation_timeout_ms/1000)) {fault(m::Reason::relocation_timeout);finish();return;}
    if(reason!=m::Reason::none){suspend_reconstruction();reconcile_=true;transition(m::Phase::holding,reason);finish();return;}
    if(readiness_target_.provenance.kind!=target.provenance.kind||
       readiness_target_.provenance.selection_epoch!=target.provenance.selection_epoch||
       readiness_target_.provenance.domain!=target.provenance.domain||
       readiness_target_.source.motion_epoch!=target.source.motion_epoch||
       readiness_target_.source.session_epoch!=target.source.session_epoch||
       readiness_target_.source.boost_generation!=target.source.boost_generation)readiness_target_=target;
    frozen_pose_diagnostics(state,target,frozen);
    if(!reference_valid()){suspend_reconstruction();transition(m::Phase::holding,profile_.qualified?m::Reason::reference_unqualified:m::Reason::profile_unqualified);finish();return;}
    if(!owned_) {
        if(override_value()!=0){fault(m::Reason::override_conflict);finish();return;}
        int one=1;XPLMSetDatavi(override_,&one,0,1);owned_=override_value()==1;
        if(!owned_){fault(m::Reason::override_lost);finish();return;}
        reset_reconstruction();
    }
    if(relocating_) {
        // Newest destination replaces pending work. No obsolete completion may apply.
        if(distant(placed_.geo,target.geo)){place(target);finish();return;}
        if(placement_applied_ && current_pose(report_.status.presentation)) {
            relocating_=placement_ready_=placement_applied_=false;
            readiness_destination_=false;
            transition(m::Phase::following,m::Reason::none);
            complete(m::AckResult::applied,m::Reason::none);finish();return;
        }
        placement_applied_=false;
        m::LocalPose actual;m::GeoPose where{};
        if(!read(actual)){finish();return;}
        XPLMLocalToWorld(actual.origin.x,actual.origin.y,actual.origin.z,&where.latitude_deg,&where.longitude_deg,&where.altitude_msl_m);
        if(!m::valid(where)||m::distance_m(where,placed_.geo)>100 || std::abs(where.altitude_msl_m-placed_.geo.altitude_msl_m)>100){finish();return;}
        m::Vec3 at;XPLMWorldToLocal(target.geo.latitude_deg,target.geo.longitude_deg,target.geo.altitude_msl_m,&at.x,&at.y,&at.z);
        if(!m::finite(at)) {finish();return;}
        XPLMProbeInfo_t terrain{};terrain.structSize=sizeof(terrain);
        // Readiness only: 100 km local-Y clearance avoids treating PSX-below-XP
        // terrain as missing scenery. This engineering margin never changes the pose.
        if(XPLMProbeTerrainXYZ(probe_,static_cast<float>(at.x),static_cast<float>(at.y+100000.0),static_cast<float>(at.z),&terrain)!=xplm_ProbeHitTerrain ||
           !std::isfinite(terrain.locationX)||!std::isfinite(terrain.locationY)||!std::isfinite(terrain.locationZ)){finish();return;}
        // SDK placement has returned, current lifecycle/profile is verified, and destination readback agrees.
        // This is not a terrain-conformance assertion.
        placement_ready_=true;need_initial_=false;reconcile_=false;finish();return;
    }
    if(need_initial_ || (reconcile_ && have_pose_ && distant(report_.status.presentation.geo,target.geo))) {place(target);finish();return;}
    reconcile_=false;readiness_destination_=false;finish();
}
void MotionController::post(const b::LatestState* state,bool connected,const b::MotionWindow* window,const b::CompletePoseWindow* complete_window) noexcept {
    if(!sdk_ || sdk_->ready()!=Status::ok)return;
    const auto begin=clock_();if(last_post_ && begin>=last_post_)report_.interval.add(static_cast<std::uint64_t>(begin-last_post_));last_post_=begin;
    auto finish=[&]{flags();};
    if(!owned_ || faulted_){finish();return;}
    if(override_value()!=1){owned_=false;fault(m::Reason::override_lost);finish();return;}
    if(competing()){fault(m::Reason::competing_writer);finish();return;}
    m::PresentationPose target;bool frozen{};const auto reason=source(state,connected,target,frozen,complete_window,report_.status.source_preference);
    if(relocating_ && !placement_ready_){finish();return;}
    if(!follow_){finish();return;}
    // Match pre's source/reference precedence, and never rewrite a held
    // pose against an invalid fixed aircraft reference, including during gaps.
    if(!reference_valid()) {
        suspend_reconstruction();
        transition(m::Phase::holding,reason!=m::Reason::none?reason:
            profile_.qualified?m::Reason::reference_unqualified:m::Reason::profile_unqualified);finish();return;
    }
    if(reason!=m::Reason::none) {
        suspend_reconstruction();
        if(relocating_) {transition(m::Phase::holding,reason);finish();return;}
        reconcile_=true;if(have_pose_)apply(report_.status.presentation);
        transition(m::Phase::holding,reason);finish();return;
    }
    if(selection_.source==m::PoseSource::public_main_boost&&state && !m::same_motion_domain(b::history_domain(state->session()),seen_domain_)) {
        suspend_reconstruction();
        reconcile_=true;if(have_pose_)apply(report_.status.presentation);transition(m::Phase::holding,m::Reason::epoch_changed);finish();return;
    }
    if(reconcile_ || (relocating_ && distant(placed_.geo,target.geo))) {if(have_pose_&&!relocating_)apply(report_.status.presentation);finish();return;}
    auto& status=report_.status;m::ReconstructionResult result;
    if(!reconstruct_target(*state,window,complete_window,target,begin,result)) {
        if(have_pose_&&!relocating_)apply(status.presentation);
        transition(m::Phase::holding,status.reason);finish();return;
    }
    double handoff_position{},handoff_angle{};
    if(handoff_pending_&&have_pose_) {
        handoff_position=std::hypot(m::distance_m(status.presentation.geo,result.pose.geo),status.presentation.geo.altitude_msl_m-result.pose.geo.altitude_msl_m);
        const auto attitude=[](const m::GeoPose& g){return m::quaternion(m::compose(m::earth_tangent(g.latitude_deg,g.longitude_deg),m::body_rotation(g.heading_deg,g.pitch_deg,g.bank_deg)));};
        handoff_angle=m::orientation_error_deg(attitude(status.presentation.geo),attitude(result.pose.geo));
    }
    if(apply(result.pose)) {
        if(handoff_pending_){status.handoff_position_m=handoff_position;status.handoff_attitude_deg=handoff_angle;handoff_pending_=false;}
        cursor_=result.trace.presentation_qpc;status.reconstruction_trace=applied_trace_=result.trace;
        reconstruction_metrics(status.applied_qpc);
        if(relocating_) {
            placement_applied_=true;finish();return; // Completion is reported in the next pre callback.
        }
        transition(m::Phase::following,m::Reason::none);
        complete(m::AckResult::applied,m::Reason::none);
    }
    finish();
}
bool MotionController::reconstruct_target(const b::LatestState& state,const b::MotionWindow* window,const b::CompletePoseWindow* complete_window,
    const m::PresentationPose& target,std::int64_t now,m::ReconstructionResult& result) noexcept {
    auto& status=report_.status;status.reason=m::Reason::source_unavailable;
    bool reconstructed=false;
    const bool exporter=selection_.source==m::PoseSource::exporter;
    if(exporter) {
        status.boost_history_count=status.pitch_history_count=0;
        if(!complete_matches(complete_window)){status.exporter_history_count=0;status.reconstruction_trace={};close_hold();return false;}
        const auto& history=complete_window->samples;
        const auto branch=history.samples[history.count-1].flags&(m::raw_ordinary_altitude|m::raw_paused_altitude);
        if(history.sequence_floor!=exporter_floor_||branch!=exporter_branch_) {
            suspend_reconstruction();reconcile_=true;status.reason=m::Reason::epoch_changed;return false;
        }
        status.exporter_history_count=history.count;
        if(!reconstruction_ready_){seed_anchor(target);anchor_trace_.window_revision=history.revision;}
        reconstructed=m::reconstruct(history,status.reconstruction,now,frequency_,cursor_,result,psx::sync::freshness_now(now)-now);
    } else {
        status.exporter_history_count=0;const auto& session=state.session();
        // Worker admission validates immutable records. Independently acquired
        // SDK slots must still match the current control/session envelope.
        if(!window||window->connection_id!=session.connection_id||window->session_revision!=session.revision||
           window->selection_epoch!=selection_.selection_epoch||!m::same_motion_domain(window->samples.domain,b::history_domain(session))||
           window->samples.domain.configuration_epoch!=session.configuration_epoch||!window->samples.boost_count) {
            close_hold();status.reconstruction_trace={};status.boost_history_count=status.pitch_history_count=0;return false;
        }
        const auto& history=window->samples;const auto newest=history.boost[history.boost_count-1].stamp.received_qpc;
        if(d::freshness(true,psx::sync::freshness_now(now),newest,frequency_,boost_timeout_ms_)!=d::Freshness::current) {
            suspend_reconstruction();status.reason=m::Reason::boost_delayed;return false;
        }
        status.boost_history_count=history.boost_count;status.pitch_history_count=static_cast<std::uint16_t>(history.pitch_count+(history.has_pitch_anchor?1:0));
        if(!reconstruction_ready_) {
            bool boost_present=false,pitch_present=false;
            for(std::size_t i=0;i<history.boost_count;++i)boost_present=boost_present||history.boost[i].stamp.sequence==target.source.boost_sequence;
            if(history.has_pitch_anchor)pitch_present=history.pitch_anchor.stamp.sequence==target.source.pitch_sequence;
            for(std::size_t i=0;i<history.pitch_count;++i)pitch_present=pitch_present||history.pitch[i].stamp.sequence==target.source.pitch_sequence;
            if(!boost_present||!pitch_present) {
                m::reconstruct(history,status.reconstruction,now,frequency_,0,result,m::instant_reference_revision,psx::sync::freshness_now(now)-now);status.reconstruction_trace=result.trace;
                if(result.trace.state==m::ReconstructionState::no_pitch)++status.no_pitch_count;close_hold();return false;
            }
            seed_anchor(target);anchor_trace_.window_revision=history.revision;
        }
        reconstructed=m::reconstruct(history,status.reconstruction,now,frequency_,cursor_,result,m::instant_reference_revision,psx::sync::freshness_now(now)-now);
    }
    const auto delay=static_cast<std::int64_t>(frequency_*status.reconstruction.delay_ms/1000);
    const auto wanted=now>=delay?now-delay:0;
    const auto anchor_pitch=exporter?cursor_:anchor_.source.pitch_received_qpc;
    const bool actual_before_anchor_pitch=!exporter&&(reconstructed||result.trace.state==m::ReconstructionState::no_pitch)&&result.trace.presentation_qpc<anchor_pitch;
    if(status.reconstruction.mode==m::ReconstructionMode::interpolated&&anchor_valid_&&
       (wanted<=(std::max)(cursor_,anchor_pitch)||actual_before_anchor_pitch)) {
        result.pose=anchor_;result.trace=anchor_trace_;result.trace.state=m::ReconstructionState::newest_hold;result.trace.target_qpc=wanted;
    } else if(!reconstructed) {
        status.reconstruction_trace=result.trace;if(result.trace.state==m::ReconstructionState::no_pitch)++status.no_pitch_count;close_hold();return false;
    } else anchor_valid_=false;
    result.pose.provenance.selection_epoch=selection_.selection_epoch;status.reason=m::Reason::none;return true;
}
void MotionController::close_hold() noexcept {
    if(hold_state_!=m::ReconstructionState::unavailable && last_eligible_>=hold_started_)
        report_.hold_duration.add(static_cast<std::uint64_t>(last_eligible_-hold_started_));
    hold_state_=m::ReconstructionState::unavailable;hold_started_=last_eligible_=0;
}
void MotionController::reset_reconstruction() noexcept {
    close_hold();cursor_=0;anchor_valid_=reconstruction_ready_=false;
    // applied_trace_ stays paired with the last actually written presentation;
    // acceptance is reset separately, including across multiple queued Follows.
    report_.status.reconstruction_trace={};report_.status.boost_history_count=report_.status.pitch_history_count=0;
    report_.status.exporter_history_count=0;
    ++report_.status.history_reset_count;
}
void MotionController::suspend_reconstruction() noexcept {if(reconstruction_ready_||anchor_valid_)reset_reconstruction();else close_hold();}
void MotionController::seed_anchor(const m::PresentationPose& pose,const m::ReconstructionTrace* trace) noexcept {
    anchor_=pose;anchor_trace_=trace?*trace:m::ReconstructionTrace{};
    if(!trace || trace->state==m::ReconstructionState::unavailable) {
        if(pose.provenance.kind==m::PoseSource::exporter) {
            anchor_trace_.left_sequence=anchor_trace_.right_sequence=pose.provenance.left_sequence;
            anchor_trace_.left_received_qpc=anchor_trace_.right_received_qpc=pose.provenance.received_qpc;
            anchor_trace_.presentation_qpc=pose.provenance.sample_end_qpc;
        } else {
            anchor_trace_.left_sequence=anchor_trace_.right_sequence=pose.source.boost_sequence;
            anchor_trace_.left_received_qpc=anchor_trace_.right_received_qpc=anchor_trace_.presentation_qpc=pose.source.boost_received_qpc;
        }
    }
    cursor_=anchor_trace_.presentation_qpc;anchor_valid_=reconstruction_ready_=true;
}
void MotionController::reconstruction_metrics(std::int64_t now) noexcept {
    auto& s=report_.status;const auto state=s.reconstruction_trace.state;
    // A callback gap has no observed presentation interval. Finish the prior
    // episode at its last measured callback; never charge scenery/source gaps.
    if(last_eligible_ && now-last_eligible_>=static_cast<std::int64_t>(frequency_*callback_delayed_ms_/1000)) {
        close_hold();last_eligible_=now;return;
    }
    const bool hold=state==m::ReconstructionState::oldest_hold||state==m::ReconstructionState::newest_hold;
    if(hold && state!=hold_state_) {close_hold();hold_state_=state;hold_started_=now;++s.hold_transitions;}
    else if(!hold)close_hold();
    last_eligible_=now;
    if(state==m::ReconstructionState::interpolated)++s.bracketed_count;
    else if(state==m::ReconstructionState::oldest_hold)++s.oldest_hold_count;
    else if(state==m::ReconstructionState::newest_hold)++s.newest_hold_count;
    if(now>=s.reconstruction_trace.presentation_qpc)report_.output_age.add(static_cast<std::uint64_t>(now-s.reconstruction_trace.presentation_qpc));
}
}
