#pragma once
#include "runtime.hpp"
#include "motor_mapping_panel.hpp"
#include <ass_graphics/graphics.hpp>
#include <deque>
namespace hwio {
struct ActuatorInputChoice {MotorFeedbackDefinition input;std::wstring name;};
std::vector<ActuatorInputChoice> actuator_input_choices(const RuntimeView&,bool switches);
class StepperPanel {
    friend struct StepperPanelProbe;
    enum Field:unsigned {slot,device,name,mode,feedback,invert,current,holding,velocity,acceleration,steps,gearing,kp,ki,kd,vff,aff,feedback_velocity,feedback_acceleration,backlash,interval,failsafe,inspect,back,forward,stop,save,revert,clear,target,move,zero,count};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};ass::graphics::Control pane_,status_;
    std::array<ass::graphics::Control,count> controls_,labels_;std::vector<MotorDeviceAddress> devices_;std::vector<ActuatorInputChoice> feedback_;
    struct Event{unsigned id,kind;double value;};std::deque<Event> events_;unsigned selected_{};bool binding_{},visible_{},edited_{},held_{};int direction_{},width_{690},height_{664};
    std::uint64_t inventory_{UINT64_MAX};std::optional<StepperDefinition> displayed_;std::wstring problem_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void bind();bool commit();void layout();void choices(const RuntimeView&);
public:
    static constexpr unsigned dispatch_id=119;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();void tick();void hide();
};
class ActuatorPanel {
    friend struct ActuatorPanelProbe;
    enum Field:unsigned {name,kind,output,source,record,field,fields,unit,scale,offset,source_min,source_max,power,power_mask,demand,enabled,automatic,sw1,sw2,active1,active2,two,pot,invert,sensor_min,sensor_max,command_min,command_max,capture_min,capture_max,search_direction,search_distance,search_velocity,latch_velocity,clearance,travel,timeout,debounce,rotary,startup,source_revolution,output_revolution,index_window,direction,apply,revert,clear,home,stop,count};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};ass::graphics::Control pane_,up_,caption_,status_,form_,list_,root_,mode_;
    std::unique_ptr<MotorMappingPanel> motors_;
    std::array<ass::graphics::Control,3> groups_;std::array<ass::graphics::Control,10> items_;std::array<ass::graphics::Control,count> controls_,labels_;
    std::vector<NumericSource> sources_;std::vector<ActuatorInputChoice> switches_,pots_;RuntimeView inventory_;
    struct Event{unsigned id,kind;double value;};std::deque<Event> events_;int group_{-1},selected_{-1},output_{-1},width_{690},height_{664};bool binding_{},visible_{},edited_{},manual_home_{},axis_mode_{};std::wstring problem_;
    ActuatorMapping draft_;std::optional<ActuatorMapping> displayed_;std::uint64_t input_revision_{UINT64_MAX};
    std::array<float,63> scroll_{};
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void layout();void navigate(int,int);void bind();bool commit();void choices();void output_choices();
public:
    static constexpr unsigned dispatch_id=120;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void dispatch();void hide();
    void mapping_dispatch(){if(motors_)motors_->dispatch();}
};
}
