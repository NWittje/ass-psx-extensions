#pragma once
#include "motion_controller.hpp"
#include <psx/status/protocol.hpp>

namespace psx::xp {
struct StatusContext {
    bool enabled{},aircraft_loaded{};
    std::uint64_t enable{},aircraft{},scenery{},local_frame{},callback_count{};
    bool lifecycle_pending{};
};
// Fixed SDK-owner observer; produces facts only. No transport, formatting or writer.
class StatusSource final {
public:
    using Sink=void (*)(void*,const status::Snapshot&,bool) noexcept;
    void begin(SdkApi&,motion::InstanceId,std::uint64_t,std::int64_t (*)() noexcept,Sink,void*) noexcept;
    void observe(const StatusContext&,const MotionController&,bool forced=false,std::uint32_t notification=0) noexcept;
    const status::Snapshot& latest() const noexcept {return latest_;}
private:
    SdkApi* sdk_{};motion::InstanceId producer_{};std::uint64_t frequency_{},sdk_generation_{},sequence_{},lifecycle_{1};
    std::int64_t (*clock_)() noexcept{};Sink sink_{};void* context_{};
    XPLMDataRef async_{};status::Snapshot latest_{};
    void bind() noexcept;
};
}
