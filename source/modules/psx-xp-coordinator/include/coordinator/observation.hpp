#pragma once
#include "coordinator/status.hpp"
#include "coordinator/motion_control.hpp"
#include "coordinator/exporter.hpp"
#include "coordinator/audio.hpp"
#include "coordinator/terrain.hpp"
#include "coordinator/terrain_evidence.hpp"
#include "coordinator/aircraft_evidence.hpp"
#include "coordinator/environment_evidence.hpp"
#include "coordinator/readiness_evidence.hpp"
#include "coordinator/traffic_evidence.hpp"
#include "coordinator/non_normal.hpp"
#include <psx/bridge/protocol.hpp>
#include <psx/scenery/client.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <psx/diagnostics/journal.hpp>

namespace coordinator {
struct ObservationEndpoint {
    std::string ipv4{"127.0.0.1"};
    std::uint16_t main_port{10747}, boost_port{10749};
};
struct ChannelCounters {
    State state{State::idle};
    std::uint64_t attempts{}, bytes_in{}, bytes_out{}, lines{}, errors{}, truncated{};
    std::uint32_t retry_seconds{}, last_error{};
};
struct CutoffSource {std::uint64_t sequence{},session_epoch{},configuration_epoch{},boost_generation{};};
struct CaptureCutoff {
    bool acknowledged{};
    std::uint64_t capture_id{};
    std::int64_t owner_qpc{};
    std::uint64_t ingestion_sequence{},session_epoch{},configuration_epoch{},boost_generation{};
    std::array<CutoffSource,psx::bridge::source_slots> sources{};
    std::uint64_t motion_epoch{},motion_sequence_floor{};
    psx::motion::Boundary motion_boundary{psx::motion::Boundary::none};
};
enum class CaptureStopReason : std::uint8_t { none, cancelled, queue_limit, file_limit, file_io, setup, exporter_handoff, exporter_evidence, audio_handoff, audio_evidence, audio_transition, audio_lease, motion_evidence, network_evidence, sidecar_evidence };
const char* capture_stop_reason(CaptureStopReason) noexcept;
struct CaptureStatus {
    bool active{}, complete{}, incomplete{},finalizing{};
    std::uint64_t records{}, bytes{}, queued{}, overflows{};
    std::uint32_t error{};
    std::uint64_t capture_id{};
    CaptureCutoff cutoff;
    std::int64_t finalized_qpc{};
    CaptureStopReason stop_reason{CaptureStopReason::none}; // local diagnostic; CAP7 unchanged
};
struct MotionWindowSelection {
    bool available{};
    std::uint64_t connection_id{},session_revision{},window_revision{};
    std::uint16_t boost_count{};
    std::uint64_t first_sequence{},last_sequence{};
    std::int64_t first_received_qpc{},last_received_qpc{},published_qpc{};
};
struct ObservationSnapshot {
    std::int64_t snapshot_qpc{}; // time the facade copied this bounded view
    psx::bridge::LatestState latest;
    ChannelCounters main, boost;
    State bridge{State::idle};
    std::uint32_t bridge_last_error{},bridge_peer_process_id{};
    std::uint64_t bridge_attachments{}, bridge_rejected{}, bridge_coalesced{}, bridge_control_overflows{}, bridge_write_stalls{};
    bool plugin_available{};
    std::int64_t plugin_received_qpc{};
    std::uint64_t diagnostic_handoff_lost{};
    psx::bridge::PluginStatus plugin;
    bool scenery_view_available{},scenery_view_reply_available{};
    psx::bridge::ViewReply scenery_view;
    psx::bridge::Distribution decode_duration;
    std::uint64_t selected{}, malformed{}, unsupported{}, bootstrap_errors{};
    CaptureStatus capture;
    MotionSnapshot motion;
    psx::motion::SampleWindow motion_window;
    psx::motion::HistoryCounters history_counters;
    std::uint64_t motion_windows_published{},motion_windows_sent{},motion_windows_superseded{};
    MotionWindowSelection selected_window; // last encoded selection, not peer acknowledgement
    ExporterState exporter;
    psx::motion::CompletePoseWindow complete_window;
    psx::motion::CompleteHistoryCounters complete_counters;
    std::uint64_t complete_windows_sent{},complete_windows_superseded{};
    AudioView audio;
    TerrainView terrain;
    TerrainEvidenceStatus terrain_evidence;
    AircraftView aircraft;
    AircraftEvidenceStatus aircraft_evidence;
    EnvironmentView environment;
    EnvironmentEvidenceStatus environment_evidence;
    ReadinessView readiness;
    ReadinessEvidenceStatus readiness_evidence;
    TrafficView traffic;
    TrafficVisualView traffic_visual;
    TrafficEvidenceStatus traffic_evidence;
};
// Production endpoint is derived from TokenUser SID; no arbitrary pipe override.
std::wstring observation_pipe_name();
// Thread-safe UI/harness facade. Construction does not start workers or sockets.
// Connect enables independent 1/2/4/8-second capped retries and the bridge server.
// Disconnect/stop are asynchronous. join() only after done() on a UI thread.
class ObservationController {
public:
    explicit ObservationController(EventLog&,psx::diagnostics::DiagnosticsHub* diagnostics=nullptr);
    ~ObservationController();
    ObservationController(const ObservationController&) = delete;
    ObservationController& operator=(const ObservationController&) = delete;
    bool connect(ObservationEndpoint);
    void disconnect() noexcept;
    bool done() const noexcept;
    void join();
    ObservationSnapshot snapshot() const;
    // CREATE_NEW only. File work runs on capture worker; never acquisition owner.
    bool start_capture(std::wstring absolute_path);
    void stop_capture() noexcept;
    // Enqueue an already stamped UI event for the acquisition-owned capture.
    void record_diagnostic(const psx::diagnostics::Event&) noexcept;
    // Explicit operator action only. Zero means rejected locally; never replayed.
    std::uint64_t request_motion(psx::motion::Action,psx::motion::ReconstructionSettings settings={},psx::motion::SourcePreference preference=psx::motion::SourcePreference::automatic);
    // Automatic-start policy only: restore independent selections when a fresh
    // plugin status already confirms Follow. Never issues another placement.
    bool resume_automatic_follow_services();
    void mute_aircraft_audio(bool muted);
    // Independent explicit selection; does not require an aircraft motion profile.
    bool follow_environment(bool enabled);
    // Explicit RPC only. Zero rejects locally; no automatic activation or reconnect replay.
    psx::scenery::Result check_scenery(const psx::scenery::Query&,std::uint32_t timeout_ms=2000) noexcept; // Worker-only.
    std::uint64_t request_scenery_view(psx::bridge::ViewAction);
    bool start_non_normal();void stop_non_normal()noexcept;
    void join_non_normal();bool non_normal_done()const noexcept;
    NonNormalView non_normal_status()const;
private:
    friend struct ObservationTestAccess;
    friend struct EvidenceTestAccess;
    friend struct AircraftTestAccess;
    friend struct EnvironmentTestAccess;
    friend struct TrafficVisualTestAccess;
    friend struct NonNormalTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct ReplayResult {
    bool valid{}, complete{};
    std::uint64_t qpc_frequency{}, records{}, selected{}, malformed{};
    std::uint32_t capture_version{};
    std::uint64_t diagnostic_events{};
    std::uint64_t motion_requests{},motion_acks{},motion_statuses{},motion_boundaries{},local_cancellations{};
    MotionSnapshot motion;
    CaptureCutoff cutoff;
    std::int64_t finalized_qpc{};
    psx::bridge::LatestState latest;
    psx::motion::SampleWindow motion_window;
    psx::motion::HistoryCounters history_counters;
    psx::motion::ReconstructionSettings inspection_settings{};
    std::uint64_t motion_windows{},inspection_samples{};
    std::array<std::uint64_t,7> inspection_states{};
    std::array<psx::motion::ReconstructionResult,64> inspection_tail{};
    std::size_t inspection_tail_count{};
    std::string error;
    ExporterState exporter;
    psx::motion::CompletePoseWindow complete_window;
    std::uint64_t exporter_events{},source_selections{},complete_windows{};
    AudioView audio;std::uint64_t audio_inputs{},audio_frames{},audio_leases{};
    std::uint64_t network_packets{},network_checkpoints{};
    ReadinessReplayResult readiness;
    TrafficReplayResult traffic;
};
// Synchronous offline file replay; opens no simulator or plugin endpoint.
ReplayResult replay_capture(const std::wstring& path,psx::motion::ReconstructionSettings inspection={});
}
