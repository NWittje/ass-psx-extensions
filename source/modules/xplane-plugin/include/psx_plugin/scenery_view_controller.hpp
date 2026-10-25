#pragma once
#include "sdk_api.hpp"
#include <psx/bridge/protocol.hpp>
#include <XPLMUtilities.h>
#include <array>

namespace psx::xp {
// Reports command/readback ownership, never a claim about pixels or another plugin's overlays.
using SceneryViewState=bridge::ViewState;
class SceneryViewController final {
public:
    SceneryViewController()=default;
    ~SceneryViewController(){stop();}
    SceneryViewController(const SceneryViewController&)=delete;
    SceneryViewController& operator=(const SceneryViewController&)=delete;
    void begin(SdkApi&,std::int64_t (*)() noexcept,std::uint64_t frequency) noexcept;
    void stop() noexcept;
    void select(bool) noexcept;
    void tick() noexcept;
    bridge::ViewReply request(const bridge::ViewRequest&,std::uint64_t connection,std::uint64_t load,std::uint64_t enable) noexcept;
    bool selected() const noexcept { return selected_; }
    SceneryViewState state() const noexcept { return state_; }
    int observed_view() const noexcept { return observed_; }
private:
    static int block(XPLMCommandRef,XPLMCommandPhase,void*) noexcept;
    static int observed(XPLMCommandRef,XPLMCommandPhase,void*) noexcept;
    bool bind() noexcept;
    SdkApi* sdk_{};
    std::int64_t (*clock_)() noexcept{};
    std::uint64_t frequency_{},generation_{};
    std::int64_t last_attempt_{};
    XPLMCommandRef target_{};
    XPLMDataRef view_{};
    std::array<XPLMCommandRef,64> blocked_{};
    std::size_t blocked_count_{};
    unsigned failures_{};
    int expected_{},observed_{};
    bool selected_{},hooked_{},sampled_{},attempted_{};
    SceneryViewState state_{SceneryViewState::inactive};
    bridge::ViewRequest last_request_{};
    bridge::ViewReason last_reason_{bridge::ViewReason::none};
    std::uint64_t connection_{};
};
}
