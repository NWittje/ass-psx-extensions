#pragma once
#include <psx/exporter/protocol.hpp>
#include <psx/bridge/protocol.hpp>
#include <memory>
#include <string>
#include <vector>

namespace coordinator {
struct ProcessIdentity {std::uint32_t pid{};std::uint64_t creation_filetime{};};
inline bool operator==(ProcessIdentity a,ProcessIdentity b) noexcept{return a.pid==b.pid&&a.creation_filetime==b.creation_filetime;}
inline bool operator!=(ProcessIdentity a,ProcessIdentity b) noexcept{return !(a==b);}
bool exporter_identity_matches(ProcessIdentity,std::uint32_t client_pid,std::uint64_t frequency,const psx::exporter::Hello&) noexcept;
// Match the server half of the actual established IPv4 connection, not a port
// listener or the coordinator's client-side PID. No simulator writes.
bool main_server_process(std::uintptr_t socket,ProcessIdentity&,std::uint32_t& error);
struct ProcessAssociationRetry {
    std::uint64_t next{};
    unsigned step{};
    bool accepted{};
    bool due(std::uint64_t now) const noexcept { return !accepted && now >= next; }
    void failed(std::uint64_t now) noexcept {
        next = now + (step == 0 ? 250 : step == 1 ? 500 : 1000);
        if (step < 2) ++step;
    }
};
std::wstring exporter_pipe_name(ProcessIdentity);

enum class ExporterEventKind:std::uint8_t {association=1,hello=2,snapshot=3,health=4,unavailable=5,audio_snapshot=6,audio_ack=7};
struct ExporterEvent {
    ExporterEventKind kind{ExporterEventKind::unavailable};
    std::int64_t received_qpc{};
    ProcessIdentity process{};
    std::uint32_t client_pid{},error{};
    psx::exporter::Frame frame{}; // Original accepted PXSE bytes, including frame sequence.
};
struct ExporterState {
    ProcessIdentity process{};
    bool peer{},ready{};
    std::uint32_t error{};
    psx::exporter::Hello hello{};
    psx::exporter::Health health{};
    std::uint16_t protocol_version{psx::exporter::protocol_version};
    std::uint64_t raw{},invalid{},sequence_gaps{},frame_gaps{},handoff_dropped{},boundaries{};
    std::uint64_t last_sequence{},last_frame{};
    std::int64_t stable_since{},last_valid_qpc{},last_sample_end_qpc{},last_sampling_width{};
    psx::motion::SourcePreference preference{psx::motion::SourcePreference::automatic};
    psx::motion::ReconstructionSettings settings{};
    psx::bridge::SourceSelection selection{1,1,1};
    bool boundary_pending{};
};
struct PhysicalPoseCache {
    bool available{};
    psx::motion::InstanceId producer{};
    psx::motion::RawPose raw{};
    std::int64_t received_qpc{};
    std::uint64_t revision{};
};
bool valid_physical_pose(const psx::motion::RawPose&,std::int64_t received) noexcept;
// Pure acquisition-owner state, used unchanged by CAP6 replay. All clocks are
// the original QPC ticks; public Main lifecycle is deliberately not an input.
class ExporterModel {
public:
    explicit ExporterModel(std::uint64_t frequency=0,std::uint32_t reference_revision=psx::motion::instant_reference_revision):frequency_(frequency){state_.selection.domain.reference_revision=reference_revision;}
    void frequency(std::uint64_t value) noexcept{frequency_=value;}
    // CAP6 always stores this value in SourceSelection, including public/empty
    // domains. It is a capture convention, independent of producer lifecycle.
    std::uint32_t reference_revision() const noexcept{return state_.selection.domain.reference_revision;}
    bool event(const ExporterEvent&,std::uint64_t configuration_epoch=0) noexcept;
    void preference(psx::motion::SourcePreference p,psx::motion::ReconstructionSettings s) noexcept{state_.preference=p;state_.settings=s;}
    // Returns true only for a required ordered selection boundary.
    bool select(std::int64_t now,bool public_ready) noexcept;
    bool fresh(std::int64_t now) const noexcept;
    bool eligible(std::int64_t now) const noexcept;
    const ExporterState& state() const noexcept{return state_;}
    const psx::motion::CompletePoseHistory& history() const noexcept{return history_;}
    const PhysicalPoseCache& physical() const noexcept{return physical_;}
    bool restore(const ExporterState&,const psx::motion::CompletePoseWindow&,const psx::motion::CompleteHistoryCounters&) noexcept;
    void handoff_dropped(std::uint64_t count) noexcept{state_.handoff_dropped=count;}
private:
    void unavailable(std::uint32_t error) noexcept;
    void boundary(const psx::motion::PoseDomain&,std::uint64_t floor) noexcept;
    std::uint64_t frequency_{};
    ExporterState state_{};
    psx::motion::CompletePoseHistory history_{};
    PhysicalPoseCache physical_{};
};
// Independent cancelable native worker. Bounded handoff is drained by the
// acquisition owner BEFORE pose-window coalescing and CAP6 cutoff.
class ExporterService {
public:
    ExporterService();~ExporterService();
    ExporterService(const ExporterService&)=delete;
    ExporterService& operator=(const ExporterService&)=delete;
    bool associate(ProcessIdentity,std::uint64_t frequency);
    void stop() noexcept;
    bool pop(ExporterEvent&);
    // Borrowed manual-reset HANDLE: signaled exactly while the handoff is nonempty.
    std::uintptr_t ready_event() const noexcept;
    std::uint64_t dropped() const noexcept;
    std::uint64_t audio_dropped() const noexcept;
    std::uint64_t owner() const noexcept;
    bool lease(const psx::audio::Lease&);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
const char* pose_source_name(psx::motion::PoseSource) noexcept;
const char* selection_reason_name(psx::motion::SelectionReason) noexcept;
std::vector<std::uint8_t> exporter_checkpoint(const ExporterModel&,std::uint16_t bridge_version=psx::bridge::protocol_version);
bool restore_exporter_checkpoint(ExporterModel&,const std::uint8_t*,std::size_t);
}
