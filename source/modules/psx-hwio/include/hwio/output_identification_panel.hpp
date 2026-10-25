#pragma once
#include "output_atlas.hpp"
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <application-icon.hpp>

namespace hwio {
class OutputIdentificationPanel {
    friend struct OutputIdentificationPanelProbe;
    ass::graphics::Context* context_{};
    Runtime* runtime_{};
    std::unique_ptr<ass::graphics::Window> window_;
    std::optional<psx::windows::WindowIcons> icons_;
    ass::graphics::Control pane_,status_,message_,navigation_,footer_,up_;
    std::array<ass::graphics::Control,4> buttons_;
    std::unique_ptr<ass::graphics::Canvas> image_;
    std::vector<ass::graphics::Control> breadcrumbs_,children_,breadcrumb_captions_,child_captions_;
    std::vector<std::uint32_t> child_regions_;
    OutputIdentification model_;
    OutputAtlas atlas_;
    HardwareConfiguration original_;
    std::filesystem::path atlas_directory_;
    IdentificationGesture rendered_{},pressed_{};
    std::uint32_t region_{},pressed_region_{OutputAtlasRegion::none};
    std::uint64_t image_submission_{},image_target_{},pressed_target_{},next_inventory_{};
    ag_rect fitted_{};
    AtlasRectangle view_{};
    bool shown_{},layout_dirty_{true},updating_{},review_{},finishing_{},servicing_{};
    int width_{},height_{};
    IdentificationPhase displayed_phase_{IdentificationPhase::idle};
    std::wstring status_text_,message_text_;
    struct Action{unsigned id{};IdentificationGesture token;std::uint32_t region{OutputAtlasRegion::none};};
    std::vector<Action> actions_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    static void __cdecl pointer(void*,std::uint32_t,ag_point)noexcept;
    std::uint32_t hit(ag_point)const noexcept;
    bool image_ready()const noexcept;
    void create_window(ass::graphics::Context&);
    void dispatch();void service();void render();void layout();void controls();
    void send_requests();void activate(std::uint32_t,IdentificationGesture);
public:
    ~OutputIdentificationPanel();
    bool show(ass::graphics::Context&,Runtime&,const std::string& device,std::string& error);
    void refresh();
    void hide();
};
}
