#pragma once
#include <psx/display/tcp.hpp>
#include <psx/cdu/blackout.hpp>
#include "config.hpp"
#include <atomic>
#include <mutex>
#include <psx/diagnostics/journal.hpp>
#include <thread>
namespace du_host
{
struct Status
{
    bool running{}, stopping{}, fatal{}, publication_active{}, configuration_required{}, configuration_applied{},
        loopback_active{};
    unsigned multicast_bindings{},multicast_unavailable{};
    int main_socket_error{};
    std::uint64_t configuration_ack{}, generation{1}, connections{}, records{}, accepted{}, rejections{}, published{},
        coalesced{}, busy{}, owned{}, owned_peak{}, max_delay_ns{}, last_record_ns{}, last_publication_ns{};
    psx::cdu::SourceState source_state{psx::cdu::SourceState::disconnected};
    std::array<psx::cdu::CduSnapshot, 3> screens{};
    std::array<char, 256> error{};
    std::array<char, 16> address{};
    std::array<int, 3> norm_version{};
    psx::du::transport::TcpHostView du;
    psx::cdu::blackout::Status blackout;
    bool blackout_available{};
    DWORD blackout_error{};
    bool diagnostics_enabled{};
    psx::diagnostics::JournalStatus journal;
    std::shared_ptr<const psx::network::Inventory> inventory;
};
class Runtime
{
    Config config_;
    std::uint64_t origin_{psx::norm::monotonic_ns()};
    mutable SRWLOCK status_lock_ = SRWLOCK_INIT;
    Status status_;
    std::mutex command_lock_;
    Config pending_;
    std::uint64_t next_command_{}, pending_id_{};
    bool reconnect_{};
    HANDLE stop_{}, command_{};
    std::thread worker_;
    std::atomic<bool> stopped_{true};
    bool allowed_{};
    std::uint64_t accepted_{}; // Network worker only; copied through Status.
    psx::diagnostics::Options diagnostics_options_;
    std::atomic<bool> diagnostics_requested_{};
    psx::diagnostics::DiagnosticsHub journal_;
    psx::diagnostics::DiagnosticProducer events_;
    std::shared_ptr<psx::du::transport::Trace> du_trace_;
    std::filesystem::path du_source_trace_;
    std::atomic<std::shared_ptr<psx::du::transport::TcpHostPublisher>> du_runtime_;
    // Only request_stop crosses threads; the service state and pipe I/O stay on the Main worker.
    std::atomic<std::shared_ptr<psx::cdu::blackout::Service>> blackout_service_;
    void run() noexcept;
    void store(const Status &);

  public:
    Runtime(Config, psx::diagnostics::Options, bool network_allowed = true,std::filesystem::path trace_output = {});
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    Status status() const;
    std::uint64_t configure(Config);
    void diagnostics(bool enabled) noexcept { diagnostics_requested_=enabled;SetEvent(command_); }
    void reconnect();
    void request_stop() noexcept;
    bool stopped() const noexcept
    {
        return stopped_.load(std::memory_order_acquire);
    }
    void join();
    std::uint64_t origin() const noexcept
    {
        return origin_;
    }
};
psx::diagnostics::Options diagnostics_options();
} // namespace du_host
