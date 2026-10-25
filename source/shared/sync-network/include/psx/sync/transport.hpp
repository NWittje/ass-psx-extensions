#pragma once
#include <psx/sync/clock.hpp>
#include <psx/sync/protocol.hpp>
#include <memory>
#include <string>
namespace psx::sync {
struct Options {
 Role role{Role::coordinator};std::string host;
 std::uint16_t receive_port{coordinator_port},destination_port{plugin_port};
 std::wstring dll_path;
 double fixture_loss_percent{};
};
struct LinkStatus {
 bool running{},connected{},clock_valid{};std::uint32_t error{};
 std::array<char,192> failure{};
 Identity local,peer;std::uint64_t connection{},generation{},local_frequency{},remote_frequency{};
 std::int64_t last_received_qpc{};double clock_uncertainty_ms{};
 std::array<char,16> remote_address{};std::uint16_t remote_receive_port{};
 std::uint64_t queued{},sent{},received{},replaced{},expired{},rejected{},reconnects{},native_errors{},capture_lost{};
 std::uint64_t native_objects_sent{},native_objects_new{},native_objects_complete{},native_objects_aborted{};
 std::array<std::uint64_t,8> service_generations{};
 std::array<std::uint32_t,8> local_services{},remote_services{};std::uint32_t ready_services{};
 double native_control_rate_bps{};
 // Worker diagnostic values only; no wire/Capture checkpoint layout change.
 std::array<double,8> native_lane_rate_bps{};
 bool fixture_fixed_state_pacing{};
};
struct Received {
 Header header;std::array<std::uint8_t,max_payload> payload{};std::size_t size{};
 std::int64_t received_qpc{};Projection source_time;bool transmitted{};
};
struct LinkContext {LinkStatus status;ClockMap clock;};
// All public transfer functions belong to workers. Native and SDK callbacks only
// use their existing fixed handoffs. No NORM API is called by the caller.
class Endpoint {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 Endpoint();~Endpoint();Endpoint(const Endpoint&)=delete;
 bool start(const Options&);void stop()noexcept;
 bool send(Family,std::span<const std::uint8_t>,std::uint64_t expected_connection=0,std::uint64_t expected_service=0);
 bool receive(Family,Received&,LinkContext* context=nullptr,std::uint64_t expected_connection=0,std::uint64_t expected_service=0);
 struct Binding {std::uint64_t connection{},generation{};};
 Binding open_service(Family);
 void close_service(Family,Binding)noexcept;
 bool evidence(Received&);void capture(bool)noexcept;
 void fixture_loss(double percent)noexcept; // Authored fault seam; no product setting.
 void fixture_command_stall(bool)noexcept; // Suppress native command completion in authored tests.
 void fixture_state_stall(bool)noexcept; // Hold replaceable DATA only; link/control still progress.
 void fixture_completion_stall(bool)noexcept; // Withhold DATA completion confirmation in authored tests.
 void fixture_fixed_state_pacing(bool)noexcept; // Structural fixtures only; default state congestion control remains adaptive.
 LinkStatus status()const;
 LinkContext context()const;
 bool connected_fast()const noexcept;
 Projection remote_time(std::int64_t)const;Projection local_time(std::int64_t)const;
 ClockMap clock_map()const;
 std::uintptr_t ready_event()const noexcept;
};
std::int64_t native_qpc()noexcept;std::uint64_t native_frequency()noexcept;
std::uint64_t clock_margin_ticks()noexcept;
}
