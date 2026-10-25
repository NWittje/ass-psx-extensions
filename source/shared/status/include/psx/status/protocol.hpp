#pragma once
#include <psx/motion/full_pose.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::status {
inline constexpr std::uint16_t protocol_version=1;
inline constexpr std::uint32_t status_jni_abi=1;
inline constexpr std::uint32_t snapshot_interval_ms=250,source_stale_ms=1000,peer_timeout_ms=5000;
inline constexpr std::size_t header_size=16,max_payload=512,max_frame=header_size+max_payload;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.status.plugin.v1.";
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.status.exporter.v1.";
enum class Role:std::uint32_t {plugin=1,exporter=2};
enum class Kind:std::uint16_t {hello=1,subscribe=2,snapshot=3,heartbeat=4};
enum class Reason:std::uint32_t {none=0,unsupported=1,identity=2,lifecycle=3,stale=4,loading=5,binding=6,native_fault=7,disconnected=8,timeout=9};
// Explicit PXS1 mapping; motion::PoseSource uses different numeric values.
enum class TargetSource:std::uint32_t {none=0,public_main_boost=1,exporter=2};
enum Capability:std::uint32_t {fatal_state=1,xplane_readiness=2};
enum Flag:std::uint32_t {
 qualified=1,aircraft_loaded=2,bindings_ready=4,readback_ready=8,
 placement_active=16,placement_returned=32,reconciling=64,
 async_available=128,async_busy=256,fatal_valid=512,fatal_active=1024,
 fatal_requested=2048,pause_valid=4096,paused=8192,motion_frozen=16384,
 plugin_enabled=32768,readback_required=65536,reference_ready=131072
};
inline constexpr std::uint32_t all_flags=262143;
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {Role role{Role::plugin};motion::InstanceId instance{};std::uint32_t pid{},capabilities{};std::uint64_t process_creation_filetime{},qpc_frequency{};};
struct Subscribe {motion::InstanceId instance{};std::uint64_t connection_id{},qpc_frequency{};std::uint32_t pid{};};
// Source timestamps NEVER become worker heartbeat timestamps. Flags are source facts,
// not coordinator assertions. Identity replacement requires new source evidence.
struct Snapshot {
 motion::InstanceId producer{};Role role{Role::plugin};
 std::uint64_t connection_id{},sequence{},lifecycle{},enable_generation{},aircraft_generation{},scenery_generation{},local_frame_generation{};
 std::uint64_t command_id{},placement_attempt{},target_epoch{},target_sequence{},callback_count{};
 std::int64_t observed_qpc{},phase_start_qpc{},call_returned_qpc{},notification_qpc{};
 std::uint32_t flags{},notification_kind{},target_source{};Reason reason{Reason::none};
 motion::InstanceId target_producer{};std::uint64_t selection_epoch{};
};
struct Heartbeat {motion::InstanceId producer{};std::uint64_t connection_id{};std::int64_t generated_qpc{};std::uint32_t capabilities{};Reason reason{Reason::none};};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Snapshot&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
#define PSX_STATUS_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept;bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_STATUS_CODEC(Hello) PSX_STATUS_CODEC(Subscribe) PSX_STATUS_CODEC(Snapshot) PSX_STATUS_CODEC(Heartbeat)
#undef PSX_STATUS_CODEC
using FrameHandler=bool (*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
 void reset() noexcept {used_=0;needed_=header_size;last_sequence_=0;failed_=false;}
 bool failed() const noexcept {return failed_;}
private:std::array<std::uint8_t,max_frame> bytes_{};std::size_t used_{},needed_{header_size};std::uint32_t last_sequence_{};bool failed_{};
};
}
