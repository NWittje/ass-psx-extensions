#pragma once
#include <psx/bridge/protocol.hpp>
#include <bit>

namespace psx::xp {
// Worker-owned admission gate. Only an admitted, complete immutable window is
// published to the SDK owner. No retained pointers into decoder storage.
class MotionWindowGate final {
public:
    void reset() noexcept { previous_={};have_=false; }
    bool accept(const bridge::MotionWindow& window,const bridge::SessionStatus& session) noexcept {
        if(!bridge::matches(window,session))return false;
        const auto& next=window.samples;const auto& old=previous_.samples;
        if(have_ && window.connection_id==previous_.connection_id && motion::same_motion_domain(next.domain,old.domain)) {
            if(next.revision<old.revision)return false;
            if(!compatible(old.boost.data(),old.boost_count,next.boost.data(),next.boost_count))return false;
            std::array<motion::PitchSample,motion::history_capacity+1> a{},b{};
            const auto ac=pitches(old,a),bc=pitches(next,b);
            if(!compatible(a.data(),ac,b.data(),bc))return false;
            if(next.revision==old.revision &&
               (next.domain.configuration_epoch!=old.domain.configuration_epoch || next.boost_count!=old.boost_count ||
                next.pitch_count!=old.pitch_count || next.has_pitch_anchor!=old.has_pitch_anchor ||
                !same_records(old.boost.data(),next.boost.data(),old.boost_count) || !same_records(a.data(),b.data(),ac)))return false;
        }
        previous_=window;have_=true;return true;
    }
private:
    bridge::MotionWindow previous_{};
    bool have_{};
    static bool same(double a,double b) noexcept {return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
    static bool same(const aircraft::SampleStamp& a,const aircraft::SampleStamp& b) noexcept {
        return a.session_epoch==b.session_epoch&&a.configuration_epoch==b.configuration_epoch&&a.sequence==b.sequence&&a.received_qpc==b.received_qpc;
    }
    static bool same(const motion::PitchSample& a,const motion::PitchSample& b) noexcept {
        return same(a.stamp,b.stamp)&&same(a.pitch_deg,b.pitch_deg)&&a.pitch_reference==b.pitch_reference;
    }
    static bool same(const motion::BoostSample& a,const motion::BoostSample& b) noexcept {
        return same(a.stamp,b.stamp)&&same(a.geo.latitude_deg,b.geo.latitude_deg)&&same(a.geo.longitude_deg,b.geo.longitude_deg)&&
            same(a.geo.altitude_msl_m,b.geo.altitude_msl_m)&&same(a.geo.heading_deg,b.geo.heading_deg)&&
            same(a.geo.pitch_deg,b.geo.pitch_deg)&&same(a.geo.bank_deg,b.geo.bank_deg)&&
            a.source_millisecond_mod1000==b.source_millisecond_mod1000&&a.has_source_millisecond==b.has_source_millisecond&&
            a.on_ground==b.on_ground&&a.position_reference==b.position_reference&&a.heading_reference==b.heading_reference&&a.pitch_reference==b.pitch_reference;
    }
    template<class T> static bool same_records(const T* a,const T* b,std::size_t count) noexcept {
        for(std::size_t i=0;i<count;++i)if(!same(a[i],b[i]))return false;return true;
    }
    template<class T> static bool compatible(const T* old,std::size_t oc,const T* next,std::size_t nc) noexcept {
        if(!oc)return true;
        if(!nc || next[nc-1].stamp.sequence<old[oc-1].stamp.sequence || next[nc-1].stamp.received_qpc<old[oc-1].stamp.received_qpc)return false;
        // A sliding window may evict a prefix, never rewrite, insert into or
        // remove a known retained interval. New identities advance receipt time.
        std::size_t i=0;
        for(std::size_t j=0;j<nc;++j) {
            const auto seq=next[j].stamp.sequence;
            if(seq<old[0].stamp.sequence)return false;
            while(i<oc&&old[i].stamp.sequence<seq)++i;
            if(i<oc&&old[i].stamp.sequence==seq){if(!same(old[i],next[j]))return false;}
            else if(seq<=old[oc-1].stamp.sequence || next[j].stamp.received_qpc<old[oc-1].stamp.received_qpc)return false;
        }
        // Every old identity still inside the new interval must be present.
        std::size_t j=0;
        for(i=0;i<oc;++i)if(old[i].stamp.sequence>=next[0].stamp.sequence) {
            while(j<nc&&next[j].stamp.sequence<old[i].stamp.sequence)++j;
            if(j==nc||next[j].stamp.sequence!=old[i].stamp.sequence)return false;
        }
        return true;
    }
    static std::size_t pitches(const motion::SampleWindow& w,std::array<motion::PitchSample,motion::history_capacity+1>& out) noexcept {
        std::size_t n=0;if(w.has_pitch_anchor)out[n++]=w.pitch_anchor;
        for(std::size_t i=0;i<w.pitch_count;++i)out[n++]=w.pitch[i];return n;
    }
};
}
