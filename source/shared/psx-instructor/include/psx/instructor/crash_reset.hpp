#pragma once
#include "communicator.hpp"
namespace psx::instructor {
// Public pause path only: motion-frozen running second, then full pause.
class CrashReset {
  PsxCommunicator &writer_;
  enum class Phase { idle, sending, running, pausing, observing } phase_{Phase::idle};
  Tick deadline_{};
  std::optional<int> mask_;
  int finish_mask_{};
  bool final_observed_{};
  std::string state_{"idle"},detail_;
  void finish(std::string,std::string);
public:
  explicit CrashReset(PsxCommunicator &writer) : writer_(writer) {}
  void record(std::string_view);
  void start(Tick = now());
  void pump(Tick);
  void cancel();
  void disconnected();
  bool busy() const { return phase_!=Phase::idle; }
  const auto &state() const { return state_; }
  const auto &detail() const { return detail_; }
  int finish_mask() const { return finish_mask_; }
};
}
