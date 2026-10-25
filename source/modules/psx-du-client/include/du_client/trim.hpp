#pragma once
#include <psx/display/tcp.hpp>
#include <psx/display/replay.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace du_client
{
struct TrimView
{
    std::shared_ptr<const psx::du::Frame> frame;
    psx::du::transport::Origin origin;
    psx::norm::Correlation correlation;
    unsigned connection{};
    uint64_t received{}, accepted{}, replaced{}, invalid{}, budget_rejected{}, epoch{};
    bool enabled{}, available{}, connected{}, observation_current{};
    std::wstring status;
};

// The socket/pipe producer never waits for native replay. Two 1-MiB raw slots
// plus one discard slot keep a slow decoder from blocking the main DU stream.
class TrimReceiver
{
    std::mutex mutex_;
    std::condition_variable ready_;
    std::array<std::vector<uint8_t>, 2> slots_;
    std::vector<uint8_t> discard_;
    int assembling_{-1}, pending_{-1}, working_{-1};
    std::array<uint64_t, 2> epochs_{}, received_{};
    std::array<psx::norm::Correlation,2> correlations_{};
    std::array<unsigned,2> connections_{};
    std::array<psx::du::transport::Origin, 2> origins_;
    bool discarding_{}, quit_{}, prepared_{};
    TrimView view_;
    psx::du::transport::tcp::Identity active_session_{};
    uint64_t active_generation_{};
    std::thread decoder_;
    void decode();
  public:
    explicit TrimReceiver(bool force_prepare_failure=false);
    ~TrimReceiver();
    bool enable(bool);
    void reset();
    void shutdown();
    void join();
    std::span<uint8_t> acquire(size_t);
    void abort();
    psx::du::transport::tcp::Disposition publish(const psx::du::transport::InstrumentEnvelope &, uint64_t,unsigned connection=0);
    void offer(psx::du::transport::Owned, const psx::du::transport::LocalView &);
    void status(std::wstring, bool connected, bool available = true);
    TrimView view();
};
} // namespace du_client
