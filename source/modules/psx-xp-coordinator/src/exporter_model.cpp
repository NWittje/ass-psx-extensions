#include "coordinator/exporter.hpp"
#include <algorithm>
#include <cmath>
namespace coordinator {
namespace m=psx::motion;namespace e=psx::exporter;
bool valid_physical_pose(const m::RawPose& raw,std::int64_t received) noexcept {
    const auto branch=raw.flags&(m::raw_ordinary_altitude|m::raw_paused_altitude);
    return m::valid(raw)&&received>=0&&(received>=raw.sample_end_qpc||raw.sample_end_qpc-received==1)&&
        (raw.flags&m::raw_pose_valid)&&!(raw.flags&(m::raw_grid_north|m::raw_grid_south|m::raw_normalization|m::raw_transition|m::raw_guard_rejected))&&
        (branch==m::raw_ordinary_altitude||branch==m::raw_paused_altitude)&&std::abs(raw.latitude_rad)<=89*m::pi/180&&
        std::abs(raw.total_pitch_rad)<=m::pi&&std::abs(raw.bank_rad)<=m::pi&&
        raw.altitude_ft>=-10000&&raw.altitude_ft<=200000;
}
bool exporter_identity_matches(ProcessIdentity p,std::uint32_t client,std::uint64_t frequency,const e::Hello& h) noexcept {
    e::Frame checked;return p.pid&&p.creation_filetime&&client==p.pid&&h.pid==p.pid&&h.process_creation_filetime==p.creation_filetime&&
        h.qpc_frequency==frequency&&e::encode(h,0,checked);
}
const char* pose_source_name(m::PoseSource s) noexcept{return s==m::PoseSource::exporter?"Exporter":"Public Main/Boost";}
const char* selection_reason_name(m::SelectionReason r) noexcept {
    constexpr const char* labels[]={"Startup","Exporter ready","Exporter valid-sample timeout","Exporter unavailable","Public source unavailable","Follow preference","Pose discontinuity","Connection"};
    const auto i=static_cast<unsigned>(r);return i<std::size(labels)?labels[i]:"Unknown";
}
void ExporterModel::unavailable(std::uint32_t error) noexcept {state_.peer=false;state_.ready=false;state_.stable_since=0;state_.error=error;physical_.available=false;}
void ExporterModel::boundary(const m::PoseDomain& domain,std::uint64_t floor) noexcept {
    history_.synchronize(domain,floor);state_.stable_since=0;state_.ready=false;++state_.boundaries;
    state_.boundary_pending=true;
}
bool ExporterModel::event(const ExporterEvent& value,std::uint64_t configuration) noexcept {
    if(value.received_qpc<0)return false;
    if(value.kind==ExporterEventKind::association){
        if(!value.process.pid||!value.process.creation_filetime)return false;
        if(value.process!=state_.process){state_.process=value.process;unavailable(0);state_.hello={};state_.health={};state_.last_sequence=state_.last_frame=0;history_.clear();state_.boundary_pending=true;}
        return true;
    }
    if(value.process!=state_.process)return false;
    if(value.kind==ExporterEventKind::unavailable){unavailable(value.error);return true;}
    e::Header h;if(value.frame.size<e::header_size||!e::read_header(value.frame.bytes.data(),e::header_size,h)||value.frame.size!=e::header_size+h.payload_size)return false;
    const auto* p=value.frame.bytes.data()+e::header_size;
    if(value.kind==ExporterEventKind::hello){
        e::Hello hello;if(h.kind!=e::Kind::hello||!e::decode(p,h.payload_size,hello)||!exporter_identity_matches(state_.process,value.client_pid,frequency_,hello)){unavailable(13);return false;}
        if(!state_.peer||hello.producer!=state_.hello.producer){history_.clear();physical_.available=false;state_.stable_since=0;state_.last_sequence=state_.last_frame=0;state_.boundary_pending=true;}
        state_.hello=hello;state_.protocol_version=h.version;state_.peer=true;state_.error=0;return true;
    }
    if(!state_.peer)return false;
    if(value.kind==ExporterEventKind::audio_snapshot||value.kind==ExporterEventKind::audio_ack)return h.version==2;
    if(value.kind==ExporterEventKind::health){e::Health health;if(h.kind!=e::Kind::health||!e::decode(p,h.payload_size,health))return false;state_.health=health;return true;}
    if(value.kind!=ExporterEventKind::snapshot||h.kind!=e::Kind::snapshot)return false;
    ++state_.raw;e::Snapshot raw;
    if(!e::decode(p,h.payload_size,raw)){++state_.invalid;return true;}
    if(history_.window().domain.producer==state_.hello.producer&&raw.pose_epoch<history_.window().domain.pose_epoch){++state_.invalid;return true;}
    if(state_.last_sequence&&raw.sequence>state_.last_sequence+1)state_.sequence_gaps+=raw.sequence-state_.last_sequence-1;
    if(state_.last_frame&&raw.frame_sequence>state_.last_frame+1)state_.frame_gaps+=raw.frame_sequence-state_.last_frame-1;
    if(raw.sequence<=state_.last_sequence||raw.frame_sequence<=state_.last_frame){++state_.invalid;return true;}
    state_.last_sequence=raw.sequence;state_.last_frame=raw.frame_sequence;
    // Physical terrain acquisition precedes and is independent of the eye recipe.
    // event()'s return value denotes processing, never physical sample admission.
    if(valid_physical_pose(raw,value.received_qpc)&&
       (!physical_.available||physical_.producer!=state_.hello.producer||
        (raw.pose_epoch>=physical_.raw.pose_epoch&&raw.sample_end_qpc>=physical_.raw.sample_end_qpc&&value.received_qpc>=physical_.received_qpc))) {
        physical_.available=true;physical_.producer=state_.hello.producer;physical_.raw=raw;
        physical_.received_qpc=value.received_qpc;++physical_.revision;
    }
    m::CompletePoseSample sample;if(!m::derive_pose(raw,value.received_qpc,configuration,sample,reference_revision())){++state_.invalid;return true;}
    const m::PoseDomain domain{state_.hello.producer,raw.pose_epoch,reference_revision()};
    const auto& w=history_.window();bool reset=w.domain!=domain;
    if(w.count){const auto& previous=w.samples[w.count-1];
        const auto delta=sample.sample_end_qpc-previous.sample_end_qpc;
        reset|=delta>=static_cast<std::int64_t>(frequency_*m::exporter_timeout_ms/1000);
        reset|=(sample.flags&(m::raw_ordinary_altitude|m::raw_paused_altitude))!=(previous.flags&(m::raw_ordinary_altitude|m::raw_paused_altitude));
        if(delta>=0&&delta<=static_cast<std::int64_t>(frequency_/2)){m::GeoPose a,b;if(m::geographic_pose(previous,a)&&m::geographic_pose(sample,b)){
            const double seconds=static_cast<double>(delta)/static_cast<double>(frequency_);const double rate=(std::max)(1.0,raw.simulation_rate);
            reset|=m::distance_m(a,b)>(std::max)(1000.0,800.0*seconds*rate)||std::abs(a.altitude_msl_m-b.altitude_msl_m)>(std::max)(500.0,250.0*seconds*rate);
        }}
    }
    if(reset)boundary(domain,sample.sequence);
    if(!history_.append(sample)){++state_.invalid;return true;}
    if(!state_.stable_since)state_.stable_since=sample.sample_end_qpc;
    state_.last_valid_qpc=value.received_qpc;state_.last_sample_end_qpc=sample.sample_end_qpc;
    state_.last_sampling_width=sample.sample_end_qpc-sample.sample_start_qpc;return true;
}
bool ExporterModel::fresh(std::int64_t now) const noexcept {
    return frequency_&&state_.peer&&history_.window().count&&now>=state_.last_sample_end_qpc&&
        now-state_.last_sample_end_qpc<static_cast<std::int64_t>(frequency_*m::exporter_timeout_ms/1000);
}
bool ExporterModel::eligible(std::int64_t now) const noexcept {
    if(!fresh(now)||!state_.stable_since||now-state_.stable_since<static_cast<std::int64_t>(frequency_*m::exporter_stable_ms/1000))return false;
    if(state_.settings.mode==m::ReconstructionMode::direct)return true;
    const auto delay=static_cast<std::int64_t>(frequency_*state_.settings.delay_ms/1000);
    if(now<delay)return false;const auto target=now-delay;const auto& w=history_.window();
    return w.count>=2&&w.samples[0].sample_end_qpc<=target&&w.samples[w.count-1].sample_end_qpc>=target;
}
bool ExporterModel::select(std::int64_t now,bool public_ready) noexcept {
    auto source=state_.selection.source;auto reason=state_.selection.reason;m::PoseDomain domain=state_.selection.domain;
    state_.ready=eligible(now);
    if(state_.preference==m::SourcePreference::public_only){source=m::PoseSource::public_main_boost;reason=m::SelectionReason::preference;domain={{},0,reference_revision()};}
    else if(source==m::PoseSource::exporter){
        if(!state_.peer||!fresh(now)||state_.boundary_pending){source=m::PoseSource::public_main_boost;domain={{},0,reference_revision()};reason=!state_.peer?m::SelectionReason::exporter_unavailable:state_.boundary_pending?m::SelectionReason::discontinuity:m::SelectionReason::exporter_timeout;}
    }else if(state_.ready){source=m::PoseSource::exporter;domain=history_.window().domain;reason=m::SelectionReason::exporter_ready;}
    else if(!public_ready)reason=m::SelectionReason::public_unavailable;
    if(!fresh(now))state_.stable_since=0;
    const bool changed=source!=state_.selection.source||domain!=state_.selection.domain;
    state_.boundary_pending=false;
    if(changed){++state_.selection.selection_epoch;++state_.selection.revision;state_.selection.source=source;state_.selection.domain=domain;state_.selection.reason=reason;state_.selection.selected_qpc=now;}
    // Reason is an explanation of the last ordered selection, never an
    // unstamped mutation of an already-published required control.
    return changed;
}
bool ExporterModel::restore(const ExporterState& state,const m::CompletePoseWindow& window,const m::CompleteHistoryCounters& counters) noexcept {
    // A retained flight-state checkpoint is untrusted input. Qualify its bounded
    // history before consulting the newest sample or replacing current state.
    if(window.count>m::history_capacity||(window.count&&!m::valid(window)))return false;
    psx::bridge::Frame frame;if(!frequency_||!m::valid(state.settings)||static_cast<unsigned>(state.preference)>1||!psx::bridge::encode(state.selection,0,frame))return false;
    if(state.peer&&!exporter_identity_matches(state.process,state.process.pid,frequency_,state.hello))return false;
    if(state.invalid>state.raw||counters.accepted>state.raw||state.ready&&!state.peer)return false;
    if(m::valid(window.domain)&&window.domain.reference_revision!=state.selection.domain.reference_revision)return false;
    if(window.count){const auto& last=window.samples[window.count-1];if(window.domain.producer!=state.hello.producer||last.sequence>state.last_sequence||last.frame_sequence>state.last_frame||last.sample_end_qpc!=state.last_sample_end_qpc||last.received_qpc!=state.last_valid_qpc)return false;}
    if(!history_.restore_capture_checkpoint(window,counters))return false;state_=state;physical_.available=false;return true;
}
}
