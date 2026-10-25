#pragma once
#include "settings_exchange.hpp"
#include "weather.hpp"
#include "runway_selection.hpp"
namespace psx::instructor {
struct WeatherEventRequest {
  std::optional<int> shear_mode; // 0 Off, 1 Left, 2 Ahead, 3 Right
  int shear_top_ft{1000};
  std::optional<bool> ash;
};
Point ash_destination(Point, double true_heading_rad); // 210 NM, evaluated once at Execute.
std::string ash_coordinate(Point);
class WeatherEvents {
  SettingsExchange exchange_;
  RunwaySelection flight_;
  std::array<std::optional<WeatherSettings>,8> zones_;
  std::optional<unsigned> focused_;
  std::optional<std::string> aloft_;
  std::optional<WeatherSettings> expected_zone_;
  std::optional<std::string> expected_aloft_;
  unsigned target_{};
  bool zone_seen_{}, ash_seen_{}, different_{};
  void compare();
public:
  WeatherEvents(PsxCommunicator &w, PsxCommunicator &r) : exchange_(w,r) {}
  void record(std::string_view, Tick = now());
  std::string prepare(const WeatherEventRequest &, Tick = now()); // Validate before pause; no mutation.
  void start(std::string_view packet, Tick = now());
  void pump(Tick t) { exchange_.pump(t); }
  void cancel() { exchange_.cancel(); }
  void disconnected();
  bool busy() const { return exchange_.busy(); }
  const auto &result() const { return exchange_.result(); }
  const auto &focused() const { return focused_; }
  const auto &settings() const { return zones_; }
  const auto &aloft() const { return aloft_; }
  std::optional<bool> ash_on() const;
};
}
