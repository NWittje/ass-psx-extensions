#pragma once
#include <psx/traffic_source/protocol.hpp>
namespace psx::traffic_visual {
inline constexpr std::uint16_t protocol_version=1;
inline constexpr std::uint32_t model_revision=1,profile_revision=1;
inline constexpr std::size_t header_size=16,max_payload=1024,max_frame=1040,history_capacity=32;
inline constexpr std::uint32_t heartbeat_ms=250,lease_ms=1000,peer_timeout_ms=5000,progress_timeout_ms=1000;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.traffic.plugin.v1.";
enum class Kind:std::uint16_t {hello=1,claim=2,bundle=3,ack=4,heartbeat=5,report=6,capture=7,detail=8};
enum class Action:std::uint32_t {acquire=1,renew=2,release=3};
enum class Phase:std::uint32_t {observing,pending,owned,released,rejected};
enum class Reason:std::uint32_t {none,unsupported,awaiting_source,loading,stale,identity,lifecycle,native_fault,disconnected,timeout,protocol,overflow,incompatible,model_unavailable,model_fault,binding,contention,released};
enum Capability:std::uint32_t {supported=1,qualified_model=2,loaded_model=4};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {motion::InstanceId instance{};std::uint32_t pid{},capabilities{};std::uint64_t process_creation_filetime{},qpc_frequency{};std::uint32_t model{model_revision},profile{profile_revision};};
// The source floor is established before Acquire submission and preserved by Renew.
struct Claim {
 motion::InstanceId plugin{},owner{};std::uint64_t connection{},lease{},request{};
 motion::InstanceId source_producer{};std::uint64_t source_connection{},source_selection{},lifecycle{};
 std::uint32_t source_pid{};std::uint64_t source_birth{},frequency{};std::int64_t source_floor_qpc{};
 Action action{Action::acquire};std::uint32_t delay_ms{25},presentation_mode{},model{model_revision},profile{profile_revision}; // mode0 interpolated,1 direct
};
struct Bundle {motion::InstanceId plugin{},owner{};std::uint64_t connection{},lease{};traffic_source::Sample source{};};
struct Heartbeat {motion::InstanceId sender{};std::uint64_t connection{};std::int64_t generated_qpc{};std::uint32_t capabilities{};Reason reason{Reason::none};};
// Ack acknowledges control; Report describes actual SDK work, never pixel readback.
struct Report {
 motion::InstanceId plugin{},owner{};std::uint64_t connection{},lease{},request{};std::int64_t generated_qpc{};
 motion::InstanceId source_producer{};std::uint64_t source_connection{},source_selection{},lifecycle{},source_sequence{};std::int64_t source_observed_qpc{};
 Phase phase{Phase::observing};Reason reason{Reason::none};std::uint32_t drawable_mask{},submitted_mask{},held_mask{},hidden_mask{};
 std::uint32_t model{model_revision},profile{profile_revision};std::uint64_t sdk_sequence{},scenery_generation{},origin_generation{};std::int64_t applied_qpc{};std::uint64_t resource_generation{};
};
struct Ack:Report {};
// Independent Capture detail intent, renewed at500ms, expires after2000ms.
struct Capture {motion::InstanceId plugin{};std::uint64_t connection{},generation{};std::int64_t sent_qpc{};std::uint32_t enabled{},duration_ms{2000};};
enum DetailFlag:std::uint32_t {detail_pose_valid=1,detail_submitted=2,detail_visible=4,detail_incomplete=8};
struct Detail {
 motion::InstanceId plugin{},owner{};std::uint64_t connection{},lease{},request{},capture_generation{},sdk_sequence{};
 std::uint32_t slot{},flags{};motion::InstanceId source_producer{};std::uint64_t source_connection{},source_selection{},lifecycle{},source_sequence{},source_frame{},generation{};
 std::int64_t source_observed_qpc{},generated_qpc{},target_qpc{},presentation_qpc{},left_observed_qpc{},right_observed_qpc{},left_received_qpc{},right_received_qpc{},applied_qpc{};
 std::uint64_t left_sequence{},right_sequence{},left_frame{},right_frame{},scenery_generation{},origin_generation{},resource_generation{},dropped{};
 std::uint32_t state{},validity{},delay_ms{25},presentation_mode{},model{model_revision},profile{profile_revision};
 double alpha{},latitude_deg{},longitude_deg{},altitude_msl_m{},heading_deg{},pitch_deg{},bank_deg{};
 double local_x{},local_y{},local_z{},local_heading_deg{},local_pitch_deg{},local_bank_deg{},strobe_phase{},nav_brightness{},strobe_brightness{};
};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Claim&) noexcept;bool valid(const Bundle&) noexcept;bool valid(const Report&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
#define PSX_VISUAL_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept;bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_VISUAL_CODEC(Hello) PSX_VISUAL_CODEC(Claim) PSX_VISUAL_CODEC(Bundle) PSX_VISUAL_CODEC(Ack) PSX_VISUAL_CODEC(Heartbeat) PSX_VISUAL_CODEC(Report) PSX_VISUAL_CODEC(Capture) PSX_VISUAL_CODEC(Detail)
#undef PSX_VISUAL_CODEC
using FrameHandler=bool(*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
 void reset() noexcept {used_=0;needed_=header_size;last_sequence_=0;failed_=false;}bool failed() const noexcept {return failed_;}
private:std::array<std::uint8_t,max_frame> bytes_{};std::size_t used_{},needed_{header_size};std::uint32_t last_sequence_{};bool failed_{};
};
}
