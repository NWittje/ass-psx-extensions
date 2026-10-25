#include "hwio/motor_mapping_panel.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <limits>
namespace hwio {
namespace gfx=ass::graphics;
namespace {
struct RepaintBatch {gfx::Control& pane;bool active{true};explicit RepaintBatch(gfx::Control& p):pane(p){pane.update_begin();}void finish(){active=false;pane.update_end();}~RepaintBatch(){if(active)try{pane.update_end();}catch(...) {}}};
std::uint64_t inventory_signature(const RuntimeView& v){std::uint64_t h=14695981039346656037ull;auto hash=[&](std::string_view t){for(const unsigned char c:t){h^=c;h*=1099511628211ull;}};for(const auto& d:v.devices){hash(d.identity.hardware_key);hash(d.identity.instance_path);hash(d.identity.serial);hash(d.identity.name);hash(std::to_string(d.generation));hash(std::to_string(d.slot));hash(std::to_string(d.connected));for(const auto& c:d.controls){hash(c.name);hash(std::to_string(c.key.usage));hash(std::to_string(c.key.data_index));hash(std::to_string(c.minimum));hash(std::to_string(c.maximum));}}return h;}
std::wstring device_name(const DeviceIdentity& i){return wide(i.name+(i.serial.empty()?"":" / "+i.serial));}
}
void __cdecl MotorMappingPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double value)noexcept{auto& p=*static_cast<MotorMappingPanel*>(user);try {
    if(p.binding_)return;if(id>=100){const auto row=(id-100)/100,f=(id-100)%100;if(row<motor_axes && f<count){if(kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED)p.rows_[row].pending|=1u<<f;}}
    if(kind==AG_TEXT_CHANGED)return;
    if(kind==AG_CLICK || kind==AG_VALUE_CHANGED || kind==AG_DROPDOWN_CHANGED || kind==AG_FOCUS_LOST || kind==AG_NAVIGATE){if(p.events_.size()==64){p.hide();return;}p.events_.push_back({id,kind,value});ag_window_post(p.owner_->handle(),dispatch_id);}
}catch(...){p.hide();}}
void MotorMappingPanel::attach(gfx::Context& c,gfx::Window& w,Runtime& r,void* parent){context_=&c;owner_=&w;runtime_=&r;auto create=[&](unsigned kind,unsigned id,void* p,const wchar_t* text){ag_control_desc d{};d.kind=kind;d.id=id;d.text=text;d.bounds={0,0,1,1};d.maximum=1;return gfx::Control(c,p,d,event,this);};pane_=create(AG_PANEL,0,parent,L"");add_=create(AG_BUTTON,1,pane_.hwnd(),L"Add mapping");
    const auto definitions=r.motor_mappings().definitions();for(unsigned i=0;i<motor_axes;++i){auto& row=rows_[i];row.shown=definitions[i].enabled;constexpr unsigned kinds[]{AG_CHOICE,AG_CHOICE,AG_BUTTON,AG_BUTTON,AG_BUTTON,AG_CHECKBOX,AG_CHOICE,AG_CHOICE,AG_BUTTON,AG_BUTTON,AG_LABEL};constexpr const wchar_t* captions[]{L"",L"",L"Home now",L"Stop",L"Clear",L"Auto-homing on start",L"",L"",L"Capture 0%",L"Capture 100%",L""};
        for(unsigned f=0;f<count;++f){row.controls[f]=create(kinds[f],100+i*100+f,pane_.hwnd(),captions[f]);ag_visible(row.controls[f].handle(),0);unsigned flags=AG_CONTROL_NAV_TAB;if(kinds[f]==AG_EDIT)flags|=AG_CONTROL_NOTIFY_FOCUS|AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_ACTIVATE;if(kinds[f]==AG_CHOICE)flags|=AG_CONTROL_NOTIFY_DROPDOWN;ag_control_events(row.controls[f].handle(),flags);}
        row.controls[axis].add(L"Select axis…");for(unsigned a=0;a<motor_axes;++a)row.controls[axis].add(motor_axis_name(MotorAxis(a)));row.controls[axis].value(i+1);row.controls[motor].add(L"Select motor…");for(unsigned n=0;n<motor_slots;++n)row.controls[motor].add((L"Motor "+std::to_wstring(n)).c_str());
        ag_tooltip_set(row.controls[home].handle(),L"Throttle: forward idle to full forward only. Uses captured applied calibration. Motor encoder map lasts for this session.");
    }ag_visible(pane_.handle(),0);
}
void MotorMappingPanel::select_output(int output){
    if(output_!=output)hide();output_=output;const auto definitions=runtime_->motor_mappings().definitions();unsigned selected=motor_axes;
    if(output>=0){for(unsigned i=0;i<motor_axes;++i)if(definitions[i].analogue_output==unsigned(output)){selected=i;break;}if(selected==motor_axes)for(unsigned i=0;i<motor_axes;++i)if(!definitions[i].enabled && definitions[i].analogue_output==UINT_MAX){selected=i;break;}}
    for(unsigned i=0;i<motor_axes;++i)rows_[i].shown=output<0?definitions[i].enabled:i==selected;
    ag_visible(add_.handle(),output<0);layout_dirty_=true;update();
}
void MotorMappingPanel::layout(){if(!pane_.handle() || !layout_dirty_)return;for(auto& row:rows_)if(row.shown)for(Field f:{axis,motor,device,channel}){uint32_t open{};ag_choice_open(row.controls[f].handle(),&open);if(open)return;}RepaintBatch batch(pane_);
    float y=8;const float w=std::max(320.f,pane_.viewport().right-pane_.viewport().left),left=12,right=w-20,available=right-left,gap=6;add_.bounds({left,y,left+150,y+30});if(output_<0)y+=42;
    for(unsigned i=0;i<motor_axes;++i){auto& row=rows_[i];const std::uint32_t visibility=row.shown?((1u<<count)-1)&(i>=5?UINT32_MAX:~((1u<<device)|(1u<<channel)|(1u<<low)|(1u<<high))):0;for(unsigned f=0;f<count;++f)if((visibility^row.visibility)&(1u<<f))ag_visible(row.controls[f].handle(),(visibility>>f)&1);row.visibility=visibility;if(!row.shown)continue;
        auto box=[&](Field f,float x,float top,float size,float h=28){const ag_rect bounds{x,top,x+size,top+h};auto& previous=row.bounds[f];if(bounds.left!=previous.left || bounds.top!=previous.top || bounds.right!=previous.right || bounds.bottom!=previous.bottom){row.controls[f].bounds(bounds);previous=bounds;}};const bool narrow=available<560;
        if(narrow){box(axis,left,y,available*.65f-gap);box(motor,left+available*.65f,y,available*.35f);y+=36;const auto third=(available-2*gap)/3;box(home,left,y,third);box(stop,left+third+gap,y,third);box(clear,left+2*(third+gap),y,third);y+=36;}
        else {const float col=(available-4*gap)/5;box(axis,left,y,col*1.45f);box(motor,left+col*1.45f+gap,y,col*.9f);box(home,left+col*2.35f+2*gap,y,col*.95f);box(stop,left+col*3.3f+3*gap,y,col*.8f);box(clear,left+col*4.1f+4*gap,y,col*.9f);y+=36;}
        box(automatic,left,y,available);y+=36;const auto half=(available-gap)/2;
        const bool indication=i>=5;
        if(indication){box(device,left,y,available*.55f-gap);box(channel,left+available*.55f,y,available*.45f);y+=36;box(low,left,y,half);box(high,left+half+gap,y,half);y+=36;}
        const auto extent=row.controls[status].text_extent(available);box(status,left,y,available,std::max(44.f,extent.y+8));y+=std::max(44.f,extent.y+8)+12;
    }pane_.extent(y);batch.finish();layout_dirty_=false;}
void MotorMappingPanel::update(){if(!visible_)return;binding_=true;try {
    const auto d=runtime_->motor_mappings().definitions();const auto views=runtime_->motor_mappings().views();const auto signature=inventory_signature(inventory_);
    for(unsigned i=0;i<motor_axes;++i){auto& r=rows_[i];if(!r.shown)continue;auto value=[&](Field f,double n){uint32_t open{};if(f==axis || f==motor)ag_choice_open(r.controls[f].handle(),&open);if(!open && !(r.pending&(1u<<f)) && r.controls[f].value()!=n)r.controls[f].value(n);};auto text=[&](Field f,std::wstring t){if(!(r.edited&(1u<<f)) && GetFocus()!=r.controls[f].hwnd() && r.controls[f].text()!=t){r.controls[f].text(t.c_str());if(f==status)layout_dirty_=true;}};value(axis,d[i].enabled || d[i].analogue_output!=UINT_MAX?i+1:0);value(motor,d[i].enabled?d[i].motor+1:0);value(automatic,d[i].auto_home?1:0);
        text(status,problem_.empty()?wide(views[i].status.empty()?(d[i].enabled?"Assigned; Home now required":"Choose an axis and motor"):views[i].status):problem_);
        if(i<5)continue;uint32_t a{},b{};ag_choice_open(r.controls[device].handle(),&a);ag_choice_open(r.controls[channel].handle(),&b);if(a || b || (r.pending&((1u<<device)|(1u<<channel))))continue;if(r.inventory==signature)continue;
        r.devices.clear();std::vector<const DeviceInfo*> connected;std::vector<std::wstring> names{L"Feedback device…"};for(const auto& dev:inventory_.devices)if(dev.connected && std::any_of(dev.controls.begin(),dev.controls.end(),[](const auto& c){return !c.relative && c.key.kind==ControlKind::axis;})){r.devices.push_back(dev.identity);connected.push_back(&dev);names.push_back(device_name(dev.identity));}
        const auto match=match_profile(d[i].feedback,r.devices);int selected=match>=0?match+1:0;if(match<0 && !d[i].feedback.id.empty()){r.devices.push_back(d[i].feedback.identity);names.push_back(device_name(d[i].feedback.identity)+L" (unavailable)");selected=int(r.devices.size());}
        std::vector<const wchar_t*> pointers;for(const auto& name:names)pointers.push_back(name.c_str());r.controls[device].replace(pointers,selected);r.sources.clear();names={L"Feedback channel…"};int channel_selected=0;
        if(match>=0)for(const auto& control:connected[unsigned(match)]->controls)if(control.key.kind==ControlKind::axis && !control.relative){auto source=button_source(*connected[unsigned(match)],control);source.explicit_path=true;r.sources.push_back(std::move(source));names.push_back(wide(control.name));if(!d[i].feedback.bindings.empty() && !d[i].feedback.bindings[0].sources.empty() && same(d[i].feedback.bindings[0].sources[0].key,control.key))channel_selected=int(r.sources.size());}
        if(!channel_selected && !d[i].feedback.bindings.empty()){r.sources.push_back(d[i].feedback);names.push_back(L"Saved channel (unavailable)");channel_selected=int(r.sources.size());}
        pointers.clear();for(const auto& name:names)pointers.push_back(name.c_str());r.controls[channel].replace(pointers,channel_selected);r.inventory=signature;
    }binding_=false;layout();
}catch(...){binding_=false;throw;}}
bool MotorMappingPanel::commit(unsigned i,unsigned field,double value){auto defs=runtime_->motor_mappings().definitions();auto& d=defs[i];auto& r=rows_[i];std::string reason;unsigned target=i;if(output_>=0)d.analogue_output=unsigned(output_);
    if(field==motor){if(value<1 || value>motor_slots)return false;d.motor=unsigned(value)-1;d.enabled=true;}
    else if(field==axis){if(value<1 || value>motor_axes)return false;target=unsigned(value)-1;if(target!=i){if(defs[target].enabled || rows_[target].shown || (defs[target].analogue_output!=UINT_MAX && defs[target].analogue_output!=unsigned(output_))){problem_=L"That axis already has a mapping row.";return false;}defs[target]=d;defs[i]={};}}
    else if(field==clear)d={};
    else if(field==automatic)d.auto_home=value!=0;
    else if(field==device){const auto index=unsigned(value);if(!index || index>r.devices.size()){d.feedback={};d.captured=0;}else {Profile selected;selected.identity=r.devices[index-1];selected.explicit_path=true;std::vector<DeviceIdentity> identities;std::vector<const DeviceInfo*> devices;for(const auto& dev:inventory_.devices)if(dev.connected){identities.push_back(dev.identity);devices.push_back(&dev);}const auto match=match_profile(selected,identities);if(match<0){problem_=L"Selected feedback device is unavailable or ambiguous.";return false;}const auto& dev=*devices[unsigned(match)];const auto c=std::find_if(dev.controls.begin(),dev.controls.end(),[](const auto& x){return x.key.kind==ControlKind::axis && !x.relative;});if(c==dev.controls.end())return false;d.feedback=button_source(dev,*c);d.feedback.explicit_path=true;d.captured=0;}}
    else if(field==channel){const auto index=unsigned(value);if(!index || index>r.sources.size())return false;d.feedback=r.sources[index-1];d.captured=0;}
    else if(field==low || field==high){if(d.feedback.bindings.empty() || d.feedback.bindings[0].sources.empty())return false;std::vector<DeviceIdentity> identities;std::vector<const DeviceInfo*> devices;for(const auto& dev:inventory_.devices)if(dev.connected){identities.push_back(dev.identity);devices.push_back(&dev);}const auto match=match_profile(d.feedback,identities);bool found=false;
        if(match>=0){const auto& dev=*devices[unsigned(match)];for(unsigned c=0;c<dev.controls.size();++c)if(same(dev.controls[c].key,d.feedback.bindings[0].sources[0].key))for(const auto& sample:inventory_.controls)if(sample.device==dev.slot && sample.control==c && sample.generation==dev.generation && !sample.invalid){if(field==low){d.low=sample.value;d.captured|=1;}else {d.high=sample.value;d.captured|=2;}found=true;}}
        if(!found){problem_=L"Current unambiguous feedback unavailable; endpoint not captured.";return false;}}
    if(!runtime_->assign_motor_mappings(defs,reason)){problem_=wide(reason);return false;}
    if(field==axis && target!=i){rows_[target].shown=true;r.shown=false;layout_dirty_=true;}if(field==clear){r.shown=false;layout_dirty_=true;if(output_>=0)select_output(output_);}r.edited&=~(1u<<field);r.inventory=UINT64_MAX;rows_[target].inventory=UINT64_MAX;problem_.clear();return true;
}
void MotorMappingPanel::dispatch(){auto events=std::move(events_);events_.clear();for(const auto& e:events){if(e.id==1){for(auto& row:rows_)if(!row.shown){row.shown=true;layout_dirty_=true;break;}continue;}if(e.id<100)continue;const auto i=(e.id-100)/100,f=(e.id-100)%100;if(i>=motor_axes || f>=count)continue;auto& r=rows_[i];
        if(e.kind==AG_NAVIGATE && (int(e.value)==AG_NAV_TAB_NEXT || int(e.value)==AG_NAV_TAB_PREVIOUS)){const int direction=int(e.value)==AG_NAV_TAB_NEXT?1:-1;int next=int(f);for(unsigned attempt=0;attempt<count;++attempt){next=(next+direction+int(count))%int(count);if(next==status)continue;const auto control=static_cast<HWND>(r.controls[unsigned(next)].hwnd());if(IsWindowVisible(control) && IsWindowEnabled(control)){RECT bounds{};GetWindowRect(control,&bounds);MapWindowPoints(nullptr,static_cast<HWND>(pane_.hwnd()),reinterpret_cast<POINT*>(&bounds),2);const float scale=float(GetDpiForWindow(control))/96.f;const auto viewport=pane_.viewport();float scroll{};ag_panel_scroll_get(pane_.handle(),&scroll);const float height=viewport.bottom-viewport.top;if(bounds.top/scale<0)ag_panel_scroll_set(pane_.handle(),scroll+bounds.top/scale);else if(bounds.bottom/scale>height)ag_panel_scroll_set(pane_.handle(),scroll+bounds.bottom/scale-height);SetFocus(control);break;}}continue;}
        if((f==axis || f==motor || f==device || f==channel) && (e.kind==AG_VALUE_CHANGED || e.kind==AG_DROPDOWN_CHANGED)){uint32_t open{};ag_choice_open(r.controls[f].handle(),&open);if(open)continue;const auto value=r.controls[f].value();commit(i,f,value);r.pending&=~(1u<<f);continue;}
        const auto d=runtime_->motor_mappings().definitions()[i];if(f==home){if(d.enabled)runtime_->motor_mappings().home(d.motor);}else if(f==stop){if(d.enabled)runtime_->motor_mappings().stop(d.motor);}else if(e.kind==AG_CLICK || e.kind==AG_VALUE_CHANGED || e.kind==AG_FOCUS_LOST || (e.kind==AG_NAVIGATE && int(e.value)==AG_NAV_ENTER))commit(i,f,e.value);r.pending&=~(1u<<f);
    }layout();update();}
void MotorMappingPanel::arrange(int x,int y,int w,int h,bool visible){const bool changed=w!=width_ || h!=height_;width_=w;height_=h;if(visible_ && !visible)hide();visible_=visible;if(changed){layout_dirty_=true;pane_.bounds({float(x),float(y),float(x+w),float(y+h)});layout();}ag_visible(pane_.handle(),visible);update();}
void MotorMappingPanel::refresh(const RuntimeView& view){inventory_=view;update();}
void MotorMappingPanel::hide(){events_.clear();for(auto& r:rows_)r.pending=0;if(runtime_)runtime_->motor_mappings().hide_manual();}
}
