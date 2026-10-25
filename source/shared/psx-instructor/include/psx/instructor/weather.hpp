#pragma once
#include "communicator.hpp"
#include "airport_database.hpp"
#include <span>

namespace psx::instructor {
enum class WeatherTarget { planet, zone1, all };
enum class WeatherField {
  lower_base, lower_top, lower_coverage, upper_base, upper_top, upper_coverage,
  cb_base, cb_top, cb_coverage, qnh, surface_temperature, visibility,
  precipitation, wind_direction, wind_variation, wind_speed, gust,
  inversion_enabled, inversion_height, inversion_temperature
};
struct WeatherEdit { WeatherField field; double value; };
class WeatherSettings {
  std::array<std::string, 24> tokens_;
  WeatherSettings() = default;
  friend class Weather;
public:
  static WeatherSettings parse(std::string_view);
  double value(WeatherField) const; // QNH hPa; other fields use native integer units.
  int pressure_raw() const;
  int qnh_display_hpa() const;
  std::string encode() const;
  const auto &tokens() const { return tokens_; }
  WeatherSettings with(std::span<const WeatherEdit>) const;
};
struct WeatherZone {
  float latitude{}, longitude{}; // radians, matching PSX's weather centre precision
  int radius{}, elevation{}, month{};
  std::string identifier; // four-character identifier, preferred runway and P/p
  static WeatherZone parse(std::string_view);
  std::string encode() const;
  bool operator==(const WeatherZone &) const = default;
};
struct WeatherStation {
  std::string identifier;
  float latitude{}, longitude{};
  int elevation{};
};
class WeatherStations {
  std::vector<WeatherStation> stations_;
public:
  // Caller loads this outside the communication worker. No simulator writes.
  static WeatherStations load(const std::filesystem::path &psx_root);
  const WeatherStation &nearest(Point) const;
  std::size_t size() const { return stations_.size(); }
};
enum class WeatherStatus { idle, pending, match, different, unavailable, failed };
struct WeatherDifference { std::string field, requested, returned; };
struct WeatherResult {
  WeatherStatus status{WeatherStatus::idle};
  std::string detail;
  std::vector<WeatherDifference> differences;
  std::optional<WeatherSettings> settings;
  std::optional<WeatherZone> zone;
  std::optional<std::string> metar;
};
// Caller owns connection/scheduling and feeds reader records through record().
// This object never connects, pumps sockets, starts threads or changes pause.
class Weather {
  PsxCommunicator &writer_, &reader_;
  enum class Phase { idle, locating, sending, waiting, reading };
  enum class Action { apply, nearest, manual };
  Phase phase_{Phase::idle};
  Action action_{Action::apply};
  Tick deadline_{};
  WeatherTarget target_{WeatherTarget::planet};
  std::array<std::optional<WeatherSettings>, 8> settings_;
  unsigned read_settings_{};
  bool read_manual_{};
  std::optional<WeatherZone> zone_;
  std::optional<bool> automatic_;
  std::optional<int> focused_;
  std::optional<Point> position_;
  std::optional<std::string> metar_;
  std::optional<WeatherSettings> expected_;
  std::optional<WeatherZone> expected_zone_;
  std::vector<WeatherField> edited_;
  bool read_zone_{}, read_metar_{};
  const WeatherStations *stations_{};
  WeatherResult result_;
  void begin(Action, std::string_view);
  void finish(WeatherStatus, std::string);
  void compare();
  void place_nearest();
public:
  Weather(PsxCommunicator &writer, PsxCommunicator &reader) : writer_(writer), reader_(reader) {}
  explicit Weather(PsxCommunicator &connection) : Weather(connection, connection) {}
  bool request_read(); // one bang; feed resulting records to record()
  void record(std::string_view);
  void apply(WeatherTarget, std::span<const WeatherEdit>, bool manual = false);
  void set_nearest_airport(const WeatherStations &, Tick = now());
  void select_manual();
  void pump(Tick); // call after pumping sockets; wait starts after writer drains
  void disconnected(); // optional immediate notification; pump also checks transport
  void cancel(); // stop this local wait; do not replay or clear observed settings
  bool busy() const { return phase_ != Phase::idle; }
  const WeatherResult &result() const { return result_; }
  const auto &settings(WeatherTarget target) const { return settings_[target == WeatherTarget::zone1 ? 1 : 0]; }
  const auto &zone_settings() const { return settings_; }
  const auto &zone() const { return zone_; }
  const auto &automatic() const { return automatic_; }
  const auto &focused_zone() const { return focused_; }
  const auto &position() const { return position_; }
};
} // namespace psx::instructor
