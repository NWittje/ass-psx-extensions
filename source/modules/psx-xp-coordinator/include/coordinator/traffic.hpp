#pragma once
#include "coordinator/exporter.hpp"
#include <psx/traffic_source/protocol.hpp>
#include <memory>
#include <string>

namespace coordinator {
namespace traffic_wire=psx::traffic_source;
struct TrafficView {
 bool communication_healthy{},source_available{},retained{};
 traffic_wire::Reason reason{traffic_wire::Reason::disconnected};
 ProcessIdentity selected{},process{};
 psx::motion::InstanceId producer{};
 std::uint64_t connection_id{},selection_epoch{},lifecycle{},sequence{};
 std::int64_t observed_qpc{},received_qpc{},admitted_qpc{},source_age_qpc{-1},heartbeat_qpc{};
 std::uint32_t capabilities{},active_slots{},pose_valid_slots{},error{};
 std::uint64_t accepted{},obsolete{},faults{},generation_changes{},revision{};
 psx::traffic::Snapshot source{}; // Original complete source; never normalized or retimed.
 std::int64_t status_received_qpc{};traffic_wire::Frame status_frame{}; // Original worker statistics, independent of source freshness.
};
const char* traffic_reason_name(traffic_wire::Reason) noexcept;
enum class TrafficEventKind:std::uint32_t {selection,hello,frame,lost};
enum class TrafficAdmission:std::uint32_t {accepted,obsolete,fault};
struct TrafficEvent {
 TrafficEventKind kind{TrafficEventKind::lost};
 ProcessIdentity process{};
 std::uint64_t connection_id{},selection_epoch{};
 std::int64_t received_qpc{},subscribe_floor_qpc{};
 std::uint32_t error{};
 traffic_wire::Reason reason{traffic_wire::Reason::disconnected};
 traffic_wire::Hello hello{};
 traffic_wire::Frame frame{};
};
// Acquisition-owned reducer: it has no synchronization, writer or lease API.
class TrafficModel {
public:
 void configure(std::uint64_t) noexcept;
 TrafficAdmission event(const TrafficEvent&,std::int64_t admission_qpc) noexcept;
 void tick(std::int64_t) noexcept;
 void disconnect(std::int64_t) noexcept;
 const TrafficView& view() const noexcept {return view_;}
 struct Peer {
  bool connected{},have_snapshot{};
  ProcessIdentity process{};traffic_wire::Hello hello{};
  std::uint64_t connection{},selection{},wire_sequence{},source_sequence{},lifecycle{};
  std::int64_t floor{},source_floor{},heartbeat{},received{},admitted{};
  std::uint32_t capabilities{},error{};
  traffic_wire::Reason reason{traffic_wire::Reason::disconnected};
  psx::traffic::Snapshot snapshot{};
  std::int64_t status_received{};traffic_wire::Frame status_frame{};
 };
 struct Checkpoint {ProcessIdentity selected{};Peer peer{};TrafficView view{};};
 Checkpoint checkpoint() const noexcept {return {selected_,peer_,view_};}
 bool restore(const Checkpoint&,std::int64_t) noexcept;
private:
 std::uint64_t frequency_{};ProcessIdentity selected_{};Peer peer_{};TrafficView view_{};
};
std::wstring traffic_exporter_pipe_name(ProcessIdentity);
class TrafficService {
public:
 TrafficService();~TrafficService();
 void start(std::uint64_t);void associate(ProcessIdentity);
 void request_stop() noexcept;void join();bool done() const noexcept;
 bool pop(TrafficEvent&);std::uintptr_t ready_event() const noexcept;
 std::uint64_t dropped() const noexcept;void fault(std::uint32_t) noexcept;
 void fixture_name(std::wstring); // Private isolated test seam; never a runtime setting.
 void fixture_subscribe_barrier(std::uintptr_t); // Before start: authored early-response/completion ordering fixture.
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
