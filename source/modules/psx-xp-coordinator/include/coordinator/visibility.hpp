#pragma once
#include "coordinator/exporter.hpp"
#include <psx/environment/protocol.hpp>
#include <array>

namespace coordinator {
// Normalized context supplied only after the native exporter contract qualifies it.
// These module values are not a wire format or a guessed Weather slot mapping.
enum class VisibilityTarget : std::uint32_t { planet=0,zone1=1,other=2,all=4 };
struct VisibilityContext {
 ProcessIdentity psx{};
 psx::environment::Sample source{};
 std::int64_t configured_observation_start_qpc{},configured_observation_qpc{};
 VisibilityTarget active{VisibilityTarget::other};
 std::uint32_t configured_mask{};
 std::array<std::uint32_t,5> configured_m{};
 std::array<char,8> zone1_station{};
 bool station_available{};
};
struct VisibilitySelection {
 double metres{};
 std::int64_t configured_readback_qpc{};
 std::array<char,8> station{};
};
enum class VisibilityPolicyKind : std::uint32_t { native=0,sentinel_ceiling=1,session_selection=2 };
struct VisibilityPolicy {
 VisibilityPolicyKind kind{VisibilityPolicyKind::native};
 VisibilityTarget target{VisibilityTarget::other};
 double metres{};
 std::uint64_t revision{};
};
// The environment acquisition owner calls this bounded value reducer. No IO/timer.
class VisibilitySelections {
public:
 void configure(std::uint64_t frequency) noexcept {frequency_=frequency;}
 void associate(ProcessIdentity) noexcept;
 void ios(std::uint64_t epoch,std::uint64_t session,bool connected) noexcept;
 void clear() noexcept;
 bool select(VisibilityTarget,double,ProcessIdentity,std::int64_t,
             const std::array<char,8>&,std::int64_t) noexcept;
 bool observe(const VisibilityContext&,std::int64_t) noexcept;
 VisibilityPolicy policy(const psx::environment::Sample&,std::int64_t) const noexcept;
 const std::array<VisibilitySelection,5>& selections() const noexcept{return selections_;}
 const VisibilityContext& context() const noexcept{return context_;}
 bool current(std::int64_t) const noexcept;
 std::uint64_t revision() const noexcept{return revision_;}
 bool session(std::uint64_t value) const noexcept{return ios_connected_&&ios_session_==value;}
private:
 bool source_matches(const psx::environment::Sample&) const noexcept;
 std::uint64_t frequency_{},ios_epoch_{},ios_session_{},revision_{};
 bool ios_connected_{};
 ProcessIdentity psx_{};
 VisibilityContext context_{};
 std::array<VisibilitySelection,5> selections_{};
};
}
