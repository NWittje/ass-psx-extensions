#pragma once
#include <psx/motion/full_pose.hpp>
#include <psx/traffic_source/traffic_snapshot.hpp>
#include <array>
namespace psx::traffic_source {
inline constexpr std::uint16_t protocol_version=1;
inline constexpr std::size_t header_size=16,max_payload=1024,max_frame=1040;
inline constexpr std::uint32_t sample_interval_ms=10,heartbeat_ms=250,source_stale_ms=1000,peer_timeout_ms=5000;
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.traffic.exporter.v1.";
enum class Kind:std::uint16_t {hello=1,subscribe=2,sample=3,heartbeat=4,status=5};
enum class Reason:std::uint32_t {none,unsupported,awaiting_source,loading,stale,identity,lifecycle,native_fault,disconnected,timeout,protocol,overflow,incompatible};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {motion::InstanceId instance{};std::uint32_t pid{},capabilities{};std::uint64_t process_creation_filetime{},qpc_frequency{};};
struct Subscribe {motion::InstanceId instance{};std::uint64_t connection_id{},selection_epoch{},qpc_frequency{};std::uint32_t pid{};std::uint64_t process_creation_filetime{};};
struct Sample {motion::InstanceId producer{};std::uint64_t connection_id{},selection_epoch{};traffic::Snapshot snapshot{};};
struct Heartbeat {motion::InstanceId producer{};std::uint64_t connection_id{},selection_epoch{};std::int64_t generated_qpc{};std::uint32_t capabilities{};Reason reason{Reason::none};};
struct Status {motion::InstanceId producer{};std::uint64_t connection_id{},selection_epoch{};std::int64_t generated_qpc{};std::uint32_t capabilities{};Reason reason{Reason::none};std::uint64_t drained{},sent{},sequence_skips{},work_ticks{},max_work_ticks{};};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const traffic::Snapshot&) noexcept;
bool valid(const Sample&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
#define PSX_TRAFFIC_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept;bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_TRAFFIC_CODEC(Hello) PSX_TRAFFIC_CODEC(Subscribe) PSX_TRAFFIC_CODEC(Sample) PSX_TRAFFIC_CODEC(Heartbeat) PSX_TRAFFIC_CODEC(Status)
#undef PSX_TRAFFIC_CODEC
using FrameHandler=bool(*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:
 bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
 void reset() noexcept {used_=0;needed_=header_size;last_sequence_=0;failed_=false;}
 bool failed() const noexcept {return failed_;}
private:std::array<std::uint8_t,max_frame> bytes_{};std::size_t used_{},needed_{header_size};std::uint32_t last_sequence_{};bool failed_{};
};
}
