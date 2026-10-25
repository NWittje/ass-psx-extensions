#pragma once
#include "settings_exchange.hpp"
#include <span>
namespace psx::instructor {
struct TcasPreset { std::string_view id, name; int mode; };
std::span<const TcasPreset> tcas_presets();
const TcasPreset &tcas_preset(std::string_view);
class Tcas {
  SettingsExchange exchange_;
  std::optional<int> mode_;
  int expected_{};
public:
  Tcas(PsxCommunicator &writer, PsxCommunicator &reader) : exchange_(writer, reader) {}
  explicit Tcas(PsxCommunicator &connection) : Tcas(connection, connection) {}
  bool request_read() { return exchange_.request_read(); }
  void record(std::string_view);
  void select(std::string_view preset, Tick = now());
  void pump(Tick t) { exchange_.pump(t); }
  void disconnected() { mode_.reset(); exchange_.cancel(); }
  void cancel() { exchange_.cancel(); }
  bool busy() const { return exchange_.busy(); }
  const SettingsResult &result() const { return exchange_.result(); }
  const auto &mode() const { return mode_; }
};
}
