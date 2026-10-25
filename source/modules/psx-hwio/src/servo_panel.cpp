#include "hwio/servo_panel.hpp"
#include <phidget22.h>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace hwio {
namespace gfx=ass::graphics;
namespace {
struct UiBinding {bool& value;bool previous;explicit UiBinding(bool& v):value(v),previous(v){value=true;}~UiBinding(){value=previous;}};
std::wstring number(double n){char t[64];const auto r=std::to_chars(t,t+sizeof(t),n,std::chars_format::general,8);return r.ec==std::errc{}?wide(std::string_view(t,std::size_t(r.ptr-t))):L"";}
void text(gfx::Control& c,const std::wstring& t){if(c.text()!=t)c.text(t.c_str());}
}
void __cdecl ServoPanel::event(void* context,std::uint32_t id,std::uint32_t kind,double value)noexcept{
    auto& p=*static_cast<ServoPanel*>(context);if(p.binding_)return;
    try{if(kind==AG_CLICK || kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED){
        if(p.events_.size()>=64){p.hide();return;}p.events_.push_back({id,kind,value});ag_window_post(p.owner_->handle(),dispatch_id);
    }}catch(...){if(p.selected_>=0)p.runtime_->servos().stop(unsigned(p.selected_));}
}
void ServoPanel::attach(gfx::Context& context,gfx::Window& owner,Runtime& runtime,void* parent){
    context_=&context;owner_=&owner;runtime_=&runtime;sources_=servo_sources();
    auto make=[&](unsigned kind,unsigned id,void* container,const wchar_t* caption){ag_control_desc d{};d.kind=kind;d.id=id;d.text=caption;d.bounds={0,0,1,1};d.minimum=0;d.maximum=1;d.step=kind==AG_SLIDER?.0005:1;return gfx::Control(context,container,d,event,this);};
    pane_=make(AG_PANEL,0,parent,L"");list_=make(AG_PANEL,0,pane_.hwnd(),L"");detail_=make(AG_PANEL,0,pane_.hwnd(),L"");
    for(unsigned n=0;n<slots_.size();++n)slots_[n]=make(AG_BUTTON,100+n,list_.hwnd(),(L"Servo "+std::to_wstring(n)).c_str());
    status_=make(AG_LABEL,0,detail_.hwnd(),L"");range_=make(AG_LABEL,0,detail_.hwnd(),L"");captures_=make(AG_LABEL,0,detail_.hwnd(),L"");
    constexpr const wchar_t* captions[]{L"Device / channel",L"PSX data",L"Manual position",L"Min",L"Centre",L"Max",L"Manual calibration values",L"Real value 1",L"Assign",L"Real value 2",L"Assign"};
    for(unsigned n=0;n<count;++n){const bool choice=n==device || n==source;const bool button=n==set_min || n==set_centre || n==set_max || n==assign0 || n==assign1;
        const auto kind=choice?AG_CHOICE:button?AG_BUTTON:n==slider?AG_SLIDER:n==manual_values?AG_CHECKBOX:AG_EDIT;
        controls_[n]=make(kind,n+1,detail_.hwnd(),button || n==manual_values?captions[n]:L"");
        labels_[n]=make(AG_LABEL,0,detail_.hwnd(),button || n==manual_values?L"":captions[n]);
        unsigned flags=AG_CONTROL_NAV_TAB;if(choice)flags|=AG_CONTROL_NOTIFY_DROPDOWN;ag_control_events(controls_[n].handle(),flags);
    }
    controls_[device].add(L"Unassigned");controls_[source].add(L"Select PSX data");for(const auto& s:sources_)controls_[source].add(wide(s.name).c_str());
    controls_[slider].value(.5);layout();ag_visible(pane_.handle(),0);
}
void ServoPanel::select(unsigned n){
    if(n>=slots_.size())return;hide();selected_=int(n);inventory_=UINT64_MAX;displayed_.reset();manual_submitted_.reset();problem_.clear();
    pending_slider_.reset();active_page_=true;runtime_->servos().manual_page(n,true);{UiBinding guard(binding_);controls_[slider].value(.5);}bind();layout();
}
bool ServoPanel::back(){if(selected_<0)return false;hide();selected_=-1;displayed_.reset();problem_.clear();layout();return true;}
bool ServoPanel::store(ServoDefinition d){
    std::string error;auto& service=runtime_->servos();if(!service.edit(unsigned(selected_),std::move(d),error)){problem_=wide(error);return false;}
    if(!service.save_automatic(unsigned(selected_),error)){problem_=wide(error);return false;}
    problem_.clear();return true;
}
void ServoPanel::bind(){
    if(selected_<0)return;UiBinding guard(binding_);const auto v=runtime_->servos().snapshot(unsigned(selected_));const auto& d=v.draft;const auto& s=d.source;
    const auto source_it=std::find_if(sources_.begin(),sources_.end(),[&](const auto& p){return p.id==s.id;});
    const double source_index=source_it==sources_.end()?0:double(source_it-sources_.begin()+1);if(controls_[source].value()!=source_index)controls_[source].value(source_index);
    const auto device_it=std::find(devices_.begin(),devices_.end(),d.address);const double device_index=device_it==devices_.end()?0:double(device_it-devices_.begin()+1);if(controls_[device].value()!=device_index)controls_[device].value(device_index);
    const bool resize=manual_fields_!=d.manual_values;manual_fields_=d.manual_values;if(controls_[manual_values].value()!=double(manual_fields_))controls_[manual_values].value(manual_fields_);
    for(unsigned i=0;i<2;++i){auto& c=controls_[i?custom1:custom0];if(GetFocus()!=c.hwnd())text(c,d.manual_points[i]?number(d.manual_points[i]->value):L"");}
    text(range_,L"Internal scale: "+number((s.minimum-s.offset)/s.scale)+L" to "+number((s.maximum-s.offset)/s.scale)+L"\nReal scale: "+number(s.minimum)+L" to "+number(s.maximum)+(s.unit.empty()?L"":L" "+wide(s.unit)));
    displayed_=d;if(resize)layout();
}
void ServoPanel::choices(const PhidgetInventory& inventory){
    if(selected_<0 || inventory_==inventory.revision)return;uint32_t open{},other{};ag_choice_open(controls_[device].handle(),&open);ag_choice_open(controls_[source].handle(),&other);if(open || other)return;UiBinding guard(binding_);
    devices_.clear();std::vector<std::wstring> names{L"Unassigned"};
    for(const auto& channel:inventory.channels)if(channel.channel_class==PHIDCHCLASS_RCSERVO){devices_.push_back(channel.address);names.push_back(wide(std::to_string(channel.address.serial)+" / channel "+std::to_string(channel.address.channel)+" / "+channel.name+(channel.address.remote?" / "+channel.address.server:"")));}
    const auto saved=runtime_->servos().snapshot(unsigned(selected_)).draft.address;
    if(saved.assigned() && std::find(devices_.begin(),devices_.end(),saved)==devices_.end()){devices_.push_back(saved);names.push_back(wide("Unavailable: "+saved.device+" / channel "+std::to_string(saved.channel)));}
    const auto found=std::find(devices_.begin(),devices_.end(),saved);std::vector<const wchar_t*> pointers;for(const auto& name:names)pointers.push_back(name.c_str());controls_[device].replace(pointers,found==devices_.end()?0:int(found-devices_.begin())+1);inventory_=inventory.revision;
}
void ServoPanel::layout(){
    if(!pane_.handle())return;auto batch=pane_.update_scope();const float w=std::max(320.f,float(width_));
    list_.bounds({0,0,w,float(height_)});detail_.bounds({0,0,w,float(height_)});ag_visible(list_.handle(),selected_<0);ag_visible(detail_.handle(),selected_>=0);
    for(unsigned n=0;n<slots_.size();++n)slots_[n].bounds({12,float(n*44+4),w-28,float(n*44+42)});list_.extent(448);
    const auto viewport=detail_.viewport();const float right=std::max(300.f,viewport.right-viewport.left)-18,available=right-12,third=(available-16)/3;float y=8;
    status_.bounds({12,y,right,y+50});y+=54;
    auto full=[&](Field f){labels_[f].bounds({12,y,right,y+20});controls_[f].bounds({12,y+22,right,y+54});y+=62;};
    full(device);full(source);range_.bounds({12,y,right,y+48});y+=52;full(slider);y+=24; // ASS slider readout occupies 24 DIP below its trackbar.
    unsigned i{};for(const auto f:{set_min,set_centre,set_max}){const float left=12+float(i++)*(third+8);controls_[f].bounds({left,y,left+third,y+38});}y+=46;
    captures_.bounds({12,y,right,y+56});y+=62;controls_[manual_values].bounds({12,y,right,y+28});y+=36;
    for(unsigned n=0;n<2;++n){const auto field=n?custom1:custom0,button=n?assign1:assign0;ag_visible(labels_[field].handle(),manual_fields_);ag_visible(controls_[field].handle(),manual_fields_);ag_visible(controls_[button].handle(),manual_fields_);if(manual_fields_){labels_[field].bounds({12,y,right,y+20});controls_[field].bounds({12,y+22,right-100,y+54});controls_[button].bounds({right-92,y+22,right,y+54});y+=62;}}
    detail_.extent(y+8);
}
void ServoPanel::arrange(int x,int y,int width,int height,bool shown){
    const bool resize=x!=x_ || y!=y_ || width!=width_ || height!=height_;x_=x;y_=y;width_=width;height_=height;
    if(visible_ && !shown)hide();visible_=shown;if(resize){pane_.bounds({float(x),float(y),float(x+width),float(y+height)});layout();}ag_visible(pane_.handle(),shown);tick();
}
void ServoPanel::tick(){
    if(!visible_ || selected_<0)return;const auto window=static_cast<HWND>(owner_->hwnd());
    if(!IsWindowVisible(window) || IsIconic(window)){hide();return;}
    if(!active_page_){runtime_->servos().manual_page(unsigned(selected_),true);active_page_=true;manual_submitted_.reset();UiBinding guard(binding_);controls_[slider].value(.5);}
}
void ServoPanel::refresh(const RuntimeView& view){
    if(!pane_.handle())return;choices(view.phidgets);if(!visible_ || selected_<0)return;const auto v=runtime_->servos().snapshot(unsigned(selected_));
    uint32_t a{},b{};ag_choice_open(controls_[device].handle(),&a);ag_choice_open(controls_[source].handle(),&b);if(!a && !b && (!displayed_ || *displayed_!=v.draft))bind();
    if(manual_submitted_ && v.manual==manual_submitted_)manual_submitted_.reset();
    if(pending_slider_ && v.draft.parameters.pulse_max>v.draft.parameters.pulse_min)request_manual(*pending_slider_);
    text(status_,problem_.empty()?wide(v.status)+(v.live.released_at_target?L" — signal released at target":L""):problem_);
    ServoCurve curve;const bool ready=curve.compile(v.draft);text(captures_,L"Min: "+std::wstring(v.draft.at_min?L"assigned":L"—")+L"    Centre: "+(v.draft.at_centre?L"assigned":L"—")+L"    Max: "+(v.draft.at_max?L"assigned":L"—")+L"\n"+(ready?L"Calibration ready":L"Assign at least two distinct scale values"));
    const bool accepted=v.live.configured && v.manual && v.live.accepted && v.live.desired && std::abs(*v.manual-*v.live.desired)<1e-9 && !manual_submitted_;
    for(const auto f:{set_min,set_centre,set_max,assign0,assign1})controls_[f].enabled(accepted);
}
void ServoPanel::request_manual(double normalized){
    const auto v=runtime_->servos().snapshot(unsigned(selected_));const auto& p=v.draft.parameters;
    if(!v.draft.address.assigned()){problem_=L"Choose a servo device/channel";return;}
    if(p.pulse_max<=p.pulse_min){pending_slider_=normalized;problem_=L"Preparing manual control";return;}
    pending_slider_.reset();
    const double pulse=p.pulse_min+std::clamp(normalized,0.,1.)*(p.pulse_max-p.pulse_min);manual_submitted_=pulse;problem_.clear();runtime_->servos().manual(unsigned(selected_),pulse);
}
void ServoPanel::dispatch(){
    while(!events_.empty()){const auto e=events_.front();events_.pop_front();if(e.id>=100 && e.id<110 && e.kind==AG_CLICK){select(e.id-100);continue;}
        if(selected_<0 || !e.id || e.id>count)continue;const auto f=Field(e.id-1);auto& service=runtime_->servos();
        if(f==device || f==source){uint32_t open{};ag_choice_open(controls_[f].handle(),&open);if(open || (e.kind!=AG_VALUE_CHANGED && e.kind!=AG_DROPDOWN_CHANGED))continue;auto d=service.snapshot(unsigned(selected_)).draft;
            if(f==device){const auto index=unsigned(controls_[device].value());const auto address=index && index<=devices_.size()?devices_[index-1]:MotorDeviceAddress{};if(address==d.address)continue;d.address=address;}
            else{const auto index=unsigned(controls_[source].value());if(!index || index>sources_.size())continue;if(d.source==sources_[index-1])continue;d.source=sources_[index-1];}
            if(store(std::move(d))){service.manual_page(unsigned(selected_),true);manual_submitted_.reset();UiBinding guard(binding_);controls_[slider].value(.5);bind();}continue;
        }
        if(f==slider && e.kind==AG_VALUE_CHANGED){request_manual(e.value);continue;}
        if(f==manual_values && (e.kind==AG_VALUE_CHANGED || e.kind==AG_CLICK)){auto d=service.snapshot(unsigned(selected_)).draft;const bool enabled=controls_[manual_values].value()!=0;if(d.manual_values==enabled)continue;d.manual_values=enabled;if(store(std::move(d)))bind();continue;}
        if(e.kind!=AG_CLICK)continue;unsigned point{};std::optional<double> value;
        if(f==set_min)point=0;else if(f==set_centre)point=1;else if(f==set_max)point=2;else if(f==assign0 || f==assign1){point=f==assign0?3:4;value=servo_numeric(utf8(controls_[f==assign0?custom0:custom1].text()),-1,1);if(!value){problem_=L"Enter a real scale value";continue;}}else continue;
        std::string error;if(service.assign_point(unsigned(selected_),point,value,error))problem_.clear();else problem_=wide(error);
    }
}
void ServoPanel::hide(){events_.clear();manual_submitted_.reset();pending_slider_.reset();if(active_page_ && selected_>=0 && runtime_)runtime_->servos().manual_page(unsigned(selected_),false);active_page_=false;}
}
