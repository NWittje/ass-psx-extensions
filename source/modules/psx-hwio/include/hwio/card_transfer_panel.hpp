#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <application-icon.hpp>

namespace hwio {
// Shared card-property controls. The confirmation retains the parsed proposal,
// never reopens a potentially changed XML after the user confirms it.
class CardTransferPanel {
    friend struct CardTransferPanelProbe;
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};
    std::unique_ptr<ass::graphics::Window> confirmation_;
    std::optional<psx::windows::WindowIcons> icons_;
    ass::graphics::Control export_,import_,message_,warning_,confirm_,cancel_;
    std::optional<CardImport> proposal_;
    std::string key_;std::vector<unsigned> events_;bool dispatching_{},layout_dirty_{true};
    std::uint64_t epoch_{};
    std::function<void()> reflow_;
    void message(const std::wstring& text){if(message_.text()!=text){message_.text(text.c_str());layout_dirty_=true;}}
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void dispatch();void choose(bool importing);void finish(bool confirmed);
    void offer(CardImport,bool visible=true);
public:
    void attach(ass::graphics::Context&,ass::graphics::Window&,void* parent,Runtime&);
    void device(const std::string&);
    float arrange(float x,float y,float width);
    void hide();
    bool layout_dirty()const noexcept{return layout_dirty_;}
    void reflow(std::function<void()> f){reflow_=std::move(f);}
    void visible(bool value){if(!value)hide();if(context_)for(auto* c:{&export_,&import_,&message_})ag_visible(c->handle(),value);}
};
}
