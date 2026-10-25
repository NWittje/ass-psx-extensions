#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <CommCtrl.h>
namespace hwio {
class InputOverridePanel {
    friend struct InputOverridePanelProbe;
    ass::graphics::Context* context_{};
    Runtime* runtime_{};
    ass::graphics::Control pane_,clear_,capture_,search_,add_,status_,list_;
    HWND popup_{},owner_{};
    std::vector<InputOverrideControl> controls_;
    std::pair<std::size_t,std::size_t> catalogue_shape_{};
    std::vector<std::size_t> matches_;
    std::string selected_;
    std::map<std::string,std::pair<ass::graphics::Control,ass::graphics::Control>> rows_;
    std::vector<std::string> row_ids_;
    std::uint64_t revision_{UINT64_MAX};
    bool binding_{},visible_{},pointer_down_{};
    int width_{600},height_{480};
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double) noexcept;
    static LRESULT CALLBACK edit_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    static LRESULT CALLBACK popup_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    static LRESULT CALLBACK host_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    bool position_popup();
    void filter();
    void select();
    void action(unsigned);
public:
    ~InputOverridePanel();
    void attach(ass::graphics::Context&,Runtime&,void*);
    void arrange(int,int,bool);
    void refresh(const RuntimeView&);
    void hide();
};
}
