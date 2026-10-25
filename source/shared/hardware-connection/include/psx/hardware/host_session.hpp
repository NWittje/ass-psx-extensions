#pragma once
#include "psx/hardware/link.hpp"

namespace psx::hardware {
namespace testing {struct HostAccess;}
enum class HostPhase { closed, discovering, starting, ready, failed };
enum class HostFault { none, discovery_timeout, handshake_timeout, identity_mismatch, peer_timeout, transport };
struct ProfileState {
    std::uint8_t type{};
    std::uint16_t size{};
    std::array<std::uint8_t,max_payload> payload{};
};
struct HostBinding {
    std::uint64_t expected_device{}; // Zero permits first identification of this explicitly selected candidate.
    std::uint16_t board{}, profile{}, revision{1};
};
struct PublicationPolicy {
    std::uint64_t minimum_interval_us{33334}, heartbeat_us{250000}, peer_timeout_us{3000000};
};
struct HostCallbacks {
    void* context{};
    bool (*identify)(void*,const Descriptor&) noexcept{};
    bool (*message)(void*,const Message&,std::uint64_t) noexcept{};
};
struct HostStatus {
    HostPhase phase{HostPhase::closed};
    HostFault fault{HostFault::none};
    Descriptor identity{};
    std::uint32_t session{}, last_sent{}, last_device{}, rejected{}, oversized{};
    bool identified{}, has_sent{}, has_device_sequence{};
};

// Host endpoint: time, entropy and transport are supplied by its owner.
class HostSession {
public:
    void start(HostBinding,HostCallbacks,std::uint64_t challenge,std::uint32_t session,
               std::uint64_t now_us,PublicationPolicy={}) noexcept;
    void receive(Bytes,std::uint64_t now_us) noexcept;
    void service(std::uint64_t now_us) noexcept;
    SendResult publish_latest(const ProfileState&) noexcept;
    Bytes pending_tx() const noexcept;
    bool consume_tx(std::size_t) noexcept;
    void close() noexcept;
    void transport_fault() noexcept;
    HostStatus status() const noexcept { return status_; }
    std::uint64_t next_wake(std::uint64_t now_us) const noexcept;
private:
    friend struct testing::HostAccess;
    void discover(std::uint64_t) noexcept;
    void fail(HostFault) noexcept;
    void dispatch(const Message&,std::uint64_t) noexcept;
    bool queue(Type,Bytes,std::uint32_t session,std::uint32_t sequence) noexcept;
    HostBinding binding_{};
    HostCallbacks callbacks_{};
    PublicationPolicy policy_{};
    HostStatus status_{};
    std::array<std::uint8_t,max_wire-1> rx_{};
    std::array<std::uint8_t,max_wire-2> decoded_{};
    std::size_t rx_size_{},tx_offset_{};
    bool discard_{},dirty_{},has_latest_{},hello_sent_{};
    Frame tx_{};
    ProfileState latest_{};
    std::uint64_t challenge_{},overall_deadline_{},phase_deadline_{},control_due_{},last_rx_{},last_send_{},send_due_{};
    std::uint32_t sequence_{1};
};
}
