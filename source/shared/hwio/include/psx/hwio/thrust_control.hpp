#pragma once
#include "situation_controls.hpp"
#include <cmath>
namespace psx::hwio::thrust_control {
inline constexpr wchar_t pipe_prefix[]=L"\\\\.\\pipe\\PSX.HWIO.Thrust.v1.";
enum class Operation:std::uint32_t {status=1,set_hold,release};
enum class Result:std::uint32_t {ok,malformed,unavailable,session_mismatch,conflict,busy};
enum class Phase:std::uint32_t {unavailable,ready,positioning,correcting,held,limit,interrupted,fault,releasing};
enum LeverFlag:std::uint32_t {covered=1,ready=2,physical_valid=4,drive_released=8};
enum Flag:std::uint32_t {all_covered=1,can_set=2,owned=4,all_held=8,releasing=16,interrupted=32};
struct Process {std::uint32_t pid{},reserved{};std::uint64_t birth{};bool operator==(const Process&)const=default;};
struct Lever {
    std::uint32_t flags{},group{},master{};Phase phase{};
    std::uint64_t generation{},calibration{};
    double physical_percent{},requested_percent{},effective_percent{};
    std::array<char,96> reason{};std::uint64_t operation_id{};
};
static_assert(sizeof(Lever)==160);
struct Snapshot {
    std::uint32_t signature{0x31544348};std::uint16_t version{1},bytes{704};
    std::uint64_t session{},revision{},token{},operation_id{};
    std::int64_t sampled_qpc{};std::uint64_t frequency{};
    std::uint32_t owner_pid{},flags{};std::array<Lever,4> levers{};
};
static_assert(sizeof(Snapshot)==704);
struct Packet {
    std::uint32_t signature{0x31505448};std::uint16_t version{1},bytes{2048};
    Operation operation{Operation::status};Result result{Result::ok};
    std::uint64_t request_id{},session{},revision{},token{},operation_id{},reserved{};
    std::array<double,4> percent{};Snapshot status;std::array<char,128> message{};
    std::array<char,128> host{};std::uint16_t port{};std::array<std::byte,990> padding{};
};
static_assert(sizeof(Packet)==2048 && offsetof(Packet,percent)==64 && offsetof(Packet,status)==96 && std::is_trivially_copyable_v<Packet>);
inline bool valid(const Packet& p)noexcept {
    if(p.signature!=0x31505448 || p.version!=1 || p.bytes!=sizeof(p) || p.result!=Result::ok || p.reserved || p.operation<Operation::status || p.operation>Operation::release)return false;
    if(p.operation!=Operation::status && !p.request_id)return false;
    if(p.operation==Operation::set_hold)for(const auto value:p.percent)if(!std::isfinite(value) || value<0 || value>100)return false;
    return true;
}
// HPC1 stays at offset zero. Older v3 clients ignore this optional sibling.
inline constexpr std::size_t snapshot_offset=sizeof(situation_controls::Snapshot);
static_assert(snapshot_offset+sizeof(Snapshot)<=sizeof(actuators::Packet::padding));
inline void put(actuators::Packet& p,const Snapshot& s)noexcept {std::memcpy(p.padding.data()+snapshot_offset,&s,sizeof(s));}
inline std::optional<Snapshot> get(const actuators::Packet& p,std::int64_t now,std::uint64_t frequency)noexcept {
    Snapshot s;std::memcpy(&s,p.padding.data()+snapshot_offset,sizeof(s));
    if(s.signature!=0x31544348 || s.version!=1 || s.bytes!=sizeof(s) || !s.session || s.session!=p.session || !frequency || s.frequency!=frequency || s.sampled_qpc<=0 || s.sampled_qpc>now || now-s.sampled_qpc>std::int64_t(2*frequency) || (s.flags&~63u))return {};
    for(const auto& l:s.levers)if((l.flags&~15u) || l.master>3 || unsigned(l.phase)>unsigned(Phase::releasing) || !std::isfinite(l.physical_percent) || !std::isfinite(l.requested_percent) || !std::isfinite(l.effective_percent) || l.requested_percent<0 || l.requested_percent>100 || l.effective_percent<0 || l.effective_percent>100 || ((l.flags&covered) && (!l.group || !l.generation || !l.calibration)))return {};
    return s;
}
}
