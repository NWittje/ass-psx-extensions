#pragma once
#include "coordinator/environment.hpp"
namespace coordinator {
// Full PXW1 signed UTC range; conversion is presentation only, never a clock writer.
struct EnvironmentUtcCalendar {bool valid{};unsigned year{},month{},day{},hour{},minute{},second{},millisecond{},day_index{};};
EnvironmentUtcCalendar environment_utc_calendar(std::int64_t) noexcept;
bool environment_time_report_qualified(const environment_wire::Applied&) noexcept;
struct EnvironmentTimeStatus {
 bool source_current{},utc_qualified{},rate_qualified{},paused{},motion_frozen{},report_retained{},report_current{},readback_current{};
 bool source_age_available{},readback_age_available{};
 std::int64_t source_age_qpc{},readback_age_qpc{},readback_error_millis{};
 environment_wire::Phase phase{environment_wire::Phase::unavailable};environment_wire::Reason reason{environment_wire::Reason::unsupported};
};
EnvironmentTimeStatus environment_time_status(const EnvironmentView&,std::int64_t,std::uint64_t) noexcept;
const char* environment_phase_name(environment_wire::Phase) noexcept;
const char* environment_reason_name(environment_wire::Reason) noexcept;
}
