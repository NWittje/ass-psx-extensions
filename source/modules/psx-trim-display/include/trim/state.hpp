#pragma once
#include "trim/protocol.hpp"
namespace trim {
constexpr std::uint64_t stale_us=1500000;
struct Observation {
    Snapshot value{};
    std::uint64_t received_us{};
    std::uint32_t session{}, sequence{};
    bool active{}, has_state{};
};
enum class Mode : std::uint8_t { unavailable=0, off=1, normal=2 };
struct Visual {
    Mode mode{Mode::unavailable};
    std::uint16_t bar_height{};
    std::uint8_t green{}, brightness{255};
    bool operator==(const Visual&) const = default;
};
Visual evaluate(const Observation&,Role,std::uint64_t now_us) noexcept;
std::uint16_t display_bar_height(std::uint16_t angle_millidegrees) noexcept;
}
