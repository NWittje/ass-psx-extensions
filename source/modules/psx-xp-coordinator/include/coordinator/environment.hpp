#pragma once
#include "coordinator/exporter.hpp"
#include "coordinator/environment_diagnostics.hpp"
#include "coordinator/visibility.hpp"
#include <psx/environment/protocol.hpp>
#include <psx/environment/presentation.hpp>
#include <array>
#include <memory>
#include <string>
namespace coordinator {
namespace environment_wire=psx::environment;
namespace presentation_wire=psx::environment::presentation;
enum class EnvironmentEventKind:std::uint8_t {frame,lost};
enum class EnvironmentAdmission:std::uint8_t {accepted,obsolete,fault};
struct EnvironmentEvent {EnvironmentEventKind kind{EnvironmentEventKind::frame};environment_wire::Role role{environment_wire::Role::plugin};ProcessIdentity process{};std::int64_t received_qpc{};std::uint32_t error{};std::uint64_t connection_generation{};environment_wire::Frame frame{};};
struct EnvironmentGroupView {bool available{},requested{},owned{},pending{},blocked{},applied_available{};environment_wire::Reason reason{environment_wire::Reason::unsupported};std::int64_t expires_qpc{},renewed_qpc{},baseline_floor{};environment_wire::Applied applied{};
 bool applied_retained{};std::int64_t report_received_qpc{};environment_wire::Phase phase{environment_wire::Phase::unavailable};
 std::uint64_t weather_target{},current_lifecycle{},current_sequence{};std::int64_t current_source_end_qpc{},weather_observed_qpc{};};
struct EnvironmentView {bool following{},plugin_connected{},exporter_connected{},source_available{};std::uint64_t owner{},accepted{},rejected{},obsolete{},controls{},acks{},boundaries{},overflows{},dispatched{};environment_wire::Hello plugin{},exporter{};environment_wire::Sample source{};std::array<EnvironmentGroupView,2> groups{};
 bool plugin_v2{},exporter_v2{};std::uint32_t plugin_capabilities{},exporter_capabilities{};
 presentation_wire::Context source_context{};presentation_wire::VisibilityPolicy visibility_policy{},applied_visibility_policy{};
 bool applied_visibility_available{};std::array<VisibilitySelection,5> visibility_selections{};
 presentation_wire::LocalVisibility local_visibility{};presentation_wire::WeatherChange source_change{};std::uint64_t weather_target{},weather_target_sequence{},weather_target_revision{};std::int64_t weather_target_source_qpc{};};
struct EnvironmentDispatch {environment_wire::Kind kind{environment_wire::Kind::sample};environment_wire::Control control{};environment_wire::Sample sample{};environment_wire::Capture capture{};environment_wire::Hello hello{};environment_wire::Applied applied{};
 bool version2{};std::uint32_t capabilities{};presentation_wire::Context context{};presentation_wire::VisibilityPolicy visibility{};
 presentation_wire::LocalVisibility local_visibility{};presentation_wire::WeatherChange change{};presentation_wire::WeatherTarget weather{};presentation_wire::AppliedPresentation weather_report{};};
bool encode_environment_dispatch(const EnvironmentDispatch&,environment_wire::Role,std::uint32_t,environment_wire::Frame&) noexcept;
// Acquisition-owned bounded reducer. Clock/weather realization belongs to plugin.
class EnvironmentModel {
public:
 void configure(std::uint64_t,std::uint64_t) noexcept;
 void following(bool,std::int64_t) noexcept;
 void associate_visibility(ProcessIdentity) noexcept;
 void visibility_ios(std::uint64_t,std::uint64_t,bool) noexcept;
 void invalidate_visibility(std::int64_t) noexcept;
 presentation_wire::VisibilityResult visibility_request(const presentation_wire::VisibilityRequest&,std::int64_t) noexcept;
 EnvironmentAdmission admit(const EnvironmentEvent&) noexcept;
 void tick(std::int64_t) noexcept;
 bool next(EnvironmentDispatch&) noexcept;
 const EnvironmentView& view() const noexcept{return view_;}
 const presentation_wire::WeatherTarget& weather_target() const noexcept{return weather_target_;}
 const EnvironmentDiagnosticInput& diagnostic_input() const noexcept{return diagnostic_input_;}
private:
 bool fresh(const environment_wire::Sample&,std::int64_t) const noexcept;
 bool command(std::size_t,environment_wire::Action,std::int64_t) noexcept;
 void release(std::size_t,environment_wire::Reason,std::int64_t,bool) noexcept;
 void lose(environment_wire::Role,std::int64_t) noexcept;
 EnvironmentAdmission receive(const EnvironmentEvent&) noexcept;
 bool issued(const environment_wire::Control&) const noexcept;
 bool issued(const environment_wire::Sample&) const noexcept;
 bool retained_report(const environment_wire::Applied&,const presentation_wire::VisibilityPolicy&,bool) const noexcept;
 EnvironmentView view_{};std::uint64_t frequency_{},operation_{};std::int64_t policy_now_{},selection_floor_qpc_{};
 EnvironmentDiagnosticInput diagnostic_input_{};
 std::array<environment_wire::Control,16> commands_{};std::size_t head_{},size_{};
 std::array<environment_wire::Control,2> pending_{};
 std::array<environment_wire::Control,64> issued_controls_{};
 std::array<environment_wire::Sample,64> issued_samples_{};std::size_t sample_cursor_{};
 std::array<presentation_wire::VisibilityPolicy,64> issued_policies_{};
 struct RetainedSource {environment_wire::Sample source{};presentation_wire::VisibilityPolicy policy{};psx::motion::InstanceId provider{};
  std::uint64_t owner{},provider_generation{};bool presentation{};environment_wire::Phase phase{environment_wire::Phase::unavailable};};
 // Last correlated terminal and admitted active report per group survive healthy
 // forwarding in the other group. They permit obsolete classification only.
 std::array<RetainedSource,2> terminal_sources_{},report_sources_{};
 VisibilitySelections visibility_;ProcessIdentity visibility_psx_{};
 std::array<std::uint64_t,2> generations_{};
 bool dirty_{},needs_baseline_{};std::int64_t source_floor_{};std::uint64_t announced_lifecycle_{},announced_configuration_{};
 presentation_wire::WeatherChange source_change_{};presentation_wire::WeatherTarget weather_target_{};
 std::uint64_t next_weather_target_{};bool weather_policy_pending_{};
};
std::wstring environment_plugin_pipe_name();std::wstring environment_exporter_pipe_name(ProcessIdentity);
class EnvironmentService {
public:
 EnvironmentService();~EnvironmentService();
 void start(std::uint64_t,std::uint64_t);void associate(ProcessIdentity);void plugin(ProcessIdentity);void stop() noexcept;
 bool pop(EnvironmentEvent&);bool send(environment_wire::Role,const EnvironmentDispatch&);void fault(environment_wire::Role,std::uint32_t) noexcept;
 std::uintptr_t ready_event() const noexcept;std::uint64_t dropped() const noexcept;std::uint64_t coalesced() const noexcept;
private:
 friend struct EnvironmentTestAccess;friend class ObservationController;
 void fixture_names(std::wstring,std::wstring);bool fixture_event(const EnvironmentEvent&);bool fixture_output(environment_wire::Role,environment_wire::Frame&);
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
