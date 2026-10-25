#include <psx/instructor/settings_exchange.hpp>
#include <stdexcept>

namespace psx::instructor {

void SettingsExchange::begin(std::string_view packet, std::string expected, Tick t) {
  if (busy()) throw std::logic_error("Settings sender busy");
  result_ = {SettingsStatus::pending, "Sending configured settings", std::move(expected), {}};
  // Tick is monotonic milliseconds. A failed send can already have written bytes;
  // retrying would replay a simulator command whose outcome is uncertain.
  phase_ = Phase::sending;
  deadline_ = t + 5000;
  if (!writer_.send(packet)) finish(SettingsStatus::failed, "Send unsuccessful; partial transmission is possible; no retry");
}

void SettingsExchange::finish(SettingsStatus status, std::string detail, std::string returned) {
  phase_ = Phase::idle;
  result_.status = status;
  result_.detail = std::move(detail);
  result_.returned = std::move(returned);
}

void SettingsExchange::cancel() {
  if (busy()) finish(SettingsStatus::unavailable, "Settings wait cancelled; no replay");
}

void SettingsExchange::pump(Tick t) {
  if (!busy()) return;
  if (writer_.state() != network::State::connected || reader_.state() != network::State::connected) {
    finish(SettingsStatus::unavailable, "Disconnected; transmission may have occurred; no replay");
    return;
  }
  // Finish transmission, wait one second, then request one configured-value read.
  // Readback is not independent proof of the aircraft's actual atmosphere.
  if (phase_ == Phase::sending && !writer_.sending()) {
    phase_ = Phase::waiting;
    deadline_ = t + 1000;
    result_.detail = "Waiting one second after transmission";
  } else if (phase_ == Phase::waiting && t >= deadline_) {
    phase_ = Phase::reading;
    deadline_ = t + 5000;
    result_.detail = "Reading configured settings";
    if (!request_read()) finish(SettingsStatus::unavailable, "Readback request unsuccessful; no retry");
  } else if ((phase_ == Phase::sending || phase_ == Phase::reading) && t >= deadline_) {
    finish(SettingsStatus::unavailable, phase_ == Phase::sending ? "Transmission deadline; partial transmission is possible; no retry" : "Readback unavailable within five seconds; no retry");
  }
}

}
