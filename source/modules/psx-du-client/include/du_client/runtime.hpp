#pragma once
#include <psx/display/transport.hpp>
#include <psx/display/diagnostics.hpp>
#include <psx/display/tcp.hpp>
#include "trim.hpp"
#include "failure_evidence.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <psx/display/replay.hpp>
#include <thread>
#include <windows.h>
namespace du_client
{
using Source=psx::du::transport::Source;
std::vector<Source> discover(unsigned unit = 0);
uint64_t qpc();
uint64_t frequency();
struct View
{
    std::shared_ptr<const psx::du::Frame> frame;
    uint32_t source_pid{};
    uint64_t source_creation{};
    std::shared_ptr<const std::string> identity;
    std::wstring status{L"Discovering local grabber…"};
    uint64_t accepted{}, raw_replaced{}, decoded_replaced{}, invalid{}, connection_epoch{}, last_received{},
        last_decoded{}, source_lost{}, frame_received{}, replay_rejected{},cpu_budget_rejected{};
    bool active{true}, connected{}, stopped{}, same_clock{true}, source_ready{};
    psx::norm::Correlation correlation;
    uint64_t validated{};
    uint64_t network_attempts{}, network_failures{}, protocol_timeouts{};
    unsigned connection{};int send_buffer{},receive_buffer{};
    std::array<char,16> local_ipv4{},remote_ipv4{};
    uint64_t delivery_warnings{}, diagnostic_lost{};
    double delivery_max_ms{};
    bool diagnostics_failed{};
    psx::norm::NativeSnapshot native;
    FailureSnapshot failures;
    uint64_t drawn{};
    bool diagnostics_enabled{};
};
class Runtime
{
    TrimReceiver trim_;
    std::atomic<bool> trim_enabled_{};
    std::thread reader_, decoder_, health_;
    std::atomic<bool> quit_{}, enabled_{true}, reconnect_{};
    std::atomic<bool> reader_finished_{},decoder_finished_{};
    psx::norm::NativeMonitor native_monitor_;
    std::mutex mutex_, queue_mutex_;
    std::condition_variable ready_;
    View view_;
    FailureEvidence failures_;
    FailureRecord failure_record(FailureStage,uint64_t,uint64_t) const noexcept;
    uint64_t acknowledged_{};
    std::wstring selected_;
    std::array<std::vector<uint8_t>, 3> slots_;
    int pending_{-1}, working_{-1};
    int assembling_{-1};
    bool decoder_done_{};
    std::array<uint64_t, 3> slot_epochs_{}, slot_generations_{}, slot_sequences_{}, slot_received_{};
    std::atomic<unsigned> unit_{};
    std::array<unsigned, 3> slot_units_{},slot_connections_{};
    std::array<bool,3> slot_same_clock_{};
    std::array<psx::norm::Correlation,3> slot_correlations_;
    std::shared_ptr<psx::du::transport::Trace> trace_owner_;
    std::atomic<psx::du::transport::Trace*> trace_{};
    HANDLE control_{};
    HANDLE health_stop_{},health_wake_{};
    std::atomic<bool> health_enabled_{};
    std::atomic<uint64_t> health_generation_{};
    unsigned decode_delay_ms_{};
    std::filesystem::path health_output_;
    bool network_{};
    std::string host_{"127.0.0.1"};
    std::wstring adapter_guid_;
    uint16_t tcp_port_{47480};
    unsigned reconnect_backoff_{250};
    psx::du::transport::tcp::Disposition publish(std::span<const uint8_t>,const psx::du::transport::Origin&,uint64_t,uint64_t,uint64_t,bool=true,const psx::norm::Correlation& = {},unsigned connection=0);
    void reader();
    void decoder();
    void health();
    void status(std::wstring, bool connected = false);

  public:
    void diagnostics(bool enabled) noexcept;
    void select_trim(bool enabled);
    TrimView trim_view() { return trim_.view(); }
    explicit Runtime(bool enabled = true, unsigned decode_delay_ms = 0, unsigned unit = 0, bool instrument_prepare_failure = false, std::filesystem::path health_output = {});
    ~Runtime();
    void acknowledge(uint64_t epoch, uint64_t sequence);
    void rejected(uint64_t epoch, uint64_t sequence, uint32_t error=uint32_t(E_FAIL), std::wstring_view detail=L"ASS replay rejected");
    void failure(FailureStage,uint64_t epoch,uint64_t sequence,uint32_t error,std::wstring_view detail,uint64_t count=1,const psx::du::transport::Origin* source=nullptr);
    void recovered(FailureStage,uint64_t epoch,uint64_t sequence,bool drawn=false);
    void connect(std::wstring pipe = {});
    void select_unit(unsigned);
    void configure_source(bool network,std::string host,std::wstring adapter_guid,uint16_t port=47480);
    void stop();
    void reconnect();
    void shutdown();
    void join();
    void observe(std::shared_ptr<psx::du::transport::Trace> trace){std::lock_guard lock(mutex_);if(trace_owner_)throw std::runtime_error("Attach client trace once");trace_owner_=std::move(trace);trace_.store(trace_owner_.get());}
    View view(bool adopt = false);
};
} // namespace du_client
