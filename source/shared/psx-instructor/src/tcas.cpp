#include <psx/instructor/tcas.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
namespace psx::instructor {
namespace {
constexpr std::array<TcasPreset, 13> presets{{
  {"off", "Off", 0}, {"crossing_descent", "Intruder crossing while descending", 5},
  {"same_altitude", "Intruder at same altitude", 6}, {"crossing_climb", "Intruder crossing while climbing", 7},
  {"descent_ignoring_ra", "Descending intruder ignoring RA", 8}, {"same_ignoring_ra", "Same-altitude intruder ignoring RA", 9},
  {"climb_ignoring_ra", "Climbing intruder ignoring RA", 10}, {"above_below", "One intruder above and one below", 11},
  {"slow_lateral", "Intruder approaching slowly laterally", 12}, {"slow_vertical", "Intruder approaching slowly vertically", 13},
  {"no_altitude", "Intruder without altitude information", 14}, {"above_level", "Intruder above in level flight", 15},
  {"below_level", "Intruder below in level flight", 16}
}};
}
std::span<const TcasPreset> tcas_presets() { return presets; }
const TcasPreset &tcas_preset(std::string_view id) {
  auto i = std::find_if(presets.begin(), presets.end(), [&](const auto &p) {return p.id == id;});
  if (i == presets.end()) throw std::invalid_argument("Unknown TCAS preset"); return *i;
}
void Tcas::select(std::string_view preset, Tick t) {
  if (busy()) throw std::logic_error("TCAS sender busy"); expected_ = tcas_preset(preset).mode;
  exchange_.begin("Qi201=" + std::to_string(expected_ == 0 ? 0 : 1000 + expected_) + '\n', std::to_string(expected_), t);
}
void Tcas::record(std::string_view line) {
  if (line == "load1" || line == "exit") { disconnected(); return; }
  if (!line.starts_with("Qi201=")) return;
  mode_.reset(); auto n = integer(line.substr(6));
  if (!n || !(*n >= 0 && *n <= 16)) {
    if (exchange_.reading()) exchange_.finish(SettingsStatus::unavailable, "Malformed TCAS readback; no retry"); return;
  }
  mode_ = *n;
  if (exchange_.reading()) exchange_.finish(*n == expected_ ? SettingsStatus::match : SettingsStatus::different,
    *n == expected_ ? "Configured traffic mode matches; TA/RA or visible traffic unverified" : "Configured traffic mode differs; no retry", std::to_string(*n));
}
}
