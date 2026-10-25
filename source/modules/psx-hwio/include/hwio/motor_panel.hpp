#pragma once
#include "runtime.hpp"
#include "motor_mapping_panel.hpp"
#include "servo_panel.hpp"
#include "actuator_panel.hpp"
#include <ass_graphics/graphics.hpp>
#include <deque>
namespace hwio {
class MotorPanel {
    friend struct MotorPanelProbe;
    enum Field:unsigned {device,channel,clear,minimum,maximum,max_velocity,max_acceleration,current,velocity,acceleration,backward,forward,zero,disengage,save,revert,kp,ki,kd,vff,aff,wave,amplitude,play,absolute,fb_kind,fb_device,fb_channel,fb_invert,drive_expand,max_duty,pwm_ramp,native_velocity,native_acceleration,speed_kp,speed_ki,speed_kd,speed_deadband,speed_feedback,sensor_buffer,auto_tune,tune_window,backlash,find_backlash,find_last,field_count};
    struct Detail {
        ass::graphics::Control pane;
        std::array<ass::graphics::Control,field_count> controls;
        std::vector<ass::graphics::Control> labels;
        std::unique_ptr<ass::graphics::Chart> chart;
        std::vector<MotorDeviceAddress> channels;
        std::vector<std::string> devices,feedback_devices;
        std::vector<MotorFeedbackDefinition> feedback_sources;MotorFeedbackDefinition displayed_feedback;
        MotorFeedbackDefinition temporary_pot;bool find_select{},find_seen{};
        std::string selected_feedback_device;std::uint64_t feedback_inventory{UINT64_MAX};bool drive_open{};
        std::string selected_device;
        std::optional<MotorDefinition> submitted;
        MotorDefinition draft;std::uint64_t inventory{UINT64_MAX},history{UINT64_MAX},end{};
        MotorDeviceAddress displayed_address;
        std::uint64_t edited{},choice_pending{}; // Native text remains authoritative until its deferred commit succeeds.
        int width{};float scroll{},tune_height{};bool valid{},created{},wave_pending{};
    };
    struct Event {unsigned id{},kind{};double value{};int slot{-1};};
    ass::graphics::Context* context_{};ass::graphics::Window* owner_{};Runtime* runtime_{};MotorBench* bench_{};
    ass::graphics::Control pane_,up_,breadcrumb_,message_,root_,list_;
    std::array<ass::graphics::Control,4> groups_;
    std::array<ass::graphics::Control,motor_slots> motors_;
    std::array<Detail,motor_slots> details_;
    ServoPanel servos_;StepperPanel steppers_;
    PhidgetInventory inventory_;std::vector<DeviceInfo> input_devices_;std::uint64_t feedback_inventory_{};std::deque<Event> events_;
    LastAxisMovement last_axis_;
    int group_{-1},slot_{-1},manual_slot_{-1},x_{},y_{},width_{690},height_{664};bool visible_{},binding_{},arranged_{},held_{},hide_sent_{};
    static constexpr bool edit_field(unsigned f){return (f>=minimum && f<=current) || (f>=kp && f<=aff) || f==amplitude || f==sensor_buffer || f==backlash || (f>=max_duty && f<=speed_deadband);}
    std::wstring problem_;
    static void __cdecl event(void*,std::uint32_t,std::uint32_t,double)noexcept;
    void navigate(int,int);void detail(unsigned);void layout();void update();bool commit(unsigned,bool tuning=false);void graph(unsigned,const MotorBenchView&);
    void manual_page();void feedback_choices(unsigned);
    const std::vector<MotorDeviceEntry>& motor_inventory(unsigned n)const{return n<position_motor_slots?inventory_.position_motors:inventory_.velocity_motors;}
public:
    static constexpr unsigned dispatch_id=114;
    void attach(ass::graphics::Context&,ass::graphics::Window&,Runtime&,void*);
    void arrange(int,int,int,int,bool);void refresh(const RuntimeView&);void tick();void dispatch();void hide();
    void servo_dispatch(){const auto selected=servos_.selected();servos_.dispatch();if(selected!=servos_.selected())layout();}void stepper_dispatch(){steppers_.dispatch();}
};
}
