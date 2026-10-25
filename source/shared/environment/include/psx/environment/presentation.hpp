#pragma once
#include "protocol.hpp"
#include <span>

// Independently versioned PXW2 envelopes. PXW1/JNI1 Sample and Applied stay unchanged.
namespace psx::environment::presentation {
inline constexpr std::uint16_t version=2;
inline constexpr std::uint32_t magic=0x32575850u;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.environment.plugin.v2.";
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.environment.exporter.v2.";
inline constexpr char plugin_pipe_prefix3[]="\\\\.\\pipe\\psx-ios.environment.plugin.v3.";
inline constexpr char exporter_pipe_prefix3[]="\\\\.\\pipe\\psx-ios.environment.exporter.v3.";
inline constexpr char plugin_pipe_prefix4[]="\\\\.\\pipe\\psx-ios.environment.plugin.v4.";
inline constexpr char exporter_pipe_prefix4[]="\\\\.\\pipe\\psx-ios.environment.exporter.v4.";
inline constexpr std::uint32_t context_jni_abi=1;
enum Capability:std::uint32_t {source_context=1,visibility_presentation=2,manual_weather=4,local_visibility_target=8};
enum class Kind:std::uint16_t {hello=1,subscribe=2,sample=3,control=4,ack=5,applied=6,status=7,capture=8,
 source=9,presentation=10,applied_presentation=11,visibility_request=12,visibility_result=13};
enum class Target:std::uint32_t {planet=0,zone1=1,other=2,unknown=3,all=4};
enum ContextValid:std::uint32_t {active_target_valid=1,planet_visibility_valid=2,zone1_visibility_valid=4,zone1_station_valid=8};
struct Hello {environment::Hello base{};std::uint32_t capabilities{};};
struct Context {
 std::uint64_t source_sequence{};
 std::int64_t observation_start_qpc{},observation_end_qpc{};
 std::uint32_t valid_mask{};std::int32_t active_native_zone{-1};
 std::uint32_t planet_visibility_m{},zone1_visibility_m{};
 std::array<char,8> zone1_station{};
 bool operator==(const Context&)const=default;
};
// PXW3 only. Zero revision means qualified observation before the first manual edit.
// A completion belongs to its original lifecycle and clock; it is never retimed.
struct WeatherChange {
 std::uint64_t revision{},lifecycle{};std::int64_t completed_qpc{};
 std::uint32_t qualified{},reason{}; // 1=panel edit, 2=accepted Main command, 3=native load
 bool operator==(const WeatherChange&)const=default;
};
// PXW4: final local visibility before PSX's native smoother, with original phase.
// The raw Sample continues to describe the currently consumed atmosphere.
struct LocalVisibility {std::uint64_t lifecycle{};std::int64_t observed_qpc{};double metres{};std::uint32_t qualified{};bool operator==(const LocalVisibility&)const=default;};
struct Source {environment::Sample sample{};Context context{};WeatherChange change{};LocalVisibility local_visibility{};};
enum class PolicyKind:std::uint32_t {native=0,sentinel_finite=1,extended=2,local_target=3};
struct VisibilityPolicy {
 PolicyKind kind{PolicyKind::native};Target target{Target::unknown};
 std::uint64_t owner{},lifecycle{},source_sequence{},selection_revision{};
 double visibility_m{};
 bool operator==(const VisibilityPolicy&)const=default;
};
struct WeatherTarget {std::uint64_t identity{};Source source{};VisibilityPolicy visibility{};};
struct Presentation {Source source{};VisibilityPolicy visibility{};WeatherTarget weather{};};
struct AppliedPresentation {environment::Applied applied{};VisibilityPolicy visibility{};
 std::uint64_t weather_target{},current_lifecycle{},current_sequence{};std::int64_t current_source_end_qpc{},weather_observed_qpc{};};

// Same authenticated coordinator IPC endpoint as PCN1, distinguished by PXW2 framing.
// No reinterpretation of actor/reset PCN1 fields or command meanings.
struct ProcessIdentity {std::uint32_t pid{};std::uint64_t creation_filetime{};bool operator==(const ProcessIdentity&)const=default;};
enum class VisibilityCommand:std::uint32_t {query=0,select=1,clear=2};
enum class VisibilityPhase:std::uint32_t {unavailable=0,pending=1,selected=2,released=3,rejected=4};
enum class VisibilityReason:std::uint32_t {none=0,unsupported=1,identity=2,expired=3,disconnected=4,busy=5,malformed=6,
 source_stale=7,context_unavailable=8,inactive_target=9,configured_edit=10,station_changed=11,lifecycle=12,
 not_following=13,plugin_unavailable=14,binding=15,pending_readback=16};
struct VisibilityRequest {
 VisibilityCommand command{VisibilityCommand::query};ProcessIdentity client{},server{},psx{};
 std::uint64_t session{},request{},qpc_frequency{};
 std::int64_t issued_qpc{},deadline_qpc{},configured_readback_qpc{};
 Target target{Target::planet};double commanded_visibility_m{};
 std::array<char,8> zone1_station{};
 bool operator==(const VisibilityRequest&)const=default;
};
enum VisibilityResultValid:std::uint32_t {commanded_valid=1,configured_valid=2,accepted_valid=4,actual_valid=8};
struct VisibilityResult {
 ProcessIdentity server{};std::uint64_t session{},request{},selection_revision{};
 std::int64_t observed_qpc{},source_observation_qpc{},application_qpc{};
 Target target{Target::planet},active_target{Target::unknown};
 VisibilityPhase phase{VisibilityPhase::unavailable};VisibilityReason reason{VisibilityReason::unsupported};
 std::uint32_t valid_mask{},weather_requested{},weather_owned{},presentation_available{};
 double commanded_visibility_m{},configured_visibility_m{},accepted_visibility_m{},actual_visibility_m{};
 bool operator==(const VisibilityResult&)const=default;
};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};std::uint16_t wire_version{version};};
bool read_header(const std::uint8_t*,std::size_t,Header&)noexcept;
bool valid(const Source&)noexcept;bool valid(const Presentation&)noexcept;
bool valid(const VisibilityRequest&)noexcept;bool valid(const VisibilityResult&)noexcept;
bool same_sample(const environment::Sample&,const environment::Sample&)noexcept;
#define PSX_PRESENTATION_CODEC(T) bool encode(const T&,std::uint32_t,environment::Frame&)noexcept;bool decode(std::span<const std::uint8_t>,T&)noexcept;
PSX_PRESENTATION_CODEC(Hello)
PSX_PRESENTATION_CODEC(Source)
PSX_PRESENTATION_CODEC(Presentation)
PSX_PRESENTATION_CODEC(AppliedPresentation)
PSX_PRESENTATION_CODEC(VisibilityRequest)
PSX_PRESENTATION_CODEC(VisibilityResult)
#undef PSX_PRESENTATION_CODEC
// Base-message wrappers retain the original PXW1 payload bytes exactly.
bool encode(const environment::Subscribe&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Sample&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Applied&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Control&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Ack&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Status&,std::uint32_t,environment::Frame&)noexcept;
bool encode(const environment::Capture&,std::uint32_t,environment::Frame&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Subscribe&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Sample&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Applied&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Control&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Ack&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Status&)noexcept;
bool decode(std::span<const std::uint8_t>,environment::Capture&)noexcept;
using Handler=bool(*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
 std::array<std::uint8_t,environment::max_frame> bytes_{};std::size_t used_{},needed_{environment::header_size};bool failed_{};
public:
 bool feed(const void*,std::size_t,Handler,void*)noexcept;
 void reset()noexcept {used_=0;needed_=environment::header_size;failed_=false;}
 bool failed()const noexcept{return failed_;}
};
}
