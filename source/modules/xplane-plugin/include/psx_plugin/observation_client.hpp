#pragma once
#include <psx/bridge/protocol.hpp>
#include <psx/diagnostics/journal.hpp>
#include <psx/non_normal/protocol.hpp>
#include "motion_support.hpp"
#include "environment_types.hpp"
#include <memory>
#include <string>

namespace psx::xp {
struct ObservationOptions {
    // Empty selects the production SID endpoint. Nonempty is for offline mocks;
      // accepted only in the local \\.\pipe\ namespace. A qualified configured
      // coordinator may select NORM; failed local attachment never selects it.
    std::wstring endpoint;
    std::wstring scenery_root; // Captured once on the SDK thread; files belong to the coverage worker.
    diagnostics::DiagnosticsHub* diagnostics{}; // Borrowed; outlives start/stop.
    std::uint64_t plugin_load_id{}, enable_id{};
    // Deterministic offline timing seam. Production uses QPC for both endpoints.
    std::int64_t (*callback_clock)() noexcept{};
    std::uint32_t boost_timeout_ms{motion::default_boost_timeout_ms};
    // Offline complete-frame timing seam; never replaces source/transport clocks.
    std::int64_t (*receipt_clock)() noexcept{};
};
struct ObservationLifecycle {
    std::uint64_t aircraft_generation{}, scenery_generation{};
    std::array<char,256> aircraft{}; // SDK filename, not a qualified fingerprint.
};
std::wstring observation_endpoint(); // TokenUser SID; empty on failure.
std::int64_t observation_qpc() noexcept;
std::uint64_t observation_qpc_frequency() noexcept;

// Native Windows client, no SDK dependency. Lifecycle and callback/status have
// one owner (SDK thread in production). start/stop must not race callback.
class ObservationClient final {
public:
    ObservationClient() noexcept;
    ~ObservationClient();
    ObservationClient(const ObservationClient&) = delete;
    ObservationClient& operator=(const ObservationClient&) = delete;
    bool start(const ObservationOptions& = {}) noexcept;
    void stop() noexcept; // Signal, cancel outstanding pipe I/O, drain, join.
    // SDK owner only: clear consumption immediately on aircraft/scenery events.
    void invalidate_lifecycle(const ObservationLifecycle&) noexcept;
    // Fixed work; no SDK, locks, heap allocation, I/O, waits or percentile sort.
    const bridge::PluginStatus& callback(const ObservationLifecycle&) noexcept;
    const bridge::PluginStatus& status() const noexcept;
    // Last callback-owned cache; pointer valid until stop or next callback.
    const bridge::LatestState* latest() const noexcept;
    const bridge::MotionWindow* motion_window() const noexcept; // SDK-owned slot, valid until refresh/callback/stop.
    const bridge::CompletePoseWindow* complete_window() const noexcept; // Independent selected exporter history.
    void refresh() noexcept; // SDK pre callback: consume latest snapshot without diagnostic callback count.
    bool connected() const noexcept;
    non_normal::ProcessIdentity authenticated_peer() const noexcept; // Local OS evidence, no wire change.
    non_normal::ProcessIdentity selected_peer() const noexcept; // Remote identities are declared provenance.
    bool pop_motion_control(MotionControl&) noexcept;
    bool submit_motion_ack(const bridge::MotionAck&) noexcept;
    bool pop_view_request(bridge::ViewRequest&) noexcept;
    bool submit_view_reply(const bridge::ViewReply&) noexcept;
    void publish_motion(const MotionReport&) noexcept;
    void publish_environment_timing(const EnvironmentTiming&) noexcept;
    bool motion_transport_fault() const noexcept;
    void request_profile(const ProfileRequest&) noexcept;
    bool poll_profile(MotionProfile&) noexcept;
    const audio::State* audio_state() const noexcept;
    bool pop_audio_control(audio::Control&) noexcept;
    bool pop_audio_transition(audio::Transition&) noexcept;
    void publish_audio(const audio::Status&) noexcept;
    bool audio_transport_fault() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bridge::PluginStatus stopped_{};
};
} // namespace psx::xp
