#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
namespace psx::norm
{
constexpr std::size_t max_data = 2048, receive_budget = 256 * 1024;
struct Options
{
    std::string group{"239.255.74.74"}, interface_address{"127.0.0.1"}, expected_host;
    std::uint16_t port{47474};
    std::wstring dll_path;
    double loss_percent{};
    double initial_grtt_seconds{.25};
    unsigned tx_objects{64}, tx_cache_bytes{256 * 1024};
};
struct Stats
{
    std::uint64_t queued{}, purged{}, busy{}, completed{}, wrong_host{}, oversized{}, aborted{}, errors{};
    std::size_t owned{}, owned_peak{};
};
struct Packet
{
    std::span<const std::uint8_t> bytes;
    std::array<std::uint8_t, 4> source{};
};
using Receive = void (*)(void *, const Packet &);
class Adapter
{
    friend class DuEngine;
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Adapter();
    // Loads the same required exports without creating network resources.
    static std::array<int, 3> inspect_runtime(const std::wstring &absolute_path = {});
    ~Adapter();
    Adapter(const Adapter &) = delete;
    Adapter &operator=(const Adapter &) = delete;
    void start(const Options &, bool sender);
    void stop() noexcept;
    bool send(std::span<const std::uint8_t>);
    unsigned pump(Receive = nullptr, void *user = nullptr, unsigned budget = 64);
    HANDLE event() const noexcept;
    bool started() const noexcept;
    std::uint32_t node_id() const noexcept;
    std::array<int, 3> version() const noexcept;
    const Stats &stats() const noexcept;
    static std::size_t receive_bytes() noexcept;
    static std::size_t receive_peak() noexcept;
    static std::uint64_t allocation_denied() noexcept;
};
std::uint64_t monotonic_ns() noexcept;
void random_bytes(std::span<std::uint8_t>);
} // namespace psx::norm
