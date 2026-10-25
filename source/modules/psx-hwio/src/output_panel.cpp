#include "hwio/output_panel.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>

namespace hwio {
namespace gfx=ass::graphics;
namespace {
constexpr unsigned search_id=2,import_id=3,up_id=4,clear_search_id=5,row_base=1000,stride=6;
bool boolean_resource(const HardwareResource& resource) {
    return resource.output && resource.minimum==0 && resource.maximum==1 &&
        (resource.kind==pokeys::Channel::digital || resource.kind==pokeys::Channel::led || resource.kind==pokeys::Channel::poext || resource.kind==ResourceKind::boolean_output || resource.kind==ResourceKind::dataline_output);
}
std::wstring folded(std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
bool matches(std::wstring_view text,const std::wstring& query) {
    std::size_t start=0;
    while(start<query.size()) {while(start<query.size() && std::iswspace(query[start]))++start;if(start==query.size())break;
        auto end=start;while(end<query.size() && !std::iswspace(query[end]))++end;
        if(text.find(std::wstring_view(query).substr(start,end-start))==text.npos)return false;start=end;}
    return true;
}
}
void __cdecl OutputPanel::event(void* user,std::uint32_t id,std::uint32_t kind,double value) noexcept {
    auto& self=*static_cast<OutputPanel*>(user);
    try {
        if(self.rebuilding_)return;
        if(kind==AG_VIEWPORT_CHANGED){self.realize();return;}
        if(kind!=AG_CLICK && kind!=AG_VALUE_CHANGED && kind!=AG_TEXT_CHANGED && kind!=AG_NAVIGATE)return;
        Event e{id,kind,static_cast<int>(value),{},0};
        if(id>=row_base){const auto index=(id-row_base)/stride;if(index>=self.rows_.size())return;e.key=self.rows_[index].key;e.generation=self.rows_[index].generation;}
        self.events_.push_back(std::move(e));ag_window_post(self.owner_->handle(),dispatch_id);
    }catch(const std::exception& e){if(self.message_.handle())self.message_.text(wide(e.what()).c_str());}
}
void OutputPanel::attach(gfx::Context& context,gfx::Window& owner,Runtime& runtime,void* parent) {
    context_=&context;owner_=&owner;runtime_=&runtime;configuration_=runtime.hardware();
    auto control=[&](unsigned kind,unsigned id,void* parent,ag_rect bounds,const wchar_t* text) {
        ag_control_desc d{};d.kind=kind;d.id=id;d.bounds=bounds;d.text=text;d.maximum=1;d.step=1;
        return gfx::Control(context,parent,d,event,this);
    };
    pane_=control(AG_PANEL,0,parent?parent:owner.hwnd(),{810,36,1500,700},L"");
    chapter_=control(AG_LABEL,0,pane_.hwnd(),{14,8,662,34},L"Digital outputs");
    search_label_=control(AG_LABEL,0,pane_.hwnd(),{14,80,64,104},L"Search");
    search_=control(AG_EDIT,search_id,pane_.hwnd(),{68,80,578,104},L"");
    clear_search_=control(AG_BUTTON,clear_search_id,pane_.hwnd(),{584,80,662,104},L"Clear");
    up_=control(AG_BUTTON,up_id,pane_.hwnd(),{14,112,70,138},L"Up");
    breadcrumb_=control(AG_LABEL,0,pane_.hwnd(),{78,112,662,150},L"All ATA chapters");
    ag_control_events(search_.handle(),AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_VERTICAL);
    ag_tooltip_set(search_.handle(),L"Search output names and ATA chapters; all typed words must match");
    message_=control(AG_LABEL,0,pane_.hwnd(),{14,42,662,76},L"Assign and test configured outputs without PSX.");
    import_=control(AG_BUTTON,import_id,pane_.hwnd(),{14,160,350,188},L"Add output catalogue entries…");
    list_=control(AG_PANEL,0,pane_.hwnd(),{0,162,690,664},L"");list_.viewport_notify(true);
    measure_=control(AG_LABEL,0,pane_.hwnd(),{0,0,1,1},L"");ag_visible(measure_.handle(),0);
    for(const auto& group:active_catalogue().groups)if(!group.digital_outputs.empty()){
        groups_[group.chapter].push_back(&group);
        for(const auto& output:group.digital_outputs)output_groups_.emplace(output.id,&group);
    }
    model();ag_visible(pane_.handle(),0);
}
void OutputPanel::arrange(int x,int y,int width,int height,bool visible) {
    if(!pane_.handle())return;
    if(!visible)clear_tests();
    float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);
    const auto dpi=GetDpiForWindow(static_cast<HWND>(owner_->hwnd()));const bool resized=width_!=width || dpi_!=dpi;dpi_=dpi;
    width_=width;height_=height;visible_=visible;pane_.bounds({float(x),float(y),float(x+width),float(y+height)});
    chapter_.bounds({14,8,float(width-28),34});search_.bounds({68,80,float(width-112),104});
    clear_search_.bounds({float(width-106),80,float(width-28),104});breadcrumb_.bounds({78,112,float(width-28),150});
    message_.bounds({14,42,float(width-28),76});import_.bounds({14,160,float(width-28),188});
    const bool empty=active_catalogue().digital_outputs.empty();ag_visible(import_.handle(),empty);
    list_.bounds({0,empty?204.f:162.f,float(width),float(height)});
    if(resized){heights_.clear();model();ag_panel_scroll_set(list_.handle(),scroll);}
    ag_visible(pane_.handle(),visible);realize();
}
void OutputPanel::model() {
    rebuilding_=true;++layout_revision_;entries_.clear();int y=4;
    auto append=[&](Entry entry) {
        const auto& caption=entry.caption;
        const auto available=width_-(entry.output?146:50);const auto key=std::make_pair(available,caption);auto found=heights_.find(key);
        if(found==heights_.end()){measure_.text(wide(caption).c_str());const auto size=measure_.text_extent(float(available));found=heights_.emplace(key,std::max(24,int(std::ceil(size.y))+2)).first;}
        entry.y=y;entry.caption_height=found->second;entry.height=found->second+(entry.output?(width_<600?108:78):14);y+=entry.height;entries_.push_back(std::move(entry));
    };
    if(query_.empty() && !selected_chapter_) {
        for(const auto& [chapter,groups]:groups_){(void)groups;Entry entry;entry.kind=Kind::chapter;entry.chapter=chapter;entry.key="chapter/"+std::to_string(chapter);entry.caption=active_catalogue().chapters.at(chapter);append(std::move(entry));}
    }else if(query_.empty() && selected_group_.empty() && groups_.at(selected_chapter_).size()>1) {
        for(const auto* group:groups_.at(selected_chapter_)){Entry entry;entry.kind=Kind::group;entry.chapter=selected_chapter_;entry.group=group->id;entry.key="group/"+group->id;entry.caption=group->name;append(std::move(entry));}
    }else for(const auto& [id,out]:active_catalogue().digital_outputs) {
        const auto* group=output_groups_.at(id);
        const auto path=active_catalogue().chapters.at(out->chapter)+" / "+group->name+" / "+out->name;
        if(query_.empty() && (out->chapter!=selected_chapter_ || (!selected_group_.empty() && group->id!=selected_group_)))continue;
        if(!matches(folded(wide(path)),query_))continue;
        Entry entry;entry.output=out;entry.chapter=out->chapter;entry.group=group->id;entry.key="output/"+id;entry.caption=query_.empty()?out->name:active_catalogue().chapters.at(out->chapter)+" / "+out->name;append(std::move(entry));
    }
    std::sort(entries_.begin(),entries_.end(),[](const auto& a,const auto& b){return a.chapter!=b.chapter?a.chapter<b.chapter:a.caption<b.caption;});
    y=4;for(auto& e:entries_){e.y=y;y+=e.height;}
    std::string breadcrumb=!query_.empty()?"Search results":selected_chapter_?active_catalogue().chapters.at(selected_chapter_):"All ATA chapters";
    if(query_.empty() && !selected_group_.empty())for(const auto* group:groups_.at(selected_chapter_))if(group->id==selected_group_){breadcrumb+=" / "+group->name;break;}
    breadcrumb_.text(wide(breadcrumb).c_str());ag_tooltip_set(breadcrumb_.handle(),wide(breadcrumb).c_str());
    up_.enabled(selected_chapter_ || !query_.empty());
    list_.extent(float(y));rebuilding_=false;realize();
    if(active_catalogue().digital_outputs.empty())message_.text(L"The active catalogue has no digital outputs. Add the supplied entries, then exit and reopen HWIO.");
}
void OutputPanel::show(Row& row,unsigned field,bool value){if(row.shown[field]!=int(value)){ag_visible(row.controls[field].handle(),value);row.shown[field]=value;}}
void OutputPanel::enable(Row& row,unsigned field,bool value){if(row.enabled[field]!=int(value)){row.controls[field].enabled(value);row.enabled[field]=value;}}
void OutputPanel::text(Row& row,unsigned field,const std::wstring& value){if(row.text[field]!=value){row.controls[field].text(value.c_str());row.text[field]=value;}}
void OutputPanel::style(Row& row,unsigned field,unsigned value){if(row.styles[field]!=value){ag_control_style(row.controls[field].handle(),value);row.styles[field]=value;}}
void OutputPanel::bind(Row& row,const Entry& entry,std::size_t slot) {
    const bool rebound=row.key!=entry.key;
    const bool layout=rebound || row.layout!=layout_revision_;
    if(rebound){row.key=entry.key;row.id=entry.output?entry.output->id:"";row.generation=++generation_;}
    if(!entry.output){
        if(layout){row.controls[4].bounds({14,float(entry.y),float(width_-32),float(entry.y+entry.caption_height+8)});row.layout=layout_revision_;}
        text(row,4,wide(entry.caption));enable(row,4,true);style(row,4,AG_STYLE_FLAT);
        for(unsigned f=0;f<stride;++f)show(row,f,f==4);return;
    }
    const auto& output=*entry.output;const auto& caption=entry.caption;
    style(row,4,0);text(row,0,wide(caption));
    if(layout){row.controls[0].bounds({14,float(entry.y),float(width_-132),float(entry.y+entry.caption_height)});
    row.controls[5].bounds({float(width_-126),float(entry.y),float(width_-32),float(entry.y+26)});
    const auto top=entry.y+entry.caption_height+4;
    row.controls[1].bounds({14,float(top),float(width_-32),float(top+24)});
    const bool narrow=width_<600;const auto pin_top=top+(narrow?60:30);
    row.controls[2].bounds({14,float(top+30),float(narrow?width_-32:width_/2-4),float(top+56)});
    row.controls[3].bounds({float(narrow?14:width_/2+4),float(pin_top),float(width_-110),float(pin_top+26)});
    row.controls[4].bounds({float(width_-104),float(pin_top),float(width_-32),float(pin_top+26)});row.layout=layout_revision_;}
    text(row,4,L"Clear");
    std::vector<const HardwareBinding*> assignments;for(const auto& b:configuration_.outputs)if(b.observation==row.id)assignments.push_back(&b);
    const auto* binding=assignments.empty()?nullptr:assignments.front();
    const auto draft=drafts_.find(row.id);row.device=draft!=drafts_.end()?draft->second.device:binding?binding->device:"";
    uint32_t device_open{},pin_open{};ag_choice_open(row.controls[2].handle(),&device_open);ag_choice_open(row.controls[3].handle(),&pin_open);
    if((rebound || row.choices!=choices_revision_) && !device_open && !pin_open) {
    row.generation=++generation_;
    row.devices.clear();std::vector<std::wstring> devices{L"Unassigned"};int selected=0;
    for(const auto& d:configuration_.devices){row.devices.push_back(d.key);devices.push_back(wide(d.name+" ["+std::to_string(d.serial)+"]"));if(d.key==row.device)selected=static_cast<int>(row.devices.size());}
    if(!row.device.empty() && !selected){row.devices.push_back(row.device);devices.push_back(wide(row.device+" (unavailable)"));selected=static_cast<int>(row.devices.size());}
    std::vector<const wchar_t*> texts;for(const auto& d:devices)texts.push_back(d.c_str());row.controls[2].replace(texts,selected);
    row.resources.clear();std::vector<std::wstring> pins{L"Pin/channel…"};int chosen=0;
    for(const auto& card:view_.hardware)if(card.key==row.device && card.connected && card.configured)for(const auto& resource:card.resources)if(boolean_resource(resource) && hardware_resource_enabled(configuration_,row.device,resource.kind)){
        row.resources.push_back(resource);pins.push_back(wide(resource.name));
        if(binding && draft==drafts_.end() && binding->resource==resource.kind && binding->channel==resource.channel)chosen=static_cast<int>(row.resources.size());
    }
    if(binding && draft==drafts_.end() && !chosen){pins.push_back(wide("Assigned channel "+std::to_string(binding->channel+1)+" (unavailable)"));chosen=static_cast<int>(pins.size()-1);}
    texts.clear();for(const auto& pin:pins)texts.push_back(pin.c_str());row.controls[3].replace(texts,chosen);
    row.choices=choices_revision_;
    }
    std::string status=view_.network.connected?"Waiting for PSX data":"PSX disconnected";
    std::string detail=status;
    for(const auto& state:view_.digital_outputs)if(state.id==row.id){status=state.available?(state.on?"On":"Off"):"Unavailable: "+state.reason;break;}
    if(!output.qualified)status="Unavailable: "+output.unavailable;
    detail=status;
    if(!output.qualified)status="Definition unavailable";
    else if(status.starts_with("Unavailable:"))status=!view_.network.connected?"PSX disconnected":status.find("Not installed")!=std::string::npos?"Not installed":
        status.find("Awaiting valid source baselines")!=std::string::npos || status.find("PSX unavailable")!=std::string::npos?"Waiting for PSX data":"Source unavailable";
    const auto test=tests_.find(row.id);
    if(test!=tests_.end())status+=test->second?" | Test ON":" | Test OFF";
    text(row,5,test==tests_.end()?L"Test":test->second?L"Test ON":L"Test OFF");
    style(row,5,AG_STYLE_FLAT|(test!=tests_.end()?AG_STYLE_SELECTED:0));
    ag_tooltip_set(row.controls[5].handle(),L"Toggle the assigned hardware output On/Off. PSX resumes when this page is hidden. Observed state remains separate.");
    if(assignments.size()>1)status+=" | "+std::to_string(assignments.size())+" destinations; edit Hardware";
    text(row,1,wide(status));
    ag_tooltip_set(row.controls[1].handle(),wide(detail+"\n"+status).c_str());
    ag_tooltip_set(row.controls[0].handle(),wide(caption+"\n"+output.id+"\n"+output.reference+"\n"+output.unavailable+"\n"+output.requirement_reason).c_str());
    const bool editable=output.qualified && assignments.size()<2;
    enable(row,2,editable);enable(row,3,editable && !row.resources.empty());enable(row,4,assignments.size()<2 && (binding || draft!=drafts_.end()));
    ag_tooltip_set(row.controls[2].handle(),output.qualified?L"Choose a PoKeys device. PSX connection is not required for assignment.":wide(output.unavailable).c_str());
    ag_tooltip_set(row.controls[3].handle(),row.device.empty()?L"Choose a device first":row.resources.empty()?L"No connected configured Boolean output channels available":L"Choose a configured Boolean output channel");
    bool testable=false;
    if(output.qualified && assignments.size()==1 && binding->enabled && binding->transform==OutputTransform::boolean && binding->minimum==0 && binding->maximum==1)
        for(const auto& card:view_.hardware)if(card.key==binding->device && card.connected && card.configured &&
            std::none_of(card.inhibited_outputs.begin(),card.inhibited_outputs.end(),[&](const auto& issue){return issue.first==binding->id;}))
            testable=hardware_resource_enabled(configuration_,binding->device,binding->resource) && std::any_of(card.resources.begin(),card.resources.end(),[&](const auto& resource){return boolean_resource(resource) && resource.kind==binding->resource && resource.channel==binding->channel;});
    enable(row,5,testable);
    for(unsigned f=0;f<stride;++f)show(row,f,true);(void)slot;
}
void OutputPanel::realize() {
    if(realizing_ || !list_.handle())return;realizing_=true;rebuilding_=true;
    tests_=runtime_->output_tests();
    const auto viewport=list_.viewport();float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);
    std::vector<const Entry*> wanted;for(const auto& e:entries_)if(e.y+e.height>=scroll-150 && e.y<=scroll+viewport.bottom-viewport.top+150)wanted.push_back(&e);
    const auto focus=GetFocus();for(const auto& row:rows_)for(const auto& control:row.controls)if(control.hwnd()==focus) {
        const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e.key==row.key;});
        if(entry!=entries_.end() && std::find(wanted.begin(),wanted.end(),&*entry)==wanted.end())wanted.push_back(&*entry);
    }
    while(rows_.size()<wanted.size()) {
        const auto index=rows_.size();Row row;const unsigned kinds[]{AG_LABEL,AG_LABEL,AG_CHOICE,AG_CHOICE,AG_BUTTON,AG_BUTTON};
        for(unsigned f=0;f<stride;++f){ag_control_desc d{};d.kind=kinds[f];d.id=row_base+static_cast<unsigned>(index)*stride+f;d.bounds={0,0,1,1};d.text=L"";d.maximum=1;d.step=1;row.controls[f]=gfx::Control(*context_,list_.hwnd(),d,event,this);if(f>=2)ag_control_events(row.controls[f].handle(),AG_CONTROL_NAV_TAB);}
        rows_.push_back(std::move(row));
    }
    std::set<std::size_t> used;
    for(const auto* e:wanted) {
        auto it=std::find_if(rows_.begin(),rows_.end(),[&](const auto& r){return r.key==e->key;});
        std::size_t index=it==rows_.end()?rows_.size():static_cast<std::size_t>(it-rows_.begin());
        if(index==rows_.size())for(std::size_t i=0;i<rows_.size();++i)if(!used.count(i) && std::none_of(wanted.begin(),wanted.end(),[&](auto w){return w->key==rows_[i].key;})){index=i;break;}
        if(index<rows_.size()){used.insert(index);bind(rows_[index],*e,index);}
    }
    for(std::size_t i=0;i<rows_.size();++i)if(!used.count(i))for(unsigned f=0;f<stride;++f)show(rows_[i],f,false);
    rebuilding_=false;realizing_=false;
}
void OutputPanel::commit(Row& row,unsigned field,int selection) {
    if(field==5){
        if(!visible_ || !IsWindowVisible(static_cast<HWND>(owner_->hwnd())) || IsIconic(static_cast<HWND>(owner_->hwnd())))return;
        bool on=false;const auto test=tests_.find(row.id);
        if(test!=tests_.end())on=test->second;
        else for(const auto& state:view_.digital_outputs)if(state.id==row.id){on=state.on;break;}
        std::string error;
        if(runtime_->test_output(row.id,!on,error))message_.text(L"Test held while Digital outputs is open. Hide this page to resume PSX.");
        else message_.text(wide(error).c_str());
        realize();return;
    }
    const auto current=runtime_->hardware();
    if(std::count_if(current.outputs.begin(),current.outputs.end(),[&](const auto& b){return b.observation==row.id;})>1){configuration_=current;++choices_revision_;message_.text(L"Multiple destinations are retained. Edit them through Hardware.");realize();return;}
    if(field==2){if(selection<=0){commit(row,4,0);return;}if(static_cast<std::size_t>(selection)>row.devices.size())return;drafts_[row.id]={row.devices[selection-1]};++choices_revision_;message_.text(L"Choose a pin/channel to apply the new destination.");realize();return;}
    auto next=current;
    if(field==4){next.outputs.erase(std::remove_if(next.outputs.begin(),next.outputs.end(),[&](const auto& b){return b.observation==row.id;}),next.outputs.end());drafts_.erase(row.id);}
    else if(field==3) {
        if(selection<=0 || static_cast<std::size_t>(selection)>row.resources.size())return;
        const auto& resource=row.resources[selection-1];auto it=std::find_if(next.outputs.begin(),next.outputs.end(),[&](const auto& b){return b.observation==row.id;});
        if(it==next.outputs.end()){HardwareBinding b;b.id=1;for(const auto& old:next.outputs)b.id=std::max(b.id,old.id+1);next.outputs.push_back(b);it=std::prev(next.outputs.end());}
        it->device=row.device;it->resource=resource.kind;it->channel=resource.channel;it->bit=UINT16_MAX;it->observation=row.id;
        it->name=active_catalogue().digital_outputs.at(row.id)->name;it->enabled=true;it->transform=OutputTransform::boolean;it->minimum=0;it->maximum=1;it->failure=0;
        it->scale=1;it->offset=0;it->threshold=.5;it->mask=0;it->table.clear();it->velocity=false;
    }else return;
    std::string error;if(runtime_->save_hardware(next,error)){configuration_=std::move(next);drafts_.erase(row.id);++choices_revision_;message_.text(L"Assignment saved.");}
    else message_.text(wide("Pending: "+error).c_str());realize();
}
void OutputPanel::merge_catalogue() {
    const auto directory=std::filesystem::path(executable_directory());
    auto current=read_catalogue(directory/L"catalogue.xml");const auto defaults=read_catalogue(directory/L"defaults/catalogue.xml");
    if(merge_digital_catalogue(*current,*defaults)){current->revision+=" + digital outputs 1.11.0";write_catalogue(*current,directory/L"catalogue.xml");}
    message_.text(L"Output entries added. Exit and reopen HWIO to load the updated catalogue.");import_.enabled(false);
}
void OutputPanel::navigate(int chapter,const std::string& group) {
    float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);positions_[{selected_chapter_,selected_group_}]=scroll;
    selected_chapter_=chapter;selected_group_=group;query_.clear();rebuilding_=true;search_.text(L"");rebuilding_=false;
    model();ag_panel_scroll_set(list_.handle(),positions_[{selected_chapter_,selected_group_}]);realize();
}
void OutputPanel::dispatch() {
    auto events=std::move(events_);events_.clear();
    for(const auto& e:events) {
        try {
            if(e.id>=row_base){const auto index=(e.id-row_base)/stride;if(index<rows_.size() && rows_[index].generation==e.generation && rows_[index].key==e.key){
                if(e.kind==AG_NAVIGATE)keyboard(e.id,e.selection);
                else {const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const auto& item){return item.key==e.key;});
                    if(entry!=entries_.end() && !entry->output){if(e.kind==AG_CLICK)navigate(entry->chapter,entry->group);}
                    else commit(rows_[index],(e.id-row_base)%stride,e.selection);}
            }}
            else if(e.kind==AG_NAVIGATE)keyboard(e.id,e.selection);
            else if(e.id==import_id && e.kind==AG_CLICK)merge_catalogue();
            else if((e.id==up_id || e.id==clear_search_id) && e.kind==AG_CLICK){
                if(!query_.empty()){query_.clear();rebuilding_=true;search_.text(L"");rebuilding_=false;model();ag_panel_scroll_set(list_.handle(),positions_[{selected_chapter_,selected_group_}]);realize();}
                else if(e.id==up_id)navigate(selected_group_.empty()?0:selected_chapter_,"");
            }else if(e.id==search_id && e.kind==AG_TEXT_CHANGED) {
                if(query_.empty()){float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);positions_[{selected_chapter_,selected_group_}]=scroll;}
                query_=folded(search_.text());model();ag_panel_scroll_set(list_.handle(),query_.empty()?positions_[{selected_chapter_,selected_group_}]:0);realize();
            }
        }catch(const std::exception& error){rebuilding_=realizing_=false;message_.text(wide(error.what()).c_str());}
    }
}
void OutputPanel::keyboard(unsigned id,int direction) {
    const bool previous=direction==AG_NAV_TAB_PREVIOUS;
    if(direction!=AG_NAV_TAB_NEXT && !previous && !(id==search_id && direction==AG_NAV_DOWN))return;
    int index=-1;unsigned field=2;
    auto focusable=[](const auto& control){const auto hwnd=static_cast<HWND>(control.hwnd());return IsWindowEnabled(hwnd) && (GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE);};
    if(id>=row_base) {
        const auto slot=(id-row_base)/stride;if(slot>=rows_.size())return;
        field=(id-row_base)%stride;
        const auto found=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e.key==rows_[slot].key;});
        if(found==entries_.end())return;index=static_cast<int>(found-entries_.begin());
        for(int f=static_cast<int>(field)+(previous?-1:1);f>=2 && f<int(stride);f+=previous?-1:1)if(focusable(rows_[slot].controls[f])){SetFocus(static_cast<HWND>(rows_[slot].controls[f].hwnd()));return;}
    }
    for(index+=previous?-1:1;index>=0 && index<static_cast<int>(entries_.size());index+=previous?-1:1) {
        const auto& entry=entries_[index];if(entry.output && !entry.output->qualified)continue;
        if(entry.output){const auto count=std::count_if(configuration_.outputs.begin(),configuration_.outputs.end(),[&](const auto& b){return b.observation==entry.output->id;});if(count>1)continue;}
        const auto viewport=list_.viewport();float scroll{};ag_panel_scroll_get(list_.handle(),&scroll);
        const auto height=viewport.bottom-viewport.top;
        if(entry.y<scroll)ag_panel_scroll_set(list_.handle(),float(entry.y));else if(entry.y+entry.height>scroll+height)ag_panel_scroll_set(list_.handle(),float(entry.y+entry.height)-height);
        realize();const auto row=std::find_if(rows_.begin(),rows_.end(),[&](const auto& r){return r.key==entry.key;});if(row==rows_.end())return;
        for(int f=previous?int(stride)-1:2;f>=2 && f<int(stride);f+=previous?-1:1)if(focusable(row->controls[f])){SetFocus(static_cast<HWND>(row->controls[f].hwnd()));return;}
    }
    SetFocus(static_cast<HWND>(search_.hwnd()));
}
void OutputPanel::refresh(const RuntimeView& view) {
    view_=view;std::uint64_t inventory=0;for(const auto& h:view.hardware)inventory^=h.revision+std::hash<std::string>{}(h.key)+(h.connected?0x100000001ull:0)+(h.configured?0x200000003ull:0);
    if(revision_!=view.hardware_revision || inventory_!=inventory){revision_=view.hardware_revision;inventory_=inventory;configuration_=runtime_->hardware();
        for(const auto& card:view.hardware)if(card.serial && configuration_.devices.size()<max_ethernet_devices && std::none_of(configuration_.devices.begin(),configuration_.devices.end(),[&](const auto& device){return device.key==card.key;})){
            NetworkDevice device;device.key=card.key;device.name=card.name;device.serial=card.serial;device.model=card.model;if(card.key.starts_with("phidget:")){device.backend=BackendKind::phidget22;device.provider_model="Phidget22";}configuration_.devices.push_back(std::move(device));
        }
        ++choices_revision_;}
    if(visible_)realize();
}
void OutputPanel::clear_tests(){if(runtime_)runtime_->clear_output_tests();tests_.clear();}
} // namespace hwio
