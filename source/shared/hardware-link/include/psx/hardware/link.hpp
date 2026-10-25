#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::hardware {
using Bytes = std::span<const std::uint8_t>;
constexpr std::size_t max_wire = 128, max_payload = 108, header_size = 16;
enum class Type : std::uint8_t { hello=1, hello_reply=2, start_session=3, ready=4, status=5, profile_first=64 };
struct Frame {
    std::array<std::uint8_t, max_wire> data{};
    std::size_t size{};
};
struct Message { std::uint8_t type{}; std::uint32_t session{}, sequence{}; Bytes payload{}; };
std::uint16_t read16(Bytes, std::size_t) noexcept;
std::uint32_t read32(Bytes, std::size_t) noexcept;
std::uint64_t read64(Bytes, std::size_t) noexcept;
void write16(std::span<std::uint8_t>, std::size_t, std::uint16_t) noexcept;
void write32(std::span<std::uint8_t>, std::size_t, std::uint32_t) noexcept;
void write64(std::span<std::uint8_t>, std::size_t, std::uint64_t) noexcept;
std::uint16_t crc16(Bytes) noexcept;
bool newer(std::uint32_t candidate, std::uint32_t previous) noexcept;
bool encode(const Message&, Frame&) noexcept;
bool decode(Bytes encoded_without_delimiter, std::span<std::uint8_t> storage, Message&) noexcept;

struct Descriptor {
    std::uint64_t device{}, boot{};
    std::uint32_t firmware{};
    std::uint16_t board{}, profile{}, profile_revision{1};
    std::array<std::uint8_t, 48> metadata{};
    std::uint8_t metadata_size{};
};
struct LinkStatus {
    std::uint32_t session{}, accepted_sequence{};
    std::uint32_t frames{}, rejected{}, oversized{}, wrong_session{}, profile_rejected{}, tx_busy{};
    bool active{}, has_accepted{};
};
struct Callbacks {
    void* context{};
    // Views expire on return; false rejects the observation without advancing its sequence.
    bool (*message)(void*, const Message&, std::uint64_t now_us) noexcept{};
    void (*session)(void*, std::uint32_t session, bool active) noexcept{};
};
enum class SendResult { accepted, busy, invalid };

// Device-side endpoint. No clock, transport, task, allocation or instrument policy is owned here.
class HardwareLink {
public:
    HardwareLink(Descriptor descriptor, Callbacks callbacks) noexcept;
    HardwareLink(const HardwareLink&)=delete;
    HardwareLink& operator=(const HardwareLink&)=delete;
    void receive(Bytes bytes, std::uint64_t now_us) noexcept;
    void service(std::uint64_t now_us) noexcept;
    SendResult try_send(std::uint8_t type, Bytes payload) noexcept;
    Bytes pending_tx() noexcept;
    bool consume_tx(std::size_t count) noexcept;
    void reset_transport() noexcept;
    LinkStatus status() const noexcept { return status_; }
private:
    bool enqueue(std::uint8_t type, std::uint32_t session, Bytes payload) noexcept;
    void dispatch(const Message&, std::uint64_t now_us) noexcept;
    Descriptor descriptor_;
    Callbacks callbacks_;
    LinkStatus status_{};
    std::array<std::uint8_t, max_wire-1> rx_{};
    std::array<std::uint8_t, max_wire-2> decoded_{};
    std::size_t rx_size_{};
    bool discard_{};
    Frame active_{};
    std::size_t active_offset_{};
    std::array<Frame,4> queue_{};
    std::size_t head_{}, count_{};
    std::uint32_t next_tx_{1};
    std::uint64_t challenge_{}, challenge_time_{}, session_challenge_{};
    bool challenged_{};
};
}
