#pragma once
#include "psx/hardware/link.hpp"
#include <array>
#include <cstdint>
namespace trim {
using psx::hardware::Bytes;
enum class Role : std::uint8_t { captain=1, first_officer=2 };
constexpr std::uint8_t state_type=64;
constexpr std::uint16_t trim_known=1, indicators_known=2, brightness_known=4, source_connected=8;
struct Snapshot {
    std::uint16_t angle{}, indicators{}, validity{};
    std::uint8_t brightness{255};
    Role role{Role::captain};
    std::uint32_t source_generation{};
};
bool decode_snapshot(Bytes, Role expected, Snapshot&) noexcept;
std::array<std::uint8_t,12> encode_snapshot(const Snapshot&) noexcept;
}
