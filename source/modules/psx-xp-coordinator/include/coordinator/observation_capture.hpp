#pragma once
#include "coordinator/observation.hpp"
#include "coordinator/observation_session.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace coordinator {
enum class CaptureKind : std::uint32_t { event=1, main=2, boost=3, context=4, seed=5, plugin=6, footer=7, diagnostics=8, cutoff=9, motion_request=10, motion_ack=11, motion_status=12, motion_boundary=13, motion_window=14, exporter_event=15, exporter_input=16, source_selection=17, complete_pose_window=18,audio_input=19,audio_frame=20,audio_lease=21,network_packet=22,network_checkpoint=23 };
struct CaptureLimits { std::size_t queue_bytes{8*1024*1024}; std::uint64_t file_bytes{512ull*1024*1024}; };
struct CaptureEvidenceCompletion {bool done{true},complete{true};std::uint32_t error{};};
using CaptureEvidenceProbe=CaptureEvidenceCompletion (*)(void*) noexcept;
// Limits are injectable only for deterministic native failure tests.
class ObservationCapture {
public:
    explicit ObservationCapture(CaptureLimits limits={},psx::diagnostics::DiagnosticsHub* diagnostics=nullptr);
    ~ObservationCapture();
    // UI/test owner prepares storage and a waiting worker before acquisition's
    // bounded checkpoint copy. Does not create a capture until start().
    bool prepare(const std::wstring&);
    // Set before acquisition starts this prepared capture. The file worker polls
    // completion without holding capture/acquisition locks; context outlives join().
    bool evidence_completion(CaptureEvidenceProbe,void*) noexcept;
    bool start(const std::wstring&, const ObservationSession&, std::uint64_t frequency, std::int64_t now,const ExporterModel* exporter=nullptr,const AudioModel* audio=nullptr);
    void event(SessionEvent,std::int64_t);
    void line(SourceStream,std::string_view,std::int64_t received,std::int64_t decoded);
    void plugin(const psx::bridge::PluginStatus&,std::int64_t);
    void diagnostics(const psx::bridge::DiagnosticBatch&,std::int64_t);
    void motion(const MotionRecord&);
    void motion_status(const psx::bridge::MotionStatus&,std::int64_t);
    void boundary(const ObservationSession&,const LineResult&,std::int64_t);
    void window(const ObservationSession&,std::int64_t,std::uint64_t selection_epoch=1);
    void finish(const ObservationSession&,std::int64_t,const ExporterModel* exporter=nullptr,const AudioModel* audio=nullptr);
    void audio_input(const AudioInput&);
    void audio_frame(const psx::bridge::Frame&,std::int64_t);
    void audio_lease(const psx::audio::Lease&,std::int64_t);
    void exporter_event(const ExporterEvent&,std::uint64_t configuration);
    void exporter_input(bool public_ready,psx::motion::SourcePreference,psx::motion::ReconstructionSettings,std::int64_t);
    void selection(const psx::bridge::SourceSelection&,std::int64_t);
    void complete_window(const ExporterModel&,std::int64_t);
    void network(); // Acquisition worker drains bounded original-domain evidence.
    // Emergency/cancellation stop lacks acquisition acknowledgement and is incomplete.
    void stop(CaptureStopReason reason=CaptureStopReason::cancelled) noexcept;
    bool done() const noexcept { return done_; }
    void join();
    CaptureStatus status() const;
private:
    friend struct ObservationTestAccess;
    std::uint32_t test_seed_delay_ms_{}; // deterministic slow-writer fixture, no runtime option
    bool append(CaptureKind,std::int64_t,const std::vector<std::uint8_t>&);
    void observe_network_loss(std::uint64_t);
    void run(std::wstring,std::uint64_t,std::int64_t);
    struct Seed;
    std::unique_ptr<Seed> seed_;
    std::wstring prepared_path_;
    bool seed_ready_{},cancel_prepared_{};
    CaptureEvidenceProbe evidence_probe_{};void* evidence_context_{};
    std::uint64_t seed_records_{};
    std::mutex worker_mutex_; // UI preparation versus acquisition shutdown/join
    CaptureLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<std::uint8_t> ring_;
    std::size_t head_{}, size_{};
    std::uint64_t ordinal_{}, accepted_{};
    CaptureStatus status_;
    std::thread worker_;
    std::atomic<bool> done_{true};
    std::atomic<bool> recording_{false};
    psx::diagnostics::DiagnosticProducer file_events_;
    std::uint64_t frequency_{};
    bool remote_{};std::uint64_t network_lost_{},network_connection_{};std::int64_t network_checkpoint_{};
};
}
