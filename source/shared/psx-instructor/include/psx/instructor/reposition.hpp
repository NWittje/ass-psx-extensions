#pragma once
#include "communicator.hpp"
#include "air_data.hpp"

namespace psx::instructor {
struct RepositionTarget {
  double lat{}, lon{}, heading{}, altitude{}, pitch{}, ias{}, terrain{};
  bool ground{};
};
std::string placement(const RepositionTarget &, double tas);
double corrected_tas(double ias, double altitude, double oat, double qnh);

class Reposition {
public:
  using Send = std::function<bool(std::string_view)>;
  using Log = std::function<void(std::string_view)>;
private:
  enum class Phase { idle, pause_query, placement, running, weather, finished };
  Phase phase_{Phase::idle};
  RepositionTarget target_;
  std::optional<AirData> air_data_;
  Tick deadline_{};
  int mask_{-1}, finish_mask_{-1}, zone_{-1};
  std::optional<double> oat_;
  std::array<std::optional<double>, 8> pressures_;
  Log log_;
  std::string stage_, state_{"queued"}, detail_;
  void report(std::string_view) const noexcept;
  void begin(std::string_view);
  void success(std::string_view);
  bool write(const Send &, const std::string &);
  std::string missing_weather() const;
public:
  void start(const RepositionTarget &, Tick, const Send &, Log = {});
  void record(std::string_view);
  void pump(Tick, const Send &);
  void stop(std::string reason, bool uncertain = true);
  bool finished() const { return phase_ == Phase::finished; }
  const std::string &state() const { return state_; }
  const std::string &detail() const { return detail_; }
  int finish_mask() const { return finish_mask_; }
};
}
