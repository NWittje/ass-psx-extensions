#pragma once
#include "types.hpp"
#include <memory>
#include <string>
#include <vector>

namespace psx::diagnostics {
struct Config {
    bool enabled{true}, crash_dumps{false};
    // Defaults preserve existing consumers; filtering is independent of disk persistence.
    bool events_enabled{true};
    Severity minimum_severity{Severity::debug};
    std::uint32_t segment_bytes{8*1024*1024}, retained_segments{8}, flush_ms{1000}, repeat_ms{10000}, metrics_ms{1000}, status_ms{250}, delayed_ms{1000}, gap_ms{100};
    bool valid{true};
};
struct Options {
    std::wstring directory; // component-wide quota, never a per-run quota
    std::wstring config_path;
    Component component{Component::application};
    Config config{};
    std::string build;
};
struct JournalStatus {
    bool running{}, persistence{}, degraded{}, complete{};
    std::uint64_t run_id{}, accepted{}, lost{}, lost_errors{}, persisted{}, persistence_lost{}, retained_bytes{}, repeated{};
    std::uint32_t last_error{}, persistence_error{}; // general event vs current storage failure
    std::array<char,160> last_failure{};
};
struct SealResult { bool complete{}; std::uint64_t ticket{}; std::vector<std::wstring> files; };
struct RecentBatch { std::uint64_t cursor{}; bool overwritten{}; std::uint16_t count{}; std::array<Event,8> events{}; };
class DiagnosticsHub;
class DiagnosticProducer {
public:
    DiagnosticProducer() noexcept=default;
    bool emit(Event event) const noexcept;
    bool emit(Code code,Severity severity=Severity::info,Context context={},ErrorDomain domain=ErrorDomain::none,std::uint32_t error=0,
              std::string_view message={},std::array<std::uint64_t,6> values={}) const noexcept;
private:
    friend class DiagnosticsHub;
    DiagnosticsHub* owner_{};std::uint16_t index_{};Component component_{};
};
// Construction is passive. Index 7 is reserved for the journal. Register each other fixed index for exactly one producer
// thread; sequential ownership transfer is permitted. Emit is allocation-free,
// wait-free SPSC submission. Never share one producer concurrently across threads.
class DiagnosticsHub {
public:
    DiagnosticsHub();~DiagnosticsHub();
    DiagnosticsHub(const DiagnosticsHub&)=delete;DiagnosticsHub& operator=(const DiagnosticsHub&)=delete;
    bool start(const Options&);
    void set_event_filter(bool enabled, Severity minimum = Severity::info) noexcept;
    void stop() noexcept; // retires file I/O and joins; no detached DLL code
    DiagnosticProducer producer(std::uint16_t index,Component component) noexcept;
    bool emit(std::uint16_t index,Event event) noexcept;
    bool emit_stamped(std::uint16_t index,Event& event) noexcept; // same submission, returns assigned provenance even if queue is full
    JournalStatus status() const;
    std::vector<Event> recent() const;
    RecentBatch recent_since(std::uint64_t cursor) const; // short lock; off SDK, fixed storage
    void count_lost(std::uint16_t index,bool error=false,std::uint64_t count=1) noexcept; // atomic, any thread; no SPSC write
    SealResult seal(std::uint32_t timeout_ms=2000); // background callers only
    void release_seal(std::uint64_t ticket) noexcept;
    std::uint64_t run_id() const noexcept;
    const Options& options() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
std::int64_t qpc_now() noexcept;
std::uint64_t qpc_frequency() noexcept;
std::uint64_t new_identity() noexcept;
std::wstring data_directory();
Options default_options(Component component); // reads INI, never writes it
std::string event_json(const Event&); // formatting belongs off SDK/transport
} // namespace psx::diagnostics
