#pragma once
#include "config.hpp"
#include "rates.hpp"
#include <mutex>
#include <psx/diagnostics/journal.hpp>
#include <thread>
namespace cdu_client
{
struct View
{
    bool diagnostics_enabled{};
    psx::diagnostics::JournalStatus journal{};
    psx::cdu::CduSnapshot screen{};
    psx::cdu::ReceiverStatus source{};
    psx::cdu::ReceiverStats stats{};
    psx::cdu::blackout::Received blackout{};
    psx::norm::Stats ownership{};
    uint64_t epoch{}, ack{}, accepted_ns{};
    uint64_t received_updates{};
    double received_fps{};
    bool has_screen{}, healthy{}, initialized{}, active{}, stopped{}, fatal{};
    std::array<char, 256> error{};
};
// Select a private black source without altering accepted PSX cells/blanking or effect resources.
psx::cdu::ScreenView presentation_view(const View &, psx::cdu::CduSnapshot &empty) noexcept;
class Runtime
{
    Config config_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    View view_;
    std::mutex commands_;
    Config pending_;
    uint64_t next_{}, pending_id_{};
    bool reconnect_{}, allowed_{};
    HANDLE stop_{}, command_{};
    std::thread worker_;
    std::atomic<bool> stopped_{};
    psx::diagnostics::Options diagnostics_options_;
    std::atomic<bool> diagnostics_requested_{};
    psx::diagnostics::DiagnosticsHub journal_;
    psx::diagnostics::DiagnosticProducer events_;
    void run() noexcept;
    void store(const View &);

  public:
    Runtime(Config, bool allowed, std::string profile, psx::diagnostics::Options options = {});
    ~Runtime();
    View view() const;
    uint64_t configure(Config);
    void diagnostics(bool enabled) noexcept { diagnostics_requested_=enabled;SetEvent(command_); }
    void reconnect();
    void request_stop() noexcept;
    bool stopped() const
    {
        return stopped_.load();
    }
    void join();
};
} // namespace cdu_client
