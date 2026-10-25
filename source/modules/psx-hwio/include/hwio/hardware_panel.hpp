#pragma once
#include "runtime.hpp"
#include "card_transfer_panel.hpp"
#include <ass_graphics/graphics.hpp>
#include <application-icon.hpp>
#include <optional>
namespace hwio {
// Per-card host options and vendor readback, reached from Modules. No setup writes.
class HardwarePanel {
    friend struct HardwarePanelProbe;
    Runtime* runtime_{};
    std::unique_ptr<ass::graphics::Window> window_;
    std::optional<psx::windows::WindowIcons> icons_;
    ass::graphics::Control pane_,use_,poext_,identity_,address_,firmware_,status_,message_,close_;
    std::array<ass::graphics::Control,2> headings_;
    struct PinRow {ass::graphics::Control number,function;};
    std::array<PinRow,55> pins_;
    CardTransferPanel transfer_;
    std::string key_;
    EthernetHealth readback_;
    bool shown_{},updating_{},layout_dirty_{true};
    struct Event {unsigned id{},kind{};std::string key;};
    std::vector<Event> events_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    void dispatch();void layout();
public:
    ~HardwarePanel();
    void show(ass::graphics::Context&,Runtime&,const std::string& key,bool visible=true);
    void hide();void refresh(const RuntimeView&);
    void remember(const EthernetHealth& readback){if(readback.key==key_ && readback.configured){readback_=readback;layout_dirty_=true;}}
};
} // namespace hwio
