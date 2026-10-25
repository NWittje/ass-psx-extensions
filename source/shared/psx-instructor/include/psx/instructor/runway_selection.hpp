#pragma once
#include "communicator.hpp"
#include "airport_database.hpp"
#include <array>
#include <span>
#include <vector>
namespace psx::instructor {
struct RunwayOption {
  std::string airport, runway;
  Point threshold;
  double heading{}, distance_nm{}; // radians, true north
  bool fmc{};
};
class RunwaySelection {
  std::optional<Point> position_;
  std::optional<double> track_;
  Tick position_at_{};
  unsigned route_{};
  std::optional<double> heading_;
  std::optional<bool> ground_;
  std::array<std::string,2> origins_, departure_runways_;
  std::array<std::string,2> destinations_;
  std::string runway_;
public:
  void record(std::string_view, Tick = now());
  void reset();
  const std::optional<Point> &position() const { return position_; }
  std::optional<double> track(Tick = now()) const;
  std::pair<std::string,std::string> fmc(bool departure = false) const;
  const auto &heading() const { return heading_; }
  const auto &ground() const { return ground_; }
};
std::vector<RunwayOption> runway_candidates(std::span<const RunwayOption>, Point,
    std::optional<double> true_ground_track, std::string_view fmc_airport, std::string_view fmc_runway);
// Windy alone may use heading when stationary. TCAS candidate semantics remain unchanged.
std::optional<RunwayOption> windy_runway(std::span<const RunwayOption>, const RunwaySelection &, Tick = now());
}
