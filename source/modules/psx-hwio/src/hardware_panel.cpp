#include "hwio/hardware_panel.hpp"
#include "hwio/catalogue.hpp"
#include <algorithm>
namespace hwio {
namespace gfx=ass::graphics;
HardwarePanel::~HardwarePanel(){hide();}
void __cdecl HardwarePanel::event(void* user,std::uint32_t id,std::uint32_t kind,double) noexcept {
    auto& self=*static_cast<HardwarePanel*>(user);
    try {
        if(self.updating_)return;
        if(kind==AG_RESIZE){self.layout_dirty_=true;self.layout();return;}
        if(kind==AG_DISPATCH){self.dispatch();return;}
        if(kind==AG_CLOSE || kind==AG_CLICK || kind==AG_VALUE_CHANGED){self.events_.push_back({id,kind,self.key_});ag_window_post(self.window_->handle(),1);}
    }catch(const std::exception& e){if(self.message_.handle())self.message_.text(wide(e.what()).c_str());}
}
void HardwarePanel::show(gfx::Context& context,Runtime& runtime,const std::string& key,bool visible){
    runtime_=&runtime;
    if(!window_){
        window_=std::make_unique<gfx::Window>(context,L"HWIO — PoKeys properties",760,900,event,this);
        icons_.emplace(static_cast<HWND>(window_->hwnd()));window_->defer_close(true);
        ag_control_desc d{};d.kind=AG_PANEL;d.text=L"";d.bounds={12,12,740,842};pane_=gfx::Control(context,window_->hwnd(),d);
        auto add=[&](gfx::Control& control,unsigned kind,unsigned id,const wchar_t* caption,void* parent){d.kind=kind;d.id=id;d.text=caption;d.bounds={12,12,700,42};d.minimum=0;d.maximum=1;d.step=1;control=gfx::Control(context,parent,d,event,this);};
        add(use_,AG_CHECKBOX,1,L"Use this card in HWIO",pane_.hwnd());
        add(poext_,AG_CHECKBOX,2,L"Use PoExtBus for this card",pane_.hwnd());
        add(identity_,AG_LABEL,0,L"",pane_.hwnd());add(address_,AG_LABEL,0,L"",pane_.hwnd());add(firmware_,AG_LABEL,0,L"",pane_.hwnd());
        add(status_,AG_LABEL,0,L"",pane_.hwnd());add(message_,AG_LABEL,0,L"",pane_.hwnd());
        add(headings_[0],AG_LABEL,0,L"Pin",pane_.hwnd());add(headings_[1],AG_LABEL,0,L"Assigned function (read from card)",pane_.hwnd());
        for(unsigned n=0;n<pins_.size();++n){add(pins_[n].number,AG_LABEL,0,std::to_wstring(n+1).c_str(),pane_.hwnd());add(pins_[n].function,AG_LABEL,0,L"Waiting for card readback",pane_.hwnd());}
        add(close_,AG_BUTTON,3,L"Close",window_->hwnd());
        transfer_.attach(context,*window_,pane_.hwnd(),runtime);
        transfer_.reflow([this]{layout_dirty_=true;layout();});
    }
    if(key_!=key){key_=key;readback_={};events_.clear();ag_panel_scroll_set(pane_.handle(),0);message_.text(L"");layout_dirty_=true;}
    transfer_.device(key);shown_=true;refresh(runtime.snapshot());layout();window_->fit();shown_=visible;window_->show(visible);
}
void HardwarePanel::layout(){
    if(!layout_dirty_ || !window_ || !pane_.handle())return;
    const auto bounds=window_->client_bounds();const float width=std::max(360.f,bounds.right),height=std::max(300.f,bounds.bottom);
    pane_.bounds({12,12,width-12,height-52});close_.bounds({width-112,height-40,width-12,height-10});
    // Reflow after the native scrollbar reduces the viewport width.
    for(unsigned pass=0;pass<2;++pass){
        const auto viewport=pane_.viewport();const float right=std::max(320.f,viewport.right-viewport.left)-12;
        float y=8;
        for(auto* c:{&use_,&poext_,&identity_,&address_,&firmware_,&status_,&message_}){const float h=(c==&use_ || c==&poext_)?30.f:std::max(30.f,c->text_extent(right-12).y+8);c->bounds({12,y,right,y+h});y+=h+4;}
        y+=transfer_.arrange(12,y,right-12);
        headings_[0].bounds({12,y,78,y+30});headings_[1].bounds({84,y,right,y+30});y+=34;
        for(auto& pin:pins_){const auto h=std::max(26.f,pin.function.text_extent(right-84).y+6);pin.number.bounds({12,y,78,y+h});pin.function.bounds({84,y,right,y+h});y+=h;}
        pane_.extent(y+8);const auto after=pane_.viewport();if(after.right-after.left==viewport.right-viewport.left)break;
    }
    layout_dirty_=false;
}
void HardwarePanel::refresh(const RuntimeView& view){
    if(!shown_ || !runtime_)return;
    layout_dirty_=layout_dirty_||transfer_.layout_dirty();
    const auto configuration=runtime_->hardware();const auto device=std::find_if(configuration.devices.begin(),configuration.devices.end(),[&](const auto& d){return d.key==key_ && d.backend==BackendKind::pokeys_ethernet;});
    updating_=true;
    try {
        auto text=[&](gfx::Control& control,const std::wstring& value){if(control.text()!=value){control.text(value.c_str());layout_dirty_=true;}};
        const bool editable=device!=configuration.devices.end();
        if(bool(IsWindowEnabled(static_cast<HWND>(use_.hwnd())))!=editable){use_.enabled(editable);poext_.enabled(editable);}
        if(editable){if((use_.value()!=0)!=device->enabled)use_.value(device->enabled);if((poext_.value()!=0)!=device->use_poext)poext_.value(device->use_poext);}
        const auto current=std::find_if(view.hardware.begin(),view.hardware.end(),[&](const auto& d){return d.key==key_;});
        // Retain readback while this exact card is disabled or disconnected.
        if(current!=view.hardware.end()){if(current->configured)readback_=*current;else if(!current->firmware.empty())readback_.firmware=current->firmware;}
        text(identity_,wide("Controller identity: "+key_+(editable?" / "+device->name:"")));
        text(address_,wide("Current address: "+(current!=view.hardware.end() && current->connected?current->address:"Not connected")));
        text(firmware_,wide("Firmware version: "+(readback_.firmware.empty()?"Unavailable":readback_.firmware)));
        text(status_,wide((current!=view.hardware.end()?current->status:"Not connected")+std::string(readback_.configured && (current==view.hardware.end() || !current->configured)?". Pin table: last readback.":".")+" PoExtBus attachment cannot be detected; the checkbox controls HWIO use only."));
        for(unsigned n=0;n<pins_.size();++n)text(pins_[n].function,readback_.configured?wide(readback_.pin_functions[n].empty()?"Function unavailable":readback_.pin_functions[n]):L"Waiting for card readback");
        updating_=false;layout();
    }catch(...){updating_=false;throw;}
}
void HardwarePanel::dispatch(){
    auto pending=std::move(events_);events_.clear();
    for(const auto& e:pending){
        if(e.key!=key_)continue;
        if(e.kind==AG_CLOSE || e.id==3){hide();continue;}
        if(e.id!=1 && e.id!=2)continue;
        auto configuration=runtime_->hardware();const auto device=std::find_if(configuration.devices.begin(),configuration.devices.end(),[&](const auto& d){return d.key==key_ && d.backend==BackendKind::pokeys_ethernet;});
        std::string error;
        if(device==configuration.devices.end())error="This card is no longer present in the configuration";
        else {const bool value=(e.id==1?use_:poext_).value()!=0;if(value==(e.id==1?device->enabled:device->use_poext))continue;if(e.id==1)device->enabled=value;else device->use_poext=value;if(!runtime_->save_hardware(configuration,error) && error.empty())error="Settings could not be saved";}
        message_.text(error.empty()?L"Saved. Controller configuration stays in the PoKeys application.":L"Could not save card settings. Previous settings retained.");
        ag_tooltip_set(message_.handle(),wide(error).c_str());layout_dirty_=true;refresh(runtime_->snapshot());
    }
}
void HardwarePanel::hide(){shown_=false;transfer_.hide();if(window_)window_->show(false);}
} // namespace hwio
