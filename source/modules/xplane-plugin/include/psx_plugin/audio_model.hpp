#pragma once
#include "motion_support.hpp"
#include <psx/audio/audio.hpp>
#include <string_view>

namespace psx::xp {
enum class Trigger {engine_run,engine_crank,engine_ignite,engine_shutdown,reverse,gear,hydraulic,boolean,apu_run,apu_start,apu_stop,wind,flap_move,flap_stop,fuel_stop,pack,pack_stop,snapshot};
struct EventSpec {const char* path;const char* bank;audio::Group group;audio::Family family;std::uint32_t index;Trigger trigger;motion::Vec3 position;
    std::array<std::uint8_t,16> guid{};bool oneshot{},is3d{};};
struct AssetSpec {const char* name;std::uint64_t bytes;const char* sha256;};
#include "../../assets/sparky_audio_manifest.inc"
inline constexpr std::size_t audio_event_count=std::size(sparky_events),audio_parameter_capacity=16;
struct AudioHost {
    std::uint64_t connection{},plugin_generation{},aircraft_generation{};
    bool connected{},follow{};MotionProfile profile{};float flight_time{};bool flight_time_valid{};
    std::array<char,512> aircraft_path{};
};
struct Input {
    enum class Kind {missing,constant,field,flight_time,family,eye_y,eye_z} kind{};
    audio::Group group{};std::uint32_t field{};float scale{1},constant{};
};
struct Evaluation {bool available{},on{};};
Input parameter_source(std::string_view,const EventSpec&) noexcept;
bool input_value(const Input&,const audio::State&,const AudioHost&,float&) noexcept;
Evaluation evaluate(const EventSpec&,const audio::State&) noexcept;
bool transition_matches(const EventSpec&,const audio::Transition&) noexcept;
inline bool event_family(const EventSpec& e,audio::Family family) noexcept {return e.family==audio::Family::unknown||e.family==family;}
inline bool edge_event(Trigger t) noexcept {return t==Trigger::engine_ignite||t==Trigger::engine_shutdown||t==Trigger::apu_start||t==Trigger::apu_stop||t==Trigger::flap_stop||t==Trigger::fuel_stop||t==Trigger::pack_stop;}
// Independent sources retain their own stamps; no smoothing, spool estimation
// or conversion of commands to physical state occurs here.
class AudioStateGate final {
public:
    void reset() noexcept {value_={};retained_={};seen_=0;}
    const audio::State& value() const noexcept {return value_;}
    bool accept(const audio::State& s,std::uint64_t connection) noexcept;
private:audio::State value_{};std::array<audio::Snapshot,audio::group_count> retained_{};std::uint32_t seen_{};
};
} // namespace psx::xp
