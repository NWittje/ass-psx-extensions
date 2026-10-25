#include "trim/state.hpp"
#include "trim/layout.hpp"
namespace trim {
std::uint16_t display_bar_height(std::uint16_t angle) noexcept {
    if(angle>15000)angle=15000;
    const auto index=angle/500u, fraction=angle%500u;
    const auto start=std::uint32_t(layout::scale_ticks_q6[index]);
    const auto end=index==30?start:layout::scale_ticks_q6[index+1];
    const auto endpoint=(start+((end-start)*fraction+250u)/500u+32u)/64u;
    return std::uint16_t(endpoint-layout::bar_y);
}
Visual evaluate(const Observation& o,Role role,std::uint64_t now) noexcept {
    Visual v{};
    const auto& s=o.value;
    // USB supplies power independently of the PC sender. Until the first current
    // snapshot, or when its heartbeat expires, park the instrument at aircraft OFF.
    // A live PC explicitly reporting unknown PSX data remains distinguishable.
    if(!o.active||!o.has_state||now<o.received_us||now-o.received_us>=stale_us){v.mode=Mode::off;return v;}
    if(s.role!=role||!(s.validity&source_connected)||!(s.validity&indicators_known))return v;
    if(s.validity&brightness_known)v.brightness=s.brightness;
    const auto power=role==Role::captain?1u:2u;
    if(!(s.indicators&power)){v.mode=Mode::off;return v;}
    if(!(s.validity&trim_known)||s.angle>15000)return v;
    v.mode=Mode::normal;
    v.bar_height=display_bar_height(s.angle);
    v.green=std::uint8_t((s.indicators>>2)&3u);return v;
}
}
