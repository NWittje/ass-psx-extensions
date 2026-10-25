#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psx::hwio::failures {
inline constexpr wchar_t pipe_prefix[] = L"\\\\.\\pipe\\PSX.HWIO.Failures.v1.";
inline constexpr std::uint32_t magic = 0x31465748; // HWF1, little endian.
inline constexpr std::uint16_t version = 1, packet_bytes = 512;
inline constexpr unsigned max_arms = 4;
inline constexpr unsigned owner_timeout_ms = 3000, sample_max_age_ms = 2000;
enum class Operation : std::uint32_t { status=1, bind, set, arm, cancel_arm, clear_all, altitude_sample };
enum class Result : std::uint32_t { ok, malformed, unavailable, session_mismatch, not_bound, invalid_value, missing_baseline, conflict, capacity, random_unavailable };
enum class Trigger : std::uint32_t { none, ias, native_altitude };
enum Flag : std::uint32_t { ready=1, bound=2, ias_available=4, altitude_available=8, sample_valid=16 };
enum Axis : std::uint32_t { elevator=1, aileron=2, rudder=4 };
struct ArmInfo {
    std::uint64_t id{};
    double threshold{};
    std::uint32_t axis_mask{}, trigger{};
    std::int32_t direction{};
    std::uint32_t trim{};
};
struct Payload {
    std::int64_t qpc{};
    std::uint64_t revision{}, arm_id{}, source_generation{}, source_start_ms{};
    std::int64_t sampled_unix_ms{}; // Original exporter sample time, never IPC receipt time.
    double value{};
    std::uint32_t axis_mask{};
    std::int32_t trim{}; // Request: Boolean. Reply: chosen -1/0/+1 direction.
    Trigger trigger{};
    std::uint32_t source_pid{}, flags{}, arm_count{};
    std::uint16_t port{}, reserved{};
    std::array<char,128> host{}, message{};
    std::array<ArmInfo,max_arms> arms{};
    std::array<std::byte,8> padding{};
};
struct Packet {
    std::uint32_t signature{magic};
    std::uint16_t protocol{version}, bytes{packet_bytes};
    Operation operation{Operation::status};
    Result result{Result::ok};
    std::uint64_t request_id{}, session{};
    Payload data{};
};
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(ArmInfo)==32 && sizeof(Payload)==480 && sizeof(Packet)==512);
static_assert(offsetof(Packet,data)==32 && offsetof(Payload,host)==84);
static_assert(std::is_trivially_copyable_v<Packet>);
inline bool valid(const Packet& p) noexcept {
    return p.signature==magic && p.protocol==version && p.bytes==packet_bytes &&
        p.operation>=Operation::status && p.operation<=Operation::altitude_sample && p.result==Result::ok;
}
} // namespace psx::hwio::failures
