#pragma once
#include <psx/motion/motion.hpp>
#include <array>
#include <limits>

namespace psx::motion {
inline constexpr std::size_t history_capacity=32;
enum class ReconstructionMode : std::uint8_t { direct=0, interpolated=1 };
struct ReconstructionSettings { ReconstructionMode mode{ReconstructionMode::interpolated}; std::uint16_t delay_ms{25}; };
inline bool valid(const ReconstructionSettings& s) noexcept {
    return (s.mode==ReconstructionMode::direct || s.mode==ReconstructionMode::interpolated) && s.delay_ms>=10 && s.delay_ms<=50;
}
enum class ReconstructionState : std::uint8_t { unavailable, direct, interpolated, oldest_hold, newest_hold, no_pitch, invalid_timing };
struct HistoryDomain {
    std::uint64_t session_epoch{}, configuration_epoch{}, boost_generation{}, motion_epoch{}, sequence_floor{};
};
inline bool same_motion_domain(const HistoryDomain& a,const HistoryDomain& b) noexcept {
    return a.session_epoch==b.session_epoch && a.boost_generation==b.boost_generation && a.motion_epoch==b.motion_epoch && a.sequence_floor==b.sequence_floor;
}
struct BoostSample {
    aircraft::SampleStamp stamp{};
    GeoPose geo{}; // pitch is RAW conditional Boost pitch, never substituted with Main pitch.
    std::uint16_t source_millisecond_mod1000{};
    bool has_source_millisecond{},on_ground{};
    aircraft::PositionReference position_reference{aircraft::PositionReference::psx_flight_deck};
    aircraft::HeadingReference heading_reference{aircraft::HeadingReference::true_north};
    aircraft::PitchReference pitch_reference{aircraft::PitchReference::boost_conditional};
};
struct PitchSample {
    aircraft::SampleStamp stamp{};
    double pitch_deg{};
    aircraft::PitchReference pitch_reference{aircraft::PitchReference::horizon};
};
struct SampleWindow {
    HistoryDomain domain{};
    std::uint64_t revision{};
    std::uint16_t boost_count{},pitch_count{};
    bool has_pitch_anchor{};
    PitchSample pitch_anchor{};
    std::array<BoostSample,history_capacity> boost{};
    std::array<PitchSample,history_capacity> pitch{};
};
inline bool valid_stamp(const aircraft::SampleStamp& s,const HistoryDomain& d) noexcept {
    return s.session_epoch==d.session_epoch && s.sequence && s.sequence>=d.sequence_floor && s.received_qpc>=0 && s.configuration_epoch<=d.configuration_epoch;
}
inline bool valid(const BoostSample& b,const HistoryDomain& d) noexcept {
    return valid_stamp(b.stamp,d) && valid(b.geo) &&
        b.position_reference==aircraft::PositionReference::psx_flight_deck &&
        (b.heading_reference==aircraft::HeadingReference::true_north || b.heading_reference==aircraft::HeadingReference::grid_north) &&
        b.pitch_reference==aircraft::PitchReference::boost_conditional &&
        b.source_millisecond_mod1000<1000;
}
inline bool valid(const PitchSample& p,const HistoryDomain& d) noexcept {
    return valid_stamp(p.stamp,d) && std::isfinite(p.pitch_deg) && std::abs(p.pitch_deg)<=180 && p.pitch_reference==aircraft::PitchReference::horizon;
}
inline bool valid(const SampleWindow& w) noexcept {
    if(!w.revision || !w.domain.session_epoch || !w.domain.boost_generation || !w.domain.motion_epoch || w.boost_count>history_capacity || w.pitch_count>history_capacity)return false;
    for(std::size_t i=0;i<w.boost_count;++i) {
        if(!valid(w.boost[i],w.domain))return false;
        if(i && (w.boost[i].stamp.sequence<=w.boost[i-1].stamp.sequence || w.boost[i].stamp.received_qpc<w.boost[i-1].stamp.received_qpc))return false;
    }
    if(w.has_pitch_anchor && !valid(w.pitch_anchor,w.domain))return false;
    const PitchSample* previous=w.has_pitch_anchor?&w.pitch_anchor:nullptr;
    for(std::size_t i=0;i<w.pitch_count;++i) {
        const auto& p=w.pitch[i];if(!valid(p,w.domain))return false;
        if(previous && (p.stamp.sequence<=previous->stamp.sequence || p.stamp.received_qpc<previous->stamp.received_qpc))return false;
        previous=&p;
    }
    return true;
}
struct HistoryCounters {
    std::uint64_t decoded_boost{},decoded_pitch{},evicted_boost{},evicted_pitch{},equal_time_boost{},rejected{},resets{};
};
// One acquisition owner. Copies are fixed-size snapshots, never a shared mutable ring.
class MotionHistory {
public:
    const SampleWindow& window() const noexcept {return window_;}
    const HistoryCounters& counters() const noexcept {return counters_;}
    // Offline capture checkpoint only; never a live history/window input API.
    // Bootstrap/disconnected checkpoints may have no Boost generation yet.
    bool restore_capture_checkpoint(const SampleWindow& window,const HistoryCounters& counters) noexcept {
        if(window.boost_count>history_capacity || window.pitch_count>history_capacity)return false;
        const bool empty=!window.boost_count && !window.pitch_count && !window.has_pitch_anchor;
        if(!empty) {
            auto checked=window;
            if(!checked.domain.boost_generation && !checked.boost_count)checked.domain.boost_generation=1;
            if(!valid(checked))return false;
        }
        window_=window;counters_=counters;return true;
    }
    void synchronize(const HistoryDomain& domain) noexcept {
        if(!same_motion_domain(domain,window_.domain)) {
            // A Boost restart must not erase unchanged, still-valid Main pitch.
            const bool keep_pitch=domain.session_epoch==window_.domain.session_epoch && domain.motion_epoch==window_.domain.motion_epoch && domain.sequence_floor==window_.domain.sequence_floor;
            window_.boost_count=0;
            if(!keep_pitch){window_.pitch_count=0;window_.has_pitch_anchor=false;}
            ++window_.revision;++counters_.resets;
        }
        if(domain.configuration_epoch!=window_.domain.configuration_epoch)++window_.revision;
        window_.domain=domain;
        if(!window_.revision)window_.revision=1;
    }
    void clear() noexcept {const auto revision=window_.revision+1;window_={};window_.revision=revision;++counters_.resets;}
    bool append(const aircraft::Observation& o) noexcept {
        using namespace aircraft;
        if(o.source!=SourceRecord::boost && o.source!=SourceRecord::qs121)return false;
        if(!well_formed(o) || !valid_stamp(o.stamp,window_.domain)){++counters_.rejected;return false;}
        if(o.source==SourceRecord::boost) {
            BoostSample b;b.stamp=o.stamp;b.position_reference=o.position_reference;b.heading_reference=o.heading_reference;b.pitch_reference=o.pitch_reference;
            b.source_millisecond_mod1000=o.source_millisecond_mod1000;b.has_source_millisecond=o.has_source_millisecond;
            double ground{};
            if(!value(o,PropertyId::latitude_deg,b.geo.latitude_deg)||!value(o,PropertyId::longitude_deg,b.geo.longitude_deg)||
               !value(o,PropertyId::altitude_msl_m,b.geo.altitude_msl_m)||!value(o,PropertyId::heading_deg,b.geo.heading_deg)||
               !value(o,PropertyId::pitch_deg,b.geo.pitch_deg)||!value(o,PropertyId::bank_deg,b.geo.bank_deg)||
               !value(o,PropertyId::on_ground,ground)|| (ground!=0 && ground!=1)|| !valid(b,window_.domain)){++counters_.rejected;return false;}
            b.on_ground=ground==1;
            if(window_.boost_count) {
                const auto& previous=window_.boost[window_.boost_count-1].stamp;
                if(b.stamp.sequence<=previous.sequence || b.stamp.received_qpc<previous.received_qpc){++counters_.rejected;return false;}
                if(b.stamp.received_qpc==previous.received_qpc)++counters_.equal_time_boost;
            }
            if(window_.boost_count==history_capacity){remove_first(window_.boost,window_.boost_count);++counters_.evicted_boost;}
            window_.boost[window_.boost_count++]=b;++counters_.decoded_boost;
            // Retain exactly the last pitch event preceding the oldest Boost sample.
            while(window_.pitch_count && window_.pitch[0].stamp.received_qpc<window_.boost[0].stamp.received_qpc) {
                window_.pitch_anchor=window_.pitch[0];window_.has_pitch_anchor=true;
                remove_first(window_.pitch,window_.pitch_count);
            }
        } else {
            PitchSample p;p.stamp=o.stamp;p.pitch_reference=o.pitch_reference;
            if(!value(o,PropertyId::pitch_deg,p.pitch_deg)||!valid(p,window_.domain)){++counters_.rejected;return false;}
            const auto* previous=window_.pitch_count?&window_.pitch[window_.pitch_count-1]:(window_.has_pitch_anchor?&window_.pitch_anchor:nullptr);
            if(previous && (p.stamp.sequence<=previous->stamp.sequence || p.stamp.received_qpc<previous->stamp.received_qpc)){++counters_.rejected;return false;}
            if(window_.pitch_count==history_capacity){window_.pitch_anchor=window_.pitch[0];window_.has_pitch_anchor=true;remove_first(window_.pitch,window_.pitch_count);++counters_.evicted_pitch;}
            window_.pitch[window_.pitch_count++]=p;++counters_.decoded_pitch;
        }
        ++window_.revision;return true;
    }
private:
    template<class T> static void remove_first(std::array<T,history_capacity>& array,std::uint16_t& count) noexcept {
        for(std::size_t i=1;i<count;++i)array[i-1]=array[i];--count;
    }
    SampleWindow window_{};HistoryCounters counters_{};
};
struct ReconstructionTrace {
    ReconstructionState state{ReconstructionState::unavailable};
    std::uint64_t window_revision{},left_sequence{},right_sequence{};
    std::int64_t target_qpc{},presentation_qpc{},left_received_qpc{},right_received_qpc{};
    double alpha{};
};
struct ReconstructionResult {PresentationPose pose{};ReconstructionTrace trace{};};
// Pure bounded evaluation. Cursor is owned by the motion controller and cleared
// on domain/ownership transitions. Original receive times are never rewritten.
inline bool reconstruct(const SampleWindow& w,const ReconstructionSettings& settings,std::int64_t now,std::uint64_t frequency,
                        std::int64_t cursor,ReconstructionResult& out,std::uint32_t reference_revision=instant_reference_revision,std::int64_t future_bound=0) noexcept {
    out={};out.trace.window_revision=w.revision;
    if(!known_reference_revision(reference_revision)||!valid(settings)||!frequency||frequency>1000000000000ULL||now<0||cursor<0||!valid(w)||!w.boost_count)return false;
    const auto delay=static_cast<std::int64_t>((frequency*settings.delay_ms)/1000);
    const auto wanted=settings.mode==ReconstructionMode::direct?now:(now>=delay?now-delay:0);
    out.trace.target_qpc=wanted;
    const auto& newest=w.boost[w.boost_count-1];
    if(future_bound<0||(newest.stamp.received_qpc>now&&newest.stamp.received_qpc-now>future_bound)||cursor>now){out.trace.state=ReconstructionState::invalid_timing;return false;}
    const auto target=(std::max)(wanted,cursor);
    std::size_t left=0,right=0;
    double alpha=0;
    if(settings.mode==ReconstructionMode::direct) {left=right=w.boost_count-1;out.trace.state=ReconstructionState::direct;}
    else if(target<w.boost[0].stamp.received_qpc) {
        // The last entry at the first timestamp is the authoritative endpoint.
        while(right+1<w.boost_count && w.boost[right+1].stamp.received_qpc==w.boost[0].stamp.received_qpc)++right;
        left=right;out.trace.state=ReconstructionState::oldest_hold;
    } else {
        while(left+1<w.boost_count && w.boost[left+1].stamp.received_qpc<=target)++left;
        right=left;
        if(left+1<w.boost_count) {
            right=left+1;
            while(right+1<w.boost_count && w.boost[right+1].stamp.received_qpc==w.boost[right].stamp.received_qpc)++right;
            alpha=static_cast<double>(target-w.boost[left].stamp.received_qpc)/static_cast<double>(w.boost[right].stamp.received_qpc-w.boost[left].stamp.received_qpc);
            out.trace.state=ReconstructionState::interpolated;
        } else out.trace.state=ReconstructionState::newest_hold;
    }
    const auto& a=w.boost[left];const auto& b=w.boost[right];
    if(a.heading_reference!=aircraft::HeadingReference::true_north || b.heading_reference!=aircraft::HeadingReference::true_north || std::abs(a.geo.latitude_deg)>89 || std::abs(b.geo.latitude_deg)>89)return false;
    auto& trace=out.trace;trace.left_sequence=a.stamp.sequence;trace.right_sequence=b.stamp.sequence;trace.left_received_qpc=a.stamp.received_qpc;trace.right_received_qpc=b.stamp.received_qpc;trace.alpha=alpha;
    trace.presentation_qpc=left==right?a.stamp.received_qpc:target;
    if(trace.presentation_qpc<cursor){trace.state=ReconstructionState::invalid_timing;return false;}
    const auto pitch_time=settings.mode==ReconstructionMode::direct?now:trace.presentation_qpc;
    const PitchSample* pitch=nullptr;
    if(w.has_pitch_anchor && w.pitch_anchor.stamp.received_qpc<=pitch_time)pitch=&w.pitch_anchor;
    for(std::size_t i=0;i<w.pitch_count && w.pitch[i].stamp.received_qpc<=pitch_time;++i)pitch=&w.pitch[i];
    if(!pitch){trace.state=ReconstructionState::no_pitch;return false;}
    auto mix=[alpha](double x,double y) noexcept {return x+(y-x)*alpha;};
    auto angle=[alpha](double x,double y) noexcept {return x+std::remainder(y-x,360.0)*alpha;};
    out.pose.geo={mix(a.geo.latitude_deg,b.geo.latitude_deg),std::remainder(angle(a.geo.longitude_deg,b.geo.longitude_deg),360.0),
        mix(a.geo.altitude_msl_m,b.geo.altitude_msl_m),wrap_heading(angle(a.geo.heading_deg,b.geo.heading_deg)),pitch->pitch_deg,std::remainder(angle(a.geo.bank_deg,b.geo.bank_deg),360.0)};
    if(reference_revision==instant_reference_revision)out.pose.geo.bank_deg=psx_bank_to_right_positive(out.pose.geo.bank_deg);
    out.pose.provenance.domain.reference_revision=reference_revision;
    out.pose.on_ground=a.on_ground;
    out.pose.source={w.domain.session_epoch,w.domain.boost_generation,w.domain.motion_epoch,b.stamp.sequence,pitch->stamp.sequence,b.stamp.received_qpc,pitch->stamp.received_qpc};
    return valid(out.pose.geo);
}
} // namespace psx::motion
