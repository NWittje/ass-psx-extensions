#pragma once
#include "motors.hpp"
namespace psx::hwio::actuators {
inline constexpr wchar_t pipe_prefix[]=L"\\\\.\\pipe\\PSX.HWIO.Actuators.v3.";
inline constexpr unsigned slots=60,per_kind=20;
enum class Kind:std::uint32_t {motor,servo,stepper};
enum class Reference:std::uint32_t {none,measured,switch_estimate,pulse_command};
using Operation=motors::Operation;using Result=motors::Result;using Phase=motors::Phase;
struct Status {
    std::uint64_t generation{},fault_revision{},operation_id{};
    std::uint32_t flags{};Phase phase{Phase::waiting};std::array<char,96> cause{};
    double position{},target{},minimum{},maximum{};Reference reference{};Kind kind{};
    std::uint32_t slot{},reserved{};std::uint64_t calibration_revision{},sample_sequence{};
};
static_assert(sizeof(Status)==192 && offsetof(Status,position)==sizeof(motors::Status));
struct Packet {
    std::uint32_t signature{0x33544148};std::uint16_t version{3},bytes{16384};
    Operation operation{Operation::status};Result result{Result::ok};
    std::uint64_t request_id{},session{},fault_revision{},operation_id{},revision{};
    std::uint32_t motor{};std::uint16_t port{},flags{}; // Relative index, 0..19; Kind is explicit below.
    std::array<Status,slots> motors{};
    std::array<char,128> host{},message{};Kind kind{};
    std::array<std::byte,4540> padding{};
};
static_assert(sizeof(Packet)==16384 && offsetof(Packet,motors)==64 && std::is_trivially_copyable_v<Packet>);
inline bool valid(const Packet& p)noexcept{return p.signature==0x33544148 && p.version==3 && p.bytes==sizeof(Packet) && p.result==Result::ok && p.operation>=Operation::status && p.operation<=Operation::stop && unsigned(p.kind)<=2 && (p.operation==Operation::status || p.motor<20);}
}
