#pragma once
#include <psx/bridge/protocol.hpp>
#include <bit>

namespace psx::xp {
// Worker-owned required-control admission. Duplicates are immutable; a new
// selection epoch is the only way to change source/domain on one attachment.
class SourceSelectionGate final {
public:
    void reset() noexcept {value_={};}
    const bridge::SourceSelection& value() const noexcept {return value_;}
    bool accept(const bridge::SourceSelection& s,std::uint64_t connection) noexcept {
        if(s.connection_id!=connection||!connection||!s.revision||!s.selection_epoch||s.selected_qpc<0||
           static_cast<unsigned>(s.reason)>static_cast<unsigned>(motion::SelectionReason::connection))return false;
        if(s.source==motion::PoseSource::exporter){if(!motion::current_reference(s.domain))return false;}
        else if(s.source!=motion::PoseSource::public_main_boost||motion::valid(s.domain.producer)||s.domain.pose_epoch||
                s.domain.reference_revision!=motion::instant_reference_revision)return false;
        if(value_.connection_id) {
            if(s.revision<value_.revision||s.selection_epoch<value_.selection_epoch||s.selected_qpc<value_.selected_qpc)return false;
            if(s.revision==value_.revision && (s.selection_epoch!=value_.selection_epoch||s.selected_qpc!=value_.selected_qpc||
                s.source!=value_.source||s.reason!=value_.reason||s.domain!=value_.domain))return false;
            if(s.selection_epoch==value_.selection_epoch && (s.source!=value_.source||s.domain!=value_.domain))return false;
        }
        value_=s;return true;
    }
private:bridge::SourceSelection value_{};
};
class CompleteWindowGate final {
public:
    void reset() noexcept {previous_={};high_water_={};have_=false;}
    bool accept(const bridge::CompletePoseWindow& window,const bridge::SourceSelection& selection) noexcept {
        // The codec also reads historical revision-1 captures. This is live
        // admission: never interpret their old bank convention as current.
        if(!motion::current_reference(selection.domain)||!motion::current_reference(window.samples.domain)||
           !bridge::matches(window,selection))return false;
        const auto& w=window.samples;const auto& p=previous_.samples;
        const auto branch=[](const motion::CompletePoseSample& s){return s.flags&(motion::raw_ordinary_altitude|motion::raw_paused_altitude);};
        for(std::size_t i=1;i<w.count;++i)if(branch(w.samples[i])!=branch(w.samples[0]))return false;
        const bool same_selection=have_&&window.connection_id==previous_.connection_id&&window.selection_epoch==previous_.selection_epoch;
        if(same_selection) {
            if(w.domain!=p.domain||w.sequence_floor<p.sequence_floor||w.revision<p.revision)return false;
            if(w.revision==p.revision) {
                if(w.count!=p.count||w.sequence_floor!=p.sequence_floor)return false;
                for(std::size_t i=0;i<w.count;++i)if(!same(w.samples[i],p.samples[i]))return false;
            }
            if(w.count&&high_water_.sequence) {
                const auto& newest=w.samples[w.count-1];
                if(newest.sequence<high_water_.sequence||newest.frame_sequence<high_water_.frame_sequence||
                   newest.sample_end_qpc<high_water_.sample_end_qpc||newest.received_qpc<high_water_.received_qpc||
                   (!p.count&&w.samples[0].sequence<=high_water_.sequence))return false;
            }
            if(p.count&&!w.count&&w.sequence_floor==p.sequence_floor)return false;
            if(p.count&&w.count) {
                if(w.samples[w.count-1].sequence<p.samples[p.count-1].sequence ||
                   w.samples[w.count-1].frame_sequence<p.samples[p.count-1].frame_sequence ||
                   w.samples[w.count-1].sample_end_qpc<p.samples[p.count-1].sample_end_qpc ||
                   w.samples[w.count-1].received_qpc<p.samples[p.count-1].received_qpc)return false;
                std::size_t i=0;
                for(std::size_t j=0;j<w.count;++j) {
                    const auto& s=w.samples[j];if(s.sequence<p.samples[0].sequence)return false;
                    while(i<p.count&&p.samples[i].sequence<s.sequence)++i;
                    if(i<p.count&&p.samples[i].sequence==s.sequence){if(!same(p.samples[i],s))return false;}
                    else if(s.sequence<=p.samples[p.count-1].sequence||s.frame_sequence<=p.samples[p.count-1].frame_sequence||
                            s.sample_end_qpc<p.samples[p.count-1].sample_end_qpc||s.received_qpc<p.samples[p.count-1].received_qpc)return false;
                }
                std::size_t j=0;
                for(i=0;i<p.count;++i)if(p.samples[i].sequence>=w.samples[0].sequence) {
                    while(j<w.count&&w.samples[j].sequence<p.samples[i].sequence)++j;
                    if(j==w.count||w.samples[j].sequence!=p.samples[i].sequence)return false;
                }
            }
        }
        if(!same_selection)high_water_={};if(w.count)high_water_=w.samples[w.count-1];
        previous_=window;have_=true;return true;
    }
private:
    bridge::CompletePoseWindow previous_{};bool have_{};
    motion::CompletePoseSample high_water_{}; // Retained across a legitimate empty, advancing-floor reset.
    static bool same(double a,double b) noexcept {return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
    static bool same(const motion::CompletePoseSample& a,const motion::CompletePoseSample& b) noexcept {
        return a.sequence==b.sequence&&a.frame_sequence==b.frame_sequence&&a.configuration_epoch==b.configuration_epoch&&
            a.sample_start_qpc==b.sample_start_qpc&&a.sample_end_qpc==b.sample_end_qpc&&a.received_qpc==b.received_qpc&&
            same(a.latitude_deg,b.latitude_deg)&&same(a.longitude_deg,b.longitude_deg)&&same(a.altitude_msl_m,b.altitude_msl_m)&&
            same(a.attitude.w,b.attitude.w)&&same(a.attitude.x,b.attitude.x)&&same(a.attitude.y,b.attitude.y)&&same(a.attitude.z,b.attitude.z)&&a.flags==b.flags;
    }
};
}
