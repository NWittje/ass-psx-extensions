#pragma once
#include <psx/motion/full_pose.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::environment {
inline constexpr std::uint16_t protocol_version=1;
inline constexpr std::uint32_t environment_jni_abi=1;
inline constexpr std::size_t field_capacity=32,header_size=16,max_payload=2048,max_frame=header_size+max_payload;
inline constexpr std::uint32_t renew_ms=250,lease_ms=1000,source_stale_ms=1000,snapshot_interval_ms=200,weather_interval_ms=1000;
inline constexpr std::size_t ordered_capacity=16;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.environment.plugin.v1.";
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.environment.exporter.v1.";
enum class Group:std::uint32_t {time=1,weather=2};
inline constexpr std::uint32_t all_groups=3;
enum class Field:std::uint32_t {clock_rate=0,latitude_deg=1,longitude_deg=2,altitude_msl_m=3,weather_datum_msl_m=4,
 upper_coverage=5,upper_base_msl_m=6,upper_top_msl_m=7,lower_coverage=8,lower_base_msl_m=9,lower_top_msl_m=10,
 cb_coverage=11,cb_base_msl_m=12,cb_top_msl_m=13,wind_from_deg_true=14,wind_speed_mps=15,
 temperature_c=16,qnh_pa=17,visibility_m=18,precipitation_level=19,pressure_at_aircraft_pa=20,precipitation_ratio=21,temperature_anchor_msl_m=22,pressure_altitude_m=23};
constexpr std::uint32_t bit(Field f) noexcept {return 1u<<static_cast<unsigned>(f);}
inline constexpr std::uint32_t all_fields=(1u<<24)-1,reference_fields=0x1e,cloud_fields=0x3fe0,weather_fields=all_fields&~1u;
enum class Role:std::uint32_t {plugin=1,exporter=2};
enum class Kind:std::uint16_t {hello=1,subscribe=2,sample=3,control=4,ack=5,applied=6,status=7,capture=8};
enum class Action:std::uint32_t {acquire=1,renew=2,release=3};
enum class Phase:std::uint32_t {unavailable=0,observing=1,owned=2,released=3};
enum class Reason:std::uint32_t {none=0,unsupported=1,identity=2,lifecycle=3,configuration=4,stale=5,owner=6,expired=7,stopped=8,disconnected=9,overflow=10,binding=11,loading=12};
enum SampleFlag:std::uint32_t {initialized=1,paused=2,motion_frozen=4,visibility_unlimited=8,utc_valid=16,configured_utc_valid=32};
enum AppliedFlag:std::uint32_t {regenerated=1,clouds_settling=2,calendar_year_approximate=4,opacity_approximate=8,local_profile_approximate=16,clear_visibility_approximate=32,location_readback_detailed=64};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {Role role{Role::plugin};motion::InstanceId instance{};std::uint32_t pid{},groups{};std::uint64_t process_creation_filetime{},qpc_frequency{};};
struct Subscribe {motion::InstanceId instance{};std::uint64_t owner{},qpc_frequency{};std::uint32_t pid{};};
// All weather fields are current consumed quantities; configured[] are diagnostic only.
// AGL conversion uses altitude-height captured in the same qualified native phase.
// Source UTC is the applied Earth clock, not the public request. No calendar truncation.
struct Sample {
 motion::InstanceId producer{};std::uint64_t lifecycle{},configuration{},sequence{},writer_version{},frame_sequence{};
 std::int64_t sample_start_qpc{},sample_end_qpc{},received_qpc{},utc_millis{},configured_utc_millis{};
 std::uint32_t flags{},valid_mask{},configured_mask{};
 std::array<double,field_capacity> values{},configured{};
};
// configuration is a cloud-configuration identity, not a reference/movement sequence.
struct Control {motion::InstanceId producer{};std::uint64_t owner{},operation{},lifecycle{},configuration{};Action action{Action::acquire};std::uint32_t groups{},duration_ms{lease_ms};};
struct Ack {Control control{};Phase phase{Phase::observing};Reason reason{Reason::none};std::uint32_t accepted_groups{};};
// Accepted regional settings and actual aircraft weather are deliberately separate.
struct Applied {
 Sample source{};motion::InstanceId provider{};std::uint64_t owner{},provider_generation{},regeneration_sequence{};
 std::int64_t applied_qpc{},readback_qpc{},accepted_utc_millis{},actual_utc_millis{};
 std::int64_t regeneration_begin_qpc{},regeneration_end_qpc{},settling_begin_qpc{},settling_end_qpc{};
 std::int64_t location_begin_qpc{},location_end_qpc{};
 std::uint32_t groups{},flags{},accepted_mask{},actual_mask{},approximation_mask{},location_mask{};
 Phase phase{Phase::observing};Reason reason{Reason::none};
 std::array<double,field_capacity> accepted{},actual{},location{};
};
struct Status {motion::InstanceId producer{};std::uint64_t lifecycle{},configuration{};std::uint32_t groups{};Reason reason{Reason::none};};
struct Capture {std::uint64_t owner{},generation{};std::uint32_t enabled{};};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool valid(const Sample&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
#define PSX_ENVIRONMENT_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept;bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_ENVIRONMENT_CODEC(Hello)
PSX_ENVIRONMENT_CODEC(Subscribe)
PSX_ENVIRONMENT_CODEC(Sample)
PSX_ENVIRONMENT_CODEC(Control)
PSX_ENVIRONMENT_CODEC(Ack)
PSX_ENVIRONMENT_CODEC(Applied)
PSX_ENVIRONMENT_CODEC(Status)
PSX_ENVIRONMENT_CODEC(Capture)
#undef PSX_ENVIRONMENT_CODEC
using FrameHandler=bool (*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:
 bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
 void reset() noexcept {used_=0;needed_=header_size;failed_=false;}
 bool failed() const noexcept {return failed_;}
private:std::array<std::uint8_t,max_frame> bytes_{};std::size_t used_{},needed_{header_size};bool failed_{};
};
}
