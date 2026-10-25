#pragma once
#include "settings_exchange.hpp"
#include <array>
#include <span>
#include <vector>
namespace psx::instructor {
constexpr std::size_t failure_count = 193; // 184 base records plus nine installed ordinary additions.
enum class FailureCriterion { ias, altitude, heading, distance, utc_time, random, immediate };
struct FailureInfo {
  unsigned id;
  std::string_view name, category, severe, non_severe;
};
struct FailureSetting {
  bool severe{};
  FailureCriterion criterion{FailureCriterion::immediate};
  std::string value;
  static FailureSetting parse(std::string_view);
  std::string encode() const;
  bool off() const { return value == "-99"; }
  bool active() const { return !off() && criterion == FailureCriterion::immediate; }
  bool armed() const { return !off() && !active(); }
};
struct FailureEdit {
  unsigned id{}; bool severe{}; FailureCriterion criterion{FailureCriterion::immediate}; std::string value;
  std::optional<unsigned> delay_minutes; // Converted once at dispatch using PSX UTC, never browser time.
};
struct FailureItemResult {
  unsigned id{};
  FailureSetting expected;
  SettingsStatus status{SettingsStatus::pending};
  std::optional<FailureSetting> returned;
};
std::span<const FailureInfo> failure_catalogue();
const FailureInfo &failure_info(unsigned);
std::vector<unsigned> search_failures(std::string_view);
std::string_view criterion_name(FailureCriterion);
FailureCriterion failure_criterion(std::string_view);
void validate_failure_edit(const FailureEdit &);
class Failures {
  SettingsExchange exchange_;
  std::array<std::optional<FailureSetting>, failure_count> settings_;
  std::optional<std::string> utc_;
  std::vector<FailureItemResult> batch_;
  void begin(std::vector<FailureItemResult>, Tick);
  void finish_batch();
  std::string delayed_utc(unsigned) const;
public:
  Failures(PsxCommunicator &writer, PsxCommunicator &reader) : exchange_(writer, reader) {}
  explicit Failures(PsxCommunicator &connection) : Failures(connection, connection) {}
  bool request_read() { return exchange_.request_read(); }
  void record(std::string_view);
  void set(const FailureEdit &, Tick = now());
  void clear(unsigned, Tick = now());
  void set_batch(std::span<const FailureEdit>, Tick = now());
  void clear_batch(std::span<const unsigned>, Tick = now());
  void pump(Tick);
  void disconnected();
  void cancel() { exchange_.cancel(); }
  bool busy() const { return exchange_.busy(); }
  const SettingsResult &result() const { return exchange_.result(); }
  std::span<const FailureItemResult> items() const { return batch_; }
  const std::optional<FailureSetting> &setting(unsigned) const;
};
}
