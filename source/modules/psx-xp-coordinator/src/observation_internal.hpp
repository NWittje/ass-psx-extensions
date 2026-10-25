#pragma once
#include "coordinator/observation_capture.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/diagnostics.hpp"
#include "coordinator/timing_samples.hpp"
#include <deque>

namespace coordinator {
struct ObservationController::Impl {
    explicit Impl(EventLog& l,psx::diagnostics::DiagnosticsHub* hub):log(l),diagnostics(hub),stop_owner(make_stop_event()),stop(stop_owner.get()),published(make_publication_event()),bridge_ready(make_stop_event()),capture({},hub) { view.latest=model.latest();audio.configure(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());terrain.configure(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());aircraft.configure(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());environment.configure(static_cast<std::uint64_t>(qpc_frequency()),exporter_service.owner());traffic.configure(static_cast<std::uint64_t>(qpc_frequency()));traffic_visual.configure(static_cast<std::uint64_t>(qpc_frequency()),traffic_visual_owner_id); }
    EventLog& log;
    // Set before Connect in authored fixtures; production always uses the
    // established Main connection's reverse tuple and authenticated birth.
    bool (*process_lookup)(std::uintptr_t,ProcessIdentity&,std::uint32_t&) {main_server_process};
    psx::diagnostics::DiagnosticsHub* diagnostics{};
    WinHandle stop_owner;
    HANDLE stop;
    WinHandle published;
    WinHandle bridge_ready;
    WinHandle acquisition_ready{make_publication_event()}; // UI/bridge controls, not publication feedback
    std::wstring test_bridge_name; // private fixture seam; no runtime setting
    TimingSamples receipt_to_model,model_to_publication,producer_interval,contiguous_source_gap,lifecycle_interval;
    std::int64_t complete_model_qpc{};
    std::uint64_t complete_model_revision{},timing_frame{},timing_sequence{},timing_epoch{};
    std::int64_t timing_sample_qpc{};
    std::uint64_t timing_lifecycle_changes{},timing_frame_gaps{},timing_sequence_gaps{};
    mutable std::mutex mutex;
    ObservationSnapshot view;
    std::array<psx::bridge::ViewRequest,16> view_requests{};
    std::size_t view_request_head{},view_request_size{};
    std::uint64_t view_operation{};
    psx::scenery::Server coverage_server;
    bool coverage_supported{},coverage_pending{},coverage_sent{},coverage_ready{};
    std::uint64_t coverage_operation{};
    psx::bridge::CoverageRequest coverage_request;
    psx::bridge::CoverageReply coverage_reply;
    psx::scenery::Result check_coverage(const psx::scenery::Query&,std::uint32_t) noexcept;
    ObservationSession model;
    ObservationCapture capture;
    MotionControl motion;
    ExporterModel exporter;
    ExporterService exporter_service;
    AudioModel audio;
    TerrainModel terrain;
    TerrainService terrain_service;
    TerrainEvidence terrain_capture;
    EnvironmentModel environment;
    EnvironmentService environment_service;
    EnvironmentEvidence environment_capture;
    EnvironmentDiagnosticTrace environment_trace;
    EnvironmentTimeTransitions environment_time_transitions;
    ReadinessModel readiness;
    ReadinessService readiness_service;
    ReadinessEvidence readiness_capture;
    std::uint64_t readiness_attachment{},readiness_dropped_seen{},readiness_journal_revision{};
    std::int64_t readiness_assertion_qpc{},readiness_publish_qpc{};
    void drain_readiness();
    void begin_readiness(std::int64_t);
    void end_readiness(std::int64_t);
    TrafficModel traffic;
    TrafficService traffic_service;
    TrafficEvidence traffic_capture;
    TrafficVisualModel traffic_visual;
    TrafficVisualService traffic_visual_service;
    NonNormalService non_normal;
    void drain_non_normal();
    psx::motion::InstanceId traffic_visual_owner_id{traffic_visual_owner()};
    std::array<TrafficVisualEvent,32> traffic_visual_intents{};
    std::size_t traffic_visual_intent_head{},traffic_visual_intent_size{};
    std::uint64_t traffic_visual_intent_overflows{},traffic_visual_intent_overflows_seen{},traffic_visual_dropped_seen{},traffic_visual_attachment{},traffic_visual_capture_generation{1};
    bool traffic_visual_wanted{},traffic_ownship_policy_committed{},traffic_visual_capture_enabled{};
    std::array<std::uint64_t,12> traffic_visual_source_key{};
    void queue_traffic_visual(TrafficVisualEventKind,psx::motion::ReconstructionSettings,bool=false);
    void traffic_visual_input(const TrafficVisualEvent&,std::int64_t);
    void drain_traffic_visual(std::int64_t);
    void traffic_visual_capture_intent(bool,std::int64_t);
    void traffic_visual_capture_assertion(std::int64_t,bool=false);
    std::int64_t traffic_visual_assertion_qpc{};
    std::array<std::uint64_t,18> traffic_visual_assertion_key{};
    std::uint64_t traffic_dropped_seen{},traffic_capture_revision{};
    std::int64_t traffic_publish_qpc{},traffic_journal_qpc{};
    std::array<std::uint64_t,9> traffic_journal_key{};
    void drain_traffic();
    void associate_traffic(ProcessIdentity,std::int64_t);
    void begin_traffic(std::int64_t);
    void finish_traffic_capture(std::int64_t);
    void end_traffic(std::int64_t);
    std::uint64_t environment_attachment{},environment_dropped_seen{},environment_overflows_seen{},environment_capture_generation{1};
    bool environment_capture_enabled{};
    std::int64_t environment_capture_start{},environment_admission_trace_qpc{};
    std::array<bool,2> environment_capture_pending{};
    void drain_environment();
    void environment_metrics(const EnvironmentView&,std::int64_t);
    AircraftModel aircraft;
    AircraftService aircraft_service;
    AircraftEvidence aircraft_capture;
    std::uint64_t aircraft_attachment{},aircraft_dropped_seen{},aircraft_capture_generation{1},aircraft_public_ordinal{};
    bool aircraft_capture_enabled{};
    std::int64_t (*aircraft_drain_clock)() noexcept{qpc_now}; // deterministic ingress-boundary qualification seam
    std::uint64_t aircraft_overflows_seen{};
    std::int64_t aircraft_capture_start{};
    std::array<bool,2> aircraft_capture_pending{};
    void drain_aircraft();
    void aircraft_public(const LineResult&,std::string_view,std::int64_t);
    std::uint64_t terrain_attachment{};
    std::uint64_t terrain_dropped_seen{};
    std::int64_t (*terrain_drain_clock)() noexcept{qpc_now}; // deterministic ingress-boundary qualification seam
    void drain_terrain();
    struct AudioEvidence {std::uint8_t kind{};std::int64_t time{};AudioInput input{};psx::bridge::Frame frame{};psx::audio::Lease lease{};};
    std::array<AudioEvidence,512> audio_evidence{};std::size_t audio_evidence_head{},audio_evidence_size{};
    std::uint64_t audio_evidence_lost{},audio_evidence_lost_seen{},audio_dropped_seen{},audio_reported_revision{};
    void audio_record(const AudioEvidence&);
    bool apply_audio(const AudioInput&);
    void audio_exporter(const ExporterEvent&);
    void capture_audio_evidence();
    bool audio_frame(psx::bridge::Frame&,std::int64_t);
    void audio_tick(std::int64_t);
    struct ExportEvidence {bool input{};ExporterEvent event{};std::uint64_t configuration{};std::int64_t time{};bool public_ready{};psx::motion::SourcePreference preference{};psx::motion::ReconstructionSettings settings{};bool changed{};psx::bridge::SourceSelection selection{};};
    std::array<ExportEvidence,256> exporter_evidence{};
    std::size_t exporter_evidence_head{},exporter_evidence_size{};
    std::uint64_t exporter_evidence_lost{},exporter_evidence_lost_seen{},exporter_dropped_seen{},captured_complete_revision{};
    std::array<psx::bridge::SourceSelection,32> selections{};
    std::size_t selection_head{},selection_size{};
    psx::motion::SourcePreference committed_preference{psx::motion::SourcePreference::automatic};
    psx::motion::ReconstructionSettings committed_settings{};
    std::uint64_t proposal_operation{};
    bool exporter_policy_adopted{};
    // An explicit Stop/Disconnect wins over a plugin's retained Follow status,
    // including when motion has no identity yet and cannot enqueue a Stop.
    bool follow_resume_blocked{};
    bool accept_motion_status(const psx::bridge::MotionStatus&,std::int64_t);
    void retire_plugin_bridge(std::uint32_t,bool);
    void exporter_record(const ExportEvidence&);
    void select_exporter(std::int64_t now);
    void drain_exporter();
    void capture_exporter_evidence();
    void settle_exporter(const psx::bridge::MotionAck&);
    PauseSourceState last_pause; // acquisition-owned diagnostic checkpoint
    std::uint64_t motion_overflows_reported{}; // acquisition-owned lifetime checkpoint
    ObservationEndpoint endpoint;
    std::thread io_thread,bridge_thread;
    std::atomic<bool> io_done{true},bridge_done{true};
    std::wstring capture_path;
    bool capture_stop{},capture_preparing{};
    std::atomic<std::uint32_t> capture_evidence_prepared{}; // UI preparation can overlap acquisition teardown.
    bool capture_evidence_started{}; // Acquisition-owned retirement after primary I/O failure.
    void finish_capture_evidence(std::int64_t) noexcept;
    static CaptureEvidenceCompletion capture_evidence_completion(void*) noexcept;
    std::array<psx::bridge::SessionStatus,32> controls{};
    std::size_t control_head{},control_size{};
    std::uint64_t reset_peer{};
    std::array<std::uint64_t,psx::bridge::source_slots> bridge_seen{};
    std::uint64_t bridge_window_seen{},captured_window_revision{},captured_window_session_revision{};
    std::uint64_t bridge_complete_seen{};
    std::array<std::uint64_t,512> decode_samples{};
    std::size_t decode_count{},decode_index{};
    struct DiagnosticEnvelope {psx::bridge::DiagnosticBatch batch;std::int64_t received{};};
    std::array<DiagnosticEnvelope,32> diagnostic_queue{};
    std::size_t diagnostic_head{},diagnostic_size{};
    struct Rejections {psx::diagnostics::Context context;std::uint64_t count{},first{},last{};SourceStream stream{};};
    std::array<Rejections,psx::bridge::source_slots*16> rejections{};
    void reject(const LineResult&,SourceStream,std::int64_t);
    void flush_rejections();
    void drain_motion();
    void motion_boundary(const LineResult&,std::int64_t);
    void observe_pause_state();
    void io_event(psx::diagnostics::Code code,psx::diagnostics::Component component,psx::diagnostics::Severity severity=psx::diagnostics::Severity::info,
        psx::diagnostics::ErrorDomain domain=psx::diagnostics::ErrorDomain::none,std::uint32_t error=0,std::string_view text={},std::array<std::uint64_t,6> values={},const psx::diagnostics::Context* context=nullptr);
    void run_io();
    void run_bridge();
    void transition(SessionEvent e,std::int64_t time) {model.event(e,time);capture.event(e,time);if(e==SessionEvent::main_up)motion_boundary({},time);observe_pause_state();publish();}
    void publish();
    void publish_observation();
};
}
