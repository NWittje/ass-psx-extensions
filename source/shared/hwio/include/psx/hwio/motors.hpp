#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace psx::hwio::motors {
inline constexpr wchar_t pipe_prefix[]=L"\\\\.\\pipe\\PSX.HWIO.Motors.v1.";
inline constexpr unsigned slots=10;
enum class Operation:std::uint32_t {status=1,reset_home,stop};
enum class Result:std::uint32_t {ok,malformed,unavailable,session_mismatch,conflict,busy};
enum class Phase:std::uint32_t {following,lag,recovering,limit,waiting,stopped,fault,restoring,homing};
enum Flag:std::uint32_t {assigned=1,recovering=2,major_fault=4,homing_required=8,release_confirmed=16,reset_pending=32,reset_failed=64};
struct Status {
    std::uint64_t generation{},fault_revision{},operation_id{};
    std::uint32_t flags{};Phase phase{Phase::waiting};std::array<char,96> cause{};
};
// Fixed little-endian Windows ABI. Each request/reply is bounded independently
// of device text; the same-logon pipe never admits remote Windows clients.
struct Packet {
    std::uint32_t signature{0x31544d48};std::uint16_t version{1},bytes{2048};
    Operation operation{Operation::status};Result result{Result::ok};
    std::uint64_t request_id{},session{},fault_revision{},operation_id{},revision{};
    std::uint32_t motor{};std::uint16_t port{},flags{};
    std::array<Status,slots> motors{};
    std::array<char,128> host{},message{};std::array<std::byte,448> padding{};
};
static_assert(sizeof(Status)==128 && sizeof(Packet)==2048 && offsetof(Packet,motors)==64);
static_assert(std::is_trivially_copyable_v<Packet>);
inline bool valid(const Packet& p)noexcept{return p.signature==0x31544d48 && p.version==1 && p.bytes==sizeof(Packet) && p.result==Result::ok && p.operation>=Operation::status && p.operation<=Operation::stop;}
namespace v2 {
inline constexpr wchar_t pipe_prefix[]=L"\\\\.\\pipe\\PSX.HWIO.Motors.v2.";
inline constexpr unsigned slots=20;
using Operation=motors::Operation;using Result=motors::Result;using Phase=motors::Phase;using Status=motors::Status;
struct Packet {
    std::uint32_t signature{0x32544d48};std::uint16_t version{2},bytes{4096};
    Operation operation{Operation::status};Result result{Result::ok};
    std::uint64_t request_id{},session{},fault_revision{},operation_id{},revision{};
    std::uint32_t motor{};std::uint16_t port{},flags{};
    std::array<Status,slots> motors{};
    std::array<char,128> host{},message{};std::array<std::byte,1216> padding{};
};
static_assert(sizeof(Packet)==4096 && offsetof(Packet,motors)==64);
static_assert(std::is_trivially_copyable_v<Packet>);
inline bool valid(const Packet& p)noexcept{return p.signature==0x32544d48 && p.version==2 && p.bytes==sizeof(Packet) && p.result==Result::ok && p.operation>=Operation::status && p.operation<=Operation::stop;}
}
}
