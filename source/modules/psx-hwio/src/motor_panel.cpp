#include "hwio/motor_panel.hpp"
#include "hwio/button_setup.hpp"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <sstream>
#include <limits>
namespace hwio {
namespace gfx=ass::graphics;
namespace {
std::wstring numeric(double n){char text[64]{};const auto result=std::to_chars(text,text+sizeof(text),n);return result.ec==std::errc{}?wide(std::string_view(text,std::size_t(result.ptr-text))):std::wstring{};}
std::wstring numeric(std::optional<double> n){return n?numeric(*n):L"";}
bool parse(const gfx::Control& c,std::optional<double>& value){auto text=utf8(c.text());if(text.empty()){value.reset();return true;}std::istringstream in(text);in.imbue(std::locale::classic());double n{};if(!(in>>n) || !std::isfinite(n))return false;in>>std::ws;if(!in.eof())return false;value=n;return true;}
bool parse(const gfx::Control& c,double& value){std::optional<double> n;if(!parse(c,n) || !n)return false;value=*n;return true;}
struct Batch {gfx::Control& c;explicit Batch(gfx::Control& p):c(p){c.update_begin();}~Batch(){try{c.update_end();}catch(...) {}}};
}
void __cdecl MotorPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double value)noexcept{
    auto& self=*static_cast<MotorPanel*>(user);
    try{if(kind==AG_VIEWPORT_CHANGED && id>=4000 && id<4000+motor_slots){
            const unsigned n=id-4000;if(!self.binding_){self.details_[n].scroll=float(value);if(self.slot_==int(n))self.layout();}return;
        }
        if(kind==AG_BUTTON_CANCEL){self.held_=false;const auto slot=id>=1000?(id-1000)/100:motor_slots;if(slot<motor_slots)self.bench_->disengage(slot);return;}
        if(self.binding_)return;
        if(id>=1000 && (id-1000)/100<motor_slots){const auto n=(id-1000)/100,f=(id-1000)%100;
            if((f==fb_kind || f==fb_device || f==fb_channel || f==speed_feedback) && (kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED))self.details_[n].choice_pending|=1ull<<f;
            if(f==wave && (kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED))self.details_[n].wave_pending=true;
            if(edit_field(f) && (kind==AG_TEXT_CHANGED || kind==AG_FOCUS_LOST || (kind==AG_NAVIGATE && int(value)==AG_NAV_ENTER)))self.details_[n].edited|=1ull<<f;
        }
        if(kind==AG_TEXT_CHANGED)return;
        if(kind==AG_CLICK || kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED || kind==AG_FOCUS_LOST || kind==AG_NAVIGATE || kind==AG_BUTTON_PRESS || kind==AG_BUTTON_RELEASE){
            if(self.events_.size()>=64){self.hide();return;}self.events_.push_back({id,kind,value,self.slot_});ag_window_post(self.owner_->handle(),dispatch_id);}
    }catch(...){try{self.hide();}catch(...){self.bench_->hide();}}
}
void MotorPanel::attach(gfx::Context& c,gfx::Window& owner,Runtime& runtime,void* parent){
    context_=&c;owner_=&owner;runtime_=&runtime;bench_=&runtime.motor_bench();
    auto create=[&](unsigned kind,unsigned id,void* p,const wchar_t* text,ag_rect r){ag_control_desc d{};d.kind=kind;d.id=id;d.text=text;d.bounds=r;return gfx::Control(c,p,d,event,this);};
    pane_=create(AG_PANEL,0,parent,L"",{0,0,690,664});ag_visible(pane_.handle(),0);
    up_=create(AG_BUTTON,1,pane_.hwnd(),L"Up",{12,8,70,38});breadcrumb_=create(AG_LABEL,0,pane_.hwnd(),L"Analogue outputs",{78,8,660,38});message_=create(AG_LABEL,0,pane_.hwnd(),L"",{12,42,660,100});
    root_=create(AG_PANEL,0,pane_.hwnd(),L"",{0,106,690,664});list_=create(AG_PANEL,0,pane_.hwnd(),L"",{0,106,690,664});
    for(unsigned n=0;n<4;++n)groups_[n]=create(AG_BUTTON,10+n,root_.hwnd(),n==0?L"Position-loop motors (Motor 0–9)":n==1?L"Velocity-loop motors (Motor 10–19)":n==2?L"Steppers (Motor 20–29)":L"Servos (Servo 0–9)",{12,float((n%10)*44+4),662,float((n%10)*44+42)});
    for(unsigned n=0;n<motor_slots;++n)motors_[n]=create(AG_BUTTON,20+n,list_.hwnd(),(L"Motor "+std::to_wstring(n)).c_str(),{12,float((n%10)*44+4),662,float((n%10)*44+42)});
    servos_.attach(c,owner,runtime,pane_.hwnd());steppers_.attach(c,owner,runtime,pane_.hwnd());root_.extent(188);list_.extent(448);layout();
}
void MotorPanel::detail(unsigned n){auto& d=details_[n];if(d.created)return;
    ag_control_desc descriptor{};descriptor.kind=AG_PANEL;descriptor.id=4000+n;descriptor.text=L"";descriptor.bounds={0,106,690,664};d.pane=gfx::Control(*context_,pane_.hwnd(),descriptor,event,this);ag_visible(d.pane.handle(),0);ag_panel_viewport_notify(d.pane.handle(),1);
    constexpr unsigned kinds[]{AG_CHOICE,AG_CHOICE,AG_BUTTON,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_SLIDER,AG_SLIDER,AG_BUTTON,AG_BUTTON,AG_BUTTON,AG_BUTTON,AG_BUTTON,AG_BUTTON,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_CHOICE,AG_EDIT,AG_BUTTON,AG_LABEL,AG_CHOICE,AG_CHOICE,AG_CHOICE,AG_CHECKBOX,AG_BUTTON,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_CHOICE,AG_EDIT,AG_BUTTON,AG_LABEL,AG_EDIT,AG_BUTTON,AG_BUTTON};
    constexpr const wchar_t* texts[]{L"",L"",L"Clear",L"",L"",L"",L"",L"",L"",L"",L"<<",L">>",L"Zero position",L"Disengage motor",L"Save settings",L"Revert settings",L"",L"",L"",L"",L"",L"",L"",L"Play",L"",L"",L"",L"",L"Invert feedback",L"Drive settings ▸",L"",L"",L"",L"",L"",L"",L"",L"",L"",L"",L"Auto tune",L"Sine/Square: ±A · Auto tune: total travel A (±A/2)",L"",L"Find backlash",L"Last input"};
    for(unsigned f=0;f<field_count;++f){descriptor.kind=kinds[f];descriptor.id=1000+n*100+f;descriptor.text=texts[f];descriptor.bounds={0,0,1,1};descriptor.minimum=f==acceleration?.1:0;descriptor.maximum=f==acceleration?100:1;descriptor.step=f==velocity?.01:f==acceleration?.1:1;descriptor.value=f==acceleration?100:0;
        d.controls[f]=gfx::Control(*context_,d.pane.hwnd(),descriptor,event,this);
        unsigned flags=AG_CONTROL_NAV_TAB;if(kinds[f]==AG_EDIT)flags|=AG_CONTROL_NOTIFY_FOCUS|AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_ACTIVATE;
        if(f==backward || f==forward)flags|=AG_CONTROL_NOTIFY_BUTTON_EDGES;
        if(f==wave || f==fb_kind || f==fb_device || f==fb_channel || f==speed_feedback)flags|=AG_CONTROL_NOTIFY_DROPDOWN;
        ag_control_events(d.controls[f].handle(),flags);
    }
    d.controls[wave].add(L"Sine");d.controls[wave].add(L"Square");d.controls[fb_kind].add(L"No feedback");d.controls[fb_kind].add(L"Analogue axis");d.controls[fb_kind].add(L"Encoder");d.controls[speed_feedback].add(L"Controller feedback unchanged");d.controls[speed_feedback].add(L"Encoder");d.controls[speed_feedback].add(L"Hall sensors");
    for(const auto* title:{L"Minimum position (counts)",L"Maximum position (counts)",L"Max velocity (counts/s)",L"Max acceleration (counts/s²)",L"Current limit (A)",L"Velocity 0–1",L"Acceleration 0.1–100%",L"Kp",L"Ki",L"Kd",L"Vff",L"Aff",L"Amplitude (counts)",L"Maximum PWM duty (0–1)",L"PWM ramp (duty/s)",L"Native speed ceiling",L"Native acceleration",L"Controller speed-loop Kp",L"Controller speed-loop Ki",L"Controller speed-loop Kd",L"Controller speed deadband",L"Sensor buffer (raw units; blank = Auto)"}){
        descriptor.kind=AG_LABEL;descriptor.id=0;descriptor.text=title;descriptor.bounds={0,0,1,1};d.labels.emplace_back(*context_,d.pane.hwnd(),descriptor);
    }
    d.chart=std::make_unique<gfx::Chart>(*context_,d.pane.hwnd(),ag_rect{12,0,662,136});d.draft=bench_->snapshot(n).draft;d.created=true;d.valid=true;
    descriptor.kind=AG_LABEL;descriptor.id=0;descriptor.text=L"Backlash (raw potentiometer counts; 0 = off)";d.labels.emplace_back(*context_,d.pane.hwnd(),descriptor);
    ag_tooltip_set(d.controls[backlash].handle(),L"Total reversal take-up in raw potentiometer counts. Used only by potentiometer-referenced control; encoder-only bench control is unchanged. Save settings retains this value.");
    ag_tooltip_set(d.controls[find_backlash].handle(),L"Requires a connected motor encoder and potentiometer. Potentiometer-only movement and Auto tune need no encoder; Backlash may be entered manually.");
    ag_tooltip_set(d.controls[sensor_buffer].handle(),L"Potentiometer raw-code tolerance for mapped homing and response checks. Blank: Auto, 5–10 units based on sensor resolution. Jitter is added separately. Save settings persists this motor only.");
    ag_tooltip_set(d.controls[minimum].handle(),L"Both travel fields blank selects the controller's numeric range for bench tuning; no mechanical limit is inferred.");
    ag_tooltip_set(d.controls[zero].handle(),L"Disengages and waits up to two seconds for stationary valid feedback. Sets the current coordinate to zero; performs no homing.");
    ag_tooltip_set(d.controls[velocity].handle(),L"Magnitude of the configured maximum velocity; << and >> supply direction. Zero holds the measured position.");
    for(const auto field:{kp,ki,kd})ag_tooltip_set(d.controls[field].handle(),n>=position_motor_slots?L"HWIO position-loop gain in selected sensor counts; output is normalized signed drive. These gains are independent of onboard Phidget PID.":L"Standardized onboard PID gain units. Legacy Phidget values must be converted; position units here are encoder counts.");
    ag_tooltip_set(d.controls[play].handle(),L"Peak deviation about the measured centre. Period follows the configured velocity and acceleration. Stop holds measured position.");
    ag_tooltip_set(d.controls[auto_tune].handle(),L"Moves within total travel A about the current position. Tunes HWIO's five gains, leaving onboard speed gains unchanged. Success changes the draft only; Save settings remains explicit.");
}
void MotorPanel::navigate(int group,int slot){
    if(slot_>=0){commit(unsigned(slot_));auto& d=details_[unsigned(slot_)];ag_panel_scroll_get(d.pane.handle(),&d.scroll);d.find_select=d.find_seen=false;d.temporary_pot={};d.selected_feedback_device.clear();d.feedback_inventory=UINT64_MAX;}
    if(group_==2 && group!=2)steppers_.hide();if(group_==3 && group!=3)servos_.hide();held_=false;events_.clear();group_=group;slot_=slot;problem_.clear();
    if(slot>=0){detail(unsigned(slot));bench_->activate(unsigned(slot));}
    layout();manual_page();update();
}
void MotorPanel::layout(){if(!pane_.handle())return;Batch batch(pane_);binding_=true;
    try{const float width=std::max(320.f,float(width_));up_.bounds({12,8,70,38});up_.enabled(group_>=0);breadcrumb_.bounds({78,8,width-20,38});message_.bounds({12,42,width-20,100});
        const auto servo_caption=servos_.selected()<0?std::wstring(L"Servos"):L"Servos / Servo "+std::to_wstring(servos_.selected());
        breadcrumb_.text(slot_>=0?((slot_<10?L"Position-loop motors / Motor ":L"Velocity-loop motors / Motor ")+std::to_wstring(slot_)).c_str():group_==0?L"Position-loop motors":group_==1?L"Velocity-loop motors":group_==2?L"Steppers / Motor 20–29":group_==3?servo_caption.c_str():L"Devices");
        root_.bounds({0,106,width,float(std::max(140,height_))});list_.bounds({0,106,width,float(std::max(140,height_))});ag_visible(root_.handle(),group_<0);ag_visible(list_.handle(),(group_==0 || group_==1) && slot_<0);
        for(unsigned n=0;n<4;++n)groups_[n].bounds({12,float(n*44+4),width-28,float(n*44+42)});
        for(unsigned n=0;n<motor_slots;++n){ag_visible(motors_[n].handle(),int(n/10)==group_);motors_[n].bounds({12,float((n%10)*44+4),width-28,float((n%10)*44+42)});}
        for(unsigned n=0;n<motor_slots;++n){auto& d=details_[n];if(!d.created)continue;const bool selected=int(n)==slot_;ag_visible(d.pane.handle(),selected);d.pane.bounds({0,106,width,float(std::max(140,height_))});if(!selected)continue;
            // Use the detail viewport after extent creates its scrollbar.
            for(unsigned pass=0;pass<2;++pass){const auto vp=d.pane.viewport();const float w=std::max(300.f,vp.right-vp.left),left=12,right=w-20,available=right-left,gap=8,third=(available-2*gap)/3,half=(available-gap)/2;
                auto box=[&](Field f,float x,float y,float size,float h=28){d.controls[f].bounds({x,y,x+size,y+h});};
                auto labelled=[&](unsigned label,Field f,float x,float y,float size){const float h=std::max(22.f,d.labels[label].text_extent(size).y+2);d.labels[label].bounds({x,y,x+size,y+h});const float value_label=f==velocity || f==acceleration?24.f:0.f;box(f,x,y+h,size);return h+28+value_label+8;};
                float y=4;box(device,left,y,available*.53f);box(channel,left+available*.53f+gap,y,available*.30f-gap);box(clear,right-available*.17f+gap,y,available*.17f-gap);y+=38;
                const bool software=n>=position_motor_slots;
                for(unsigned f=fb_kind;f<=speed_feedback;++f)ag_visible(d.controls[f].handle(),software && (f<=drive_expand || d.drive_open));
                for(unsigned l=13;l<=20;++l)ag_visible(d.labels[l].handle(),software && d.drive_open);
                ag_visible(d.controls[find_last].handle(),!software && d.find_select);
                if(!software && d.find_select){ag_visible(d.controls[fb_device].handle(),1);ag_visible(d.controls[fb_channel].handle(),1);box(fb_device,left,y,third);box(fb_channel,left+third+gap,y,third);box(find_last,left+2*(third+gap),y,third);y+=38;}
                if(software){box(fb_kind,left,y,third);box(fb_device,left+third+gap,y,third);box(fb_channel,left+2*(third+gap),y,third);y+=38;box(fb_invert,left,y,available);y+=34;}
                y+=std::max(labelled(0,minimum,left,y,half),labelled(1,maximum,left+half+gap,y,half));
                y+=std::max({labelled(2,max_velocity,left,y,third),labelled(3,max_acceleration,left+third+gap,y,third),labelled(4,current,left+2*(third+gap),y,third)});
                y+=labelled(21,sensor_buffer,left,y,available);
                const float backlash_height=labelled(22,backlash,left,y,available*2/3-gap);box(find_backlash,left+available*2/3,y+backlash_height-36,available/3);y+=backlash_height;
                const float velocity_width=(available-gap)*2/3;y+=std::max(labelled(5,velocity,left,y,velocity_width),labelled(6,acceleration,left+velocity_width+gap,y,available-velocity_width-gap));
                for(const auto pair:{std::pair{backward,forward},std::pair{zero,disengage},std::pair{save,revert}}){box(pair.first,left,y,half);box(pair.second,left+half+gap,y,half);y+=38;}
                y+=std::max({labelled(7,kp,left,y,third),labelled(8,ki,left+third+gap,y,third),labelled(9,kd,left+2*(third+gap),y,third)});
                const auto view=bench_->snapshot(n);const bool ff=n>=position_motor_slots || view.live.capabilities.feed_forward;
                for(unsigned f:{unsigned(vff),unsigned(aff)})ag_visible(d.controls[f].handle(),ff);for(unsigned label:{10u,11u})ag_visible(d.labels[label].handle(),ff);
                if(ff)y+=std::max(labelled(10,vff,left,y,half),labelled(11,aff,left+half+gap,y,half));
                if(software){box(drive_expand,left,y,available);y+=36;if(d.drive_open){const bool pwm=d.draft.address.mode!=MotorDriveMode::velocity;
                    for(unsigned f=max_duty;f<=speed_feedback;++f)ag_visible(d.controls[f].handle(),pwm?f==max_duty || f==pwm_ramp:f>=native_velocity);
                    for(unsigned l=13;l<=20;++l)ag_visible(d.labels[l].handle(),pwm?l<=14:l>=15);
                    if(pwm)y+=std::max(labelled(13,max_duty,left,y,half),labelled(14,pwm_ramp,left+half+gap,y,half));
                    else {y+=std::max(labelled(15,native_velocity,left,y,half),labelled(16,native_acceleration,left+half+gap,y,half));y+=std::max({labelled(17,speed_kp,left,y,third),labelled(18,speed_ki,left+third+gap,y,third),labelled(19,speed_kd,left+2*(third+gap),y,third)});y+=labelled(20,speed_deadband,left,y,half);box(speed_feedback,left,y,available);y+=38;}
                }}
                ag_visible(d.controls[current].handle(),view.live.capabilities.current_limit);ag_visible(d.labels[4].handle(),view.live.capabilities.current_limit);
                const float generator_height=labelled(12,amplitude,left+third+gap,y,third);box(wave,left,y+generator_height-36,third);box(play,left+2*(third+gap),y+generator_height-36,third);y+=generator_height;
                ag_visible(d.controls[auto_tune].handle(),software);ag_visible(d.controls[tune_window].handle(),software);
                if(software){box(auto_tune,left,y,third);const float caption_height=std::max(42.f,d.controls[tune_window].text_extent(available-third-gap).y+4);d.tune_height=caption_height;box(tune_window,left+third+gap,y,available-third-gap,caption_height);y+=caption_height+8;}
                box(absolute,left,y,available,40);y+=44;d.chart->bounds({left,y,right,y+136});y+=144;d.pane.extent(y);
                if(d.pane.viewport().right-d.pane.viewport().left==w)break;
            }
            ag_panel_scroll_set(d.pane.handle(),d.scroll);
        }
        steppers_.arrange(0,106,width_,std::max(34,height_-106),visible_ && group_==2);servos_.arrange(0,106,width_,std::max(34,height_-106),visible_ && group_==3);binding_=false;
    }catch(...){binding_=false;throw;}
}
bool MotorPanel::commit(unsigned n,bool tuning){auto& d=details_[n];auto definition=d.draft;bool ok=true;
    if(tuning){ok=parse(d.controls[kp],definition.kp) && parse(d.controls[ki],definition.ki) && parse(d.controls[kd],definition.kd);}
    else {ok=parse(d.controls[minimum],definition.minimum) && parse(d.controls[maximum],definition.maximum) && parse(d.controls[max_velocity],definition.maximum_velocity) && parse(d.controls[max_acceleration],definition.maximum_acceleration) && parse(d.controls[current],definition.current_limit) && parse(d.controls[sensor_buffer],definition.sensor_buffer) && parse(d.controls[kp],definition.kp) && parse(d.controls[ki],definition.ki) && parse(d.controls[kd],definition.kd) && parse(d.controls[amplitude],definition.amplitude);
        std::optional<double> gap;ok=ok && parse(d.controls[backlash],gap);definition.backlash=gap.value_or(0);
        if(n>=position_motor_slots){ok=ok && parse(d.controls[max_duty],definition.drive.maximum_duty) && parse(d.controls[pwm_ramp],definition.drive.pwm_ramp) && parse(d.controls[native_velocity],definition.drive.native_velocity) && parse(d.controls[native_acceleration],definition.drive.native_acceleration) && parse(d.controls[speed_kp],definition.drive.speed_kp) && parse(d.controls[speed_ki],definition.drive.speed_ki) && parse(d.controls[speed_kd],definition.drive.speed_kd) && parse(d.controls[speed_deadband],definition.drive.speed_deadband);}
        if(n>=position_motor_slots || bench_->snapshot(n).live.capabilities.feed_forward)ok=ok && parse(d.controls[vff],definition.vff) && parse(d.controls[aff],definition.aff);
        definition.velocity_fraction=d.controls[velocity].value();definition.acceleration_percent=d.controls[acceleration].value();uint32_t open{};ag_choice_open(d.controls[wave].handle(),&open);if(!open)definition.waveform=d.controls[wave].value()==1?MotorWaveform::square:MotorWaveform::sine;
    }
    std::string reason;if(!ok || !motor_definition_valid(definition,reason)){problem_=ok?wide(reason):L"Enter a finite numeric value. Travel bounds may both be empty.";message_.text(problem_.c_str());d.valid=false;return false;}
    d.valid=!tuning || d.valid;if(d.valid)problem_.clear();if(definition!=d.draft){bench_->edit(n,definition,tuning);d.submitted=definition;d.draft=std::move(definition);}
    const auto committed=tuning?((1ull<<kp)|(1ull<<ki)|(1ull<<kd)):UINT64_MAX;d.edited&=~committed;return true;
}
void MotorPanel::update(){if(!visible_)return;binding_=true;
    try {if(slot_<0){message_.text(group_==1?L"Select Motor 10–19. HWIO position PID uses the selected physical sensor.":group_==0?L"Select Motor 0–9. Devices open disengaged; Save settings persists only that motor.":group_<0?L"Native motor objects for later driven-axis mappings.":L"Assign calibrated physical axes to session-homed motors. Indicator feedback stays local.");binding_=false;return;}
        const auto n=unsigned(slot_);auto& d=details_[n];const auto view=bench_->snapshot(unsigned(slot_));
        if(d.submitted){d.submitted->address=view.draft.address;if(view.draft==*d.submitted)d.submitted.reset();}
        if(!d.submitted && d.valid)d.draft=view.draft;
        // Never overwrite an edit being typed or an open selection popup.
        uint32_t device_open{},channel_open{};ag_choice_open(d.controls[device].handle(),&device_open);ag_choice_open(d.controls[channel].handle(),&channel_open);
        if(!device_open && !channel_open && (d.inventory!=inventory_.revision || d.displayed_address!=view.draft.address)){
            d.draft.address=view.draft.address;const auto selected_device=d.selected_device.empty()?d.draft.address.device:d.selected_device;d.devices={""};std::vector<std::wstring> names{L"Output device…"};int selected=0;
            for(const auto& entry:motor_inventory(n))if(std::find(d.devices.begin(),d.devices.end(),entry.address.device)==d.devices.end()){d.devices.push_back(entry.address.device);names.push_back(wide(entry.name+" / "+entry.address.device));}
            if(!d.draft.address.device.empty() && std::find(d.devices.begin(),d.devices.end(),d.draft.address.device)==d.devices.end()){d.devices.push_back(d.draft.address.device);names.push_back(wide(d.draft.address.device+" (unavailable)"));}
            for(unsigned i=0;i<d.devices.size();++i)if(d.devices[i]==selected_device)selected=int(i);
            std::vector<const wchar_t*> ptrs;for(const auto& name:names)ptrs.push_back(name.c_str());d.controls[device].replace(ptrs,selected);
            d.channels.clear();names={L"Channel/address…"};selected=0;for(const auto& entry:motor_inventory(n))if(entry.address.device==selected_device){d.channels.push_back(entry.address);names.push_back(wide("Channel "+std::to_string(entry.address.channel)+(entry.address.mode==MotorDriveMode::pwm?" / PWM":entry.address.mode==MotorDriveMode::velocity?" / velocity":"")));if(entry.address==d.draft.address)selected=int(d.channels.size());}
            if(d.draft.address.assigned() && !selected && d.draft.address.device==selected_device){d.channels.push_back(d.draft.address);names.push_back(wide("Channel "+std::to_string(d.draft.address.channel)+" (unavailable)"));selected=int(d.channels.size());}
            ptrs.clear();for(const auto& name:names)ptrs.push_back(name.c_str());d.controls[channel].replace(ptrs,selected);d.inventory=inventory_.revision;d.displayed_address=view.draft.address;
        }
        auto text=[&](Field f,const std::wstring& value){if(!(d.edited&(1ull<<f)) && d.controls[f].hwnd()!=GetFocus() && d.controls[f].text()!=value)d.controls[f].text(value.c_str());};
        if(unsigned(slot_)>=position_motor_slots || d.find_select)feedback_choices(unsigned(slot_));
        if(view.backlash_find.active)d.find_seen=true;
        if(d.find_seen && !view.backlash_find.active){d.find_select=d.find_seen=false;d.temporary_pot={};d.selected_feedback_device.clear();d.feedback_inventory=UINT64_MAX;layout();}
        if(d.valid){text(max_duty,numeric(d.draft.drive.maximum_duty));text(pwm_ramp,numeric(d.draft.drive.pwm_ramp));text(native_velocity,numeric(d.draft.drive.native_velocity));text(native_acceleration,numeric(d.draft.drive.native_acceleration));text(speed_kp,numeric(d.draft.drive.speed_kp));text(speed_ki,numeric(d.draft.drive.speed_ki));text(speed_kd,numeric(d.draft.drive.speed_kd));text(speed_deadband,numeric(d.draft.drive.speed_deadband));text(minimum,numeric(d.draft.minimum));text(maximum,numeric(d.draft.maximum));text(max_velocity,numeric(d.draft.maximum_velocity));text(max_acceleration,numeric(d.draft.maximum_acceleration));text(current,numeric(d.draft.current_limit));text(sensor_buffer,numeric(d.draft.sensor_buffer));text(kp,numeric(d.draft.kp));text(ki,numeric(d.draft.ki));text(kd,numeric(d.draft.kd));text(vff,numeric(d.draft.vff));text(aff,numeric(d.draft.aff));text(amplitude,numeric(d.draft.amplitude));}
        if(d.valid)text(backlash,numeric(d.draft.backlash));
        if(GetFocus()!=d.controls[velocity].hwnd())d.controls[velocity].value(d.draft.velocity_fraction);if(GetFocus()!=d.controls[acceleration].hwnd())d.controls[acceleration].value(d.draft.acceleration_percent);
        uint32_t wave_open{};ag_choice_open(d.controls[wave].handle(),&wave_open);if(!wave_open && !d.wave_pending && d.controls[wave].value()!=(d.draft.waveform==MotorWaveform::square?1:0))d.controls[wave].value(d.draft.waveform==MotorWaveform::square?1:0);text(play,view.running?L"Stop":L"Play");
        const bool limits=d.draft.maximum_velocity && d.draft.maximum_acceleration && (!view.live.capabilities.current_limit || d.draft.current_limit);
        const bool ready=!view.mapping_owned && view.live.attached && view.live.configured && view.live.feedback_valid && d.draft.velocity_fraction>0 && limits;
        for(Field f:{backward,forward,play})d.controls[f].enabled(ready);d.controls[zero].enabled(view.live.attached);d.controls[disengage].enabled(view.live.attached);
        const bool finding=view.backlash_find.active;
        const bool tuning_active=view.automatic.active || view.automatic.finalizing || finding;
        for(unsigned f=0;f<field_count;++f)if(f!=absolute && f!=tune_window && f!=auto_tune && f!=find_backlash && f!=disengage && f!=clear && f!=revert){if(tuning_active)d.controls[f].enabled(false);else if(f!=backward && f!=forward && f!=play && f!=zero)d.controls[f].enabled(true);}
        const bool shaft_available=n<position_motor_slots || phidget_motor_shaft_available(inventory_,d.draft.address);
        d.controls[find_backlash].enabled(finding || (!view.automatic.active && !view.automatic.finalizing && ready && shaft_available && (n<position_motor_slots || d.draft.feedback.kind==MotorPositionSource::analogue)));text(find_backlash,finding?L"Stop":d.find_select?L"Start":L"Find backlash");
        d.controls[auto_tune].enabled(n>=position_motor_slots && !finding && (ready || tuning_active));text(auto_tune,tuning_active?L"Stop tuning":L"Auto tune");
        const double c=view.automatic.run?view.automatic.centre:view.live.position,half=d.draft.amplitude/2;
        const auto window_text=L"Sine/Square: ±A · Auto tune: ±A/2\nWindow "+numeric(c-half)+L" … "+numeric(c+half)+L" counts";
        if(d.controls[tune_window].text()!=window_text){text(tune_window,window_text);if(n>=position_motor_slots){const auto viewport=d.pane.viewport();const float available=std::max(300.f,viewport.right-viewport.left)-32,third=(available-16)/3;const float height=std::max(42.f,d.controls[tune_window].text_extent(available-third-8).y+4);if(std::abs(height-d.tune_height)>1){ag_panel_scroll_get(d.pane.handle(),&d.scroll);layout();}}}
        text(absolute,L"Command "+numeric(view.live.target)+L" / Actual "+numeric(view.live.position)+L" counts · centre "+numeric(view.centre));
        const auto status=problem_.empty()?wide(view.status)+(view.live.attached && !view.live.capabilities.failsafe?L" · Host-managed; no hardware watchdog":L"")+(view.dirty?L" · Unsaved settings":L"")+(!view.draft.address.assigned()?L" · "+wide(inventory_.status):L""):problem_;if(message_.text()!=status)message_.text(status.c_str());
        const bool shown_ff=IsWindowVisible(static_cast<HWND>(d.controls[vff].hwnd()))!=FALSE;
        if(shown_ff!=(unsigned(slot_)>=position_motor_slots || view.live.capabilities.feed_forward))layout();
        ag_visible(d.controls[current].handle(),view.live.capabilities.current_limit);ag_visible(d.labels[4].handle(),view.live.capabilities.current_limit);
        graph(unsigned(slot_),view);binding_=false;
    }catch(...){binding_=false;throw;}
}
void MotorPanel::graph(unsigned n,const MotorBenchView& view){auto& d=details_[n];if(d.history==view.history_revision && d.end==view.end_ms)return;
    const auto history=bench_->history(n);std::vector<ag_sample> samples;samples.reserve(history.size());const double window=1.05*std::max(.01,view.period);
    for(const auto& point:history){const double age=(double(view.end_ms)-double(point.ms))/1000.;if(age<0 || age>window)continue;ag_sample sample{};sample.age_seconds=age;const double target=point.command-view.centre,actual=point.actual-view.centre;
        if(point.valid && std::isfinite(actual) && std::abs(actual)<1e30){sample.values[1]=float(actual);sample.valid_mask=2;if(point.command_valid && std::isfinite(target) && std::abs(target)<1e30){sample.values[0]=float(target);sample.valid_mask|=1;}}samples.push_back(sample);}
    ag_chart_panel panel{};wcscpy_s(panel.title,L"Motor response");wcscpy_s(panel.unit,L"counts from centre");wcscpy_s(panel.horizontal_label,L"Seconds");panel.sample_count=unsigned(samples.size());panel.series_count=2;panel.time_window=window;panel.default_scale=1;panel.maximum_scale=1e30f;panel.symlog_threshold=1;panel.height_weight=1;
    ag_series series[2]{};wcscpy_s(series[0].label,L"Commanded target");series[0].color_index=1;series[0].width=2;wcscpy_s(series[1].label,L"Actual position");series[1].color_index=2;series[1].width=2;
    d.chart->submit({&panel,1},series,samples);d.history=view.history_revision;d.end=view.end_ms;
}
void MotorPanel::feedback_choices(unsigned n){auto& d=details_[n];
    if(d.choice_pending)return;
    for(Field f:{fb_kind,fb_device,fb_channel,speed_feedback}){uint32_t open{};ag_choice_open(d.controls[f].handle(),&open);if(open)return;}
    const auto& selected=n<position_motor_slots?d.temporary_pot:d.draft.feedback;
    if(d.controls[fb_kind].value()!=unsigned(selected.kind))d.controls[fb_kind].value(unsigned(selected.kind));
    if(d.controls[fb_invert].value()!=(selected.inverted?1:0))d.controls[fb_invert].value(selected.inverted?1:0);
    if(d.controls[speed_feedback].value()!=d.draft.drive.speed_feedback.value_or(0))d.controls[speed_feedback].value(d.draft.drive.speed_feedback.value_or(0));
    const auto signature=feedback_inventory_^inventory_.revision;
    if(d.feedback_inventory==signature && d.displayed_feedback==selected)return;
    struct Choice {std::string key;std::wstring device,channel;MotorFeedbackDefinition source;};std::vector<Choice> choices;
    if(selected.kind==MotorPositionSource::analogue)for(const auto& device:input_devices_)if(device.connected){
        const auto key=device.identity.hardware_key+"/"+device.identity.instance_path+"/"+device.identity.serial;
        for(const auto& control:device.controls)if(control.key.kind==ControlKind::axis && !control.relative){auto profile=button_source(device,control);profile.explicit_path=true;MotorFeedbackDefinition source;source.kind=MotorPositionSource::analogue;source.input_profile=profile_encode(profile);source.inverted=selected.inverted;choices.push_back({key,wide(device.identity.name+" / "+device.identity.serial),wide(control.name),std::move(source)});}
    }
    if(selected.kind==MotorPositionSource::encoder)for(const auto& entry:inventory_.encoders){MotorFeedbackDefinition source;source.kind=MotorPositionSource::encoder;source.encoder=entry.address;source.inverted=selected.inverted;choices.push_back({entry.address.device,wide(entry.name+" / "+entry.address.device),wide("Encoder channel "+std::to_string(entry.address.channel)),std::move(source)});}
    std::string selected_key=d.selected_feedback_device;
    if(selected_key.empty())for(const auto& choice:choices)if(choice.source==selected){selected_key=choice.key;break;}
    const bool assigned=!selected.input_profile.empty() || selected.encoder.assigned();
    if(selected_key.empty() && assigned){selected_key="saved-unavailable";choices.push_back({selected_key,L"Saved feedback (unavailable)",L"Saved channel (unavailable)",selected});}
    d.feedback_devices={""};std::vector<std::wstring> names{L"Feedback device…"};int index=0;
    for(const auto& choice:choices)if(std::find(d.feedback_devices.begin(),d.feedback_devices.end(),choice.key)==d.feedback_devices.end()){d.feedback_devices.push_back(choice.key);names.push_back(choice.device);if(choice.key==selected_key)index=int(d.feedback_devices.size()-1);}
    std::vector<const wchar_t*> pointers;for(const auto& name:names)pointers.push_back(name.c_str());d.controls[fb_device].replace(pointers,index);
    d.feedback_sources.clear();names={L"Feedback channel…"};index=0;
    for(const auto& choice:choices)if(choice.key==selected_key){d.feedback_sources.push_back(choice.source);names.push_back(choice.channel);if(choice.source==selected)index=int(d.feedback_sources.size());}
    pointers.clear();for(const auto& name:names)pointers.push_back(name.c_str());d.controls[fb_channel].replace(pointers,index);d.feedback_inventory=signature;d.displayed_feedback=selected;
}
void MotorPanel::dispatch(){auto events=std::move(events_);events_.clear();for(const auto& e:events){if(e.id==1){if(group_==3 && servos_.back()){layout();continue;}navigate(slot_>=0?slot_/10:-1,-1);continue;}if(e.id>=10 && e.id<14){navigate(int(e.id-10),-1);continue;}if(e.id>=20 && e.id<40){navigate(int((e.id-20)/10),int(e.id-20));continue;}
        if(e.slot<0 || e.slot!=slot_ || e.id<1000 || (e.id-1000)/100!=unsigned(slot_))continue;const auto n=unsigned(slot_),field=(e.id-1000)%100;if(field>=field_count)continue;auto& d=details_[n];
        const auto tuning_view=bench_->snapshot(n);if((tuning_view.automatic.active || tuning_view.automatic.finalizing || tuning_view.backlash_find.active) && field!=find_backlash && field!=auto_tune && field!=disengage && field!=clear && field!=revert)continue;
        if(e.kind==AG_BUTTON_PRESS){if(!visible_ || hide_sent_)continue;if(commit(n)){held_=true;bench_->drive(n,field==backward?-1:1);}continue;}
        if(e.kind==AG_BUTTON_RELEASE){held_=false;bench_->hold(n);continue;}
        if(e.kind==AG_NAVIGATE && (int(e.value)==AG_NAV_TAB_NEXT || int(e.value)==AG_NAV_TAB_PREVIOUS)){
            const int direction=int(e.value)==AG_NAV_TAB_NEXT?1:-1;int next=int(field);
            for(unsigned attempt=0;attempt<field_count;++attempt){next=(next+direction+int(field_count))%int(field_count);const auto control=static_cast<HWND>(d.controls[unsigned(next)].hwnd());if(next==int(absolute) || !IsWindowVisible(control) || !IsWindowEnabled(control))continue;
                RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,static_cast<HWND>(d.pane.hwnd()),reinterpret_cast<POINT*>(&r),2);const float dpi=float(GetDpiForWindow(control))/96.f;const auto viewport=d.pane.viewport();float scroll{};ag_panel_scroll_get(d.pane.handle(),&scroll);const float height=viewport.bottom-viewport.top;
                if(float(r.top)/dpi<0)ag_panel_scroll_set(d.pane.handle(),scroll+float(r.top)/dpi);else if(float(r.bottom)/dpi>height)ag_panel_scroll_set(d.pane.handle(),scroll+float(r.bottom)/dpi-height);SetFocus(control);break;
            }continue;
        }
        if(field==drive_expand && e.kind==AG_CLICK){d.drive_open=!d.drive_open;d.controls[drive_expand].text(d.drive_open?L"Drive settings ▾":L"Drive settings ▸");layout();continue;}
        if(field>=fb_kind && field<=fb_invert && (e.kind==AG_VALUE_CHANGED || e.kind==AG_DROPDOWN_CHANGED)){
            uint32_t open{};if(field!=fb_invert)ag_choice_open(d.controls[field].handle(),&open);if(open)continue;d.choice_pending&=~(1ull<<field);const auto selected_value=d.controls[field].value();
            auto definition=d.draft;if(n<position_motor_slots)definition.feedback=d.temporary_pot;
            if(field==fb_kind && int(selected_value)!=int(definition.feedback.kind)){definition.feedback={};definition.feedback.kind=static_cast<MotorPositionSource>(int(selected_value));d.selected_feedback_device.clear();}
            if(field==fb_device){const unsigned i=unsigned(selected_value);bench_->disengage(n);d.selected_feedback_device=i<d.feedback_devices.size()?d.feedback_devices[i]:"";d.feedback_inventory=UINT64_MAX;binding_=true;feedback_choices(n);binding_=false;continue;}
            if(field==fb_channel){const unsigned i=unsigned(selected_value);if(i && i<=d.feedback_sources.size())definition.feedback=d.feedback_sources[i-1];else {definition.feedback.input_profile.clear();definition.feedback.encoder={};}}
            if(field==fb_invert)definition.feedback.inverted=selected_value!=0;
            if(n<position_motor_slots){d.temporary_pot=definition.feedback;d.feedback_inventory=UINT64_MAX;continue;}
            if(definition==d.draft)continue;bench_->disengage(n);bench_->edit(n,definition);d.draft=definition;d.submitted=definition;d.feedback_inventory=UINT64_MAX;continue;
        }
        if(field==speed_feedback && (e.kind==AG_VALUE_CHANGED || e.kind==AG_DROPDOWN_CHANGED)){uint32_t open{};ag_choice_open(d.controls[speed_feedback].handle(),&open);if(open)continue;d.choice_pending&=~(1ull<<field);const auto value=int(d.controls[speed_feedback].value());d.draft.drive.speed_feedback=value?std::optional{value}:std::nullopt;commit(n);continue;}
        if(field==device && e.kind==AG_VALUE_CHANGED){const auto index=static_cast<unsigned>(e.value);if(index>=d.devices.size())continue;bench_->disengage(n);d.selected_device=d.devices[index];d.inventory=UINT64_MAX;
            d.channels.clear();std::vector<std::wstring> names{L"Channel/address…"};for(const auto& entry:motor_inventory(n))if(entry.address.device==d.selected_device){d.channels.push_back(entry.address);names.push_back(wide("Channel "+std::to_string(entry.address.channel)+(entry.address.mode==MotorDriveMode::pwm?" / PWM":entry.address.mode==MotorDriveMode::velocity?" / velocity":"")));}binding_=true;std::vector<const wchar_t*> pointers;for(const auto& name:names)pointers.push_back(name.c_str());d.controls[channel].replace(pointers,0);binding_=false;
            if(!index)bench_->clear(n);
            if(d.channels.size()==1){bench_->assign(n,d.channels.front());d.selected_device.clear();}continue;}
        if(field==channel && e.kind==AG_VALUE_CHANGED){const auto index=static_cast<unsigned>(e.value);if(index && index<=d.channels.size()){bench_->assign(n,d.channels[index-1]);d.selected_device.clear();}else bench_->clear(n);continue;}
        if(field==wave && (e.kind==AG_VALUE_CHANGED || e.kind==AG_DROPDOWN_CHANGED)){uint32_t open{};ag_choice_open(d.controls[wave].handle(),&open);if(!open){commit(n);d.wave_pending=false;}continue;}
        if(e.kind==AG_FOCUS_LOST || (e.kind==AG_NAVIGATE && int(e.value)==AG_NAV_ENTER) || ((field==velocity || field==acceleration || field==wave) && e.kind==AG_VALUE_CHANGED)){commit(n,field==kp || field==ki || field==kd);continue;}
        if(e.kind!=AG_CLICK)continue;
        if(field==find_last){
            if(!d.find_select || !last_axis_.available)continue;
            for(const auto& dev:input_devices_)if(dev.slot==last_axis_.sample.device && dev.generation==last_axis_.sample.generation && last_axis_.sample.control<dev.controls.size()){
                const auto& control=dev.controls[last_axis_.sample.control];if(control.key.kind!=ControlKind::axis || control.relative)break;
                auto profile=button_source(dev,control);profile.explicit_path=true;d.temporary_pot={};d.temporary_pot.kind=MotorPositionSource::analogue;d.temporary_pot.input_profile=profile_encode(profile);d.selected_feedback_device.clear();d.feedback_inventory=UINT64_MAX;break;
            }continue;
        }
        if(field==find_backlash){
            if(!visible_ || hide_sent_)continue;
            if(tuning_view.backlash_find.active){bench_->disengage(n);continue;}
            if(n<position_motor_slots && !d.find_select){d.find_select=true;d.temporary_pot={};d.temporary_pot.kind=MotorPositionSource::analogue;d.selected_feedback_device.clear();d.feedback_inventory=UINT64_MAX;problem_=L"Choose a potentiometer, or move it and use Last input, then Start measurement.";layout();continue;}
            if(commit(n))bench_->find_backlash(n,d.temporary_pot);continue;
        }
        switch(field){case auto_tune:if(!visible_ || hide_sent_)break;if(tuning_view.automatic.active || tuning_view.automatic.finalizing)bench_->disengage(n);else if(commit(n))bench_->auto_tune(n);break;case clear:d.selected_device.clear();bench_->clear(n);break;case zero:if(commit(n))bench_->zero(n);break;case disengage:held_=false;bench_->disengage(n);break;case save:if(commit(n))bench_->save(n);break;case revert:held_=false;d.selected_device.clear();d.selected_feedback_device.clear();d.feedback_inventory=UINT64_MAX;bench_->revert(n);d.draft=bench_->snapshot(n).saved;d.submitted=d.draft;d.edited=0;d.valid=true;problem_.clear();break;case play:if(!visible_ || hide_sent_)break;if(bench_->snapshot(n).running)bench_->hold(n);else if(commit(n))bench_->play(n);break;default:break;}
    }update();}
void MotorPanel::manual_page(){
    const bool shown=visible_ && !hide_sent_ && owner_ && IsWindowVisible(static_cast<HWND>(owner_->hwnd())) && !IsIconic(static_cast<HWND>(owner_->hwnd()));
    const int selected=shown && (group_==0 || group_==1) && slot_>=0 && IsWindowVisible(static_cast<HWND>(details_[unsigned(slot_)].pane.hwnd()))?slot_:-1;
    if(selected!=manual_slot_){if(manual_slot_>=0)runtime_->actuators().bench({ActuatorKind::motor,unsigned(manual_slot_)},false);if(selected>=0)runtime_->actuators().bench({ActuatorKind::motor,unsigned(selected)},true);runtime_->motor_mappings().manual_page(selected);manual_slot_=selected;}
}
void MotorPanel::arrange(int x,int y,int w,int h,bool visible){const bool resized=!arranged_ || x!=x_ || y!=y_ || w!=width_ || h!=height_;x_=x;y_=y;width_=w;height_=h;arranged_=true;if(visible_ && !visible)hide();visible_=visible;if(visible && IsWindowVisible(static_cast<HWND>(owner_->hwnd())) && !IsIconic(static_cast<HWND>(owner_->hwnd())))hide_sent_=false;if(resized){pane_.bounds({float(x),float(y),float(x+w),float(y+h)});layout();}ag_visible(pane_.handle(),visible);steppers_.arrange(0,106,width_,std::max(34,height_-106),visible && group_==2);servos_.arrange(0,106,width_,std::max(34,height_-106),visible && group_==3);manual_page();if(visible)update();}
void MotorPanel::refresh(const RuntimeView& view){inventory_=view.phidgets;input_devices_=view.devices;last_axis_=view.last_axis;std::uint64_t signature=14695981039346656037ull;for(const auto& device:view.devices){for(unsigned char c:device.identity.hardware_key+device.identity.instance_path+device.identity.serial){signature^=c;signature*=1099511628211ull;}signature^=device.generation+device.controls.size()+unsigned(device.connected);signature*=1099511628211ull;}feedback_inventory_=signature;servos_.refresh(view);steppers_.refresh(view);update();}
void MotorPanel::tick(){if(!visible_)return;const auto hwnd=static_cast<HWND>(owner_->hwnd());if(!IsWindowVisible(hwnd) || IsIconic(hwnd)){hide();return;}hide_sent_=false;manual_page();if(group_==2)steppers_.tick();if(group_==3)servos_.tick();if(slot_>=0){const auto n=unsigned(slot_);const auto view=bench_->snapshot(n);if(held_ || view.running || view.pending || view.automatic.active || view.backlash_find.active)bench_->heartbeat(n);}update();}
void MotorPanel::hide(){held_=false;events_.clear();for(auto& d:details_){d.wave_pending=false;d.choice_pending=0;d.find_select=d.find_seen=false;d.temporary_pot={};}if(!hide_sent_){const bool manual=manual_slot_>=0;hide_sent_=true;manual_page();if(!manual)bench_->hide();servos_.hide();steppers_.hide();}}
}
