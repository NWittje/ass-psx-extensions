#pragma once
#include <psx/motion/full_pose.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

// PXA1: independently negotiated aircraft presentation, never aircraft dynamics.
namespace psx::aircraft_sync {
inline constexpr std::uint16_t protocol_version=1;
inline constexpr std::uint32_t aircraft_jni_abi=1;
inline constexpr std::size_t group_count=6,component_capacity=32;
inline constexpr std::size_t header_size=16,max_payload=1024,max_frame=header_size+max_payload;
inline constexpr std::uint32_t renew_ms=250,lease_ms=1000,native_stale_ms=1000;
inline constexpr std::size_t ordered_capacity=16;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.aircraft.plugin.v1.";
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.aircraft.exporter.v1.";
enum class Group:std::uint32_t {controls=0,spoilers=1,high_lift=2,gear=3,reversers=4,lights=5};
inline constexpr std::uint32_t all_groups=63;
enum class Role:std::uint32_t {plugin=1,exporter=2};
enum class Kind:std::uint16_t {hello=1,subscribe=2,sample=3,control=4,ack=5,applied=6,status=7,capture=8};
enum class Source:std::uint32_t {native=1,public_record=2};
enum class Action:std::uint32_t {acquire=1,renew=2,release=3};
enum class Phase:std::uint32_t {unavailable=0,observing=1,owned=2,released=3};
enum class Reason:std::uint32_t {none=0,unsupported=1,identity=2,lifecycle=3,configuration=4,stale=5,owner=6,expired=7,stopped=8,disconnected=9,overflow=10,binding=11,loading=12};
enum SampleFlag:std::uint32_t {initialized=1,paused=2,frame_known=4};
// Canonical indices. Actual signed surface degrees; normalized physical travel [0,1].
// Controls: outL,inL,inR,outR ailerons, same elevators, upper/lower rudder, stabilizer.
// Spoilers: 1,2,3,4,5,6+7,8,9,10,11,12; degrees, positive deployed.
// High lift: LE outL,midL,inL,inR,midR,outR then TE outL,inL,inR,outR.
// Gear: nose,wingL,wingR,bodyL,bodyR deployment then same five door-open travels.
// Reversers: four engines' non-stowed endpoints (0 stowed,1 out); no travel claim.
// Lights: outboard landing L/R,inboard landing L/R,turnoff L/R,taxi,
// upper/lower beacon,nav L/R,strobe,wing,logo. Powered PSX levels 0..9;
// 1..3 heating only,4..9 emitting; not linear brightness. Binary fallback 0/9.
inline constexpr std::array<std::uint32_t,group_count> component_counts{11,11,10,10,4,14};
constexpr std::uint32_t component_mask(Group group) noexcept {
 const auto i=static_cast<std::size_t>(group);return i<group_count?(1u<<component_counts[i])-1:0;
}
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {Role role{Role::plugin};motion::InstanceId instance{};std::uint32_t pid{},groups{};std::uint64_t process_creation_filetime{},qpc_frequency{};};
struct Subscribe {motion::InstanceId instance{};std::uint64_t owner{},qpc_frequency{};std::uint32_t pid{};};
struct Sample {
 motion::InstanceId producer{};std::uint64_t lifecycle{},configuration{},sequence{},writer_version{},frame_sequence{};
 std::int64_t sample_start_qpc{},sample_end_qpc{},received_qpc{};
 Group group{Group::controls};Source source{Source::native};
 std::uint32_t flags{},valid_mask{},approximation_mask{};
 std::array<float,component_capacity> values{};
 // Native: physical copy time/source sequence. Public: receipt time/record sequence.
 // Invalid slots are zero. A group containing public records is not atomic.
 std::array<std::int64_t,component_capacity> component_qpc{};
 std::array<std::uint64_t,component_capacity> component_sequence{};
};
struct Control {motion::InstanceId producer{};std::uint64_t owner{},operation{},lifecycle{},configuration{};Action action{Action::acquire};std::uint32_t groups{},duration_ms{lease_ms};};
struct Ack {Control control{};Phase phase{Phase::observing};Reason reason{Reason::none};std::uint32_t accepted_groups{};};
struct Applied {motion::InstanceId producer{},provider{};std::uint64_t owner{},lifecycle{},configuration{},sequence{},aircraft_generation{};std::int64_t applied_qpc{};Group group{Group::controls};Phase phase{Phase::observing};Reason reason{Reason::none};std::uint32_t valid_mask{},approximation_mask{},readback_mask{};std::array<float,component_capacity> requested{},readback{};};
struct Status {motion::InstanceId producer{};std::uint64_t lifecycle{},configuration{};std::uint32_t groups{};Reason reason{Reason::none};};
struct Capture {std::uint64_t owner{},generation{};std::uint32_t enabled{};};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Sample&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
#define PSX_AIRCRAFT_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept;bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_AIRCRAFT_CODEC(Hello)
PSX_AIRCRAFT_CODEC(Subscribe)
PSX_AIRCRAFT_CODEC(Sample)
PSX_AIRCRAFT_CODEC(Control)
PSX_AIRCRAFT_CODEC(Ack)
PSX_AIRCRAFT_CODEC(Applied)
PSX_AIRCRAFT_CODEC(Status)
PSX_AIRCRAFT_CODEC(Capture)
#undef PSX_AIRCRAFT_CODEC
using FrameHandler=bool (*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:
 bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
 void reset() noexcept {used_=0;needed_=header_size;failed_=false;}
 bool failed() const noexcept {return failed_;}
private:std::array<std::uint8_t,max_frame> bytes_{};std::size_t used_{},needed_{header_size};bool failed_{};
};
}
