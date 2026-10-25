#pragma once
#include "snapshot.hpp"
#include <psx/non_normal/pipe.hpp>
namespace psx::situation {
using ProcessIdentity=psx::non_normal::ProcessIdentity;
inline constexpr std::size_t chunk_bytes=16384,frame_bytes=16+240+chunk_bytes;
enum class Command:std::uint32_t {status=0,capture=1,stage=2,recall=3,lookup=4,fetch=5,recall_fmc=6,heartbeat=7};
enum class Phase:std::uint32_t {unavailable=0,accepted=1,action_returned=2,saved_verified=3,observed=4,rejected=5,indeterminate=6};
enum class Reason:std::uint32_t {none=0,unsupported=1,identity=2,expired=3,disconnected=4,busy=5,malformed=6,binding=7,native_exception=8,replaced=9,file=10,not_retained=11};
enum Capability:std::uint32_t {native_save=1,native_recall=2,inspection=4,fmc_export=8,fmc_recall=16};
enum Evidence:std::uint32_t {native_returned=1,load_returned_true=2,pause_observed=4,zero_rates_observed=8};
struct Request {
 Command command{Command::status};ProcessIdentity client{},target{};
 std::uint64_t session{},operation{},lifecycle{},frequency{};
 std::int64_t issued_qpc{},deadline_qpc{};
 std::uint32_t offset{},total{};std::vector<std::uint8_t> data;
 bool operator==(const Request&)const=default;
};
struct Result {
 Phase phase{Phase::unavailable};Reason reason{Reason::unsupported};std::uint32_t capabilities{},evidence{};
 ProcessIdentity server{};std::uint64_t session{},operation{},lifecycle{};std::int64_t observation_qpc{};
 std::uint32_t offset{},total{};Hash artifact_hash{},source_jar_hash{};
 // EDT post-loader observation: lat/lon/heading/pitch/bank radians, MSL feet,
 // TAS knots, unqualified raw vtVel, mixed-native-unit angular zero-check,
 // effective Pause, loaded pause/freeze mask, loader-in-progress.
 std::array<double,12> readback{};std::vector<std::uint8_t> data;
};
struct Frame {std::array<std::uint8_t,frame_bytes> bytes{};std::size_t size{};};
bool encode(const Request&,std::uint32_t,Frame&)noexcept;
bool encode(const Result&,std::uint32_t,Frame&)noexcept;
bool decode(std::span<const std::uint8_t>,Request&,std::uint32_t&)noexcept;
bool decode(std::span<const std::uint8_t>,Result&,std::uint32_t&)noexcept;
std::wstring endpoint(ProcessIdentity);
// Idle wait in milliseconds; zero preserves partial input for the next poll.
// Positive waits can stream large frames; header/body share one second to finish.
bool read_frame(HANDLE,Frame&,std::uint32_t timeout_ms)noexcept;
bool write_frame(HANDLE,const Frame&,std::uint32_t timeout_ms=1000)noexcept;
// Same-machine QPC and authenticated OS process identities are mandatory.
class Client {
 HANDLE pipe_{INVALID_HANDLE_VALUE};ProcessIdentity target_{},self_{};
 std::uint64_t session_{},next_{},last_mutation_{},frequency_{};std::uint32_t sent_{},received_{};
 Result exchange(Request);
 Request request(Command,std::uint64_t operation,std::uint64_t lifecycle=0);
public:
 explicit Client(ProcessIdentity target);~Client();Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 void connect();void disconnect()noexcept;
 Result status();Result lookup(std::uint64_t operation);
 Result capture(std::vector<std::uint8_t>& artifact);
 // Retrieve the original retained capture after reconnect/lookup; never save again.
 Result fetch_capture(std::uint64_t operation,std::vector<std::uint8_t>& artifact);
 Result recall(std::span<const std::uint8_t> artifact,std::uint64_t expected_lifecycle,bool fmc=false);
 Result wait(std::uint64_t operation,std::uint32_t timeout_ms=30000);
 std::uint64_t last_operation()const noexcept{return last_mutation_;}
};
}
