#pragma once
#include "psx/hardware/connection.hpp"
#include "trim/protocol.hpp"
#include <mutex>

namespace psx::hardware {
struct TrimDeviceStatus {
    std::uint32_t accepted{},rendered{};
    bool has_accepted{},has_rendered{},panel_fault{};
    std::uint8_t presentation{},role{};
    std::array<std::uint32_t,9> diagnostics{}; // Rejected, oversized, wrong session, busy, UART, heap, DMA, comm/render stack.
};
bool decode_trim_status(Bytes,std::uint8_t expected_role,TrimDeviceStatus&) noexcept;
inline constexpr std::array<std::uint8_t,32> trim_artwork{
    0x1B,0x9F,0x3D,0xCD,0x70,0x26,0x45,0x4F,0xF7,0x8E,0x2F,0xDC,0x67,0xE0,0xB7,0x48,
    0x25,0x8A,0xB8,0x10,0xE1,0xB5,0xF4,0xED,0x7D,0x40,0x2B,0x4E,0xB6,0xF7,0x97,0x65};
// Keep the delivered pre-frame instruments usable during the firmware update.
// Both identities are explicit; an unfamiliar artwork revision still fails discovery.
inline constexpr std::array<std::uint8_t,32> trim_artwork_legacy{
    0x91,0xD6,0x2C,0x0A,0x65,0xA6,0xC0,0x9B,0xC0,0xE0,0x93,0xAA,0x2B,0xA9,0xED,0x30,
    0x0C,0x75,0x62,0xF9,0xCF,0x89,0x31,0x56,0x1E,0xA4,0x1C,0x44,0x92,0x0E,0x90,0x48};
struct TrimConnectionStatus {
    ConnectionStatus connection;
    TrimDeviceStatus device;
    std::uint8_t role{};
    bool has_status{},source_connected{};
};
class TrimDisplayConnection {
public:
    TrimDisplayConnection();
    ~TrimDisplayConnection();
    // expected_role=0 learns CPT/FO from this selected board; production bindings may pin both values.
    OpenResult open(const SerialPort&,std::uint64_t expected_device=0,std::uint8_t expected_role=0);
    SendResult publish_latest(const trim::Snapshot&) noexcept;
    TrimConnectionStatus status() const noexcept;
    void close() noexcept {connection_.close();}
private:
    friend struct testing::ConnectionAccess;
    explicit TrimDisplayConnection(detail::TransportOps);
    static bool identify(void*,const Descriptor&) noexcept;
    static bool message(void*,const Message&,std::uint64_t) noexcept;
    mutable std::mutex gate_;
    TrimDeviceStatus device_{};
    std::uint8_t role_{},expected_role_{};
    bool has_status_{},source_connected_{};
    HardwareConnection connection_; // Destroyed first, so callbacks cannot outlive their state.
};
}
