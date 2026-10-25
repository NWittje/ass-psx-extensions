#pragma once
#include "runtime.hpp"
#include <ass_graphics/graphics.hpp>
#include <deque>
namespace hwio {
class MotorMappingPanel {
    friend struct MotorMappingPanelProbe;
    enum Field:unsigned {axis,motor,home,stop,clear,automatic,device,channel,low,high,status,count};
    struct Row {std::array<ass::graphics::Control,count> controls;std::array<ag_rect,count> bounds{};std::vector<Profile> sources;std::vector<DeviceIdentity> devices;std::uint32_t edited{},pending{},visibility{};bool shown{};std::uint64_t inventory{UINT64_MAX};};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};
    ass::graphics::Control pane_,add_;
    struct Event{unsigned id{},kind{};double value{};};
    std::array<Row,motor_axes> rows_;RuntimeView inventory_;std::deque<Event> events_;
    bool binding_{},visible_{},layout_dirty_{true};int width_{},height_{},output_{-1};std::wstring problem_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void layout();void update();bool commit(unsigned,unsigned,double);
public:
    static constexpr unsigned dispatch_id=115;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);
    void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();void hide();
    void select_output(int);
};
}
