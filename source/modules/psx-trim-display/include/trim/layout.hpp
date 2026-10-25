#pragma once
#include <array>
#include <cstdint>

namespace trim::layout {
// Registration of native 2x drawing coordinates to the operator's layered
// reference, then uniform fitting of its 1079-row frame into 452 panel rows.
// Top/bottom margins are 14/480 = 2.92%; the native aspect ratio is preserved.
inline constexpr double native_scale=0.8140065945914607;
inline constexpr double native_x=8.355885078776652, native_y=-1.1623457253005007;
inline constexpr int bar_x=151, bar_y=52, bar_width=17, bar_max_height=327;
// Half-degree scale centres in panel pixels, Q6. This is a nonuniform scale.
// Runtime interpolation uses only bounded integer arithmetic, without a float
// calculation, allocation or table search for each received observation.
inline constexpr std::array<std::uint16_t,31> scale_ticks_q6{
    4458,4948,5466,6012,6584,7179,7797,8435,9092,9765,10453,
    11153,11864,12584,13310,14041,14773,15506,16236,16962,17682,
    18393,19093,19781,20455,21111,21749,22367,22962,23534,24080};
}
