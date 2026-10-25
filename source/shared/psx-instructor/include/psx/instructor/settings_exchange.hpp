#pragma once
#include "communicator.hpp"
#include <string>
namespace psx::instructor {
enum class SettingsStatus { idle, pending, match, activated, different, unavailable, failed };
struct SettingsResult {
  SettingsStatus status{SettingsStatus::idle};
  std::string detail, requested, returned;
};
// Internal finite write/wait/read sequence. The caller still owns socket pumping.
class SettingsExchange {
  PsxCommunicator &writer_, &reader_;
  enum class Phase { idle, sending, waiting, reading };
  Phase phase_{Phase::idle};
  Tick deadline_{};
  SettingsResult result_;
public:
  SettingsExchange(PsxCommunicator &writer, PsxCommunicator &reader) : writer_(writer), reader_(reader) {}
  bool request_read() { return reader_.send("bang\n"); }
  void begin(std::string_view packet, std::string expected, Tick);
  void pump(Tick);
  void finish(SettingsStatus, std::string, std::string returned = {});
  void cancel();
  bool busy() const { return phase_ != Phase::idle; }
  bool reading() const { return phase_ == Phase::reading; }
  const SettingsResult &result() const { return result_; }
};
}
