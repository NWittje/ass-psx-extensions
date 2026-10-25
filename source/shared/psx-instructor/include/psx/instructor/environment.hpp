#pragma once
#include "weather.hpp"
#include "runway_selection.hpp"
#include "weather_events.hpp"
#include <chrono>
#include <memory>
#include <deque>

namespace psx::instructor {
inline constexpr const char *weather_names[]{"lower_base","lower_top","lower_coverage","upper_base","upper_top","upper_coverage","cb_base","cb_top","cb_coverage","qnh","surface_temperature","visibility","precipitation","wind_direction","wind_variation","wind_speed","gust","inversion_enabled","inversion_height","inversion_temperature"};
struct WindArc { double middle{}, spread{}; };
double wrap_degrees(double);
WindArc wind_arc(double first, double second);
double isa_temperature(double elevation_ft);
std::vector<WeatherEdit> weather_preset(std::string_view, const WeatherSettings &, double elevation_ft);
// One startup adjustment, including the dormant CB layer. Unedited settings survive.
std::vector<WeatherEdit> minimum_cloud_spacing(const WeatherSettings &);
std::vector<WeatherEdit> windy_edits(double true_runway_heading, int direction_offset, int speed, int gust_addition, int variation_spread);
struct EnvironmentRequest {
  enum class Kind { weather, nearest, time, manual, events } kind{Kind::weather};
  WeatherTarget target{WeatherTarget::planet};
  std::vector<WeatherEdit> edits;
  std::string preset;
  std::optional<int> commanded_visibility_m; // IOS destination intent; PSX edits remain native-limited.
  std::int64_t utc_ms{};
  WeatherEventRequest events;
  std::optional<std::uint64_t> expected_revision;
};
// One shared, bounded send/wait/read cycle. The caller owns sockets and serialization.
class Environment {
  PsxCommunicator &writer_, &reader_;
  Weather weather_;
  WeatherEvents events_;
  RunwaySelection flight_;
  enum class Phase { idle, sending_time, waiting_time, reading_time, weather, events, restoring } phase_{Phase::idle};
  Tick deadline_{}, clock_sent_{};
  int entry_mask_{}, mask_{}, temporary_mask_{2};
  int external_bits_{};
  bool temporary_pause_{};
  std::deque<int> mask_echoes_;
  std::array<bool,3> clocks_read_{};
  std::array<std::optional<std::int64_t>,3> clocks_;
  std::int64_t expected_time_{};
  std::optional<Point> position_;
  std::optional<double> variation_;
  std::shared_ptr<const WeatherStations> stations_;
  std::string state_{"idle"}, detail_;
  void finish(std::string, std::string);
public:
  Environment(PsxCommunicator &w, PsxCommunicator &r) : writer_(w), reader_(r), weather_(w,r), events_(w,r) {}
  void record(std::string_view);
  void start(const EnvironmentRequest &, int mask, std::shared_ptr<const WeatherStations> = {});
  void pump(Tick);
  void abort(std::string_view); // best-effort restoration, never replay settings
  void disconnected();
  bool busy() const { return phase_ != Phase::idle; }
  const auto &state() const { return state_; }
  const auto &detail() const { return detail_; }
  const Weather &weather() const { return weather_; }
  const WeatherEvents &events() const { return events_; }
  const auto &clocks() const { return clocks_; }
  const auto &position() const { return position_; }
  const auto &variation() const { return variation_; }
  const auto &flight() const { return flight_; }
};
}
