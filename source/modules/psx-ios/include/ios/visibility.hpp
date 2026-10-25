#pragma once
#include "core.hpp"
#include <psx/environment/visibility_client.hpp>
namespace ios {
// Session presentation only. Main weather writes remain owned by Environment.
class Visibility {
  using Result=psx::environment::presentation::VisibilityResult;
  using Identity=psx::environment::presentation::ProcessIdentity;
  struct Target {
    std::optional<int> configured,commanded;
    std::optional<Result> result;
    std::optional<psx::environment::presentation::VisibilityCommand> pending;
    std::array<char,8> station{};
    std::int64_t readback_qpc{};
    std::string notice{"Coordinator unavailable"};
  };
  std::array<Target,3> targets_;
  psx::environment::presentation::VisibilityClient client_;
  Identity psx_{};
  std::wstring endpoint_;
  Tick poll_at_{};
public:
  explicit Visibility(std::wstring endpoint={}):endpoint_(std::move(endpoint)){}
  void record(std::string_view,const psx::instructor::Weather&);
  void selected(psx::instructor::WeatherTarget,int);
  void disconnected();
  // The same IOS network worker calls this outside the snapshot mutex.
  void poll(Tick,bool ready,Identity);
  std::string status() const;
};
}
