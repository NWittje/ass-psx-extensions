#pragma once
#include "flight_display.hpp"
#include <ass_graphics/ass_graphics.h>

namespace hwio {
ag_rect flight_rect(std::size_t slot) noexcept;
ag_point flight_cross(double roll, double pitch, bool fo = false) noexcept;
std::wstring flight_tooltip(const FlightSnapshot &snapshot, std::size_t slot);
// Application-owned instrument geometry only. ASS Graphics owns rendering,
// typography, target recovery and image export.
struct FlightDrawing {
    std::vector<ag_draw> commands;
    std::vector<ag_point> points;
    void update(const FlightSnapshot &snapshot, double seconds);
};
} // namespace hwio
