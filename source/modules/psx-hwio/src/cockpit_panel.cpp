#include "hwio/cockpit_panel.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>
namespace hwio {
namespace gfx=ass::graphics;
namespace {
std::wstring fold(std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
bool matches(const std::wstring& path,const std::wstring& query){std::wistringstream in(query);std::wstring word;while(in>>word)if(path.find(word)==path.npos)return false;return true;}
std::wstring thresholds(const std::vector<double>& values){std::wostringstream out;for(std::size_t n=0;n<values.size();++n){if(n)out<<L';';out<<values[n];}return out.str();}
}
void __cdecl CockpitPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double value) noexcept {
    auto& self=*static_cast<CockpitPanel*>(user);try{
        if(self.rebuilding_)return;if(kind==AG_VIEWPORT_CHANGED){self.realize();return;}
        if(kind!=AG_CLICK && kind!=AG_VALUE_CHANGED && kind!=AG_TEXT_CHANGED && kind!=AG_NAVIGATE)return;
        Event event{id,kind,value,{},0};if(id>=1000){const auto n=(id-1000)/fields;if(n>=self.rows_.size())return;event.key=self.rows_[n].key;event.generation=self.rows_[n].generation;}
        self.events_.push_back(std::move(event));ag_window_post(self.owner_->handle(),self.dispatch_);
    }catch(const std::exception& e){if(self.message_.handle())self.message_.text(wide(e.what()).c_str());}
}
void CockpitPanel::attach(gfx::Context& context,gfx::Window& owner,Runtime& runtime,bool encoder,void* parent){
    context_=&context;owner_=&owner;runtime_=&runtime;encoder_=encoder;dispatch_=encoder?encoder_dispatch:analogue_dispatch;configuration_=runtime.cockpit();
    auto create=[&](unsigned kind,unsigned id,void* parent,ag_rect bounds,const wchar_t* text){ag_control_desc d{};d.kind=kind;d.id=id;d.bounds=bounds;d.text=text;d.minimum=0;d.maximum=1;d.step=1;return gfx::Control(context,parent,d,event,this);};
    pane_=create(AG_PANEL,0,parent,{0,0,690,664},L"");
    message_=create(AG_LABEL,0,pane_.hwnd(),{14,8,662,62},encoder?L"Configured encoder counts, or raw A/B phases. One reported count = one step by default.":L"Follow calibrated hardware position. Apply changes to activate routing.");
    search_=create(AG_EDIT,2,pane_.hwnd(),{14,70,578,96},L"");clear_=create(AG_BUTTON,3,pane_.hwnd(),{584,70,662,96},L"Clear");
    up_=create(AG_BUTTON,4,pane_.hwnd(),{14,104,70,130},L"Up");breadcrumb_=create(AG_LABEL,0,pane_.hwnd(),{78,104,662,146},L"All ATA chapters");
    list_=create(AG_PANEL,0,pane_.hwnd(),{0,150,690,664},L"");list_.viewport_notify(true);
    measure_=create(AG_LABEL,0,pane_.hwnd(),{0,0,1,1},L"");ag_visible(measure_.handle(),0);
    ag_control_events(search_.handle(),AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_VERTICAL);
    for(const auto& g:active_catalogue().groups)if(std::any_of(g.cockpit.begin(),g.cockpit.end(),[&](const auto& d){return d.encoder==encoder;}))groups_[g.chapter].push_back(&g);
    model();ag_visible(pane_.handle(),0);
}
void CockpitPanel::arrange(int x,int y,int width,int height,bool visible){
    const bool resized=width!=width_;width_=width;height_=height;visible_=visible;pane_.bounds({float(x),float(y),float(x+width),float(y+height)});
    message_.bounds({14,8,float(width-28),62});search_.bounds({14,70,float(width-112),96});clear_.bounds({float(width-106),70,float(width-28),96});breadcrumb_.bounds({78,104,float(width-28),146});list_.bounds({0,150,float(width),float(height)});
    if(resized){float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);model();ag_panel_scroll_set(list_.handle(),scroll);}ag_visible(pane_.handle(),visible);if(visible)realize();
}
void CockpitPanel::model(){
    rebuilding_=true;++layout_;entries_.clear();int y=4;
    auto add=[&](Entry e){measure_.text(wide(e.caption).c_str());e.caption_height=std::max(24,int(std::ceil(measure_.text_extent(float(width_-46)).y))+2);e.height=e.caption_height+(e.definition?encoder_?180:204:14);e.y=y;y+=e.height;entries_.push_back(std::move(e));};
    if(query_.empty() && !chapter_){for(const auto& [chapter,groups]:groups_){(void)groups;add({"chapter/"+std::to_string(chapter),active_catalogue().chapters.at(chapter),{},chapter});}}
    else if(query_.empty() && group_.empty() && groups_.at(chapter_).size()>1){for(const auto* g:groups_.at(chapter_))add({"group/"+g->id,g->name,g->id,chapter_});}
    else for(const auto& [id,d]:active_catalogue().cockpit){if(d->encoder!=encoder_ || (query_.empty() && (d->chapter!=chapter_ || (!group_.empty() && d->group!=group_))))continue;
        const auto& g=active_catalogue().groups[active_catalogue().group_ids.at(d->group)];const auto path=active_catalogue().chapters.at(d->chapter)+" / "+g.name+" / "+d->name;
        if(!matches(fold(wide(path)),query_))continue;Entry e;e.key=id;e.caption=query_.empty()?d->name:active_catalogue().chapters.at(d->chapter)+" / "+d->name;e.group=d->group;e.chapter=d->chapter;e.definition=d;add(std::move(e));}
    std::string path=query_.empty()?chapter_?active_catalogue().chapters.at(chapter_):"All ATA chapters":"Search results";
    if(query_.empty() && !group_.empty())for(const auto* g:groups_.at(chapter_))if(g->id==group_)path+=" / "+g->name;
    breadcrumb_.text(wide(path).c_str());up_.enabled(chapter_ || !query_.empty());list_.extent(float(y));rebuilding_=false;if(visible_)realize();
}
void CockpitPanel::bind(Row& row,const Entry& entry){
    const bool rebound=row.key!=entry.key;const bool layout=rebound || row.layout!=layout_;if(rebound){row.key=entry.key;row.generation=++generation_;row.choices=UINT64_MAX;}
    auto show=[&](unsigned f,bool value){if(row.shown[f]!=int(value)){ag_visible(row.controls[f].handle(),value);row.shown[f]=value;}};
    auto enable=[&](unsigned f,bool value){if(row.enabled[f]!=int(value)){row.controls[f].enabled(value);row.enabled[f]=value;}};
    auto text=[&](unsigned f,std::wstring value){if((f==7 || f==8 || f==9 || f==10 || f==14) && row.controls[f].hwnd()==GetFocus())return;if(row.texts[f]!=value){row.controls[f].text(value.c_str());row.texts[f]=std::move(value);}};
    auto value=[&](unsigned f,double v){if(row.values[f]!=v){row.controls[f].value(v);row.values[f]=v;}};
    if(!entry.definition){if(layout)row.controls[12].bounds({14,float(entry.y),float(width_-32),float(entry.y+entry.caption_height+8)});text(12,wide(entry.caption));for(unsigned f=0;f<fields;++f)show(f,f==12);enable(12,true);row.layout=layout_;return;}
    const auto& definition=*entry.definition;const auto found=configuration_.rows.find(entry.key);const CockpitRow empty;const auto& configured=found==configuration_.rows.end()?empty:found->second;const auto& draft=configured.draft;
    if(layout){const auto top=float(entry.y+entry.caption_height+4);row.controls[0].bounds({14,float(entry.y),float(width_-32),float(entry.y+entry.caption_height)});
        row.controls[1].bounds({14,top,float(width_-32),top+24});row.controls[2].bounds({14,top+30,float(width_/2-4),top+56});row.controls[3].bounds({float(width_/2+4),top+30,float(width_-32),top+56});
        row.controls[4].bounds({float(width_/2+4),top+62,float(width_-32),top+88});row.controls[5].bounds({14,top+62,float(width_/2-4),top+88});
        row.controls[6].bounds({14,top+94,160,top+120});row.controls[7].bounds({170,top+94,270,top+120});row.controls[12].bounds({float(width_-196),top+94,float(width_-114),top+120});row.controls[13].bounds({float(width_-108),top+94,float(width_-32),top+120});
        row.controls[8].bounds({14,top+62,124,top+88});row.controls[9].bounds({132,top+62,242,top+88});row.controls[10].bounds({14,top+132,float(width_-166),top+158});row.controls[11].bounds({14,top+(encoder_?132:162),float(width_-32),top+(encoder_?158:188)});row.controls[14].bounds({float(width_-158),top+132,float(width_-32),top+158});row.layout=layout_;}
    text(0,wide(entry.caption));ag_tooltip_set(row.controls[0].handle(),wide(definition.reference+"\n"+entry.key+"\n"+record_name(definition.target.record)).c_str());text(12,L"Apply");text(13,L"Clear");text(5,L"Raw A/B phases");text(6,L"Invert");
    std::string state=configured.applied.enabled?"Waiting for hardware":"Unassigned";for(const auto& view:view_.cockpit)if(view.id==entry.key)state=(view.available?"Raw "+std::to_string(view.raw)+" → "+std::to_string(view.desired):"Unavailable")+(view.reason.empty()?"":" | "+view.reason);
    if(!configured.pending.empty())state="Pending: "+configured.pending;text(1,wide(state));ag_tooltip_set(row.controls[1].handle(),wide(state).c_str());
    uint32_t opened_a{},opened_b{},opened_device{};ag_choice_open(row.controls[2].handle(),&opened_device);ag_choice_open(row.controls[3].handle(),&opened_a);ag_choice_open(row.controls[4].handle(),&opened_b);
    if((rebound || row.choices!=choices_) && !opened_a && !opened_b && !opened_device){
        row.generation=++generation_;row.devices.clear();std::vector<std::wstring> labels{L"Device…"};int selected=0;for(const auto& device:view_.devices)if(!device.complete_module && device.identity.backend!=BackendKind::usb_hid){row.devices.push_back(device.identity.hardware_key);labels.push_back(wide(device.identity.name));if(device.identity.hardware_key==draft.device)selected=static_cast<int>(row.devices.size());}
        if(!draft.device.empty() && !selected){row.devices.push_back(draft.device);labels.push_back(wide(draft.device+" (unavailable)"));selected=static_cast<int>(row.devices.size());}
        std::vector<const wchar_t*> pointers;for(const auto& label:labels)pointers.push_back(label.c_str());row.controls[2].replace(pointers,selected);
        row.channels.clear();labels={encoder_?draft.software?L"Phase A pin…":L"Encoder…":L"Analogue pin…"};int a=0,b=0;
        for(const auto& device:view_.devices)if(device.connected && device.identity.hardware_key==draft.device)for(const auto& control:device.controls){const bool phidget=device.identity.backend==BackendKind::phidget22 && device.slot>=first_generic_phidget_device;const bool fds=device.identity.backend==BackendKind::fds_usb || device.identity.backend==BackendKind::fds_ethernet;const auto expected=phidget?static_cast<ResourceKind>(encoder_?(draft.software?5:8):31):encoder_?draft.software?(fds?ResourceKind::boolean_input:pokeys::Channel::digital):pokeys::Channel::encoder_count:(fds?ResourceKind::analogue_input:pokeys::Channel::analog);
            if(encoder_ && draft.software && !device.coherent_reports)continue;
            if(control.key.channel_kind!=unsigned(expected) && !(phidget && !encoder_ && control.key.channel_kind==29))continue;row.channels.push_back(control.key.channel);labels.push_back(wide(control.name));if(control.key.channel==draft.a && draft.enabled)a=static_cast<int>(row.channels.size());if(control.key.channel==draft.b && draft.enabled)b=static_cast<int>(row.channels.size());}
        pointers.clear();for(const auto& label:labels)pointers.push_back(label.c_str());row.controls[3].replace(pointers,a);if(encoder_ && draft.software){labels[0]=L"Phase B pin…";pointers.clear();for(const auto& label:labels)pointers.push_back(label.c_str());row.controls[4].replace(pointers,b);}row.choices=choices_;
    }
    value(5,draft.software);value(6,draft.inverted);text(7,std::to_wstring(draft.counts));text(8,std::to_wstring(draft.minimum));text(9,std::to_wstring(draft.maximum));text(14,thresholds({draft.hysteresis}));text(10,thresholds(draft.thresholds));
    std::wstring native_range=L"Select a configured analogue channel to see its native range.";
    for(const auto& device:view_.devices)if(device.identity.hardware_key==draft.device)for(const auto& control:device.controls)if(control.key.kind==ControlKind::axis && control.key.channel==draft.a)native_range=L"Native counts "+std::to_wstring(control.minimum)+L"–"+std::to_wstring(control.maximum)+L". Calibrate min/max; hardware follows immediately.";
    text(11,encoder_?L"Counts per step at left; default 1. Invert reverses direction.":definition.detents.empty()?native_range:L"Detent thresholds: 0–1, separated by ; (empty = uniform). Hysteresis at right.");
    ag_tooltip_set(row.controls[7].handle(),L"Reported counts per catalogue step (default 1). No automatic ×2/×4 division.");ag_tooltip_set(row.controls[8].handle(),L"Calibrated raw minimum");ag_tooltip_set(row.controls[9].handle(),L"Calibrated raw maximum");ag_tooltip_set(row.controls[14].handle(),L"Selector hysteresis as fraction of calibrated travel (0–0.25)");
    for(unsigned f=0;f<fields;++f){const bool shown=f!=4 && f!=5 && f!=7 && f!=8 && f!=9 && f!=10 && f!=11 && f!=14;
        show(f,shown || (encoder_ && (f==5 || f==7 || f==11 || (f==4 && draft.software))) || (!encoder_ && (f==8 || f==9 || f==11 || ((!definition.detents.empty()) && (f==10 || f==14)))));enable(f,f!=3 && f!=4 || !row.channels.empty());}
}
void CockpitPanel::realize(){
    if(realizing_ || !visible_)return;realizing_=rebuilding_=true;float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);const auto viewport=list_.viewport();
    std::vector<const Entry*> wanted;for(const auto& e:entries_)if(e.y+e.height>=scroll-250 && e.y<=scroll+viewport.bottom-viewport.top+250)wanted.push_back(&e);
    const auto focus=GetFocus();for(const auto& row:rows_)for(const auto& c:row.controls)if(c.hwnd()==focus){const auto e=std::find_if(entries_.begin(),entries_.end(),[&](const auto& x){return x.key==row.key;});if(e!=entries_.end() && std::find(wanted.begin(),wanted.end(),&*e)==wanted.end())wanted.push_back(&*e);}
    while(rows_.size()<wanted.size()){const auto slot=rows_.size();Row row;row.shown.fill(-1);row.enabled.fill(-1);row.values.fill(NAN);row.layout=UINT64_MAX;
        constexpr unsigned kinds[]{AG_LABEL,AG_LABEL,AG_CHOICE,AG_CHOICE,AG_CHOICE,AG_CHECKBOX,AG_CHECKBOX,AG_EDIT,AG_EDIT,AG_EDIT,AG_EDIT,AG_LABEL,AG_BUTTON,AG_BUTTON,AG_EDIT};
        for(unsigned f=0;f<fields;++f){ag_control_desc d{};d.kind=kinds[f];d.id=1000+static_cast<unsigned>(slot)*fields+f;d.bounds={0,0,1,1};d.text=L"";d.minimum=0;d.maximum=1;d.step=1;row.controls[f]=gfx::Control(*context_,list_.hwnd(),d,event,this);if(f>=2)ag_control_events(row.controls[f].handle(),kinds[f]==AG_EDIT?AG_CONTROL_NAV_TAB|AG_CONTROL_NOTIFY_TEXT:AG_CONTROL_NAV_TAB);}rows_.push_back(std::move(row));}
    std::set<std::size_t> used;for(const auto* e:wanted){auto found=std::find_if(rows_.begin(),rows_.end(),[&](const auto& r){return r.key==e->key;});auto slot=found==rows_.end()?rows_.size():static_cast<std::size_t>(found-rows_.begin());
        if(slot==rows_.size())for(std::size_t n=0;n<rows_.size();++n)if(!used.count(n) && std::none_of(wanted.begin(),wanted.end(),[&](const auto* x){return x->key==rows_[n].key;})){slot=n;break;}
        if(slot<rows_.size()){used.insert(slot);bind(rows_[slot],*e);}}
    for(std::size_t n=0;n<rows_.size();++n)if(!used.count(n))for(unsigned f=0;f<fields;++f)if(rows_[n].shown[f]!=0){ag_visible(rows_[n].controls[f].handle(),0);rows_[n].shown[f]=0;}rebuilding_=realizing_=false;
}
void CockpitPanel::commit(Row& row,unsigned field,double value){
    auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e.key==row.key;});if(entry==entries_.end())return;if(!entry->definition){navigate(entry->chapter,entry->group);return;}
    auto& configured=configuration_.rows[row.key];auto& draft=configured.draft;
    const int limit=draft.device.starts_with("phidget:")?16383:4095;
    if(field==7 || field==8 || field==9 || field==14){std::istringstream in(utf8(row.controls[field].text()));if(!(in>>value)){configured.pending="Invalid numeric value";return;}in>>std::ws;if(!in.eof() || !std::isfinite(value) || (field!=14 && (value<0 || value>limit || std::floor(value)!=value))){configured.pending="Invalid numeric value";return;}}
    if(field==2){const auto selected=static_cast<std::size_t>(value);if(!selected || selected>row.devices.size()){draft.enabled=false;draft.device.clear();}else{draft.device=row.devices[selected-1];draft.enabled=false;}++choices_;}
    else if(field==3 || field==4){const auto selected=static_cast<std::size_t>(value);if(!selected || selected>row.channels.size())return;(field==3?draft.a:draft.b)=row.channels[selected-1];draft.enabled=true;
        if(!encoder_)for(const auto& device:view_.devices)if(device.identity.hardware_key==draft.device)for(const auto& control:device.controls)if(control.key.kind==ControlKind::axis && control.key.channel==draft.a){draft.minimum=control.minimum;draft.maximum=control.maximum;}
    }
    else if(field==5){draft.software=value!=0;draft.enabled=false;++choices_;}
    else if(field==6)draft.inverted=value!=0;else if(field==7)draft.counts=static_cast<unsigned>(value);
    else if(field==8)draft.minimum=static_cast<int>(value);else if(field==9)draft.maximum=static_cast<int>(value);else if(field==14)draft.hysteresis=value;
    else if(field==10){auto text=utf8(row.controls[10].text());for(auto& c:text)if(c==';')c=' ';std::istringstream in(text);std::vector<double> values;double v{};while(in>>v)values.push_back(v);if(!in.eof()){configured.pending="Invalid threshold list";return;}draft.thresholds=std::move(values);}
    else if(field==13){draft={};configured.pending="apply";}
    else if(field==12){
        auto number=[&](unsigned f,double& v){std::istringstream in(utf8(row.controls[f].text()));if(!(in>>v) || !std::isfinite(v))return false;in>>std::ws;return in.eof();};
        double a{},b{},h{};bool valid=true;
        if(encoder_){valid=number(7,a) && a>=1 && a<=1024 && std::floor(a)==a;if(valid)draft.counts=static_cast<unsigned>(a);}
        else {valid=number(8,a) && number(9,b) && a>=0 && a<b && b<=limit && std::floor(a)==a && std::floor(b)==b;if(valid){draft.minimum=static_cast<int>(a);draft.maximum=static_cast<int>(b);}
            if(!entry->definition->detents.empty()){valid=valid && number(14,h) && h>=0 && h<=.25;auto t=utf8(row.controls[10].text());for(auto& c:t)if(c==';')c=' ';std::istringstream in(t);std::vector<double> values;double v{};while(in>>v)values.push_back(v);valid=valid && in.eof();if(valid){draft.thresholds=std::move(values);draft.hysteresis=h;}}}
        if(!valid){configured.pending="Invalid calibration/counts/threshold values";message_.text(wide(configured.pending).c_str());return;}
    }else return;
    configured.pending=field==12 || field==13?"apply":"Changes await Apply";
    std::string error;if(!runtime_->save_cockpit(configuration_,error)){configured.pending=error;message_.text(wide(error).c_str());}else configuration_=runtime_->cockpit();realize();
}
void CockpitPanel::navigate(int chapter,std::string group){float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);positions_[{chapter_,group_}]=scroll;chapter_=chapter;group_=std::move(group);query_.clear();rebuilding_=true;search_.text(L"");rebuilding_=false;model();ag_panel_scroll_set(list_.handle(),positions_[{chapter_,group_}]);realize();}
void CockpitPanel::keyboard(unsigned id,int direction){
    const bool previous=direction==AG_NAV_TAB_PREVIOUS;if(direction!=AG_NAV_TAB_NEXT && !previous && !(id==2 && direction==AG_NAV_DOWN))return;
    int index=-1,field=1;if(id>=1000){const auto slot=(id-1000)/fields;if(slot>=rows_.size())return;field=static_cast<int>((id-1000)%fields);const auto e=std::find_if(entries_.begin(),entries_.end(),[&](const auto& x){return x.key==rows_[slot].key;});if(e==entries_.end())return;index=static_cast<int>(e-entries_.begin());
        for(int f=field+(previous?-1:1);f>=2 && f<int(fields);f+=previous?-1:1)if(rows_[slot].shown[f]==1 && rows_[slot].enabled[f]==1 && f!=11){SetFocus(static_cast<HWND>(rows_[slot].controls[f].hwnd()));return;}}
    index+=previous?-1:1;if(index>=0 && index<int(entries_.size())){const auto& e=entries_[index];ag_panel_scroll_set(list_.handle(),float(e.y));realize();for(auto& row:rows_)if(row.key==e.key){for(int f=previous?int(fields)-1:2;f>=2 && f<int(fields);f+=previous?-1:1)if(row.shown[f]==1 && row.enabled[f]==1 && f!=11){SetFocus(static_cast<HWND>(row.controls[f].hwnd()));return;}}}SetFocus(static_cast<HWND>(search_.hwnd()));
}
void CockpitPanel::dispatch(){auto events=std::move(events_);events_.clear();for(const auto& e:events){
    if(e.kind==AG_NAVIGATE){keyboard(e.id,static_cast<int>(e.value));continue;}
    if(e.id>=1000){const auto n=(e.id-1000)/fields;if(n<rows_.size() && rows_[n].generation==e.generation && rows_[n].key==e.key)commit(rows_[n],(e.id-1000)%fields,e.value);}
    else if(e.id==2 && e.kind==AG_TEXT_CHANGED){if(query_.empty()){float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);positions_[{chapter_,group_}]=scroll;}query_=fold(search_.text());model();ag_panel_scroll_set(list_.handle(),query_.empty()?positions_[{chapter_,group_}]:0);realize();}
    else if(e.id==3 || e.id==4){if(!query_.empty()){query_.clear();rebuilding_=true;search_.text(L"");rebuilding_=false;model();ag_panel_scroll_set(list_.handle(),positions_[{chapter_,group_}]);realize();}else if(e.id==4)navigate(group_.empty()?0:chapter_,{});}
}}
void CockpitPanel::refresh(const RuntimeView& view){view_=view;std::uint64_t inventory{};for(const auto& d:view.devices)inventory^=d.generation+std::hash<std::string>{}(d.identity.hardware_key)+(d.connected?1234567:0);
    if(revision_!=view.cockpit_revision || inventory_!=inventory){revision_=view.cockpit_revision;inventory_=inventory;configuration_=runtime_->cockpit();++choices_;}if(visible_)realize();}
} // namespace hwio
