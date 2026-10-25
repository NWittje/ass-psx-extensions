#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
namespace hwio {
class ServoPanel {
    friend struct ServoPanelProbe;
    enum Field:unsigned {device,source,slider,set_min,set_centre,set_max,manual_values,custom0,assign0,custom1,assign1,count};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};
    ass::graphics::Control pane_,list_,detail_,status_,captures_,range_;
    std::array<ass::graphics::Control,10> slots_;
    std::array<ass::graphics::Control,count> controls_;
    std::array<ass::graphics::Control,count> labels_;
    std::vector<MotorDeviceAddress> devices_;std::vector<ServoSource> sources_;
    struct Event{unsigned id,kind;double value;};std::deque<Event> events_;
    int selected_{-1};bool binding_{},visible_{},active_page_{},manual_fields_{};int x_{},y_{},width_{690},height_{664};
    std::optional<ServoDefinition> displayed_;
    std::optional<double> manual_submitted_;
    std::optional<double> pending_slider_;
    std::uint64_t inventory_{UINT64_MAX};std::wstring problem_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void bind();void choices(const PhidgetInventory&);void layout();void select(unsigned);void request_manual(double);bool store(ServoDefinition);
public:
    static constexpr unsigned dispatch_id=118;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);
    void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();void tick();void hide();
    bool back();int selected()const noexcept{return selected_;}
};
}
