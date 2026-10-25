#pragma once
#include <psx/aircraft/state.hpp>
#include <psx/motion/motion.hpp>
#include <psx/motion/reconstruction.hpp>
#include <psx/motion/full_pose.hpp>
#include <psx/diagnostics/types.hpp>
#include <psx/audio/audio.hpp>
#include <psx/scenery/protocol.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::bridge {
inline constexpr std::uint16_t protocol_version = 6;
inline constexpr std::size_t header_size = 16;
inline constexpr std::size_t max_payload = 4096;
inline constexpr std::size_t max_frame = header_size + max_payload;
inline constexpr std::size_t source_slots = static_cast<std::size_t>(aircraft::SourceRecord::synthetic);
inline constexpr char pipe_prefix[] = "\\\\.\\pipe\\psx-ios.observation.v1."; // append current user SID, not PID
enum class Kind : std::uint16_t { hello=1, session=2, observation=3, plugin_status=4, diagnostic_events=5, motion_request=6, motion_ack=7, motion_status=8, motion_window=9, source_selection=10, complete_pose_window=11, audio_state=12, audio_transition=13, audio_control=14, audio_status=15, view_request=16, view_reply=17, coverage_request=18, coverage_reply=19 };
enum class Role : std::uint8_t { coordinator=1, plugin=2 };
// Optional explicit RPC extension: existing wire6 messages/layouts remain unchanged.
enum class ViewAction : std::uint8_t { query=0, release=1, enforce=2 };
enum class ViewState : std::uint8_t { inactive=0, released=1, pending=2, enforcing=3, unavailable=4 };
enum class ViewReason : std::uint8_t { none=0, identity=1, expired=2, binding=3 };
struct ViewRequest {
    std::uint64_t connection_id{},operation_id{},plugin_load_id{},enable_id{};
    std::int64_t requested_qpc{};
    ViewAction action{ViewAction::query};
    bool operator==(const ViewRequest& other) const noexcept {
        return connection_id==other.connection_id&&operation_id==other.operation_id&&plugin_load_id==other.plugin_load_id&&
            enable_id==other.enable_id&&requested_qpc==other.requested_qpc&&action==other.action;
    }
    bool operator!=(const ViewRequest& other) const noexcept {return !(*this==other);}
};
struct ViewReply {
    std::uint64_t connection_id{},operation_id{},plugin_load_id{},enable_id{};
    std::int64_t generated_qpc{};
    ViewState state{ViewState::inactive};ViewReason reason{ViewReason::none};
    bool selected{};std::uint32_t observed_view{}; // Native opaque enum, not a pixel/overlay assertion.
};
// PXC1 is optional and independently qualified by the cov1 Hello token.
struct CoverageRequest {
    std::uint64_t connection_id{},operation_id{},plugin_load_id{},enable_id{};
    std::int64_t deadline_qpc{};
    scenery::Query query;
    bool operator==(const CoverageRequest&) const noexcept=default;
};
struct CoverageReply {CoverageRequest request;scenery::Result result;};
bool encode(const CoverageRequest&,std::uint32_t,struct Frame&) noexcept;
bool encode(const CoverageReply&,std::uint32_t,struct Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,CoverageRequest&) noexcept;
bool decode(const std::uint8_t*,std::size_t,CoverageReply&) noexcept;

enum SessionFlag : std::uint32_t {
    // version_qualified is informational research provenance, never a gate.
    wanted=1, main_connected=2, synchronized=4, version_qualified=8,
    boost_connected=16, boost_available=32
};
enum PluginFlag : std::uint32_t { enabled=1, bridge_connected=2, observation_ready=4 };
struct Header { Kind kind{Kind::hello}; std::uint32_t payload_size{}; std::uint32_t sequence{}; std::uint16_t version{protocol_version}; };
struct Hello {
    Role role{Role::coordinator};
    std::uint32_t aircraft_revision{aircraft::contract_revision};
    std::uint64_t instance_id{}; // unique process/worker connection identity, nonzero
    std::uint64_t qpc_frequency{};
    std::array<char,64> build{};
};
struct SessionStatus {
    std::uint64_t connection_id{}; // coordinator Hello.instance_id for this pipe connection
    std::uint64_t revision{}; // strictly increasing when semantic session state changes
    std::uint64_t session_epoch{};
    std::uint64_t configuration_epoch{};
    std::uint64_t boost_generation{};
    std::int64_t published_qpc{};
    std::uint32_t flags{};
    aircraft::EngineFamily family{aircraft::EngineFamily::unknown};
    std::array<char,32> psx_version{};
    // Within one PSX session; zero is observation-only/legacy capture.
    std::uint64_t motion_epoch{}, motion_sequence_floor{};
    motion::Boundary motion_boundary{motion::Boundary::none};
};
struct ObservationMessage {
    std::uint64_t connection_id{};
    std::uint64_t session_revision{};
    std::uint64_t boost_generation{};
    std::int64_t decoded_qpc{};
    std::int64_t published_qpc{};
    aircraft::Observation observation{};
};
struct Distribution {
    std::uint64_t count{}, minimum{}, maximum{}, p50{}, p95{}, p99{}; // QPC ticks; zero count = unavailable
};
struct PluginStatus {
    std::uint64_t connection_id{};
    std::uint64_t session_revision{};
    std::uint64_t session_epoch{};
    std::uint64_t configuration_epoch{};
    std::uint64_t boost_generation{};
    std::uint64_t aircraft_generation{}, scenery_generation{}, callback_count{};
    std::int64_t callback_qpc{};
    std::uint32_t flags{};
    std::uint64_t rejected{}, coalesced{}, diagnostics_dropped{};
    std::array<std::uint64_t,source_slots> consumed_sequence{};
    std::array<std::int64_t,source_slots> received_qpc{};
    Distribution receive_to_callback{}, callback_duration{}, frame_interval{};
    std::array<char,256> aircraft{};
    std::uint64_t plugin_load_id{}, enable_id{};
    std::int64_t generated_qpc{};
    std::uint32_t diagnostic_validity{};
    std::uint64_t events_lost{}, persistence_lost{}, duration_completed_callback{};
    diagnostics::Maximum receive_to_callback_max{}, callback_duration_max{}, frame_interval_max{};
};
enum DiagnosticValidity : std::uint32_t { counter_baseline_valid=1, callback_snapshot_valid=2 };
struct DiagnosticBatch {
    std::uint64_t connection_id{};
    std::uint16_t count{};
    std::array<diagnostics::Event,8> events{};
};
struct MotionRequest {
    std::uint64_t connection_id{}, operation_id{}, plugin_load_id{}, enable_id{}, aircraft_generation{};
    std::uint64_t session_epoch{}, motion_epoch{}; // Follow preconditions; Stop is independent of source health.
    std::int64_t requested_qpc{};
    motion::Action action{motion::Action::follow};
    motion::ReconstructionSettings reconstruction{};
    motion::SourcePreference source_preference{motion::SourcePreference::automatic};
    std::uint64_t expected_selection_epoch{1};
};
struct MotionAck {
    std::uint64_t connection_id{}, operation_id{}, plugin_load_id{}, enable_id{}, aircraft_generation{};
    std::int64_t generated_qpc{};
    motion::AckResult result{motion::AckResult::rejected};
    motion::Reason reason{motion::Reason::none};
};
struct MotionStatus {
    std::uint64_t connection_id{}, plugin_load_id{}, enable_id{}, aircraft_generation{}, scenery_generation{};
    std::uint64_t revision{}, operation_id{}, applied_count{}, discontinuity_generation{};
    std::int64_t generated_qpc{}, applied_qpc{};
    motion::Phase phase{motion::Phase::observing};
    motion::Reason reason{motion::Reason::none};
    std::uint32_t flags{motion::approximate_pitch}, reference_revision{};
    motion::PresentationPose presentation{};
    motion::LocalPose written{}, readback{};
    double position_error_m{}, attitude_error_deg{};
    std::uint64_t control_dropped{}, trace_dropped{};
    Distribution pre_callback_duration{}, post_callback_duration{}, boost_receive_to_apply{}, pitch_age{}, frame_interval{};
    motion::ReconstructionSettings reconstruction{};
    motion::ReconstructionTrace reconstruction_trace{};
    std::uint16_t boost_history_count{},pitch_history_count{};
    // Current Follow counters. Holds count frames; hold transitions count episodes.
    std::uint64_t bracketed_count{},oldest_hold_count{},newest_hold_count{},no_pitch_count{},history_reset_count{},hold_transitions{};
    Distribution output_age{},hold_duration{};
    motion::PoseSource selected_source{motion::PoseSource::public_main_boost};
    std::uint64_t selection_epoch{1};
    motion::PoseDomain selected_domain{};
    motion::SourcePreference source_preference{motion::SourcePreference::automatic};
    std::uint16_t exporter_history_count{};
    Distribution exporter_sample_to_apply{},sampling_width{};
    double handoff_position_m{},handoff_attitude_deg{};
};
struct MotionWindow {
    std::uint64_t connection_id{},session_revision{};
    std::int64_t published_qpc{};
    motion::SampleWindow samples{};
    std::uint64_t selection_epoch{1};
};
// Required ordered control. It precedes every window in its selection epoch.
struct SourceSelection {
    std::uint64_t connection_id{},revision{},selection_epoch{};
    std::int64_t selected_qpc{};
    motion::PoseSource source{motion::PoseSource::public_main_boost};
    motion::SelectionReason reason{motion::SelectionReason::startup};
    motion::PoseDomain domain{};
};
struct CompletePoseWindow {
    std::uint64_t connection_id{},selection_epoch{};
    std::int64_t published_qpc{};
    motion::CompletePoseWindow samples{};
};
inline motion::HistoryDomain history_domain(const SessionStatus& s) noexcept {return {s.session_epoch,s.configuration_epoch,s.boost_generation,s.motion_epoch,s.motion_sequence_floor};}
// Semantic checks against the current control envelope, off the SDK thread.
bool matches(const MotionWindow&,const SessionStatus&) noexcept;
bool matches(const MotionWindow&,const SessionStatus&,const SourceSelection&) noexcept;
bool matches(const CompletePoseWindow&,const SourceSelection&) noexcept;
using AudioState=audio::State;
using AudioTransition=audio::Transition;
using AudioControl=audio::Control;
using AudioStatus=audio::Status;
struct Frame { std::array<std::uint8_t,max_frame> bytes{}; std::size_t size{}; };
bool read_header(const std::uint8_t* data, std::size_t size, Header& out) noexcept;
// Offline capture only. No live downgrade or permissive unknown-version parse.
bool read_capture_header(const std::uint8_t* data, std::size_t size, Header& out) noexcept;
bool decode_capture_plugin(const std::uint8_t* payload, std::size_t size, std::uint16_t version, PluginStatus&) noexcept;
bool decode_capture_session(const std::uint8_t* payload, std::size_t size, std::uint16_t version, SessionStatus&) noexcept;
bool decode_capture_motion_request(const std::uint8_t*,std::size_t,std::uint16_t,MotionRequest&) noexcept;
bool decode_capture_motion_status(const std::uint8_t*,std::size_t,std::uint16_t,MotionStatus&) noexcept;
bool decode_capture_motion_window(const std::uint8_t*,std::size_t,std::uint16_t,MotionWindow&) noexcept;
bool encode(const Hello&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const ViewRequest&,std::uint32_t,Frame&) noexcept;
bool encode(const ViewReply&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,ViewRequest&) noexcept;
bool decode(const std::uint8_t*,std::size_t,ViewReply&) noexcept;
bool encode(const SessionStatus&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const ObservationMessage&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const PluginStatus&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const DiagnosticBatch&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const MotionRequest&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const MotionAck&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const MotionStatus&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const MotionWindow&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const SourceSelection&, std::uint32_t sequence, Frame&) noexcept;
bool encode(const CompletePoseWindow&, std::uint32_t sequence, Frame&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, Hello&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, SessionStatus&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, ObservationMessage&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, PluginStatus&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, DiagnosticBatch&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, MotionRequest&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, MotionAck&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, MotionStatus&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, MotionWindow&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, SourceSelection&) noexcept;
bool decode(const std::uint8_t* payload, std::size_t size, CompletePoseWindow&) noexcept;
bool encode(const AudioState&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioState&) noexcept;
bool encode(const AudioTransition&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioTransition&) noexcept;
bool encode(const AudioControl&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioControl&) noexcept;
bool encode(const AudioStatus&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioStatus&) noexcept;
using FrameHandler = bool (*)(void*, const Header&, const std::uint8_t*, std::size_t);
// Sticky failure, no magic scanning. Handler payload lifetime ends on return.
class Framer {
public:
    bool feed(const void*, std::size_t, FrameHandler, void*) noexcept;
    void reset() noexcept { used_=0; needed_=header_size; failed_=false; }
    bool failed() const noexcept { return failed_; }
    std::size_t buffered() const noexcept { return used_; }
private:
    std::array<std::uint8_t,max_frame> buffer_{};
    std::size_t used_{}, needed_{header_size};
    bool failed_{};
};
bool supported_source(aircraft::SourceRecord) noexcept;
bool engine_source(aircraft::SourceRecord) noexcept;
// Single-owner reducer, used on each module's I/O thread. Never share this object
// concurrently; publish a copy through the module's owned mailbox/triple buffer.
class LatestState {
public:
    void reset(std::uint64_t connection_id=0) noexcept;
    bool apply(const SessionStatus&) noexcept;
    bool apply(const ObservationMessage&) noexcept;
    const SessionStatus& session() const noexcept { return session_; }
    const std::array<ObservationMessage,source_slots>& records() const noexcept { return records_; }
    const std::array<bool,source_slots>& available() const noexcept { return available_; }
private:
    SessionStatus session_{};
    std::array<ObservationMessage,source_slots> records_{};
    std::array<bool,source_slots> available_{};
};
} // namespace psx::bridge
