#pragma once
#include "core.hpp"
#include <psx/scenery/client.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace ios {
inline constexpr std::string_view terrain_warning = "No terrain at destination available. Proceed only if selected destination is over open water.";
// One bounded coverage query. Only this worker calls coordinator IPC; the Main
// acquisition thread merely exchanges immutable input/results under a short lock.
class TerrainPreflight {
public:
  using Check = std::function<psx::scenery::Result(const psx::scenery::Query &, std::uint32_t)>;
  explicit TerrainPreflight(Check = {});
  ~TerrainPreflight();
  bool poll(Operation &, Tick);
  static void decide(Operation &, bool proceed, std::string request, Tick);
private:
  struct Job { psx::scenery::Query query; Tick deadline; };
  Check check_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_{}, busy_{};
  std::optional<Job> job_;
  struct Completed { psx::scenery::Result result; Tick at; };
  std::optional<Completed> result_;
  std::thread worker_;
  void run();
};
}
