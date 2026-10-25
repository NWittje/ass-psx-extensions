#include <psx/bridge/protocol.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace psx::bridge {
static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559,
              "Observation bridge requires IEEE binary64 doubles");
namespace {
struct Writer {
    std::uint8_t* data; std::size_t size, pos{}; bool ok{true};
    void u(std::uint64_t value, std::size_t bytes) noexcept {
        if (pos>size || bytes>size-pos) { ok=false; return; }
        for (std::size_t i=0;i<bytes;++i) data[pos++]=static_cast<std::uint8_t>(value>>(8*i));
    }
    void real(double value) noexcept { std::uint64_t bits{}; std::memcpy(&bits,&value,8); u(bits,8); }
    template<std::size_t N> void text(const std::array<char,N>& value) noexcept {
        std::size_t length=0; while(length<N && value[length]) ++length;
        if(length==N) { ok=false; return; }
        u(length,2); for(std::size_t i=0;i<length;++i) u(static_cast<unsigned char>(value[i]),1);
    }
};
struct Reader {
    const std::uint8_t* data; std::size_t size, pos{}; bool ok{true};
    std::uint64_t u(std::size_t bytes) noexcept {
        if(!data || pos>size || bytes>size-pos) { ok=false; return 0; }
        std::uint64_t v=0; for(std::size_t i=0;i<bytes;++i) v|=std::uint64_t(data[pos++])<<(8*i); return v;
    }
    std::int64_t ticks() noexcept {
        auto v=u(8); if(v>std::uint64_t((std::numeric_limits<std::int64_t>::max)())) {ok=false;return 0;}
        return static_cast<std::int64_t>(v);
    }
    double real() noexcept { auto bits=u(8); double v{}; std::memcpy(&v,&bits,8); return v; }
    template<std::size_t N> void text(std::array<char,N>& v) noexcept {
        const auto n=u(2); if(n>=N) {ok=false;return;}
        for(std::size_t i=0;i<n;++i) {v[i]=static_cast<char>(u(1));if(!v[i]) ok=false;}
    }
    bool end() const noexcept {return ok && pos==size;}
};
bool valid(const ViewRequest& v) noexcept {
    return v.connection_id&&v.operation_id&&v.plugin_load_id&&v.enable_id&&v.requested_qpc>=0&&static_cast<unsigned>(v.action)<=2;
}
bool valid(const ViewReply& v) noexcept {
    return v.connection_id&&v.operation_id&&v.plugin_load_id&&v.enable_id&&v.generated_qpc>=0&&static_cast<unsigned>(v.state)<=4&&static_cast<unsigned>(v.reason)<=3&&
        ((v.state!=ViewState::pending&&v.state!=ViewState::enforcing)||v.selected)&&
        ((v.state!=ViewState::inactive&&v.state!=ViewState::released)||!v.selected)&&
        (v.state!=ViewState::enforcing||v.observed_view>0);
}
void put(Writer& w,const ViewRequest& v) noexcept {
    w.u(v.connection_id,8);w.u(v.operation_id,8);w.u(v.plugin_load_id,8);w.u(v.enable_id,8);w.u(static_cast<std::uint64_t>(v.requested_qpc),8);w.u(static_cast<unsigned>(v.action),1);
}
void get(Reader& r,ViewRequest& v) noexcept {
    v.connection_id=r.u(8);v.operation_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.requested_qpc=r.ticks();v.action=static_cast<ViewAction>(r.u(1));
}
void put(Writer& w,const ViewReply& v) noexcept {
    w.u(v.connection_id,8);w.u(v.operation_id,8);w.u(v.plugin_load_id,8);w.u(v.enable_id,8);w.u(static_cast<std::uint64_t>(v.generated_qpc),8);
    w.u(static_cast<unsigned>(v.state),1);w.u(static_cast<unsigned>(v.reason),1);w.u(v.selected?1:0,1);w.u(v.observed_view,4);
}
void get(Reader& r,ViewReply& v) noexcept {
    v.connection_id=r.u(8);v.operation_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.generated_qpc=r.ticks();
    v.state=static_cast<ViewState>(r.u(1));v.reason=static_cast<ViewReason>(r.u(1));const auto selected=r.u(1);if(selected>1)r.ok=false;v.selected=selected==1;v.observed_view=static_cast<std::uint32_t>(r.u(4));
}
bool valid(const Hello& v) noexcept {
    return (v.role==Role::coordinator || v.role==Role::plugin) &&
        v.aircraft_revision==aircraft::contract_revision && v.instance_id && v.qpc_frequency;
}
bool valid(const SessionStatus& v) noexcept {
    return v.connection_id && v.revision && v.published_qpc>=0 && !(v.flags&~63u) &&
        static_cast<unsigned>(v.motion_boundary)<=static_cast<unsigned>(motion::Boundary::recovery) &&
        (v.motion_epoch || (!v.motion_sequence_floor && v.motion_boundary==motion::Boundary::none)) &&
        static_cast<unsigned>(v.family)<=static_cast<unsigned>(aircraft::EngineFamily::rr) &&
        (!(v.flags&synchronized) || ((v.flags&main_connected) && v.session_epoch)) &&
        (!(v.flags&boost_available) || ((v.flags&boost_connected) && v.boost_generation));
}
bool valid(const ObservationMessage& v) noexcept {
    const auto& o=v.observation;
    if(!v.connection_id || !v.session_revision || !o.stamp.session_epoch || !o.stamp.sequence ||
       o.stamp.received_qpc<0 || v.decoded_qpc<o.stamp.received_qpc || v.published_qpc<v.decoded_qpc ||
       !supported_source(o.source) || !aircraft::well_formed(o) || !o.count ||
       ((o.source==aircraft::SourceRecord::boost) ? !v.boost_generation : v.boost_generation!=0)) return false;
    for(std::size_t i=0;i<o.count;++i) if(!std::isfinite(o.values[i].value)) return false;
    return true;
}
bool valid(const Distribution& d) noexcept {
    return d.count ? d.minimum<=d.p50 && d.p50<=d.p95 && d.p95<=d.p99 && d.p99<=d.maximum
                   : !(d.minimum || d.maximum || d.p50 || d.p95 || d.p99);
}
bool valid(const PluginStatus& v) noexcept {
    if(!v.connection_id || v.callback_qpc<0 || (v.flags&~7u) ||
        !valid(v.receive_to_callback) || !valid(v.callback_duration) || !valid(v.frame_interval)) return false;
    for(auto t:v.received_qpc) if(t<0) return false;
    if(v.generated_qpc<0 || (v.diagnostic_validity&~3u) ||
       ((v.diagnostic_validity&counter_baseline_valid) && !v.plugin_load_id) ||
       ((v.diagnostic_validity&callback_snapshot_valid) && (!v.plugin_load_id || !v.enable_id || !v.callback_count || !v.callback_qpc || v.generated_qpc<v.callback_qpc))) return false;
    if(v.duration_completed_callback>v.callback_count)return false;
    for(const auto* m:{&v.receive_to_callback_max,&v.callback_duration_max,&v.frame_interval_max}) {
        if(m->qpc<0 || m->previous_qpc<0 || m->previous_qpc>m->qpc || (!m->count && (m->ticks || m->callback_count || m->qpc || m->previous_qpc))) return false;
        if(m->count && (!v.plugin_load_id || !v.enable_id || !m->callback_count || m->callback_count>v.callback_count || !m->qpc || m->qpc>v.callback_qpc || m->qpc>v.generated_qpc))return false;
    }
    if(v.callback_duration_max.callback_count>v.duration_completed_callback || v.callback_duration_max.count>v.duration_completed_callback || v.frame_interval_max.count>v.callback_count)return false;
    return true;
}
bool valid(const DiagnosticBatch& v) noexcept {
    if(!v.connection_id || !v.count || v.count>v.events.size()) return false;
    for(std::size_t i=0;i<v.count;++i) if(!diagnostics::valid(v.events[i])) return false;
    return true;
}
bool valid(const MotionRequest& v) noexcept {
    return v.connection_id&&v.operation_id&&v.plugin_load_id&&v.enable_id&&v.requested_qpc>=0&&
        motion::valid(v.reconstruction)&&static_cast<unsigned>(v.source_preference)<=1&&
        (v.action==motion::Action::stop || (v.action==motion::Action::follow&&v.expected_selection_epoch));
}
bool empty(const motion::PoseDomain& d) noexcept {
    return !motion::valid(d.producer)&&!d.pose_epoch&&motion::known_reference_revision(d.reference_revision);
}
bool valid(const motion::PoseProvenance& p) noexcept {
    if(p.kind==motion::PoseSource::public_main_boost)
        return empty(p.domain)&&!p.left_sequence&&!p.right_sequence&&!p.frame_sequence&&!p.sample_start_qpc&&!p.sample_end_qpc&&!p.received_qpc;
    return p.kind==motion::PoseSource::exporter&&p.selection_epoch&&motion::valid(p.domain)&&p.left_sequence&&
        p.right_sequence>=p.left_sequence&&p.frame_sequence&&p.sample_start_qpc>=0&&
        p.sample_end_qpc>=p.sample_start_qpc&&p.received_qpc>=0&&
        (p.received_qpc>=p.sample_end_qpc||p.sample_end_qpc-p.received_qpc==1);
}
bool valid(const SourceSelection& v) noexcept {
    return v.connection_id&&v.revision&&v.selection_epoch&&v.selected_qpc>=0&&
        static_cast<unsigned>(v.reason)<=static_cast<unsigned>(motion::SelectionReason::connection)&&
        ((v.source==motion::PoseSource::public_main_boost&&empty(v.domain))||
         (v.source==motion::PoseSource::exporter&&motion::valid(v.domain)));
}
bool valid(const MotionAck& v) noexcept {
    return v.connection_id&&v.operation_id&&v.plugin_load_id&&v.enable_id&&v.generated_qpc>=0&&
        static_cast<unsigned>(v.result)>=1&&static_cast<unsigned>(v.result)<=4&&
        static_cast<unsigned>(v.reason)<=static_cast<unsigned>(motion::Reason::disabled);
}
bool valid(const motion::LocalPose& v) noexcept {
    return motion::finite(v.origin)&&std::isfinite(v.heading_deg)&&std::isfinite(v.pitch_deg)&&std::isfinite(v.bank_deg);
}
bool valid(const MotionStatus& v) noexcept {
    if(!v.connection_id||!v.plugin_load_id||!v.enable_id||v.generated_qpc<0||v.applied_qpc<0||v.applied_qpc>v.generated_qpc||
       static_cast<unsigned>(v.phase)>static_cast<unsigned>(motion::Phase::fault)||
       static_cast<unsigned>(v.reason)>static_cast<unsigned>(motion::Reason::disabled)||(v.flags&~255u)||
       !motion::valid(v.presentation.geo)||!valid(v.written)||!valid(v.readback)||
       !std::isfinite(v.position_error_m)||v.position_error_m<0||!std::isfinite(v.attitude_error_deg)||v.attitude_error_deg<0||
       !valid(v.pre_callback_duration)||!valid(v.post_callback_duration)||!valid(v.boost_receive_to_apply)||!valid(v.pitch_age)||!valid(v.frame_interval))return false;
    const auto& p=v.presentation.source;
    if(p.boost_received_qpc<0||p.pitch_received_qpc<0)return false;
    const auto& t=v.reconstruction_trace;
    if(!motion::valid(v.reconstruction) || static_cast<unsigned>(t.state)>static_cast<unsigned>(motion::ReconstructionState::invalid_timing) ||
       t.target_qpc<0 || t.presentation_qpc<0 || t.left_received_qpc<0 || t.right_received_qpc<t.left_received_qpc ||
       !std::isfinite(t.alpha) || t.alpha<0 || t.alpha>1 || v.boost_history_count>motion::history_capacity || v.pitch_history_count>motion::history_capacity+1 ||
       !valid(v.output_age) || !valid(v.hold_duration))return false;
    const auto& provenance=v.presentation.provenance;
    if(!v.selection_epoch||static_cast<unsigned>(v.source_preference)>1||v.exporter_history_count>motion::history_capacity||
       !valid(provenance)||
       !valid(v.exporter_sample_to_apply)||!valid(v.sampling_width)||
       !std::isfinite(v.handoff_position_m)||v.handoff_position_m<0||!std::isfinite(v.handoff_attitude_deg)||v.handoff_attitude_deg<0||
       !((v.selected_source==motion::PoseSource::public_main_boost&&empty(v.selected_domain))||
         (v.selected_source==motion::PoseSource::exporter&&motion::valid(v.selected_domain))))return false;
    if(v.flags&motion::pose_valid) {
        if(std::abs(v.presentation.geo.latitude_deg)>89)return false;
        if(provenance.kind==motion::PoseSource::public_main_boost) {
            if(!p.session_epoch||!p.boost_generation||!p.motion_epoch||!p.boost_sequence||!p.pitch_sequence||
               p.boost_received_qpc>v.generated_qpc||p.pitch_received_qpc>v.generated_qpc)return false;
        } else if(p.session_epoch||p.boost_generation||p.motion_epoch||p.boost_sequence||p.pitch_sequence||
                  p.boost_received_qpc||p.pitch_received_qpc||provenance.received_qpc>v.applied_qpc)return false;
    }
    return true;
}
bool valid(const MotionWindow& v) noexcept {
    if(!v.connection_id || !v.session_revision || !v.selection_epoch || v.published_qpc<0 || !motion::valid(v.samples))return false;
    const auto& w=v.samples;
    for(std::size_t i=0;i<w.boost_count;++i)if(w.boost[i].stamp.received_qpc>v.published_qpc)return false;
    for(std::size_t i=0;i<w.pitch_count;++i)if(w.pitch[i].stamp.received_qpc>v.published_qpc)return false;
    return !w.has_pitch_anchor || w.pitch_anchor.stamp.received_qpc<=v.published_qpc;
}
bool valid(const CompletePoseWindow& v) noexcept {
    if(!v.connection_id||!v.selection_epoch||v.published_qpc<0||!motion::valid(v.samples))return false;
    for(std::size_t i=0;i<v.samples.count;++i)if(v.samples.samples[i].received_qpc>v.published_qpc)return false;
    return true;
}
bool same_observation(const aircraft::Observation& a,const aircraft::Observation& b) noexcept {
    if(a.stamp.session_epoch!=b.stamp.session_epoch || a.stamp.configuration_epoch!=b.stamp.configuration_epoch ||
       a.stamp.sequence!=b.stamp.sequence || a.stamp.received_qpc!=b.stamp.received_qpc || a.source!=b.source ||
       a.family!=b.family || a.position_reference!=b.position_reference || a.heading_reference!=b.heading_reference ||
       a.pitch_reference!=b.pitch_reference || a.source_millisecond_mod1000!=b.source_millisecond_mod1000 ||
       a.has_source_millisecond!=b.has_source_millisecond || a.count!=b.count) return false;
    for(std::size_t i=0;i<a.count;++i) {
        const auto& x=a.values[i];const auto& y=b.values[i];
        if(x.id!=y.id || x.element!=y.element || x.validity!=y.validity || x.kind!=y.kind || x.value!=y.value) return false;
    }
    return true;
}
void put(Writer& w, const Hello& v) noexcept {
    w.u(static_cast<unsigned>(v.role),1);w.u(v.aircraft_revision,4);w.u(v.instance_id,8);w.u(v.qpc_frequency,8);w.text(v.build);
}
void get(Reader& r, Hello& v) noexcept {
    v.role=static_cast<Role>(r.u(1));v.aircraft_revision=static_cast<std::uint32_t>(r.u(4));v.instance_id=r.u(8);v.qpc_frequency=r.u(8);r.text(v.build);
}
void put(Writer& w, const SessionStatus& v) noexcept {
    w.u(v.connection_id,8);w.u(v.revision,8);w.u(v.session_epoch,8);w.u(v.configuration_epoch,8);w.u(v.boost_generation,8);
    w.u(static_cast<std::uint64_t>(v.published_qpc),8);w.u(v.flags,4);w.u(static_cast<unsigned>(v.family),1);w.text(v.psx_version);
    w.u(v.motion_epoch,8);w.u(v.motion_sequence_floor,8);w.u(static_cast<unsigned>(v.motion_boundary),1);
}
void get_session_v2(Reader& r, SessionStatus& v) noexcept {
    v.connection_id=r.u(8);v.revision=r.u(8);v.session_epoch=r.u(8);v.configuration_epoch=r.u(8);v.boost_generation=r.u(8);
    v.published_qpc=r.ticks();v.flags=static_cast<std::uint32_t>(r.u(4));v.family=static_cast<aircraft::EngineFamily>(r.u(1));r.text(v.psx_version);
}
void get(Reader& r,SessionStatus& v) noexcept {
    get_session_v2(r,v);v.motion_epoch=r.u(8);v.motion_sequence_floor=r.u(8);v.motion_boundary=static_cast<motion::Boundary>(r.u(1));
}
void put(Writer& w, const ObservationMessage& v) noexcept {
    w.u(v.connection_id,8);w.u(v.session_revision,8);w.u(v.boost_generation,8);
    w.u(static_cast<std::uint64_t>(v.decoded_qpc),8);w.u(static_cast<std::uint64_t>(v.published_qpc),8);
    const auto& o=v.observation;
    w.u(o.stamp.session_epoch,8);w.u(o.stamp.configuration_epoch,8);w.u(o.stamp.sequence,8);w.u(static_cast<std::uint64_t>(o.stamp.received_qpc),8);
    w.u(static_cast<unsigned>(o.source),2);w.u(static_cast<unsigned>(o.family),1);w.u(static_cast<unsigned>(o.position_reference),1);
    w.u(static_cast<unsigned>(o.heading_reference),1);w.u(static_cast<unsigned>(o.pitch_reference),1);
    w.u(o.source_millisecond_mod1000,2);w.u(o.has_source_millisecond?1:0,1);w.u(o.count,2);
    for(std::size_t i=0;i<o.count;++i) {
        const auto& p=o.values[i];w.u(static_cast<unsigned>(p.id),2);w.u(p.element,1);
        w.u(static_cast<unsigned>(p.validity),1);w.u(static_cast<unsigned>(p.kind),1);
        w.u(static_cast<unsigned>(aircraft::property_info(p.id)->unit),1);w.real(p.value);
    }
}
void get(Reader& r, ObservationMessage& v) noexcept {
    v.connection_id=r.u(8);v.session_revision=r.u(8);v.boost_generation=r.u(8);v.decoded_qpc=r.ticks();v.published_qpc=r.ticks();
    auto& o=v.observation;
    o.stamp.session_epoch=r.u(8);o.stamp.configuration_epoch=r.u(8);o.stamp.sequence=r.u(8);o.stamp.received_qpc=r.ticks();
    o.source=static_cast<aircraft::SourceRecord>(r.u(2));o.family=static_cast<aircraft::EngineFamily>(r.u(1));
    o.position_reference=static_cast<aircraft::PositionReference>(r.u(1));o.heading_reference=static_cast<aircraft::HeadingReference>(r.u(1));
    o.pitch_reference=static_cast<aircraft::PitchReference>(r.u(1));o.source_millisecond_mod1000=static_cast<std::uint16_t>(r.u(2));
    auto has=r.u(1);if(has>1) r.ok=false;o.has_source_millisecond=has==1;
    o.count=static_cast<std::uint16_t>(r.u(2));if(o.count>o.values.size()) {r.ok=false;return;}
    for(std::size_t i=0;i<o.count;++i) {
        auto& p=o.values[i];p.id=static_cast<aircraft::PropertyId>(r.u(2));p.element=static_cast<std::uint8_t>(r.u(1));
        p.validity=static_cast<aircraft::ValueValidity>(r.u(1));p.kind=static_cast<aircraft::ValueKind>(r.u(1));
        const auto unit=r.u(1);const auto* info=aircraft::property_info(p.id);
        if(!info || unit!=static_cast<unsigned>(info->unit)) r.ok=false;p.value=r.real();
    }
}
void put(Writer& w,const Distribution& d) noexcept {w.u(d.count,8);w.u(d.minimum,8);w.u(d.maximum,8);w.u(d.p50,8);w.u(d.p95,8);w.u(d.p99,8);}
void get(Reader& r,Distribution& d) noexcept {d.count=r.u(8);d.minimum=r.u(8);d.maximum=r.u(8);d.p50=r.u(8);d.p95=r.u(8);d.p99=r.u(8);}
void put(Writer& w,const diagnostics::Maximum& m) noexcept {w.u(m.count,8);w.u(m.ticks,8);w.u(m.callback_count,8);w.u(static_cast<std::uint64_t>(m.qpc),8);w.u(static_cast<std::uint64_t>(m.previous_qpc),8);}
void get(Reader& r,diagnostics::Maximum& m) noexcept {m.count=r.u(8);m.ticks=r.u(8);m.callback_count=r.u(8);m.qpc=r.ticks();m.previous_qpc=r.ticks();}
void put(Writer& w,const PluginStatus& v) noexcept {
    w.u(v.connection_id,8);w.u(v.session_revision,8);w.u(v.session_epoch,8);w.u(v.configuration_epoch,8);w.u(v.boost_generation,8);
    w.u(v.aircraft_generation,8);w.u(v.scenery_generation,8);w.u(v.callback_count,8);w.u(static_cast<std::uint64_t>(v.callback_qpc),8);
    w.u(v.flags,4);w.u(v.rejected,8);w.u(v.coalesced,8);w.u(v.diagnostics_dropped,8);
    for(auto x:v.consumed_sequence) w.u(x,8);for(auto x:v.received_qpc) w.u(static_cast<std::uint64_t>(x),8);
    put(w,v.receive_to_callback);put(w,v.callback_duration);put(w,v.frame_interval);w.text(v.aircraft);
    w.u(v.plugin_load_id,8);w.u(v.enable_id,8);w.u(static_cast<std::uint64_t>(v.generated_qpc),8);w.u(v.diagnostic_validity,4);
    w.u(v.events_lost,8);w.u(v.persistence_lost,8);w.u(v.duration_completed_callback,8);
    put(w,v.receive_to_callback_max);put(w,v.callback_duration_max);put(w,v.frame_interval_max);
}
void get_plugin_v1(Reader& r,PluginStatus& v) noexcept {
    v.connection_id=r.u(8);v.session_revision=r.u(8);v.session_epoch=r.u(8);v.configuration_epoch=r.u(8);v.boost_generation=r.u(8);
    v.aircraft_generation=r.u(8);v.scenery_generation=r.u(8);v.callback_count=r.u(8);v.callback_qpc=r.ticks();
    v.flags=static_cast<std::uint32_t>(r.u(4));v.rejected=r.u(8);v.coalesced=r.u(8);v.diagnostics_dropped=r.u(8);
    for(auto& x:v.consumed_sequence) x=r.u(8);for(auto& x:v.received_qpc) x=r.ticks();
    get(r,v.receive_to_callback);get(r,v.callback_duration);get(r,v.frame_interval);r.text(v.aircraft);
}
void get(Reader& r,PluginStatus& v) noexcept {
    get_plugin_v1(r,v);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.generated_qpc=r.ticks();v.diagnostic_validity=static_cast<std::uint32_t>(r.u(4));
    v.events_lost=r.u(8);v.persistence_lost=r.u(8);v.duration_completed_callback=r.u(8);
    get(r,v.receive_to_callback_max);get(r,v.callback_duration_max);get(r,v.frame_interval_max);
}
void put(Writer& w,const diagnostics::Event& e) noexcept {
    const auto& c=e.context;
    for(auto value:{c.run_id,c.plugin_load_id,c.enable_id,c.connection_id,c.session_epoch,c.configuration_epoch,c.boost_generation,c.aircraft_generation,c.scenery_generation}) w.u(value,8);
    w.u(e.sequence,8);w.u(static_cast<std::uint64_t>(e.qpc),8);w.u(e.producer,2);w.u(static_cast<unsigned>(e.code),2);
    w.u(static_cast<unsigned>(e.severity),1);w.u(static_cast<unsigned>(e.component),1);w.u(static_cast<unsigned>(e.error_domain),1);w.u(e.error,4);
    for(auto value:e.values)w.u(value,8);w.text(e.text);
}
void get(Reader& r,diagnostics::Event& e) noexcept {
    auto& c=e.context;c.run_id=r.u(8);c.plugin_load_id=r.u(8);c.enable_id=r.u(8);c.connection_id=r.u(8);c.session_epoch=r.u(8);c.configuration_epoch=r.u(8);c.boost_generation=r.u(8);c.aircraft_generation=r.u(8);c.scenery_generation=r.u(8);
    e.sequence=r.u(8);e.qpc=r.ticks();e.producer=static_cast<std::uint16_t>(r.u(2));e.code=static_cast<diagnostics::Code>(r.u(2));
    e.severity=static_cast<diagnostics::Severity>(r.u(1));e.component=static_cast<diagnostics::Component>(r.u(1));e.error_domain=static_cast<diagnostics::ErrorDomain>(r.u(1));e.error=static_cast<std::uint32_t>(r.u(4));
    for(auto& value:e.values)value=r.u(8);r.text(e.text);
}
void put(Writer& w,const DiagnosticBatch& v) noexcept {w.u(v.connection_id,8);w.u(v.count,2);for(std::size_t i=0;i<v.count;++i)put(w,v.events[i]);}
void get(Reader& r,DiagnosticBatch& v) noexcept {v.connection_id=r.u(8);v.count=static_cast<std::uint16_t>(r.u(2));if(v.count>v.events.size()){r.ok=false;return;}for(std::size_t i=0;i<v.count;++i)get(r,v.events[i]);}
void put(Writer& w,const motion::ReconstructionSettings& v) noexcept {w.u(static_cast<unsigned>(v.mode),1);w.u(v.delay_ms,2);}
void get(Reader& r,motion::ReconstructionSettings& v) noexcept {v.mode=static_cast<motion::ReconstructionMode>(r.u(1));v.delay_ms=static_cast<std::uint16_t>(r.u(2));}
void put(Writer& w,const motion::PoseDomain& d) noexcept {w.u(d.producer.high,8);w.u(d.producer.low,8);w.u(d.pose_epoch,8);w.u(d.reference_revision,4);}
void get(Reader& r,motion::PoseDomain& d) noexcept {d.producer.high=r.u(8);d.producer.low=r.u(8);d.pose_epoch=r.u(8);d.reference_revision=static_cast<std::uint32_t>(r.u(4));}
void put(Writer& w,const motion::PoseProvenance& p) noexcept {
    w.u(static_cast<unsigned>(p.kind),1);w.u(p.selection_epoch,8);put(w,p.domain);
    w.u(p.left_sequence,8);w.u(p.right_sequence,8);w.u(p.frame_sequence,8);
    w.u(static_cast<std::uint64_t>(p.sample_start_qpc),8);w.u(static_cast<std::uint64_t>(p.sample_end_qpc),8);w.u(static_cast<std::uint64_t>(p.received_qpc),8);
}
void get(Reader& r,motion::PoseProvenance& p) noexcept {
    p.kind=static_cast<motion::PoseSource>(r.u(1));p.selection_epoch=r.u(8);get(r,p.domain);
    p.left_sequence=r.u(8);p.right_sequence=r.u(8);p.frame_sequence=r.u(8);
    p.sample_start_qpc=r.ticks();p.sample_end_qpc=r.ticks();p.received_qpc=r.ticks();
}
void put(Writer& w,const SourceSelection& v) noexcept {
    w.u(v.connection_id,8);w.u(v.revision,8);w.u(v.selection_epoch,8);w.u(static_cast<std::uint64_t>(v.selected_qpc),8);
    w.u(static_cast<unsigned>(v.source),1);w.u(static_cast<unsigned>(v.reason),1);put(w,v.domain);
}
void get(Reader& r,SourceSelection& v) noexcept {
    v.connection_id=r.u(8);v.revision=r.u(8);v.selection_epoch=r.u(8);v.selected_qpc=r.ticks();
    v.source=static_cast<motion::PoseSource>(r.u(1));v.reason=static_cast<motion::SelectionReason>(r.u(1));get(r,v.domain);
}
void put(Writer& w,const MotionRequest& v) noexcept {
    for(auto n:{v.connection_id,v.operation_id,v.plugin_load_id,v.enable_id,v.aircraft_generation,v.session_epoch,v.motion_epoch})w.u(n,8);
    w.u(static_cast<std::uint64_t>(v.requested_qpc),8);w.u(static_cast<unsigned>(v.action),1);
    put(w,v.reconstruction);
    w.u(static_cast<unsigned>(v.source_preference),1);w.u(v.expected_selection_epoch,8);
}
void get_request_v3(Reader& r,MotionRequest& v) noexcept {
    v.connection_id=r.u(8);v.operation_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.aircraft_generation=r.u(8);v.session_epoch=r.u(8);v.motion_epoch=r.u(8);v.requested_qpc=r.ticks();v.action=static_cast<motion::Action>(r.u(1));
}
void get_request_v4(Reader& r,MotionRequest& v) noexcept {get_request_v3(r,v);get(r,v.reconstruction);}
void get(Reader& r,MotionRequest& v) noexcept {get_request_v4(r,v);v.source_preference=static_cast<motion::SourcePreference>(r.u(1));v.expected_selection_epoch=r.u(8);}
void put(Writer& w,const MotionAck& v) noexcept {
    for(auto n:{v.connection_id,v.operation_id,v.plugin_load_id,v.enable_id,v.aircraft_generation})w.u(n,8);
    w.u(static_cast<std::uint64_t>(v.generated_qpc),8);w.u(static_cast<unsigned>(v.result),1);w.u(static_cast<unsigned>(v.reason),1);
}
void get(Reader& r,MotionAck& v) noexcept {
    v.connection_id=r.u(8);v.operation_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.aircraft_generation=r.u(8);v.generated_qpc=r.ticks();v.result=static_cast<motion::AckResult>(r.u(1));v.reason=static_cast<motion::Reason>(r.u(1));
}
void put(Writer& w,motion::Vec3 v) noexcept {w.real(v.x);w.real(v.y);w.real(v.z);}
void get(Reader& r,motion::Vec3& v) noexcept {v.x=r.real();v.y=r.real();v.z=r.real();}
void put(Writer& w,const motion::LocalPose& v) noexcept {put(w,v.origin);w.real(v.heading_deg);w.real(v.pitch_deg);w.real(v.bank_deg);}
void get(Reader& r,motion::LocalPose& v) noexcept {get(r,v.origin);v.heading_deg=r.real();v.pitch_deg=r.real();v.bank_deg=r.real();}
void put(Writer& w,const motion::ReconstructionTrace& t) noexcept {
    w.u(static_cast<unsigned>(t.state),1);w.u(t.window_revision,8);w.u(t.left_sequence,8);w.u(t.right_sequence,8);
    for(auto n:{t.target_qpc,t.presentation_qpc,t.left_received_qpc,t.right_received_qpc})w.u(static_cast<std::uint64_t>(n),8);
    w.real(t.alpha);
}
void get(Reader& r,motion::ReconstructionTrace& t) noexcept {
    t.state=static_cast<motion::ReconstructionState>(r.u(1));t.window_revision=r.u(8);t.left_sequence=r.u(8);t.right_sequence=r.u(8);
    t.target_qpc=r.ticks();t.presentation_qpc=r.ticks();t.left_received_qpc=r.ticks();t.right_received_qpc=r.ticks();t.alpha=r.real();
}
void put(Writer& w,const MotionStatus& v) noexcept {
    for(auto n:{v.connection_id,v.plugin_load_id,v.enable_id,v.aircraft_generation,v.scenery_generation,v.revision,v.operation_id,v.applied_count,v.discontinuity_generation})w.u(n,8);
    w.u(static_cast<std::uint64_t>(v.generated_qpc),8);w.u(static_cast<std::uint64_t>(v.applied_qpc),8);
    w.u(static_cast<unsigned>(v.phase),1);w.u(static_cast<unsigned>(v.reason),1);w.u(v.flags,4);w.u(v.reference_revision,4);
    const auto& p=v.presentation;const auto& g=p.geo;const auto& s=p.source;
    for(auto x:{g.latitude_deg,g.longitude_deg,g.altitude_msl_m,g.heading_deg,g.pitch_deg,g.bank_deg})w.real(x);
    for(auto n:{s.session_epoch,s.boost_generation,s.motion_epoch,s.boost_sequence,s.pitch_sequence})w.u(n,8);
    w.u(static_cast<std::uint64_t>(s.boost_received_qpc),8);w.u(static_cast<std::uint64_t>(s.pitch_received_qpc),8);w.u(p.on_ground?1:0,1);
    put(w,v.written);put(w,v.readback);w.real(v.position_error_m);w.real(v.attitude_error_deg);w.u(v.control_dropped,8);w.u(v.trace_dropped,8);
    put(w,v.pre_callback_duration);put(w,v.post_callback_duration);put(w,v.boost_receive_to_apply);put(w,v.pitch_age);put(w,v.frame_interval);
    put(w,v.reconstruction);put(w,v.reconstruction_trace);w.u(v.boost_history_count,2);w.u(v.pitch_history_count,2);
    for(auto n:{v.bracketed_count,v.oldest_hold_count,v.newest_hold_count,v.no_pitch_count,v.history_reset_count,v.hold_transitions})w.u(n,8);
    put(w,v.output_age);put(w,v.hold_duration);
    w.u(static_cast<unsigned>(v.selected_source),1);w.u(v.selection_epoch,8);put(w,v.selected_domain);
    w.u(static_cast<unsigned>(v.source_preference),1);w.u(v.exporter_history_count,2);put(w,v.presentation.provenance);
    put(w,v.exporter_sample_to_apply);put(w,v.sampling_width);w.real(v.handoff_position_m);w.real(v.handoff_attitude_deg);
}
void get_status_v3(Reader& r,MotionStatus& v) noexcept {
    v.connection_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.aircraft_generation=r.u(8);v.scenery_generation=r.u(8);v.revision=r.u(8);v.operation_id=r.u(8);v.applied_count=r.u(8);v.discontinuity_generation=r.u(8);
    v.generated_qpc=r.ticks();v.applied_qpc=r.ticks();v.phase=static_cast<motion::Phase>(r.u(1));v.reason=static_cast<motion::Reason>(r.u(1));v.flags=static_cast<std::uint32_t>(r.u(4));v.reference_revision=static_cast<std::uint32_t>(r.u(4));
    auto& p=v.presentation;auto& g=p.geo;auto& s=p.source;
    g.latitude_deg=r.real();g.longitude_deg=r.real();g.altitude_msl_m=r.real();g.heading_deg=r.real();g.pitch_deg=r.real();g.bank_deg=r.real();
    s.session_epoch=r.u(8);s.boost_generation=r.u(8);s.motion_epoch=r.u(8);s.boost_sequence=r.u(8);s.pitch_sequence=r.u(8);s.boost_received_qpc=r.ticks();s.pitch_received_qpc=r.ticks();
    const auto ground=r.u(1);if(ground>1)r.ok=false;p.on_ground=ground==1;
    get(r,v.written);get(r,v.readback);v.position_error_m=r.real();v.attitude_error_deg=r.real();v.control_dropped=r.u(8);v.trace_dropped=r.u(8);
    get(r,v.pre_callback_duration);get(r,v.post_callback_duration);get(r,v.boost_receive_to_apply);get(r,v.pitch_age);get(r,v.frame_interval);
}
void get_status_v4(Reader& r,MotionStatus& v) noexcept {
    get_status_v3(r,v);get(r,v.reconstruction);get(r,v.reconstruction_trace);
    v.boost_history_count=static_cast<std::uint16_t>(r.u(2));v.pitch_history_count=static_cast<std::uint16_t>(r.u(2));
    v.bracketed_count=r.u(8);v.oldest_hold_count=r.u(8);v.newest_hold_count=r.u(8);v.no_pitch_count=r.u(8);v.history_reset_count=r.u(8);v.hold_transitions=r.u(8);
    get(r,v.output_age);get(r,v.hold_duration);
}
void get(Reader& r,MotionStatus& v) noexcept {
    get_status_v4(r,v);v.selected_source=static_cast<motion::PoseSource>(r.u(1));v.selection_epoch=r.u(8);get(r,v.selected_domain);
    v.source_preference=static_cast<motion::SourcePreference>(r.u(1));v.exporter_history_count=static_cast<std::uint16_t>(r.u(2));get(r,v.presentation.provenance);
    get(r,v.exporter_sample_to_apply);get(r,v.sampling_width);v.handoff_position_m=r.real();v.handoff_attitude_deg=r.real();
}
// Session epoch is stored once per window; each original configuration/sequence/
// receipt stamp is preserved. Fixed source kinds imply the documented units.
void put_stamp(Writer& w,const aircraft::SampleStamp& s) noexcept {w.u(s.configuration_epoch,8);w.u(s.sequence,8);w.u(static_cast<std::uint64_t>(s.received_qpc),8);}
void get_stamp(Reader& r,aircraft::SampleStamp& s,std::uint64_t session) noexcept {s.session_epoch=session;s.configuration_epoch=r.u(8);s.sequence=r.u(8);s.received_qpc=r.ticks();}
void put(Writer& w,const motion::PitchSample& p) noexcept {put_stamp(w,p.stamp);w.real(p.pitch_deg);w.u(static_cast<unsigned>(p.pitch_reference),1);}
void get_pitch(Reader& r,motion::PitchSample& p,std::uint64_t session) noexcept {get_stamp(r,p.stamp,session);p.pitch_deg=r.real();p.pitch_reference=static_cast<aircraft::PitchReference>(r.u(1));}
void put(Writer& w,const MotionWindow& v) noexcept {
    const auto& s=v.samples;const auto& d=s.domain;
    w.u(v.connection_id,8);w.u(v.session_revision,8);w.u(static_cast<std::uint64_t>(v.published_qpc),8);
    for(auto n:{d.session_epoch,d.configuration_epoch,d.boost_generation,d.motion_epoch,d.sequence_floor,s.revision})w.u(n,8);
    w.u(s.boost_count,2);w.u(s.pitch_count,2);w.u(s.has_pitch_anchor?1:0,1);
    if(s.has_pitch_anchor)put(w,s.pitch_anchor);
    for(std::size_t i=0;i<s.boost_count;++i) {
        const auto& b=s.boost[i];put_stamp(w,b.stamp);const auto& g=b.geo;
        for(auto x:{g.latitude_deg,g.longitude_deg,g.altitude_msl_m,g.heading_deg,g.pitch_deg,g.bank_deg})w.real(x);
        w.u(b.source_millisecond_mod1000,2);w.u(b.has_source_millisecond?1:0,1);w.u(b.on_ground?1:0,1);
        w.u(static_cast<unsigned>(b.position_reference),1);w.u(static_cast<unsigned>(b.heading_reference),1);w.u(static_cast<unsigned>(b.pitch_reference),1);
    }
    for(std::size_t i=0;i<s.pitch_count;++i)put(w,s.pitch[i]);
    w.u(v.selection_epoch,8);
}
void get_window_v4(Reader& r,MotionWindow& v) noexcept {
    auto& s=v.samples;auto& d=s.domain;
    v.connection_id=r.u(8);v.session_revision=r.u(8);v.published_qpc=r.ticks();
    d.session_epoch=r.u(8);d.configuration_epoch=r.u(8);d.boost_generation=r.u(8);d.motion_epoch=r.u(8);d.sequence_floor=r.u(8);s.revision=r.u(8);
    s.boost_count=static_cast<std::uint16_t>(r.u(2));s.pitch_count=static_cast<std::uint16_t>(r.u(2));const auto anchor=r.u(1);s.has_pitch_anchor=anchor==1;
    if(anchor>1 || s.boost_count>motion::history_capacity || s.pitch_count>motion::history_capacity){r.ok=false;return;}
    if(s.has_pitch_anchor)get_pitch(r,s.pitch_anchor,d.session_epoch);
    for(std::size_t i=0;i<s.boost_count;++i) {
        auto& b=s.boost[i];get_stamp(r,b.stamp,d.session_epoch);auto& g=b.geo;
        g.latitude_deg=r.real();g.longitude_deg=r.real();g.altitude_msl_m=r.real();g.heading_deg=r.real();g.pitch_deg=r.real();g.bank_deg=r.real();
        b.source_millisecond_mod1000=static_cast<std::uint16_t>(r.u(2));const auto has=r.u(1),ground=r.u(1);if(has>1 || ground>1)r.ok=false;
        b.has_source_millisecond=has==1;b.on_ground=ground==1;b.position_reference=static_cast<aircraft::PositionReference>(r.u(1));b.heading_reference=static_cast<aircraft::HeadingReference>(r.u(1));b.pitch_reference=static_cast<aircraft::PitchReference>(r.u(1));
    }
    for(std::size_t i=0;i<s.pitch_count;++i)get_pitch(r,s.pitch[i],d.session_epoch);
}
void get(Reader& r,MotionWindow& v) noexcept {get_window_v4(r,v);v.selection_epoch=r.u(8);}
void put(Writer& w,const CompletePoseWindow& v) noexcept {
    w.u(v.connection_id,8);w.u(v.selection_epoch,8);w.u(static_cast<std::uint64_t>(v.published_qpc),8);
    const auto& window=v.samples;put(w,window.domain);w.u(window.revision,8);w.u(window.sequence_floor,8);w.u(window.count,2);
    for(std::size_t i=0;i<window.count;++i) {
        const auto& s=window.samples[i];w.u(s.sequence,8);w.u(s.frame_sequence,8);w.u(s.configuration_epoch,8);
        for(auto n:{s.sample_start_qpc,s.sample_end_qpc,s.received_qpc})w.u(static_cast<std::uint64_t>(n),8);
        for(auto x:{s.latitude_deg,s.longitude_deg,s.altitude_msl_m,s.attitude.w,s.attitude.x,s.attitude.y,s.attitude.z})w.real(x);
        w.u(s.flags,4);
    }
}
void get(Reader& r,CompletePoseWindow& v) noexcept {
    v.connection_id=r.u(8);v.selection_epoch=r.u(8);v.published_qpc=r.ticks();
    auto& window=v.samples;get(r,window.domain);window.revision=r.u(8);window.sequence_floor=r.u(8);window.count=static_cast<std::uint16_t>(r.u(2));
    if(window.count>window.samples.size()){r.ok=false;return;}
    for(std::size_t i=0;i<window.count;++i) {
        auto& s=window.samples[i];s.sequence=r.u(8);s.frame_sequence=r.u(8);s.configuration_epoch=r.u(8);
        s.sample_start_qpc=r.ticks();s.sample_end_qpc=r.ticks();s.received_qpc=r.ticks();
        s.latitude_deg=r.real();s.longitude_deg=r.real();s.altitude_msl_m=r.real();
        s.attitude.w=r.real();s.attitude.x=r.real();s.attitude.y=r.real();s.attitude.z=r.real();s.flags=static_cast<std::uint32_t>(r.u(4));
    }
}
bool valid(const CoverageRequest& v) noexcept {return v.connection_id&&v.operation_id&&v.plugin_load_id&&v.enable_id&&v.deadline_qpc>0&&scenery::valid(v.query);}
bool valid(const CoverageReply& v) noexcept {return valid(v.request)&&scenery::valid(v.result)&&v.result.query==v.request.query&&v.result.plugin_load==v.request.plugin_load_id&&v.result.plugin_enable==v.request.enable_id;}
void put(Writer& w,const CoverageRequest& v) noexcept {
 w.u(v.connection_id,8);w.u(v.operation_id,8);w.u(v.plugin_load_id,8);w.u(v.enable_id,8);w.u(static_cast<std::uint64_t>(v.deadline_qpc),8);
 scenery::Frame f;if(!scenery::encode(v.query,f)){w.ok=false;return;}for(std::size_t i=0;i<f.size;++i)w.u(f.bytes[i],1);
}
void get(Reader& r,CoverageRequest& v) noexcept {
 v.connection_id=r.u(8);v.operation_id=r.u(8);v.plugin_load_id=r.u(8);v.enable_id=r.u(8);v.deadline_qpc=r.ticks();
 scenery::Frame f;f.size=16+scenery::query_bytes;for(std::size_t i=0;i<f.size;++i)f.bytes[i]=static_cast<std::uint8_t>(r.u(1));if(!scenery::decode(f.bytes.data(),f.size,v.query))r.ok=false;
}
void put(Writer& w,const CoverageReply& v) noexcept {put(w,v.request);scenery::Frame f;if(!scenery::encode(v.result,f)){w.ok=false;return;}for(std::size_t i=0;i<f.size;++i)w.u(f.bytes[i],1);}
void get(Reader& r,CoverageReply& v) noexcept {get(r,v.request);scenery::Frame f;f.size=16+scenery::result_bytes;for(std::size_t i=0;i<f.size;++i)f.bytes[i]=static_cast<std::uint8_t>(r.u(1));if(!scenery::decode(f.bytes.data(),f.size,v.result))r.ok=false;}
static_assert(85+motion::history_capacity*79+(motion::history_capacity+1)*33<=max_payload,"MotionWindow fits the bounded payload");
static_assert(70+motion::history_capacity*108<=max_payload,"CompletePoseWindow fits the bounded payload");
template<class T> bool pack(const T& value,Kind kind,std::uint32_t seq,Frame& out) noexcept {
    if(!valid(value)) return false;
    Frame temp;Writer w{temp.bytes.data()+header_size,max_payload};put(w,value);if(!w.ok) return false;
    Writer h{temp.bytes.data(),header_size};h.u(0x42535850,4);h.u(protocol_version,2);h.u(static_cast<unsigned>(kind),2);h.u(w.pos,4);h.u(seq,4);
    temp.size=header_size+w.pos;out=temp;return true;
}
template<class T> bool unpack(const std::uint8_t* data,std::size_t size,T& out) noexcept {
    if(size>max_payload) return false;T temp;Reader r{data,size};get(r,temp);if(!r.end() || !valid(temp)) return false;out=temp;return true;
}
} // namespace
bool read_capture_header(const std::uint8_t* data,std::size_t size,Header& out) noexcept {
    if(size!=header_size) return false;Reader r{data,size};if(r.u(4)!=0x42535850) return false;
    Header h;h.version=static_cast<std::uint16_t>(r.u(2));if(h.version<1 || h.version>protocol_version)return false;
    h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));
    if(!r.end() || h.payload_size>max_payload || static_cast<unsigned>(h.kind)<1 || static_cast<unsigned>(h.kind)>(h.version==1?4u:h.version==2?5u:h.version==3?8u:h.version==4?9u:h.version==5?11u:19u)) return false;out=h;return true;
}
bool read_header(const std::uint8_t* data,std::size_t size,Header& out) noexcept {
    Header h;if(!read_capture_header(data,size,h) || h.version!=protocol_version)return false;out=h;return true;
}
bool decode_capture_plugin(const std::uint8_t* data,std::size_t size,std::uint16_t version,PluginStatus& out) noexcept {
    if(version>=2 && version<=protocol_version)return decode(data,size,out);
    if(version!=1 || size>max_payload)return false;
    PluginStatus temp;Reader r{data,size};get_plugin_v1(r,temp);if(!r.end() || !valid(temp))return false;out=temp;return true;
}
bool decode_capture_session(const std::uint8_t* data,std::size_t size,std::uint16_t version,SessionStatus& out) noexcept {
    if(version>=3 && version<=protocol_version)return decode(data,size,out);
    if((version!=1&&version!=2)||size>max_payload)return false;
    SessionStatus temp;Reader r{data,size};get_session_v2(r,temp);if(!r.end()||!valid(temp))return false;out=temp;return true;
}
bool encode(const CoverageRequest& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::coverage_request,s,f);}
bool encode(const CoverageReply& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::coverage_reply,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,CoverageRequest& v) noexcept {return unpack(p,n,v);}
bool decode(const std::uint8_t* p,std::size_t n,CoverageReply& v) noexcept {return unpack(p,n,v);}
bool encode(const Hello& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::hello,s,f);}
bool encode(const ViewRequest& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::view_request,s,f);}
bool encode(const ViewReply& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::view_reply,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,ViewRequest& v) noexcept {return unpack(p,n,v);}
bool decode(const std::uint8_t* p,std::size_t n,ViewReply& v) noexcept {return unpack(p,n,v);}
bool encode(const SessionStatus& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::session,s,f);}
bool encode(const ObservationMessage& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::observation,s,f);}
bool encode(const PluginStatus& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::plugin_status,s,f);}
bool encode(const DiagnosticBatch& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::diagnostic_events,s,f);}
bool encode(const MotionRequest& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::motion_request,s,f);}
bool encode(const MotionAck& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::motion_ack,s,f);}
bool encode(const MotionStatus& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::motion_status,s,f);}
bool encode(const MotionWindow& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::motion_window,s,f);}
bool encode(const SourceSelection& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::source_selection,s,f);}
bool encode(const CompletePoseWindow& v,std::uint32_t s,Frame& f) noexcept {return pack(v,Kind::complete_pose_window,s,f);}
bool decode(const std::uint8_t* d,std::size_t n,Hello& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,SessionStatus& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,ObservationMessage& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,PluginStatus& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,DiagnosticBatch& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,MotionRequest& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,MotionAck& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,MotionStatus& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,MotionWindow& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,SourceSelection& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,CompletePoseWindow& v) noexcept {return unpack(d,n,v);}
bool decode_capture_motion_request(const std::uint8_t* data,std::size_t size,std::uint16_t version,MotionRequest& out) noexcept {
    if(version>=5&&version<=protocol_version)return decode(data,size,out);
    if((version!=3&&version!=4) || size>max_payload)return false;
    MotionRequest temp;Reader r{data,size};
    if(version==3){get_request_v3(r,temp);temp.reconstruction.mode=motion::ReconstructionMode::direct;}else get_request_v4(r,temp);
    if(!r.end() || !valid(temp) || (temp.action==motion::Action::follow&&(!temp.session_epoch||!temp.motion_epoch)))return false;out=temp;return true;
}
bool decode_capture_motion_status(const std::uint8_t* data,std::size_t size,std::uint16_t version,MotionStatus& out) noexcept {
    if(version>=5&&version<=protocol_version)return decode(data,size,out);
    if((version!=3&&version!=4) || size>max_payload)return false;
    MotionStatus temp;Reader r{data,size};
    if(version==3){get_status_v3(r,temp);temp.reconstruction.mode=motion::ReconstructionMode::direct;}else get_status_v4(r,temp);
    // Old captures predate the exporter tail and the corrected presentation bank contract.
    temp.selected_domain.reference_revision=motion::legacy_reference_revision;
    temp.presentation.provenance.domain.reference_revision=motion::legacy_reference_revision;
    if(!r.end() || !valid(temp))return false;out=temp;return true;
}
bool decode_capture_motion_window(const std::uint8_t* data,std::size_t size,std::uint16_t version,MotionWindow& out) noexcept {
    if(version>=5&&version<=protocol_version)return decode(data,size,out);
    if(version!=4 || size>max_payload)return false;
    MotionWindow temp;Reader r{data,size};get_window_v4(r,temp);if(!r.end()||!valid(temp))return false;out=temp;return true;
}
bool matches(const MotionWindow& w,const SessionStatus& s) noexcept {
    constexpr auto required=main_connected|synchronized|boost_connected|boost_available;
    return valid(w) && w.connection_id==s.connection_id && w.session_revision==s.revision && (s.flags&required)==required &&
        motion::same_motion_domain(w.samples.domain,history_domain(s)) && w.samples.domain.configuration_epoch==s.configuration_epoch;
}
bool matches(const MotionWindow& w,const SessionStatus& s,const SourceSelection& selection) noexcept {
    return valid(selection)&&selection.source==motion::PoseSource::public_main_boost&&selection.connection_id==w.connection_id&&
        selection.selection_epoch==w.selection_epoch&&matches(w,s);
}
bool matches(const CompletePoseWindow& w,const SourceSelection& selection) noexcept {
    return valid(w)&&valid(selection)&&selection.source==motion::PoseSource::exporter&&selection.connection_id==w.connection_id&&
        selection.selection_epoch==w.selection_epoch&&selection.domain==w.samples.domain;
}

namespace {
template<class T> bool pack_audio(const T& value,Kind kind,std::uint32_t sequence,Frame& out) noexcept {
 Frame temp;std::size_t n{};if(!audio::encode_payload(value,temp.bytes.data()+header_size,max_payload,n))return false;
 Writer h{temp.bytes.data(),header_size};h.u(0x42535850,4);h.u(protocol_version,2);h.u(static_cast<unsigned>(kind),2);h.u(n,4);h.u(sequence,4);temp.size=header_size+n;out=temp;return true;
}
}
bool encode(const AudioState& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_state,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioState& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode(const AudioTransition& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_transition,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioTransition& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode(const AudioControl& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_control,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioControl& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode(const AudioStatus& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_status,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioStatus& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool Framer::feed(const void* data,std::size_t size,FrameHandler fn,void* context) noexcept {
    if(failed_ || (!data && size) || !fn) {failed_=true;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(data);
    while(size) {
        const auto count=(std::min)(size,needed_-used_);std::memcpy(buffer_.data()+used_,bytes,count);used_+=count;bytes+=count;size-=count;
        if(used_==needed_) {
            Header h;if(!read_header(buffer_.data(),header_size,h)) {failed_=true;return false;}
            needed_=header_size+h.payload_size;
            if(used_==needed_) {
                try {if(!fn(context,h,buffer_.data()+header_size,h.payload_size)) {failed_=true;return false;}}
                catch(...) {failed_=true;return false;}
                used_=0;needed_=header_size;
            }
        }
    }
    return true;
}
bool supported_source(aircraft::SourceRecord s) noexcept {
    const auto i=static_cast<unsigned>(s);return i>0 && i<source_slots && s!=aircraft::SourceRecord::qs363;
}
bool engine_source(aircraft::SourceRecord s) noexcept {
    using S=aircraft::SourceRecord;return s==S::qi1 || s==S::qs481 || s==S::qs482 || s==S::qs436 || s==S::qi156 || s==S::qi157 || s==S::qi158 || s==S::qi159;
}
void LatestState::reset(std::uint64_t id) noexcept {session_={};session_.connection_id=id;available_.fill(false);}
bool LatestState::apply(const SessionStatus& s) noexcept {
    if(!valid(s) || s.connection_id!=session_.connection_id || s.revision<session_.revision || s.session_epoch<session_.session_epoch) return false;
    if(s.revision==session_.revision) {
        if(s.session_epoch!=session_.session_epoch || s.configuration_epoch!=session_.configuration_epoch || s.boost_generation!=session_.boost_generation ||
           s.flags!=session_.flags || s.family!=session_.family || s.psx_version!=session_.psx_version || s.published_qpc<session_.published_qpc ||
           s.motion_epoch!=session_.motion_epoch || s.motion_sequence_floor!=session_.motion_sequence_floor || s.motion_boundary!=session_.motion_boundary) return false;
        session_=s;return true;
    }
    const bool new_session=s.session_epoch!=session_.session_epoch;
    if(!new_session && (s.configuration_epoch<session_.configuration_epoch || s.boost_generation<session_.boost_generation || s.motion_epoch<session_.motion_epoch || s.motion_sequence_floor<session_.motion_sequence_floor)) return false;
    if(!new_session && s.motion_epoch==session_.motion_epoch && (s.motion_sequence_floor!=session_.motion_sequence_floor || s.motion_boundary!=session_.motion_boundary))return false;
    if(!new_session && s.family!=session_.family && s.configuration_epoch==session_.configuration_epoch) return false;
    if(new_session || !(s.flags&main_connected)) available_.fill(false);
    else for(std::size_t i=1;i<source_slots;++i) {
        const auto source=static_cast<aircraft::SourceRecord>(i);
        if((s.motion_epoch!=session_.motion_epoch && (source==aircraft::SourceRecord::boost || source==aircraft::SourceRecord::qs121)) ||
           (s.configuration_epoch!=session_.configuration_epoch && engine_source(source)) ||
           (source==aircraft::SourceRecord::boost && (s.boost_generation!=session_.boost_generation || !(s.flags&boost_connected)))) available_[i]=false;
    }
    session_=s;return true;
}
bool LatestState::apply(const ObservationMessage& v) noexcept {
    if(!valid(v) || v.connection_id!=session_.connection_id || v.session_revision!=session_.revision ||
       !(session_.flags&main_connected)) return false;
    const auto& o=v.observation;const auto index=static_cast<std::size_t>(o.source);
    if((o.source==aircraft::SourceRecord::boost || o.source==aircraft::SourceRecord::qs121) && o.stamp.sequence<session_.motion_sequence_floor)return false;
    if(o.stamp.session_epoch!=session_.session_epoch || o.stamp.configuration_epoch>session_.configuration_epoch ||
       (engine_source(o.source) && (o.stamp.configuration_epoch!=session_.configuration_epoch || o.family!=session_.family)) ||
       (o.source==aircraft::SourceRecord::boost && (!(session_.flags&boost_connected) || v.boost_generation!=session_.boost_generation))) return false;
    if(available_[index] && o.stamp.sequence<=records_[index].observation.stamp.sequence) {
        const auto& previous=records_[index];
        // Resending retained state after a control revision is idempotent, never
        // a new source sample. Compare semantic members, not struct padding.
        if(o.stamp.sequence!=previous.observation.stamp.sequence || v.session_revision<=previous.session_revision ||
           v.decoded_qpc!=previous.decoded_qpc || v.boost_generation!=previous.boost_generation ||
           !same_observation(o,previous.observation)) return false;
    }
    records_[index]=v;available_[index]=true;return true;
}
} // namespace psx::bridge
