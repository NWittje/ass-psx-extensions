#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::scenery {
inline constexpr std::uint16_t version=1;
inline constexpr std::uint32_t check_timeout_ms=2000;
enum class Verdict:std::uint32_t {unavailable,available,missing};
enum class Reason:std::uint32_t {none,disconnected,unsupported,timeout,identity,invalid,io,format,changed,busy};
// Geographic degrees, not SDK local coordinates or delayed presentation values.
struct Query {
    std::uint64_t operation{};
    std::uint32_t psx_pid{};
    std::uint64_t psx_birth{};
    double latitude_deg{},longitude_deg{};
    bool operator==(const Query&) const noexcept=default;
};
struct Result {
    Query query;
    Verdict verdict{Verdict::unavailable};
    Reason reason{Reason::disconnected};
    std::int32_t south{},west{};
    std::uint64_t plugin_load{},plugin_enable{},inventory_generation{};
    std::int64_t observed_qpc{}; // Mapped for transport; source_* retains original provenance.
    std::int64_t source_qpc{};
    std::uint64_t source_frequency{};
};
inline constexpr std::size_t query_bytes=36,result_bytes=112,max_frame=128;
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Query&) noexcept;
bool valid(const Result&) noexcept;
bool encode(const Query&,Frame&) noexcept;
bool encode(const Result&,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Query&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Result&) noexcept;
// Tile containing the destination: floor for negative values, +180 wraps to -180.
bool tile(double latitude,double longitude,int& south,int& west) noexcept;
}
