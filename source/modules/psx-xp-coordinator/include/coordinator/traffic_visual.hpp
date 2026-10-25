#pragma once
#include "coordinator/traffic.hpp"
#include <psx/traffic_visual/protocol.hpp>
#include <psx/traffic_visual/presentation.hpp>
namespace coordinator {
namespace visual_wire=psx::traffic_visual;
struct TrafficVisualSource {
 traffic_wire::Sample sample{};bool healthy{},available{},retained{};traffic_wire::Reason reason{traffic_wire::Reason::disconnected};
 std::int64_t received_qpc{},admitted_qpc{};
};
TrafficVisualSource visual_source(const TrafficView&) noexcept;
enum class TrafficVisualEventKind:std::uint32_t {selection,hello,frame,sent,lost,source,following,policy,capture,dispatch};
struct TrafficVisualEvent {
 TrafficVisualEventKind kind{TrafficVisualEventKind::lost};ProcessIdentity process{};std::uint64_t connection{};
 std::int64_t received_qpc{};std::uint32_t error{};visual_wire::Reason reason{visual_wire::Reason::disconnected};
 visual_wire::Hello hello{};visual_wire::Frame frame{};TrafficVisualSource source{};
 bool enabled{};psx::motion::ReconstructionSettings policy{};std::uint64_t capture_generation{};
};
enum class TrafficVisualAdmission:std::uint32_t {accepted,obsolete,fault};
struct TrafficVisualView {
 bool following{},communication_healthy{},requested{},pending{},owned{},report_retained{},report_current{};
 ProcessIdentity selected{},process{};visual_wire::Hello plugin{};psx::motion::InstanceId owner{};
 std::uint64_t connection{},lease{},request{},sent_sequence{},accepted{},obsolete{},faults{},overflows{},boundaries{},revision{};
 visual_wire::Phase phase{visual_wire::Phase::observing};visual_wire::Reason reason{visual_wire::Reason::disconnected};
 psx::motion::ReconstructionSettings policy{};std::uint32_t error{},drawable_mask{},submitted_mask{},held_mask{},hidden_mask{};
 std::int64_t source_floor_qpc{},heartbeat_qpc{},renewed_qpc{},report_received_qpc{},report_admitted_qpc{},presentation_age_qpc{-1};
 visual_wire::Report report{};std::uint64_t capture_generation{},detail_dropped{};
 std::array<visual_wire::Detail,7> details{};std::array<std::int64_t,7> detail_received_qpc{};
};
const char* traffic_visual_reason_name(visual_wire::Reason) noexcept;
// Acquisition owns every transition and bounded issued-record ledger. No SDK,
// I/O, timers, background mutation or destination pixel inference belongs here.
class TrafficVisualModel {
public:
 struct State {
  std::uint64_t frequency{};TrafficVisualView view{};TrafficVisualSource source{};
  bool connected{},source_dirty{},capture_dirty{},capture_enabled{};
  std::uint64_t wire_sequence{},request_counter{},lease_counter{},capture_generation{};
  std::int64_t cooldown_qpc{},last_capture_qpc{},capture_start_qpc{},capture_floor_qpc{};
  visual_wire::Claim binding{};std::array<visual_wire::Claim,16> commands{};std::size_t head{},size{};
  std::array<visual_wire::Claim,64> issued_controls{};
  std::array<visual_wire::Claim,64> planned_controls{};
  std::array<visual_wire::Frame,128> planned_sources{};std::size_t planned_source_cursor{};
  std::array<visual_wire::Bundle,512> issued_sources{};std::size_t source_cursor{};
  std::array<visual_wire::Capture,16> issued_captures{};std::size_t capture_cursor{};
  std::array<visual_wire::Capture,16> planned_captures{};std::size_t planned_capture_cursor{};
  struct Resource {std::uint64_t connection{},lease{},generation{};};std::array<Resource,64> retired_resources{};std::size_t resource_cursor{};
 };
 TrafficVisualModel();~TrafficVisualModel();void configure(std::uint64_t,psx::motion::InstanceId);
 TrafficVisualAdmission event(const TrafficVisualEvent&,std::int64_t) noexcept;
 void tick(std::int64_t) noexcept;bool next(visual_wire::Frame&,std::int64_t) noexcept;
 const TrafficVisualView& view() const noexcept;const State& checkpoint() const noexcept;
 bool restore(const State&,std::int64_t) noexcept;
private:
 std::unique_ptr<State> state_;
 bool fresh(std::int64_t) const noexcept;bool command(visual_wire::Action,std::int64_t) noexcept;
 void release(visual_wire::Reason,std::int64_t) noexcept;void lose(visual_wire::Reason,std::uint32_t,std::int64_t) noexcept;
 const visual_wire::Claim* issued(const visual_wire::Report&) const noexcept;
 const visual_wire::Bundle* issued(std::uint64_t,std::uint64_t,std::int64_t) const noexcept;
 TrafficVisualAdmission receive(const TrafficVisualEvent&,std::int64_t) noexcept;
};
psx::motion::InstanceId traffic_visual_owner();
std::wstring traffic_visual_plugin_pipe_name(ProcessIdentity);
class TrafficVisualService {
public:
 TrafficVisualService();~TrafficVisualService();void start(std::uint64_t,psx::motion::InstanceId);void associate(ProcessIdentity);
 void request_stop() noexcept;void join();bool done() const noexcept;
 bool pop(TrafficVisualEvent&);bool send(const visual_wire::Frame&);void fault(std::uint32_t) noexcept;
 std::uintptr_t ready_event() const noexcept;std::uint64_t dropped() const noexcept;std::uint64_t coalesced() const noexcept;
 void fixture_name(std::wstring); // Authored private endpoint before start, never a runtime setting.
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
