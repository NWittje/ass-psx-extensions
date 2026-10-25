#pragma once
#include <psx/sync/presentation.hpp>
#include "audio_model.hpp"

namespace psx::xp {
struct AudioVector {float x{},y{},z{};};
struct AudioAttributes {AudioVector position{},velocity{},forward{0,0,1},up{0,1,0};};
struct AudioParameterId {std::uint32_t first{},second{};};
struct AudioGuid {std::uint32_t first{};std::uint16_t second{},third{};std::array<std::uint8_t,8> last{};};
struct AudioParameterDescription {const char* name{};AudioParameterId id{};float minimum{},maximum{},default_value{};int type{};unsigned flags{};AudioGuid guid{};};
static_assert(sizeof(AudioParameterDescription)==56&&sizeof(AudioAttributes)==48);
// Exact FMOD 2.02 C ABI subset. Runtime resolves only already-loaded modules;
// tests inject the same small table, not a second rendering implementation.
struct FmodApi {
    void* studio{};
    int (*event)(void*,const AudioGuid*,void**){};
    int (*event_id)(void*,AudioGuid*){};
    int (*is3d)(void*,int*){};
    int (*parameter_count)(void*,int*){};
    int (*parameter)(void*,int,AudioParameterDescription*){};
    int (*create)(void*,void**){};
    int (*load_samples)(void*){};
    int (*unload_samples)(void*){};
    int (*sample_state)(void*,int*){};
    int (*oneshot)(void*,int*){};
    int (*set_parameter)(void*,AudioParameterId,float,int){};
    int (*set_attributes)(void*,const AudioAttributes*){};
    int (*set_volume)(void*,float){};
    int (*start)(void*){};
    int (*stop)(void*,int){};
    int (*release)(void*){};
    int (*playback)(void*,int*){};
    int (*listener)(void*,int,AudioAttributes*,AudioVector*){};
    int (*channel_group)(void*,void**){};
    int (*group_mute)(void*,int){};
    int (*group_get_mute)(void*,int*){};
    bool resolve(void* borrowed_studio) noexcept; // No DLL load, device or system creation.
};
bool relative_cockpit_attributes(const AudioAttributes&,motion::Vec3,motion::Vec3,AudioAttributes&) noexcept;
class AudioRenderer final {
public:
    AudioRenderer(FmodApi api,std::uint64_t frequency) noexcept:api_(api),frequency_(frequency){}
    ~AudioRenderer(){release();}
    void host(const AudioHost&) noexcept;
    bool state(const audio::State&) noexcept;
    bool control(const audio::Control&,std::int64_t now) noexcept;
    void transition(const audio::Transition&) noexcept;
    void fault() noexcept;
    void stop_intent() noexcept {stop_all();control_.follow=false;was_follow_=false;}
    void tick(std::int64_t now) noexcept;
    void release() noexcept;
    const audio::Status& status() const noexcept {return status_;}
private:
    struct Parameter {AudioParameterId id{};Input source{};float low{},high{},last{};bool sent{};};
    struct Event {void* description{};void* instance{};void* group{};std::array<Parameter,audio_parameter_capacity> parameters{};std::size_t count{};
        AudioAttributes attributes{};float volume{-1};bool positioned{};
        bool failed{},loaded{},oneshot{},playing{},baseline{},last_on{},pending_stop{},stop_sent{},core_muted{},gate_known{};std::uint64_t last_sequence{},baseline_sequence{},last_start_sequence{};};
    FmodApi api_{};std::uint64_t frequency_{};AudioHost host_{};AudioStateGate gate_{};audio::Control control_{};audio::Status status_{};
    std::array<psx::sync::ValueHistory,audio::group_count> histories_{};
    std::array<Event,audio_event_count> events_{};std::array<audio::Transition,audio::transition_capacity> transitions_{};std::size_t transition_count_{};
    std::uint64_t owner_{},operation_{},blocked_epoch_{};bool faulted_{},was_follow_{};
    std::int64_t retry_after_{};
    std::uint32_t snapshot_scope_{};
    void stop_all() noexcept;
    bool gate(Event&,bool) noexcept;
    void request_stop(Event&) noexcept;
    void service_stops() noexcept;
    std::uint32_t pending_groups() const noexcept;
    void pending_status() noexcept;
    void release_event(Event&) noexcept;
    bool prepare(std::size_t) noexcept;
};
} // namespace psx::xp
