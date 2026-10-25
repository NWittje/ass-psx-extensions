#pragma once
#include "coordinator/observation.hpp"
#include <psx/diagnostics/journal.hpp>
namespace coordinator {
struct HealthReport {
    bool source_ready{}, bridge_connected{}, plugin_consuming{}, storage_degraded{};
    psx::diagnostics::Freshness report{}, callback{};
    bool counter_baseline_valid{}, counter_delta_valid{};
    std::uint64_t diagnostic_dropped{}, events_lost{}, persistence_lost{};
};
// Pure snapshot evaluator: no clock, I/O, reset, reconnect or ECON-age inference.
class HealthEvaluator {
public:
    HealthReport evaluate(const ObservationSnapshot&, const psx::diagnostics::JournalStatus&,
        std::int64_t now, std::uint64_t frequency, std::uint32_t delayed_ms=1000) noexcept;
private:
    bool baseline_{};
    std::uint64_t load_{},enable_{},dropped_{},events_{},disk_{};
};
psx::diagnostics::Context diagnostic_context(const psx::bridge::SessionStatus&) noexcept;
void log_startup_failure(std::string_view text, psx::diagnostics::ErrorDomain, std::uint32_t error,const psx::diagnostics::Options* options=nullptr) noexcept;
}
