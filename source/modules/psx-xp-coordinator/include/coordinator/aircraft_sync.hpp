#pragma once
#include "coordinator/exporter.hpp"
#include <psx/aircraft_sync/protocol.hpp>
#include <psx/bridge/protocol.hpp>
#include <memory>
#include <string>
namespace coordinator {
namespace aircraft_wire=psx::aircraft_sync;
enum class AircraftEventKind:std::uint8_t {frame,lost};
enum class AircraftAdmission:std::uint8_t {accepted,obsolete,fault};
struct AircraftEvent {AircraftEventKind kind{AircraftEventKind::frame};aircraft_wire::Role role{aircraft_wire::Role::plugin};ProcessIdentity process{};std::int64_t received_qpc{};std::uint32_t error{};std::uint64_t connection_generation{};aircraft_wire::Frame frame{};};
struct AircraftGroupView {bool available{},requested{},owned{},pending{},applied_available{},blocked{};aircraft_wire::Sample sample{};aircraft_wire::Applied applied{};aircraft_wire::Reason reason{aircraft_wire::Reason::unsupported};std::int64_t expires_qpc{},renewed_qpc{},baseline_floor{};std::uint64_t operation{};};
struct AircraftView {bool following{},plugin_connected{},exporter_connected{};std::uint64_t owner{},accepted{},rejected{},obsolete{},controls{},acks{},boundaries{},overflows{};aircraft_wire::Hello plugin{},exporter{};std::array<AircraftGroupView,aircraft_wire::group_count> groups{};};
struct AircraftDispatch {aircraft_wire::Kind kind{aircraft_wire::Kind::sample};aircraft_wire::Control control{};aircraft_wire::Sample sample{};aircraft_wire::Capture capture{};aircraft_wire::Hello hello{};};
// Acquisition-owned reducer; no file/transport/heap operations.
class AircraftModel {
public:
 void configure(std::uint64_t frequency,std::uint64_t owner) noexcept;
 void following(bool,std::int64_t) noexcept;
 void provider_boundary(std::int64_t) noexcept;
 void public_session(const psx::bridge::SessionStatus&,std::int64_t) noexcept;
 bool public_observation(const psx::aircraft::Observation&) noexcept;
 bool public_stabilizer(std::string_view,std::int64_t,std::uint64_t) noexcept;
 bool event(const AircraftEvent&) noexcept;
 AircraftAdmission admit(const AircraftEvent&) noexcept;
 void tick(std::int64_t) noexcept;
 bool next(AircraftDispatch&) noexcept;
 const AircraftView& view() const noexcept{return view_;}
private:
 struct Output {aircraft_wire::Control value{};};
 bool command(std::size_t,aircraft_wire::Action,std::int64_t) noexcept;
 bool select(std::size_t,std::int64_t) noexcept;
 bool patch(aircraft_wire::Group,std::size_t,const psx::aircraft::PropertyValue*,std::int64_t,std::uint64_t,bool,bool=false) noexcept;
 void release(std::size_t,aircraft_wire::Reason,std::int64_t,bool) noexcept;
 void lose(aircraft_wire::Role,std::int64_t) noexcept;
 bool fresh(const aircraft_wire::Sample&,std::int64_t) const noexcept;
 AircraftAdmission receive(const AircraftEvent&) noexcept;
 bool issued(const aircraft_wire::Control&) const noexcept;
 bool issued(const aircraft_wire::Applied&) const noexcept;
 AircraftView view_{};std::uint64_t frequency_{},operation_{},public_sequence_{};
 psx::bridge::SessionStatus public_{};psx::motion::InstanceId public_id_{};
 std::array<aircraft_wire::Sample,6> native_{},fallback_{};
 std::array<bool,6> native_valid_{},fallback_valid_{},dirty_{};
 std::array<Output,16> commands_{};std::size_t head_{},size_{},cursor_{};
 std::array<aircraft_wire::Control,6> pending_{};
 std::array<std::uint64_t,6> acquire_source_floor_{};
 std::array<std::int64_t,6> acquire_queue_floor_{};
 std::array<aircraft_wire::Control,64> issued_{};
 std::array<std::uint64_t,2> generations_{};
 std::uint64_t native_lifecycle_{},native_configuration_{};
 std::int64_t native_identity_qpc_{};
};
std::wstring aircraft_plugin_pipe_name();std::wstring aircraft_exporter_pipe_name(ProcessIdentity);
class AircraftService {
public:
 AircraftService();~AircraftService();
 void start(std::uint64_t,std::uint64_t);void associate(ProcessIdentity);void plugin(ProcessIdentity);void stop() noexcept;
 bool pop(AircraftEvent&);bool send(aircraft_wire::Role,const AircraftDispatch&);void fault(aircraft_wire::Role,std::uint32_t) noexcept;
 std::uintptr_t ready_event() const noexcept;std::uint64_t dropped() const noexcept;std::uint64_t coalesced() const noexcept;
private:
 friend struct AircraftTestAccess;friend class ObservationController;
 void fixture_names(std::wstring,std::wstring);bool fixture_event(const AircraftEvent&);bool fixture_output(aircraft_wire::Role,aircraft_wire::Frame&);
 std::size_t fixture_controls_queued(aircraft_wire::Role) const;
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
