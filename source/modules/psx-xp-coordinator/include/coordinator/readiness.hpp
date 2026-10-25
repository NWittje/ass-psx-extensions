#pragma once
#include "coordinator/exporter.hpp"
#include <psx/status/protocol.hpp>
#include <array>
#include <memory>
#include <string>

namespace coordinator {
namespace status_wire=psx::status;
enum class XPlaneState:std::uint32_t {Ready,Loading,Unavailable,Unknown};
enum class PsxState:std::uint32_t {Running,Crashed,Unavailable,Unknown};
enum class ReadinessReason:std::uint32_t {
 none,disconnected,timeout,awaiting_source,unsupported,unqualified,stale,
 loading,disabled,binding,reference,producer_fault,identity,lifecycle,invalid
};
const char* xplane_state_name(XPlaneState) noexcept;
const char* psx_state_name(PsxState) noexcept;
const char* readiness_reason_name(ReadinessReason) noexcept;
struct ReadinessDetail {
 ReadinessReason reason{ReadinessReason::disconnected};
 bool communication_healthy{},observation_available{},last_known_available{};
 ProcessIdentity process{};psx::motion::InstanceId producer{},last_known_producer{};
 std::uint64_t connection_id{},lifecycle{},revision{};
 std::int64_t observed_qpc{},observation_age_qpc{-1},last_known_qpc{},last_known_phase_qpc{};
 std::uint32_t last_known_state{3};
 status_wire::Snapshot source{}; // actual accepted facts, including original source clock
};
struct ReadinessView {
 XPlaneState xplane{XPlaneState::Unavailable};PsxState psx{PsxState::Unavailable};
 ReadinessDetail plugin,exporter;
 std::uint64_t revision{},accepted{},obsolete{},faults{};
};
enum class ReadinessEventKind:std::uint32_t {hello,frame,lost,selection};
struct ReadinessEvent {
 ReadinessEventKind kind{ReadinessEventKind::lost};status_wire::Role role{status_wire::Role::plugin};
 ProcessIdentity process{};std::uint64_t connection_id{};
 std::int64_t received_qpc{},subscribe_floor_qpc{};
 std::uint32_t error{};status_wire::Hello hello{};status_wire::Frame frame{};
};
enum class ReadinessAdmission {accepted,obsolete,fault};
// Solely the acquisition owner mutates this reducer. It has no command/lease API.
class ReadinessModel {
public:
 void configure(std::uint64_t) noexcept;
 ReadinessAdmission event(const ReadinessEvent&) noexcept;
 void tick(std::int64_t) noexcept;
 void disconnect(std::int64_t) noexcept;
 const ReadinessView& view() const noexcept {return view_;}
 // A bounded exact reducer checkpoint for Capture startup; serialization is explicit.
 struct Peer {bool connected{},have_snapshot{};ProcessIdentity process{};status_wire::Hello hello{};
  status_wire::Snapshot snapshot{};std::uint64_t connection{},wire_sequence{},source_sequence{},lifecycle{};
  std::int64_t floor{},heartbeat{};std::uint32_t capabilities{};status_wire::Reason reason{status_wire::Reason::none};};
 struct Checkpoint {std::array<Peer,2> peers{};ReadinessView view{};ProcessIdentity selected_plugin{};};
 Checkpoint checkpoint() const noexcept {return {peers_,view_,selected_plugin_};}
 bool restore(const Checkpoint&,std::int64_t) noexcept;
private:
 std::array<Peer,2> peers_{};ReadinessView view_{};std::uint64_t frequency_{};
 ProcessIdentity selected_plugin_{};
 void evaluate(std::size_t,std::int64_t) noexcept;
};
std::wstring readiness_plugin_pipe_name();
std::wstring readiness_exporter_pipe_name(ProcessIdentity);
class ReadinessService {
public:
 ReadinessService();~ReadinessService();
 void start(std::uint64_t);void associate(ProcessIdentity);void plugin(ProcessIdentity);
 void request_stop() noexcept;void join();bool done() const noexcept;
 bool pop(ReadinessEvent&);std::uintptr_t ready_event() const noexcept;std::uint64_t dropped() const noexcept;
 void fault(status_wire::Role,std::uint32_t) noexcept;
 void fixture_names(std::wstring,std::wstring); // private isolated endpoint test seam
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
