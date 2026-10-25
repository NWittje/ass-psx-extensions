#pragma once
#include "settings_exchange.hpp"
#include <array>

namespace psx::instructor {
// Public Main values remain in PSX's native units: pounds and tenths of %MAC.
enum class WeightField { zfw, gw_cg, ground_power, ground_air, engine_model, fifth_engine };
WeightField weight_field(std::string_view);
struct WeightState {
  std::optional<int> zfw_lb, gw_cg10, power_bits, air_bits, engine_model, aero_bits;
  std::optional<double> fuel_lb;
};
constexpr double kg_per_lb = 0.45359237;
double display_weight(double pounds, bool imperial);
int native_weight(double shown, bool imperial);
int minimum_zfw(const WeightState &);
void validate_weight(WeightField, double);
// Pure composition, shared by production and fixtures. No cockpit power switches.
std::string weight_packet(WeightField, double, const WeightState &);
std::optional<double> fuel_quantity(std::string_view);
class WeightService {
  SettingsExchange exchange_;
  WeightState state_;
  WeightField edited_{WeightField::zfw};
  int expected_{};
  bool editing_{};
public:
  WeightService(PsxCommunicator &writer, PsxCommunicator &reader) : exchange_(writer,reader) {}
  void record(std::string_view);
  void set(WeightField, double native_value, Tick);
  void pump(Tick t) { exchange_.pump(t); if(!exchange_.busy())editing_=false; }
  void disconnected() { exchange_.cancel(); state_={}; editing_=false; }
  void cancel() { exchange_.cancel(); editing_=false; }
  bool busy() const { return exchange_.busy(); }
  const WeightState &state() const { return state_; }
  const SettingsResult &result() const { return exchange_.result(); }
};
}
