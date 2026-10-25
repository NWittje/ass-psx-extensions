#pragma once
#include "model.hpp"
#include <span>
namespace psx::cdu
{
using SessionId = std::array<std::uint8_t, 16>;
enum class SourceState : std::uint8_t
{
    disconnected,
    connecting,
    initializing,
    connected
};
struct AcceptedReceipt
{
    std::uint64_t sequence{}, elapsed_ns{};
    bool operator==(const AcceptedReceipt &) const = default;
};
struct CduSnapshot
{
    SessionId session{};
    std::uint64_t generation{1}, publication_sequence{1}, cache_revision{1}, host_elapsed_ns{};
    Unit unit{};
    SourceState source_state{SourceState::disconnected};
    std::uint32_t appearance{};
    std::int32_t brightness{}, blanking{};
    std::uint16_t text_valid{}, colour_valid{};
    std::uint8_t scalar_valid{}, source_label_size{};
    std::array<char, 128> source_label{};
    // Text rows 0..13, colour rows 14..27, appearance/brightness/blanking 28..30.
    std::array<AcceptedReceipt, 31> receipts{};
    Cells cells{};
    ScreenView view() const noexcept
    {
        return {&cells,
                unit,
                text_valid,
                colour_valid,
                bool(scalar_valid & 1),
                bool(scalar_valid & 4),
                appearance,
                blanking};
    }
    bool operator==(const CduSnapshot &) const = default;
};
constexpr std::size_t wire_header_bytes = 96, wire_base_bytes = 1600, wire_max_bytes = 1728,
                      transport_max_bytes = 2048;
struct WireBuffer
{
    std::array<std::uint8_t, transport_max_bytes> bytes{};
    std::size_t size{};
};
CduSnapshot make_snapshot(const CduScreen &, std::string_view, SourceState, const SessionId &,
                          std::uint64_t publication, std::uint64_t revision,
                          std::uint64_t sample_ns, std::uint64_t origin_ns);
bool encode(const CduSnapshot &, WireBuffer &, std::string *error = nullptr);
bool decode(std::span<const std::uint8_t>, CduSnapshot &, std::string *error = nullptr);
} // namespace psx::cdu
