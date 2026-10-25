#include "hwio/catalogue.hpp"
#include "hwio/flight_scene.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hwio {
namespace {
constexpr unsigned black = 0, muted = 1, grid = 2, red = 3, navy = 4, yellow = 5, blue = 6, white = 7, light_blue = 8, deadzone = 9;
constexpr ag_rect rect(float l, float t, float r, float b) {
    return {l, t, r, b};
}
constexpr ag_point point(float x, float y) {
    return {x, y};
}
constexpr float idle_y = 180;
constexpr double pi = 3.14159265358979323846;
std::wstring percent(double value, const wchar_t *negative, const wchar_t *positive) {
    std::wostringstream out;
    if (value < -0.0005)
        out << negative << L' ';
    else if (value > 0.0005 && *positive)
        out << positive << L' ';
    out << std::fixed << std::setprecision(0) << std::abs(value * 100) << L'%';
    return out.str();
}
} // namespace
ag_rect flight_rect(std::size_t slot) noexcept {
    if (slot >= 14 && slot < 18) {
        const float x = (slot < 16 ? 22.0f : 722.0f) + ((slot - 14) % 2) * 36.0f;
        return rect(x, 222, x + 20, 276);
    }
    if (slot < 8) {
        const float x = slot < 4 ? 96.0f : 528.0f;
        if (slot % 4 < 2)
            return rect(x, 44, x + 176, 204);
        if (slot % 4 == 2)
            return rect(x, 230, x + 176, 250);
        const float left = slot < 4 ? 14.0f : 714.0f;
        return rect(left, 88, left + 72, 160);
    }
    if (slot == 8)
        return rect(282, 44, 306, 248);
    if (slot == 13)
        return rect(494, 44, 518, 248);
    const float x = 316 + 44.0f * static_cast<float>(slot - 9);
    return rect(x, 44, x + 36, 248);
}
ag_point flight_cross(double roll, double pitch, bool fo) noexcept {
    const auto r = flight_rect(fo ? 4 : 0);
    return point(r.left + 6 + static_cast<float>((std::clamp(roll, -1.0, 1.0) + 1) * 82),
                 r.top + 6 + static_cast<float>((std::clamp(pitch, -1.0, 1.0) + 1) * 74));
}
std::wstring flight_tooltip(const FlightSnapshot &snapshot, std::size_t slot) {
    std::wostringstream out;
    auto one = [&](std::size_t index) {
        const auto &r = snapshot[index];
        out << flight_slot_name(index) << (r.shared ? L" (shared)" : L"") << L"\n" << wide(r.source) << L"\n";
        if(const auto *t=flight_target(index))out << wide(t->descriptor.name) << L" | " << wide(record_name(t->descriptor.record)) << L"\n";
        if (r.available) {
            out << L"Calculated: " << r.value << L" | raw: " << r.raw;
            if(index<8 && index%4<3)out << L"\nPhysical: " << r.physical*100 << L"% | PSX sent: "
                << (r.transmitted_available ? percent(r.transmitted,L"-",L"+") : L"Unavailable");
            if (r.split)
                out << L" | reverse raw: " << r.reverse_raw << L"\nForward: " << r.forward * 100 << L"% | reverse: "
                    << r.reverse * 100 << L"%\nFinal engine command: " << r.value;
            out << L"\n";
        }
        out << wide(r.reason);
        if (r.reverse_blocked)
            out << L"\nReverse blocked: forward input above physical idle";
        if (r.display_only)
            out << L"\nDisplay only; never commands PSX";
        else
            out << L"\nDelivery: "
                << (r.delivery == DeliveryState::sent       ? L"TCP completed"
                    : r.delivery == DeliveryState::observed ? L"observed"
                    : r.delivery == DeliveryState::queued   ? L"queued"
                                                            : L"not sent / unavailable");
    };
    one(slot);
    if (slot == 0 || slot == 4) {
        out << L"\n\n";
        one(slot + 1);
    }
    return out.str();
}
void FlightDrawing::update(const FlightSnapshot &s, double seconds) {
    commands.clear();
    points.clear();
    commands.reserve(240);
    points.reserve(140);
    constexpr ag_color colors[]{{.10f, .12f, .15f, 1},    {.45f, .47f, .5f, 1}, {.85f, .85f, .85f, 1},
                                {.827f, .184f, .184f, 1}, {0, 0, .502f, 1},     {.953f, .776f, 0, 1},
                                {.302f, .639f, 1, 1},     {1, 1, 1, 1}, {.36f,.75f,1,1}, {.36f,.75f,1,.22f}};
    auto shape = [&](uint32_t kind, ag_rect r, unsigned color = black, float width = 1) {
        ag_draw d{};
        d.kind = kind;
        d.rect = r;
        d.color = colors[color];
        d.width = width;
        commands.push_back(d);
    };
    auto text = [&](const std::wstring &value, ag_rect r, unsigned color = black, bool compact = false) {
        ag_draw d{};
        d.kind = AG_TEXT;
        d.flags = 2;
        d.rect = r;
        d.color = colors[color];
        d.font_size = compact ? 11.f : 0.f; // B612 remains library-owned; compact values fit fixed instrument columns.
        wcsncpy_s(d.text, value.c_str(), _TRUNCATE);
        commands.push_back(d);
    };
    auto line = [&](float x1, float y1, float x2, float y2, unsigned color = black, float width = 1) {
        shape(AG_LINE, {x1, y1, x2, y2}, color, width);
    };
    auto outline = [&](ag_rect r, unsigned color = black) { shape(AG_RECT, r, color, 1.5f); };
    auto unavailable = [&](std::size_t i, ag_rect r) {
        text(s[i].assigned ? L"Unavailable" : L"Unassigned", r, muted, true);
    };
    for (unsigned side = 0; side < 2; ++side) {
        const std::size_t i = side * 4;
        const auto box = flight_rect(i);
        text(side ? L"FIRST OFFICER" : L"CAPTAIN", rect(box.left, 8, box.right, 30));
        if (s[i].deadzone_available && s[i+1].deadzone_available) {
            const auto a = flight_cross(s[i].deadzone_low, s[i+1].deadzone_low, side != 0);
            const auto z = flight_cross(s[i].deadzone_high, s[i+1].deadzone_high, side != 0);
            shape(AG_FILL_RECT, rect(a.x,a.y,z.x,z.y), deadzone);
        }
        outline(box);
        line((box.left + box.right) / 2, box.top, (box.left + box.right) / 2, box.bottom, grid);
        line(box.left, 124, box.right, 124, grid);
        const auto cross = flight_cross(s[i].transmitted, s[i + 1].transmitted, side != 0);
        if (s[i].transmitted_available && s[i + 1].transmitted_available) {
            line(cross.x - 5, cross.y, cross.x + 5, cross.y, black, 1.5f);
            line(cross.x, cross.y - 5, cross.x, cross.y + 5, black, 1.5f);
        } else {
            if (s[i].transmitted_available)
                line(cross.x, box.top + 6, cross.x, box.bottom - 6, black, 1.5f);
            if (s[i + 1].transmitted_available)
                line(box.left + 6, cross.y, box.right - 6, cross.y, black, 1.5f);
            if (!s[i].transmitted_available && !s[i + 1].transmitted_available)
                text(L"PSX output unavailable", rect(box.left,box.top + 8,box.right,box.top + 26), muted, true);
        }
        if (s[i].available && s[i+1].available) {
            const auto physical = flight_cross(s[i].physical, s[i+1].physical, side != 0);
            shape(AG_ELLIPSE, rect(physical.x-7,physical.y-7,physical.x+7,physical.y+7), light_blue, 2);
        } else {
            const auto physical = flight_cross(s[i].physical, s[i+1].physical, side != 0);
            if (s[i].available) line(physical.x,box.top+6,physical.x,box.bottom-6,light_blue);
            if (s[i+1].available) line(box.left+6,physical.y,box.right-6,physical.y,light_blue);
        }
        const auto roll = s[i].transmitted_available ? percent(s[i].transmitted, L"L", L"R") : L"--";
        const auto pitch = s[i + 1].transmitted_available ? percent(s[i + 1].transmitted, L"DN", L"UP") : L"--";
        text(L"Roll " + roll + L"   Pitch " + pitch, rect(box.left - 3, 206, box.right + 3, 225), black, true);
        const auto rudder = flight_rect(i + 2);
        if (s[i+2].deadzone_available)
            shape(AG_FILL_RECT, rect((rudder.left+rudder.right)/2 + 86*float(s[i+2].deadzone_low),rudder.top,
                                    (rudder.left+rudder.right)/2 + 86*float(s[i+2].deadzone_high),rudder.bottom), deadzone);
        if (s[i + 2].transmitted_available) {
            const float zero = (rudder.left + rudder.right) / 2;
            const float position = zero + 86 * static_cast<float>(s[i + 2].transmitted);
            shape(AG_FILL_RECT,
                  rect(std::min(zero, position), rudder.top + 2, std::max(zero, position), rudder.bottom - 2), black);
            line(position, rudder.top, position, rudder.bottom, black, 1.5f);
        }
        outline(rudder);
        if (s[i+2].available) {
            const float x=(rudder.left+rudder.right)/2 + 86*float(s[i+2].physical), y=(rudder.top+rudder.bottom)/2;
            shape(AG_ELLIPSE,rect(x-6,y-6,x+6,y+6),light_blue,2);
        }
        line((rudder.left + rudder.right) / 2, rudder.top - 2, (rudder.left + rudder.right) / 2, rudder.bottom + 2,
             muted);
        text(L"Rudder " + (s[i + 2].transmitted_available ? percent(s[i + 2].transmitted, L"L", L"R") : L"--"),
             rect(box.left, 254, box.right, 273));
        const auto dial = flight_rect(i + 3);
        const float cx = (dial.left + dial.right) / 2, cy = 124;
        const double value = s[i + 3].normalized;
        if (s[i + 3].available && std::abs(value) > 0.000001) {
            ag_draw sector{};
            sector.kind = AG_POLYGON;
            sector.color = colors[black];
            sector.first_point = static_cast<uint32_t>(points.size());
            points.push_back({cx, cy});
            const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(value) * 64)));
            for (int n = 0; n <= steps; ++n) {
                const double angle = value * pi * n / steps;
                points.push_back(
                    {cx + static_cast<float>(std::sin(angle) * 34), cy - static_cast<float>(std::cos(angle) * 34)});
            }
            sector.point_count = static_cast<uint32_t>(points.size()) - sector.first_point;
            commands.push_back(sector);
        }
        shape(AG_ELLIPSE, rect(cx - 36, cy - 36, cx + 36, cy + 36), black, 1.5f);
        line(cx, 85, cx, 94, muted);
        text(L"Tiller", rect(dial.left, 166, dial.right, 184));
        if (s[i + 3].available) {
            std::wostringstream label;
            label << (value < 0   ? L"L "
                      : value > 0 ? L"R "
                                  : L"")
                  << std::fixed << std::setprecision(0) << std::abs(value * 180) << L"\u00b0";
            text(label.str(), rect(dial.left, 185, dial.right, 204));
        } else
            unavailable(i + 3, rect(dial.left - 3, 185, dial.right + 3, 204));
    }
    for (const std::size_t i : {8u, 13u}) {
        const auto r = flight_rect(i);
        const double fraction = flight_fraction(i, s[i].value);
        text(i == 8 ? L"SPLR" : L"FLAP", rect(r.left - 5, 16, r.right + 5, 35), black, true);
        if (s[i].available && fraction > 0)
            shape(AG_FILL_RECT,
                  rect(r.left + 2, r.top + 2, r.right - 2, r.top + 2 + static_cast<float>(fraction) * 200), navy);
        outline(r, i == 8 ? yellow : blue);
        if (i == 13) {
            for (std::size_t detent = 0; detent < flap_names.size(); ++detent) {
                const float y = 52 + static_cast<float>(detent) * 31.333333f;
                text(wide(flap_names[detent]), rect(r.left, y - 8, r.right, y + 8),
                     s[i].available && 46 + fraction * 200 > y ? white : muted, true);
            }
        } else
            text(L"0", rect(r.left, 44, r.right, 60), s[i].available && fraction > 0.07 ? white : muted, true);
        std::wstring value = L"--";
        if (s[i].available)
            value = i == 8 ? percent(fraction, L"", L"")
                           : wide(flap_names[static_cast<std::size_t>(std::clamp(flight_detent_index(s[i].value), 0, 6))]);
        text(value, rect(r.left - 7, 254, r.right + 7, 273), black, true);
    }
    for (std::size_t i = 9; i < 13; ++i) {
        const auto r = flight_rect(i);
        const auto &v = s[i];
        text(std::to_wstring(i - 8), rect(r.left, 16, r.right, 35));
        const float forward_end = v.split ? idle_y - 3 : idle_y;
        const float reverse_start = v.split ? idle_y + 3 : idle_y;
        if (v.available) {
            const float y = forward_end - 2 - static_cast<float>(v.forward) * (forward_end - r.top - 4);
            if (v.forward > 0)
                shape(AG_FILL_RECT, rect(r.left + 2, y, r.right - 2, forward_end - 2), black);
            const float reverse_end =
                reverse_start + 2 + static_cast<float>(v.reverse) * (r.bottom - reverse_start - 4);
            if (v.reverse > 0)
                shape(AG_FILL_RECT, rect(r.left + 2, reverse_start + 2, r.right - 2, reverse_end), red);
            if (v.split && v.reverse_blocked && v.reverse > 0) {
                shape(AG_PUSH_CLIP, rect(r.left + 2, reverse_start + 2, r.right - 2, reverse_end));
                const float phase = static_cast<float>(std::fmod(std::max(0.0, seconds) * 12, 16));
                for (float cy = reverse_start - 16 + phase; cy < reverse_end + 8; cy += 16) {
                    line(r.left + 3, cy, (r.left + r.right) / 2, cy + 7, yellow, 2);
                    line((r.left + r.right) / 2, cy + 7, r.right - 3, cy, yellow, 2);
                }
                shape(AG_POP_CLIP, {});
            }
        }
        if (v.split) {
            outline(rect(r.left, r.top, r.right, forward_end));
            outline(rect(r.left, reverse_start, r.right, r.bottom));
        } else {
            outline(r);
            line(r.left, idle_y, r.right, idle_y, muted);
        }
        text(v.available ? L"F" + percent(v.forward, L"", L"") : L"F--", rect(r.left - 3, 251, r.right + 3, 270), black,
             true);
        text(v.available ? L"R" + percent(v.reverse, L"", L"") : L"R--", rect(r.left - 3, 270, r.right + 3, 289),
             v.reverse_blocked ? red : black, true);
    }
    for (std::size_t i = 14; i < 18; ++i) {
        const auto r = flight_rect(i);
        const auto &v = s[i];
        text(i % 2 == 0 ? L"L" : L"R", rect(r.left - 5, 205, r.right + 5, 220), muted, true);
        outline(r);
        if (v.available) {
            const float value = static_cast<float>(std::clamp(flight_fraction(i, v.value), 0.0, 1.0));
            if (value > 0)
                shape(AG_FILL_RECT,
                      rect(r.left + 2, r.bottom - 2 - value * (r.bottom - r.top - 4), r.right - 2, r.bottom - 2),
                      black);
        }
        text(v.available ? percent(flight_fraction(i, v.value), L"", L"") : L"--", rect(r.left - 8, 277, r.right + 8, 292), black,
             true);
    }
    text(L"Yoke/rudder: black PSX sent, blue physical/deadzone  |  Yellow: reverse blocked",
         rect(14, 294, 786, 313), muted, true);
}
} // namespace hwio
