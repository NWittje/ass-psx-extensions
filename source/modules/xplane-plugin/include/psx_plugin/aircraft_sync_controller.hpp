#pragma once
#include <psx/sync/presentation.hpp>
#include "sdk_api.hpp"
#include "motion_support.hpp"
#include "aircraft_sync_types.hpp"

namespace psx::xp {
class AircraftController final {
public:
    bool begin(SdkApi&,motion::InstanceId,std::uint64_t,std::int64_t (*)()) noexcept;
    void retire() noexcept;
    void profile(const MotionProfile&) noexcept;
    void lifecycle(std::uint64_t,bool) noexcept;
    void connection(std::uint64_t) noexcept;
    aircraft::Ack command(const AircraftCommand&) noexcept;
    void sample(const AircraftSample&) noexcept;
    void tick(bool connected,bool loading) noexcept;
    const std::array<aircraft::Applied,aircraft::group_count>& applied() const noexcept {return applied_;}
    std::uint32_t owned() const noexcept {return owned_;}
private:
    struct Binding {XPLMDataRef ref{};int index{-1};float previous{},written{};bool have_previous{},have_written{},uncertain{};};
    struct Proxy {AircraftController* owner{};XPLMDataRef ref{},fallback{};int fallback_index{-1};
        unsigned group{},component{};bool door{},spill{},active{};};
    struct GroupState {AircraftCommand command{};AircraftSample input{};std::int64_t expires{};bool pending{},have_sample{};
        psx::sync::ValueHistory history;std::array<float,32> rendered{};};
    SdkApi* sdk_{};motion::InstanceId provider_{};std::uint64_t frequency_{},generation_{},connection_{};
    std::int64_t (*clock_)(){};std::int64_t floor_{};
    std::uint32_t owned_{};bool qualified_{},loading_{true};XPLMDataRef owned_ref_{},components_ref_{};
    bool lifecycle_pending_{},pending_loading_{},have_pending_profile_{};std::uint64_t pending_generation_{};
    std::int64_t lifecycle_retry_qpc_{};MotionProfile pending_profile_{};
    std::array<GroupState,aircraft::group_count> groups_{};
    std::array<aircraft::Applied,aircraft::group_count> applied_{};
    std::array<std::array<Binding,aircraft::component_capacity>,aircraft::group_count> bindings_{};
    std::array<Proxy,64> proxies_{};std::size_t proxy_count_{};
    std::array<std::uint32_t,aircraft::group_count> capabilities_{};
    bool proxy(const char*,unsigned,unsigned,const char*,int,bool=false,bool=false,bool=false) noexcept;
    void bind(const char*,unsigned,unsigned,int=-1) noexcept;
    void release(unsigned,aircraft::Reason) noexcept;
    bool read(const Binding&,float&,bool=false) noexcept;
    bool write(Binding&,float) noexcept;
    bool restore(Binding&) noexcept;
    bool restoration_pending(unsigned) const noexcept;
    bool resume_lifecycle(bool=false) noexcept;
    float proxy_value(const Proxy&) noexcept;
    static float read_proxy(void*) noexcept;
    static int read_spill(void*,float*,int,int) noexcept;
    static int read_owned(void*) noexcept;
    static int read_components(void*,float*,int,int) noexcept;
};
}
