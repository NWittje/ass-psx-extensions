#pragma once
#include "model.hpp"
#include <ass_graphics/ass_graphics.h>
#include <vector>
namespace psx::cdu
{
// Authored geometry profile 1, logical units. Round stroked, no glyph font.
struct Glyph
{
    char code{};
    Size size{};
    std::vector<ag_path_command> contours;
    ag_rect bounds{};
    float stroke{2};
    ag_point origin{};
    unsigned cap{AG_CAP_ROUND}, join{AG_JOIN_ROUND};
    bool fill{};
};
const std::vector<Glyph> &glyphs();
constexpr unsigned glyph_profile_revision = 1;
constexpr float screen_width = 372, screen_height = 315, column_advance = 15, row_advance = 22,
                origin_x = 12, origin_y = 14;
} // namespace psx::cdu
