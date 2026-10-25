#pragma once
#include <psx/instructor/runway_selection.hpp>
#include <deque>
#include <map>
namespace psx::instructor {
struct StatusPose { Point position; double altitude_ft{}; Tick at{}; };
struct ApproachPoint { double distance_nm{}, lateral_ft{}, height_ft{}; Tick at{}; };
ApproachPoint approach_point(const StatusPose &, const Runway &);
// Source observations only. A display refresh never makes a held value newer.
class StatusDisplay {
public:
  struct Values {
    std::optional<std::array<double,3>> axes;
    std::optional<double> tiller;
    std::optional<std::array<double,4>> n1, epr, flaps;
    std::optional<std::array<double,11>> spoilers;
    std::array<std::optional<int>,3> takeoff;
    std::optional<int> engine;
    Tick axes_at{}, tiller_at{}, n1_at{}, epr_at{}, flaps_at{}, spoilers_at{};
  };
private:
  Values values_;
  RunwaySelection flight_;
  std::optional<StatusPose> pose_;
  std::deque<StatusPose> history_;
  std::map<std::string,std::string> metars_;
  Tick next_sample_{};
  unsigned route_{};
  std::array<std::string,2> origins_, destinations_;
public:
  void record(std::string_view, Tick);
  void sample(Tick);
  void reset();
  void clear_trace();
  const Values &values() const { return values_; }
  const RunwaySelection &flight() const { return flight_; }
  const auto &pose() const { return pose_; }
  const auto &history() const { return history_; }
  std::string metar(std::string_view airport) const;
  std::string fmc_airport(bool departure) const { return route_? (departure?origins_:destinations_)[route_-1]:std::string{}; }
};
}
