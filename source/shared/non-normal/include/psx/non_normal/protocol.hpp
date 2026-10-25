#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace psx::non_normal {
constexpr std::uint16_t version=1,actor_version=2;
constexpr std::size_t header_bytes=16,request_bytes=160,actor_request_bytes=164,result_bytes=128,max_frame=180;
enum class Kind:std::uint16_t {request=1,result=2};
enum class Command:std::uint32_t {status=0,reset_malfunctions=1,place_actor=2,remove_actor=3,heartbeat=4};
enum class Phase:std::uint32_t {unavailable=0,accepted=1,loading=2,applied=3,action_returned=4,rejected=5,indeterminate=6,removed=7};
enum class Reason:std::uint32_t {none=0,unsupported=1,identity=2,expired=3,disconnected=4,busy=5,malformed=6,binding=7,native_exception=8,model=9,terrain=10,scenery=11,replaced=12,revision=13,transport=14};
enum Capability:std::uint32_t {reset_capability=1,actor_capability=2};
struct ProcessIdentity {std::uint32_t pid{};std::uint64_t creation_filetime{};bool operator==(const ProcessIdentity&)const=default;};
struct Request {
 std::uint16_t wire_version{version};
 Command command{Command::status}; ProcessIdentity client{},target{};
 std::uint64_t session{},request{},frequency{};std::int64_t issued_qpc{},deadline_qpc{};
 std::uint64_t expected_revision{},database_revision{};
 double latitude_deg{},longitude_deg{},heading_deg{};
 std::array<char,8> airport{},runway{};
 // Original IOS owner survives coordinator forwarding; never an authorization substitute.
 ProcessIdentity originator{};std::uint64_t originator_session{};
 ProcessIdentity psx{}; // Selected local Main process, mandatory for actor placement.
 std::int16_t runway_elevation_ft{}; // Signed database runway-end feet MSL; never airport mean.
 bool runway_elevation_valid{}; // v2 only, mandatory for Place, zero is a valid height.
 bool operator==(const Request&)const=default;
};
struct Result {
 std::uint16_t wire_version{version};
 ProcessIdentity server{};std::uint64_t session{},request{};Command command{Command::status};
 Phase phase{Phase::unavailable};Reason reason{Reason::unsupported};std::uint32_t capabilities{};
 std::uint64_t revision{},scenery_generation{},resource_generation{};std::int64_t observed_qpc{};
 double latitude_deg{},longitude_deg{},altitude_m{},heading_deg{},pitch_deg{},bank_deg{};
 bool operator==(const Result&)const=default;
};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Request&)noexcept;bool valid(const Result&)noexcept;
bool encode(const Request&,std::uint32_t sequence,Frame&)noexcept;
bool encode(const Result&,std::uint32_t sequence,Frame&)noexcept;
bool decode(std::span<const std::uint8_t>,Request&,std::uint32_t& sequence)noexcept;
bool decode(std::span<const std::uint8_t>,Result&,std::uint32_t& sequence)noexcept;
}
