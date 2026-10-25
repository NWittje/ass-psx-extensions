#pragma once
#include "snapshot.hpp"
#include <atomic>
#include <memory>
#include <Windows.h>

namespace psx::cdu::blackout
{
// Presentation override only. Unit order follows Unit, not the cockpit L/C/R order.
constexpr std::uint32_t all_units = 7, ready_flag = 1, bound_flag = 2;
constexpr std::uint64_t owner_lease_ns = 3000000000ull, receive_lease_ns = 1000000000ull;
constexpr wchar_t pipe_prefix[] = L"\\\\.\\pipe\\ASS.PSX.CduBlackout.";
constexpr std::size_t control_bytes = 64, pipe_bytes = 256;
constexpr std::uint32_t unit_mask(Unit u) noexcept { return 1u << unsigned(u); }
struct Control
{
    SessionId session{};
    std::uint64_t generation{}, sequence{}, revision{};
    std::uint32_t mask{}, flags{};
    bool operator==(const Control &) const = default;
};
bool encode(const Control &, std::array<std::uint8_t, control_bytes> &) noexcept;
bool decode(std::span<const std::uint8_t>, Control &) noexcept;

// Local message ABI is explicitly serialized too; no native packing or pointer fields.
enum class Operation : std::uint32_t { status, bind, set, clear_all };
enum class Result : std::uint32_t { ok, invalid, not_ready, not_bound, stale };
struct Context
{
    std::array<char, 16> host{};
    std::array<char, 48> adapter{};
    std::uint16_t port{};
    std::uint64_t binding_epoch{};
    bool operator==(const Context &) const = default;
};
bool context(Context &, std::string_view host, std::uint16_t port,
             std::string_view adapter, std::uint64_t binding_epoch) noexcept;
struct Packet
{
    Operation operation{};
    Result result{};
    std::uint64_t request{}, epoch{}, generation{}, revision{};
    std::uint32_t flags{}, selected_mask{}, active_mask{}, enabled{};
    Context main;
    std::array<char, 96> message{};
};
bool encode(const Packet &, std::array<std::uint8_t, pipe_bytes> &) noexcept;
bool decode(std::span<const std::uint8_t>, Packet &) noexcept;
struct Status
{
    std::uint64_t epoch{}, generation{}, revision{1}, lease_until{};
    std::uint32_t mask{};
    bool ready{}, bound{};
    Context main;
};
// Single network-worker owner. All clocks are injectable process-monotonic nanoseconds.
class State
{
    Status state_;
    void revoke() noexcept;
  public:
    explicit State(std::uint64_t initial_epoch = 1) noexcept;
    void update(const Context &, std::uint64_t generation, bool ready, std::uint64_t now) noexcept;
    void tick(std::uint64_t now) noexcept;
    void owner_lost() noexcept;
    Packet dispatch(const Packet &, std::uint64_t now) noexcept;
    Status status() const noexcept { return state_; }
};
struct Received
{
    bool valid{};
    std::uint32_t mask{};
    std::uint64_t revision{}, sequence{}, last_advance{};
};
class Reception
{
    SessionId session_{};
    std::uint64_t generation_{}, blocked_revision_{};
    bool pinned_{}, source_healthy_{};
    Received value_;
    std::uint32_t observed_mask_{}, observed_flags_{};
  public:
    void clear() noexcept;
    void source(bool pinned, const SessionId &, std::uint64_t generation, bool healthy,
                std::uint64_t now) noexcept;
    bool accept(const Control &, std::uint64_t now) noexcept;
    void tick(std::uint64_t now) noexcept;
    Received status() const noexcept { return value_; }
};
// No NORM/UI calls in the local service. Startup failure isolates this capability.
class Service
{
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    Service();
    ~Service();
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;
    bool start() noexcept;
    void request_stop() noexcept;
    void stop() noexcept;
    void update(const Context &, std::uint64_t generation, bool ready, std::uint64_t now) noexcept;
    void pump(std::uint64_t now) noexcept;
    HANDLE event() const noexcept;
    std::uint64_t deadline() const noexcept;
    Status status() const noexcept;
    DWORD error() const noexcept;
};
} // namespace psx::cdu::blackout
