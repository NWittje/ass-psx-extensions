#pragma once
#include "core.hpp"
#include <psx/diagnostics/journal.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <Windows.h>

namespace hwio {
enum class NetworkEventKind { line, boundary, ready, sent, status, unavailable, load_boundary };
struct NetworkEvent {
    NetworkEventKind kind{};
    std::uint64_t epoch{}, binding{};
    std::int64_t qpc{}, input_qpc{}, mapped_qpc{};
    int value{};
    std::array<char, 4096> text{};
};
struct TransportStatus {
    bool enabled{}, connected{}, ready{};
    std::uint64_t epoch{}, received{}, sent{}, dropped{}, retries{};
    unsigned error{};
    std::uint64_t write_stall_ms{},overflows{};
    std::string message, version;
};
// Explicit partial-write cursor. Record framing is never spliced across boundaries.
struct PendingWrite {
    Command command;
    std::size_t offset{};
    bool complete() const noexcept {
        return offset == command.line.size();
    }
    void advance(std::size_t n) noexcept {
        offset = std::min(offset + n, command.line.size());
    }
};
class Transport {
    friend struct FailureTransportProbe;
    std::thread thread_;
    std::atomic<bool> stop_{false}, desired_{true}, reset_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> queue_;
    TransportStatus status_;
    std::string host_{"127.0.0.1"};
    std::uint16_t port_{default_main_port};
    std::uint64_t epoch_{};
    std::atomic<std::uint64_t> active_epoch_{0};
    psx::diagnostics::DiagnosticProducer log_;
    HANDLE wake_event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::uint64_t cancellation_generation_{}, all_cancelled_{};
    std::map<std::uint64_t, std::uint64_t> cancelled_bindings_;
    std::map<std::string, std::uint64_t> superseded_records_;
    std::map<std::string, std::uint64_t> superseded_events_;
    std::atomic<bool> pending_{false};
    std::atomic<std::uint64_t> write_blocked_since_{0};
    std::atomic<bool> follower_allowed_{false};
    std::size_t write_limit_{4095};
    int send_buffer_{};
    std::set<std::string> interests_;
    void run();
    bool event(NetworkEventKind kind, std::string_view text = {}, std::uint64_t binding = 0,
               const Command *command = nullptr);
    void set_status(std::string message, unsigned error = 0);

  public:
    explicit Transport(std::size_t write_limit = 4095, int send_buffer = 0)
        : write_limit_(write_limit ? write_limit : 4095), send_buffer_(send_buffer) {}
    HANDLE available_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Spsc<NetworkEvent, 2048> incoming;
    std::atomic<std::uint64_t> overflow{0};
    ~Transport();
    void start(std::string host, std::uint16_t port, bool autoconnect, psx::diagnostics::DiagnosticProducer log);
    void connect(std::string host, std::uint16_t port);
    void disconnect();
    void resynchronize();
    void stop();
    bool submit(Command command);
    void cancel_binding(std::uint64_t id);
    void cancel_all();
    void supersede_axes(const std::vector<RecordKey> &records);
    void supersede_records(const std::vector<RecordKey>& records);
    bool drain(unsigned timeout_ms);
    void interests(const std::vector<RecordKey> &records);
    TransportStatus status() const;
};
} // namespace hwio
