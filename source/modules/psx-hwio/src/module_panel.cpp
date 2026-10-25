#include "hwio/module_panel.hpp"

#include <algorithm>
#include <cmath>
#include <climits>

#include <sstream>

namespace hwio {

namespace gfx=ass::graphics;

namespace {

struct RepaintBatch {
    gfx::Control& pane;bool active{true};
    explicit RepaintBatch(gfx::Control& control):pane(control){pane.update_begin();}
    void finish(){pane.update_end();active=false;}
    ~RepaintBatch(){if(active)try{pane.update_end();}catch(...) {}}
};
std::string category(const ModuleProfile& p){if(!p.category.empty())return p.category;if(p.id.find("cdu")!=p.id.npos)return "CDU";if(p.id.find("efis")!=p.id.npos)return "EFIS";if(p.id.find("rcp")!=p.id.npos)return "RMP";return "MCP";}

std::wstring caption(std::string_view group,std::string_view role){if(group=="CDU")return wide((role=="Left"?"CPT":role=="Right"?"FO":"Centre")+std::string(" CDU:"));if(group=="MCP")return L"MCP:";if(group==role)return wide(std::string(group)+":");return wide(std::string(role)+" "+std::string(group)+":");}

std::string connection(bool enabled,bool connected){return !enabled?"Support disabled":connected?"Connected":"Disconnected";}
bool wizard_card(const EthernetHealth& card){
    if(!card.key.starts_with("fds-") && !card.key.starts_with("pokeys:") && !card.key.starts_with("phidget:"))return false;
    if(std::any_of(card.resources.begin(),card.resources.end(),[](const auto& r){return r.output && r.kind==ResourceKind::integral_display;}))return false;
    return std::any_of(card.resources.begin(),card.resources.end(),[](const auto& r){return r.output && r.minimum==0 && r.maximum==1 && (r.kind==pokeys::Channel::digital || r.kind==pokeys::Channel::led || r.kind==pokeys::Channel::poext || r.kind==ResourceKind::boolean_output || r.kind==ResourceKind::dataline_output);});
}

const ModuleProfile* preferred(const ModuleLibrary& library,const ModuleView& device,std::string_view type={},std::string_view role={}) {

    const ModuleProfile* selected{};for(const auto& p:library.profiles)if(p.model==device.model && p.backend==device.backend && device.update>=p.minimum_update && device.update<=p.maximum_update && p.unavailable.empty() && (type.empty() || category(p)==type) && (role.empty() || std::find(p.roles.begin(),p.roles.end(),role)!=p.roles.end())){if(!selected || (selected->associations.size()>p.associations.size()))selected=&p;}return selected;

}

}

namespace {
bool immediate_choice(unsigned id) noexcept {
    return id==5 || id==20 || id==30 || id==32 || id==33 || id==40 || id==9000 ||
        (id>=100 && id<8000 && (id-100)%32==1);
}
}
void __cdecl ModulePanel::event(void* user,std::uint32_t id,std::uint32_t kind,double value) noexcept {
    auto& self=*static_cast<ModulePanel*>(user);if(self.rebuilding_)return;
    if(kind==AG_CLOSE){self.transfer_.hide();if(self.inspection_window_)self.inspection_window_->show(false);return;}
    // Native current selection includes previews; only the captured decision
    // payload may change an immediately saved assignment.
    const bool choice=immediate_choice(id);
    if(choice && kind!=AG_CHOICE_ACCEPTED && kind!=AG_CHOICE_CANCELLED)return;
    if(!choice && kind!=AG_CLICK && kind!=AG_VALUE_CHANGED)return;
    if(choice && (!std::isfinite(value) || value<0 || value>INT_MAX || std::floor(value)!=value))return;
    try {
        std::string key;
        if(id>=100 && id<8000){const auto index=(id-100)/32;if(index>=self.rows_.size())return;key=self.rows_[index].key;}
        else if(id==9000 || (id>=8100 && id<8132))key=self.inspected_;
        self.events_.push_back({id,kind,self.generation_,std::move(key),choice?static_cast<int>(value):-1});
        ag_window_post(self.owner_->handle(),dispatch_id);
    }catch(...){}
}

void ModulePanel::attach(gfx::Context& context,gfx::Window& owner,Runtime& runtime,void* parent){context_=&context;owner_=&owner;runtime_=&runtime;configuration_=runtime.modules();ag_control_desc d{};d.kind=AG_PANEL;d.bounds={0,0,800,664};d.text=L"";pane_=gfx::Control(context,parent,d);auto add=[&](gfx::Control& c,unsigned id,unsigned kind,const wchar_t* title){d.kind=kind;d.id=id;d.text=title;d.bounds={10,10,250,40};d.maximum=1;d.step=1;c=gfx::Control(context,pane_.hwnd(),d,event,this);};add(merge_,1,AG_BUTTON,L"Add catalogue definitions");add(merge_profiles_,2,AG_BUTTON,L"Add module profiles");add(diagnostics_,3,AG_BUTTON,L"Save diagnostics");add(message_,0,AG_LABEL,L"");for(unsigned n=0;n<6;++n)add(groups_[n],10+n,n==4?AG_LABEL:AG_CHECKBOX,n==0?L"FDS - Enable support":n==1?L"PoKeys - Enable support":n==2?L"Phidgets - Enable support":n==4?L"CPflight":n==5?L"Flight Illusion - Enable support":L"Enable support");
add(fi_open_,50,AG_BUTTON,L"Instruments and calibration...");add(fi_status_,0,AG_LABEL,L"");
add(cp_port_label_,0,AG_LABEL,L"MCP port:");add(cp_port_,40,AG_CHOICE,L"");add(cp_refresh_,41,AG_BUTTON,L"Refresh");add(cp_connect_,42,AG_BUTTON,L"Connect");add(cp_status_,0,AG_LABEL,L"");
add(output_cards_,5,AG_CHOICE,L"");add(output_launch_,4,AG_BUTTON,L"Assign outputs…");
add(trim_heading_,0,AG_LABEL,L"Aeronautical Simulator Service");
add(trim_port_label_,0,AG_LABEL,L"Display port:");add(trim_port_choice_,30,AG_CHOICE,L"");add(trim_identify_,31,AG_BUTTON,L"Identify");add(trim_message_,0,AG_LABEL,L"");
for(unsigned n=0;n<2;++n){add(trim_roles_[n][0],0,AG_LABEL,n?L"FO indicator:":L"CPT indicator:");add(trim_roles_[n][1],32+n,AG_CHOICE,L"");add(trim_roles_[n][2],0,AG_LABEL,L"");}
add(fdstq_label_,0,AG_LABEL,L"FDSTQ:");add(fdstq_choice_,20,AG_CHOICE,L"");add(fdstq_setup_,21,AG_BUTTON,L"Setup");add(fdstq_test_,22,AG_BUTTON,L"Test");set_message(wide(runtime_->module_library().problem));}

void ModulePanel::rebuild(){if(!visible_)return;
    RepaintBatch batch(pane_);layout_dirty_=true;rebuilding_=true;
    auto previous_rows=std::move(rows_);auto previous_inventory=std::move(inventory_);auto previous_serials=std::move(fdstq_serials_);auto previous_configuration=std::move(configuration_);
    float scroll{};ag_panel_scroll_get(pane_.handle(),&scroll);
    try {configuration_=runtime_->modules();const auto settings=runtime_->settings();groups_[0].value(settings.fds_enabled);groups_[1].value(settings.pokeys_enabled);groups_[2].value(settings.phidgets_enabled);groups_[3].value(settings.trim_displays.enabled);

    auto item=[&](unsigned group,std::string key,std::wstring text,bool inspectable,unsigned state,const std::wstring& button=L"Properties"){InventoryRow row;row.group=group;row.key=std::move(key);row.text=std::move(text);ag_control_desc d{};d.kind=AG_LABEL;d.id=0;d.text=row.text.c_str();d.bounds={40,0,float(width_-100),60};row.label=gfx::Control(*context_,pane_.hwnd(),d,event,this);if(inspectable){d.id=10000+static_cast<unsigned>(inventory_.size());d.kind=AG_BUTTON;d.text=button.c_str();d.bounds={float(width_-90),0,float(width_-12),30};row.inspect=gfx::Control(*context_,pane_.hwnd(),d,event,this);}row.marker=std::make_unique<gfx::Canvas>(*context_,pane_.hwnd(),ag_rect{12,0,32,20});ag_draw mark{};mark.kind=AG_FILL_ELLIPSE;mark.rect={2,2,16,16};mark.color=state==2?ag_color{.05f,.5f,.18f,1}:state==1?ag_color{.85f,.65f,.05f,1}:ag_color{.08f,.35f,.85f,1};row.marker->submit({&mark,1});inventory_.push_back(std::move(row));};

    {for(const auto& card:view_.hardware)if(card.key.starts_with("fds-"))item(0,card.key,wide(card.name+" / "+card.key+" / "+connection(settings.fds_enabled,card.connected)),true,card.key.starts_with("fds-net:")?1:2);for(const auto& device:view_.modules)if(device.backend!=BackendKind::cpflight_serial){const auto* p=preferred(runtime_->module_library(),device);item(0,device.device,wide(device.name+" / "+connection(settings.fds_enabled,device.connected)),true,p?(p->confidence=="complete"?2:1):0);}if(view_.modules.empty() && std::none_of(inventory_.begin(),inventory_.end(),[](const auto& r){return r.group==0;}))item(0,"",settings.fds_enabled?L"No FDS devices discovered":L"Support disabled",false,0);}

    {for(const auto& card:view_.hardware)if(!card.key.starts_with("fds-") && !card.key.starts_with("phidget:"))item(1,card.key,wide(card.name+" / "+card.key+" / "+connection(settings.pokeys_enabled,card.connected)),true,2,wide(card.name));if(std::none_of(inventory_.begin(),inventory_.end(),[](const auto& r){return r.group==1;}))item(1,"",settings.pokeys_enabled?L"No PoKeys devices discovered":L"Support disabled",false,0);}
    // Card capability, not existing logical assignments, exposes the wizard.
    for(unsigned n=0;n<inventory_.size();++n){auto& row=inventory_[n];const auto card=std::find_if(view_.hardware.begin(),view_.hardware.end(),[&](const auto& d){return d.key==row.key;});
        if(card==view_.hardware.end() || (row.group!=0 && row.group!=1))continue;
        if(wizard_card(*card)){ag_control_desc d{};d.kind=AG_BUTTON;d.id=20000+n;d.text=L"Assign outputs";d.bounds={40,0,200,30};row.assign_outputs=gfx::Control(*context_,pane_.hwnd(),d,event,this);row.assign_outputs.enabled(card->connected && card->configured && (row.group==0?settings.fds_enabled:settings.pokeys_enabled));}
    }

    output_card_keys_={""};std::vector<std::wstring> output_labels{L"Select output card"};int output_selected{};
    for(const auto& card:view_.hardware)if(wizard_card(card) && card.connected && card.configured && (card.key.starts_with("phidget:")?settings.phidgets_enabled:card.key.starts_with("fds-")?settings.fds_enabled:settings.pokeys_enabled)){output_card_keys_.push_back(card.key);output_labels.push_back(wide(card.name+" / "+card.key));if(output_card_==card.key)output_selected=static_cast<int>(output_card_keys_.size()-1);}
    if(output_card_keys_.size()==2)output_selected=1;
    if(output_card_keys_.size()==1)output_labels[0]=L"No compatible output cards connected";
    output_card_=output_card_keys_[static_cast<std::size_t>(output_selected)];std::vector<const wchar_t*> output_items;for(const auto& label:output_labels)output_items.push_back(label.c_str());
    ag_choice_replace(output_cards_.handle(),output_items.data(),static_cast<std::uint32_t>(output_items.size()),output_selected);output_cards_.events(AG_CONTROL_NOTIFY_CHOICE_DECISION);output_launch_.enabled(output_card_keys_.size()>1);

    {for(const auto& d:view_.phidgets.devices)item(2,d.key.empty()?std::to_string(d.serial):d.key,wide(d.name+" / serial "+std::to_string(d.serial)+" / "+connection(settings.phidgets_enabled,true)),true,1);if(view_.phidgets.devices.empty())item(2,"",settings.phidgets_enabled?L"No Phidget devices discovered":L"Support disabled",false,0);}
    for(unsigned n=0;n<inventory_.size();++n)if(inventory_[n].group==2){const auto card=std::find_if(view_.hardware.begin(),view_.hardware.end(),[&](const auto& h){return h.key==inventory_[n].key;});if(card!=view_.hardware.end() && wizard_card(*card)){ag_control_desc d{};d.kind=AG_BUTTON;d.id=20000+n;d.text=L"Assign outputs";d.bounds={40,0,200,30};inventory_[n].assign_outputs=gfx::Control(*context_,pane_.hwnd(),d,event,this);inventory_[n].assign_outputs.enabled(card->connected && card->configured && settings.phidgets_enabled);}}

    for(const auto& d:view_.trim_displays.devices)item(3,trim_identity(d.identity),wide("Trim display "+trim_identity(d.identity)+(d.role==1?" / CPT / ":" / FO / ")+utf8(d.port.com_name)),false,d.ready && !d.duplicate?2:0);
    cp_ports_=view_.cpflight.ports;
    if(!settings.cpflight.port.com_name.empty() && std::none_of(cp_ports_.begin(),cp_ports_.end(),[&](const auto& p){return p.com_name==settings.cpflight.port.com_name;}))cp_ports_.push_back(settings.cpflight.port);
    {std::vector<std::wstring> labels{L"None"};int selected{};for(std::size_t n=0;n<cp_ports_.size();++n){const auto& port=cp_ports_[n];labels.push_back(port.com_name+L" / "+port.friendly_name);if(port.com_name==settings.cpflight.port.com_name)selected=static_cast<int>(n+1);}std::vector<const wchar_t*> items;for(const auto& text:labels)items.push_back(text.c_str());ag_choice_replace(cp_port_.handle(),items.data(),static_cast<std::uint32_t>(items.size()),selected);cp_port_.events(AG_CONTROL_NOTIFY_CHOICE_DECISION);}
    for(const auto& d:view_.modules)if(d.backend==BackendKind::cpflight_serial)item(4,d.device,wide(d.name+" / "+view_.cpflight.serial+" / "+utf8(view_.cpflight.com_name)),true,d.connected?2:0);
    trim_ports_=view_.trim_displays.ports;
    std::vector<std::wstring> port_labels{L"Select display COM port"};int port_selected{};
    for(unsigned n=0;n<trim_ports_.size();++n){const auto& p=trim_ports_[n];port_labels.push_back(p.com_name+L" / "+p.friendly_name);if(p.com_name==trim_selected_port_)port_selected=static_cast<int>(n+1);}
    std::vector<const wchar_t*> port_items;for(const auto& label:port_labels)port_items.push_back(label.c_str());
    ag_choice_replace(trim_port_choice_.handle(),port_items.data(),static_cast<std::uint32_t>(port_items.size()),port_selected);trim_port_choice_.events(AG_CONTROL_NOTIFY_CHOICE_DECISION);
    for(unsigned role=0;role<2;++role){auto& choices=trim_choices_[role];choices={0};std::vector<std::wstring> labels{L"Unassigned"};int selected{};
        const auto saved=settings.trim_displays.roles[role].device;
        for(const auto& d:view_.trim_displays.devices)if(d.role==role+1 && !d.duplicate){choices.push_back(d.identity);labels.push_back(wide(trim_identity(d.identity))+L" / "+d.port.com_name+(d.ready?L"":L" (unavailable)"));if(d.identity==saved)selected=static_cast<int>(choices.size()-1);}
        if(saved && !selected){choices.push_back(saved);labels.push_back(wide(trim_identity(saved))+L" (saved; unavailable)");selected=static_cast<int>(choices.size()-1);}
        std::vector<const wchar_t*> items;for(const auto& label:labels)items.push_back(label.c_str());ag_choice_replace(trim_roles_[role][1].handle(),items.data(),static_cast<std::uint32_t>(items.size()),selected);trim_roles_[role][1].events(AG_CONTROL_NOTIFY_CHOICE_DECISION);
    }
    fdstq_serials_={0};std::vector<std::wstring> tq_labels{L"Unassigned"};
    for(const auto& d:view_.phidgets.devices)if(d.name.find("InterfaceKit")!=d.name.npos){fdstq_serials_.push_back(d.serial);tq_labels.push_back(wide("InterfaceKit "+std::to_string(d.serial)));}
    if(settings.fdstq.serial>0 && std::find(fdstq_serials_.begin(),fdstq_serials_.end(),settings.fdstq.serial)==fdstq_serials_.end()){fdstq_serials_.push_back(settings.fdstq.serial);tq_labels.push_back(wide(std::to_string(settings.fdstq.serial)+" (configured)"));}
    std::vector<const wchar_t*> tq_items;for(const auto& label:tq_labels)tq_items.push_back(label.c_str());const auto tq_selected=std::find(fdstq_serials_.begin(),fdstq_serials_.end(),settings.fdstq.serial)-fdstq_serials_.begin();
    ag_choice_replace(fdstq_choice_.handle(),tq_items.data(),static_cast<uint32_t>(tq_items.size()),static_cast<int>(tq_selected));fdstq_choice_.events(AG_CONTROL_NOTIFY_CHOICE_DECISION);
    item(0,"fdstq",settings.fdstq.serial?wide("FDS FDSTQ / serial "+std::to_string(settings.fdstq.serial)):L"FDS FDSTQ",true,1);
    std::set<std::pair<std::string,std::string>> roles;for(const auto& p:runtime_->module_library().profiles)if(p.category!="FDSTQ")for(const auto& role:p.roles)roles.emplace(category(p),role);

    for(const auto& [type,role]:roles)for(const bool additional:{false,true}){if(additional && (type!="CDU" || !configuration_.additional_cdu_roles.count(role)))continue;Row row;row.category=type;row.role=role;row.additional_cdu=additional;row.key=type+"/"+role+(additional?"/Additional":"");const auto base=100+32*static_cast<unsigned>(rows_.size());auto add=[&](unsigned kind,const wchar_t* text){ag_control_desc d{};d.kind=kind;d.id=base+static_cast<unsigned>(row.controls.size());d.text=text;d.bounds={10,0,200,30};d.maximum=1;d.step=1;row.controls.emplace_back(*context_,pane_.hwnd(),d,event,this);};add(AG_LABEL,((additional?std::wstring(L"Additional "):std::wstring{})+caption(type,role)).c_str());add(AG_CHOICE,L"");add(AG_BUTTON,L"Test");add(AG_LABEL,L"");if(type=="CDU" && !additional){add(AG_CHECKBOX,L"Use another CDU for this role");row.controls.back().value(configuration_.additional_cdu_roles.count(role)!=0);}row.choices.push_back({"",""});row.labels.push_back(L"Unassigned");if(!additional && std::any_of(runtime_->module_library().profiles.begin(),runtime_->module_library().profiles.end(),[&](const auto& p){return category(p)==type && p.roles.size()==1;})){row.choices.push_back({"@auto",""});row.labels.push_back(L"Auto");}

        std::string selected_device,selected_profile;for(const auto& [id,assigned]:configuration_.rows)if(const auto* p=module_profile(runtime_->module_library(),assigned.applied.profile);p && category(*p)==type && assigned.applied.role==role && assigned.applied.additional_cdu==additional){selected_device=id;selected_profile=p->id;}

        for(const auto& device:view_.modules)if(device.parent.empty()){
            const auto occupied=configuration_.rows.find(device.device);if(type=="CDU" && occupied!=configuration_.rows.end() && !occupied->second.applied.profile.empty() && occupied->second.applied.role==role && occupied->second.applied.additional_cdu!=additional)continue;
            for(const auto& p:runtime_->module_library().profiles)if(p.model==device.model && p.backend==device.backend && device.update>=p.minimum_update && device.update<=p.maximum_update && p.unavailable.empty() && category(p)==type && std::find(p.roles.begin(),p.roles.end(),role)!=p.roles.end()){
                row.choices.push_back({device.device,p.id});row.labels.push_back(wide(device.device+" / "+p.name+(device.connected?"":" (unavailable)")));
            }
        }

        if(!selected_device.empty() && std::none_of(row.choices.begin(),row.choices.end(),[&](const auto& c){return c.first==selected_device && c.second==selected_profile;})){const auto& saved=configuration_.rows.at(selected_device);row.choices.push_back({selected_device,saved.applied.profile});row.labels.push_back(wide(selected_device+" (saved; unavailable)"));}

        int selected=0;for(std::size_t n=0;n<row.choices.size();++n)if(row.choices[n].first==selected_device && row.choices[n].second==selected_profile && !selected_device.empty())selected=static_cast<int>(n);if(selected_device.empty() && row.choices.size()>1 && row.choices[1].first=="@auto"){const auto auto_choice=configuration_.automatic.find(row.key);if(auto_choice==configuration_.automatic.end() || auto_choice->second)selected=1;}for(const auto& label:row.labels)row.controls[1].add(label.c_str());row.controls[1].value(selected);row.controls[1].events(AG_CONTROL_NOTIFY_CHOICE_DECISION);rows_.push_back(std::move(row));}

    update_states();layout();ag_panel_scroll_set(pane_.handle(),scroll);
        batch.finish();++generation_;signature_=pending_signature_;rebuilding_=false;
    }catch(const std::exception& e){rows_=std::move(previous_rows);inventory_=std::move(previous_inventory);fdstq_serials_=std::move(previous_serials);configuration_=std::move(previous_configuration);layout_dirty_=true;rebuilding_=false;set_message(wide(std::string("Modules refresh failed; retained previous rows: ")+e.what()));}
    catch(...){rows_=std::move(previous_rows);inventory_=std::move(previous_inventory);fdstq_serials_=std::move(previous_serials);configuration_=std::move(previous_configuration);layout_dirty_=true;rebuilding_=false;set_message(L"Modules refresh failed; retained previous rows");}


}

void ModulePanel::layout(){
    if(!visible_ || !layout_dirty_)return;
    if(message_dirty_){message_.text(message_text_.c_str());message_dirty_=false;}
    // The scrollbar occupies client width. Reflow once if setting the content
    // extent creates it, using the same native width for bounds and wrapping.
    for(unsigned pass=0;pass<2;++pass){
        const auto viewport=pane_.viewport();const float width=std::max(320.f,std::min(float(width_),viewport.right-viewport.left));
        const float third=(width-32)/3;merge_.bounds({10,10,10+third,40});merge_profiles_.bounds({18+third,10,18+2*third,40});diagnostics_.bounds({26+2*third,10,width-10,40});
        const float button=std::min(160.f,(width-32)/2);output_cards_.bounds({12,48,width-button-20,78});output_launch_.bounds({width-button-12,48,width-12,78});
        float y=86;const auto h=message_text_.empty()?0.f:std::max(30.f,message_.text_extent(width-24).y+8);message_.bounds({12,y,width-12,y+std::max(1.f,h)});y+=h+12;
        for(unsigned group=0;group<6;++group){
            if(group==3){const auto title_height=std::max(26.f,trim_heading_.text_extent(width-24).y+8);trim_heading_.bounds({12,y,width-12,y+title_height});y+=title_height;}
            groups_[group].bounds({12,y,width-12,y+30});y+=38;
            if(group==5){fi_open_.bounds({12,y,width-12,y+32});y+=40;const auto height=std::max(50.f,fi_status_.text_extent(width-24).y+8);fi_status_.bounds({12,y,width-12,y+height});y+=height+12;}
            if(group==4){cp_port_label_.bounds({12,y,100,y+30});if(width<560){cp_port_.bounds({106,y,width-12,y+30});y+=38;cp_refresh_.bounds({106,y,200,y+30});cp_connect_.bounds({208,y,width-12,y+30});}else {cp_port_.bounds({106,y,width-200,y+30});cp_refresh_.bounds({width-194,y,width-110,y+30});cp_connect_.bounds({width-104,y,width-12,y+30});}y+=38;const auto height=std::max(30.f,cp_status_.text_extent(width-24).y+8);cp_status_.bounds({12,y,width-12,y+height});y+=height+12;}
            for(auto& item:inventory_)if(item.group==group){item.marker->bounds({12,y+4,32,y+24});if(item.group==1 && item.inspect.handle()){item.inspect.bounds({40,y,width-12,y+30});y+=34;}const auto height=std::max(34.f,item.label.text_extent(width-(item.group==1?64:170)).y+10);item.label.bounds({40,y,width-(item.group==1?12:130),y+height});if(item.group!=1 && item.inspect.handle())item.inspect.bounds({width-120,y,width-12,y+30});y+=height+5;if(item.assign_outputs.handle()){item.assign_outputs.bounds({40,y,width-12,y+30});y+=35;}}
            if(group==0){fdstq_label_.bounds({12,y,100,y+30});fdstq_choice_.bounds({106,y,width-192,y+30});fdstq_setup_.bounds({width-186,y,width-106,y+30});fdstq_test_.bounds({width-100,y,width-12,y+30});y+=42;}
            if(group==4)for(auto& row:rows_){
                const float label=std::min(190.f,width/3);const auto row_height=std::max(30.f,row.controls[0].text_extent(label-12).y+6);
                row.controls[0].bounds({12,y,label,y+row_height});row.controls[1].bounds({label+6,y,width-130,y+30});row.controls[2].bounds({width-122,y,width-12,y+30});y+=row_height+4;
                const auto height=std::max(20.f,row.controls[3].text_extent(width-label-24).y+4);row.controls[3].bounds({label+6,y,width-12,y+height});y+=height+8;
                if(row.category=="CDU" && !row.additional_cdu){row.controls[4].bounds({label+6,y,width-12,y+28});y+=36;}
            }
            if(group==3){
                trim_port_label_.bounds({12,y,100,y+30});trim_port_choice_.bounds({106,y,width-116,y+30});trim_identify_.bounds({width-110,y,width-12,y+30});y+=38;
                const auto info_height=std::max(30.f,trim_message_.text_extent(width-24).y+8);trim_message_.bounds({12,y,width-12,y+info_height});y+=info_height+8;
                for(auto& role:trim_roles_){const auto label=std::min(170.f,width/3);role[0].bounds({12,y,label,y+30});role[1].bounds({label+6,y,width-12,y+30});y+=34;const auto status_height=std::max(24.f,role[2].text_extent(width-24).y+6);role[2].bounds({12,y,width-12,y+status_height});y+=status_height+10;}
            }
            y+=12;
        }
        pane_.extent(y);const auto after=pane_.viewport();if(after.right-after.left==width)break;
    }
    layout_dirty_=false;
}

void ModulePanel::update_states(){const auto tq_settings=runtime_->settings();
    groups_[5].value(tq_settings.flightillusion_enabled);
    const auto fi_text=wide((tq_settings.flightillusion_enabled?std::string():"Support disabled. ")+view_.flightillusion.status+"\nAircraft attitude / healthy standby IAS; barber pole uses fresh local Main exporter data. Gyro caging and pointer failure unavailable. Other mappings take priority over instrument controls.");
    if(fi_status_.text()!=fi_text){fi_status_.text(fi_text.c_str());layout_dirty_=true;}
    const auto cp_text=wide(view_.cpflight.status);if(cp_text!=cp_status_text_){cp_status_text_=cp_text;cp_status_.text(cp_text.c_str());layout_dirty_=true;}const std::wstring connect=tq_settings.cpflight.enabled?L"Disconnect":L"Connect";if(connect!=cp_connect_text_){cp_connect_text_=connect;cp_connect_.text(connect.c_str());}

    const auto& trim=view_.trim_displays;
    const std::wstring trim_status=!trim.enabled?L"Support disabled":trim.devices.empty()?L"No trim displays connected":L"";
    if(trim_status!=trim_message_text_){trim_message_text_=trim_status;trim_message_.text(trim_message_text_.c_str());layout_dirty_=true;}
    for(unsigned n=0;n<2;++n){const auto id=tq_settings.trim_displays.roles[n].device;const auto found=std::find_if(trim.devices.begin(),trim.devices.end(),[&](const auto& d){return d.identity==id && d.role==n+1;});const auto text=!id?L"":!trim.enabled?L"Support disabled":found==trim.devices.end()?L"Saved display unavailable":found->duplicate?L"Duplicate device identity":found->ready?L"Connected":L"Disconnected";if(text!=trim_role_status_[n]){trim_role_status_[n]=text;trim_roles_[n][2].text(text);layout_dirty_=true;}}
    const std::wstring tq_label=view_.fdstq_testing?L"Stop test":L"Test";
    if(tq_label!=fdstq_test_text_){fdstq_test_.text(tq_label.c_str());fdstq_test_text_=tq_label;}
    const bool tq_enabled=tq_settings.fds_enabled && tq_settings.fdstq.enabled && view_.fdstq_identification_available;
    if(fdstq_test_enabled_!=tq_enabled){fdstq_test_.enabled(tq_enabled);fdstq_test_enabled_=tq_enabled;}
    for(auto& row:rows_){
    const auto selected=static_cast<std::size_t>(row.controls[1].value());const auto choice=selected<row.choices.size()?row.choices[selected]:std::pair<std::string,std::string>{};
    const auto* p=module_profile(runtime_->module_library(),choice.second);const auto device=std::find_if(view_.modules.begin(),view_.modules.end(),[&](const auto& d){return d.device==choice.first;});
    const bool unknown_readouts=p && !p->identification_lamps.empty() && device!=view_.modules.end() && !device->readouts_known;
    const auto test=view_.identification==choice.first && !choice.first.empty();
    const bool enabled=tq_settings.fds_enabled && p && device!=view_.modules.end() && device->connected && (!p->identification_lamps.empty() || p->identification_brightness) && (test || !unknown_readouts || !device->applied);
    if(row.test_enabled!=enabled){row.controls[2].enabled(enabled);row.test_enabled=enabled;}
    const std::wstring label=test?L"Stop test":L"Test";if(row.test_text!=label){row.controls[2].text(label.c_str());row.test_text=label;}const auto auto_problem=view_.module_auto_errors.find(row.key);
    auto state=(choice.first=="@auto" && auto_problem!=view_.module_auto_errors.end())?wide(auto_problem->second):device==view_.modules.end()?choice.first=="@auto"?L"Waiting for device":choice.first.empty()?L"":L"Saved device unavailable":wide(connection(device->backend==BackendKind::cpflight_serial?tq_settings.cpflight.enabled:tq_settings.fds_enabled,device->connected));
    if(const auto error=role_errors_.find(row.key);error!=role_errors_.end())state=error->second;
    if(row.status_text!=state){row.controls[3].text(state.c_str());row.status_text=std::move(state);layout_dirty_=true;}}

    for(auto& item:inventory_)if(item.group==0 && !item.key.empty()){const auto d=std::find_if(view_.modules.begin(),view_.modules.end(),[&](const auto& m){return m.device==item.key;});if(d!=view_.modules.end()){const auto text=wide(d->name+" / "+connection(tq_settings.fds_enabled,d->connected));if(text!=item.text){item.text=text;item.label.text(text.c_str());layout_dirty_=true;}}}
    for(auto& item:inventory_)if(!item.key.empty()) {std::wstring text;if(item.group<2){const auto d=std::find_if(view_.hardware.begin(),view_.hardware.end(),[&](const auto& c){return c.key==item.key;});if(d!=view_.hardware.end())text=wide(d->name+" / "+d->key+" / "+connection(item.group==0?tq_settings.fds_enabled:tq_settings.pokeys_enabled,d->connected));}else if(item.group==2) {const auto d=std::find_if(view_.phidgets.devices.begin(),view_.phidgets.devices.end(),[&](const auto& c){return (c.key.empty()?std::to_string(c.serial):c.key)==item.key;});if(d!=view_.phidgets.devices.end())text=wide(d->name+" / serial "+std::to_string(d->serial)+" / "+connection(tq_settings.phidgets_enabled,true));}if(!text.empty() && text!=item.text){item.text=text;item.label.text(text.c_str());layout_dirty_=true;}}
}

void ModulePanel::set_message(std::wstring text){
    if(text!=message_text_){message_text_=std::move(text);message_dirty_=true;layout_dirty_=true;}
}

bool ModulePanel::choice_open() const {
    uint32_t open{};
    auto active=[&](const gfx::Control& choice){gfx::check(ag_choice_open(choice.handle(),&open));return open!=0;};
    if(active(output_cards_) || active(fdstq_choice_) || active(trim_port_choice_) || active(cp_port_))return true;
    for(const auto& role:trim_roles_)if(active(role[1]))return true;
    for(const auto& row:rows_)if(active(row.controls[1]))return true;
    if(!inspection_profiles_.empty() && association_controls_.size()>1 && active(association_controls_[1]))return true;
    for(std::size_t n=0;n<association_choices_.size();++n)if(active(association_controls_[3+2*n]))return true;
    return false;
}

void ModulePanel::apply_visuals(){
    // Discovery and export completion continue while hidden. Native controls use
    // the latest retained state on reveal, before the page accepts interaction.
    if(!visible_)return;
    const auto dpi=GetDpiForWindow(static_cast<HWND>(pane_.hwnd()));
    if(dpi!=layout_dpi_){layout_dpi_=dpi;layout_dirty_=true;}
    const bool defer=choice_open();
    // Native popup selection must not race status text/reflow/enable batches.
    // Keep the latest model and render it once the native popup closes.
    if(defer)return;
    if(message_dirty_){message_.text(message_text_.c_str());message_dirty_=false;}
    if(diagnostics_enabled_!=diagnostics_applied_){diagnostics_.enabled(diagnostics_enabled_);diagnostics_applied_=diagnostics_enabled_;}
    if(has_view_ && pending_signature_!=signature_ && !defer){rebuild();return;}
    // Native text/enable changes already invalidate their retained controls.
    // Avoid suppressing and invalidating the entire subtree on every unchanged
    // inventory tick: that erases child backgrounds at refresh frequency.
    if(has_view_)update_states();if(layout_dirty_){RepaintBatch batch(pane_);layout();batch.finish();}
}

void ModulePanel::arrange(int x,int y,int width,int height,bool visible){
    if(width_!=width || height_!=height)layout_dirty_=true;
    const ag_rect bounds{float(x),float(y),float(x+width),float(y+height)};
    if(!arranged_ || x!=x_ || y!=y_ || width!=width_ || height!=height_){pane_.bounds(bounds);x_=x;y_=y;arranged_=true;}
    width_=width;height_=height;
    if(visible_ && !visible)clear_tests();
    const bool reveal=visible && !visible_;visible_=visible;
    if(!visible){gfx::check(ag_visible(pane_.handle(),false));return;}
    if(reveal){RepaintBatch batch(pane_);apply_visuals();gfx::check(ag_visible(pane_.handle(),true));batch.finish();}
    else apply_visuals();
}

void ModulePanel::refresh(const RuntimeView& incoming){
    output_wizard_.refresh();
    const auto settings=runtime_->settings();view_=incoming;
    for(const auto& d:incoming.hardware){const bool enabled=d.key.starts_with("fds-")?settings.fds_enabled:d.key.starts_with("phidget:")?settings.phidgets_enabled:settings.pokeys_enabled;if(!enabled)continue;auto found=std::find_if(retained_hardware_.begin(),retained_hardware_.end(),[&](const auto& c){return c.key==d.key;});if(found==retained_hardware_.end()){if(retained_hardware_.size()>=96)continue;retained_hardware_.emplace_back();found=std::prev(retained_hardware_.end());}if(d.configured || found->key.empty())*found=d;else {found->key=d.key;found->name=d.name;}}
    for(const auto& d:retained_hardware_)if(!(d.key.starts_with("fds-")?settings.fds_enabled:d.key.starts_with("phidget:")?settings.phidgets_enabled:settings.pokeys_enabled) && std::none_of(view_.hardware.begin(),view_.hardware.end(),[&](const auto& c){return c.key==d.key;})){auto previous=d;previous.status="Support disabled; last discovered device";view_.hardware.push_back(std::move(previous));}
    if(settings.phidgets_enabled)retained_phidgets_=incoming.phidgets.devices;else {view_.phidgets.devices=retained_phidgets_;view_.phidgets.status="Support disabled; last discovered devices";}
    if(!settings.fds_enabled)for(auto& d:view_.modules){d.connected=false;d.connection="FDS support disabled";}
    std::ostringstream signature;signature<<settings.fds_enabled<<settings.pokeys_enabled<<settings.phidgets_enabled<<settings.fdstq.serial<<settings.fdstq.enabled;for(const auto& d:view_.modules)signature<<d.device<<d.model<<d.update<<d.parent<<d.profile<<d.role;for(const auto& d:view_.hardware)signature<<d.key<<d.revision<<d.connected<<d.configured;signature<<view_.hardware_revision;for(const auto& d:view_.phidgets.devices)signature<<d.serial<<d.key;signature<<settings.trim_displays.enabled;for(const auto& b:settings.trim_displays.roles)signature<<b.device<<utf8(b.port.com_name)<<utf8(b.port.instance_id);for(const auto& p:view_.trim_displays.ports)signature<<utf8(p.com_name)<<utf8(p.instance_id);for(const auto& d:view_.trim_displays.devices)signature<<d.identity<<d.role<<d.duplicate<<d.ready<<utf8(d.port.com_name);signature<<settings.cpflight.enabled<<utf8(settings.cpflight.port.com_name)<<settings.cpflight.serial<<view_.cpflight.generation<<view_.cpflight.connected;for(const auto& port:view_.cpflight.ports)signature<<utf8(port.com_name)<<utf8(port.instance_id);configuration_=runtime_->modules();for(const auto& role:configuration_.additional_cdu_roles)signature<<"additional-cdu:"<<role;for(const auto& [key,on]:configuration_.automatic)signature<<key<<on;for(const auto& [id,row]:configuration_.rows){signature<<id<<row.applied.profile<<row.applied.role<<row.applied.version<<row.applied.additional_cdu;for(const auto& [association,child]:row.applied.associations)signature<<association<<child;}pending_signature_=signature.str();has_view_=true;
    if(export_.valid() && export_.wait_for(std::chrono::seconds(0))==std::future_status::ready){set_message(wide(export_.get()));diagnostics_enabled_=true;}
    refresh_phidget();card_.refresh(view_);if(events_.empty())apply_visuals();else dispatch();
}

void ModulePanel::inspect(const std::string& key){transfer_.hide();if(key.starts_with("phidget:")){inspect_phidget(key);return;}if(!key.starts_with("fds-") && key!="fdstq"){card_.show(*context_,*runtime_,key);const auto cached=std::find_if(retained_hardware_.begin(),retained_hardware_.end(),[&](const auto& d){return d.key==key;});if(cached!=retained_hardware_.end())card_.remember(*cached);card_.refresh(view_);return;}if(key=="fdstq"){configure_fdstq();return;}const auto found=std::find_if(view_.modules.begin(),view_.modules.end(),[&](const auto& d){return d.device==key;});if(found==view_.modules.end()){inspect_generic(key);return;}if(!inspection_window_){inspection_window_=std::make_unique<gfx::Window>(*context_,L"HWIO — Properties",700,650,event,this);inspection_icons_.emplace(static_cast<HWND>(inspection_window_->hwnd()));inspection_window_->defer_close(true);ag_control_desc d{};d.kind=AG_PANEL;d.text=L"";d.bounds={10,10,678,628};inspection_pane_=gfx::Control(*context_,inspection_window_->hwnd(),d);d.kind=AG_LABEL;d.bounds={10,10,640,60};inspection_text_=gfx::Control(*context_,inspection_pane_.hwnd(),d);}transfer_.visible(false);inspected_=key;association_controls_.clear();association_choices_.clear();inspection_profiles_.clear();const auto saved=configuration_.rows.find(key);const ModuleProfile* p=saved==configuration_.rows.end()?preferred(runtime_->module_library(),*found):module_profile(runtime_->module_library(),saved->second.applied.profile);if(!p)p=preferred(runtime_->module_library(),*found);
    const auto text=found->name+"\n"+found->connection+"\n"+found->completeness+"\n"+(p?p->reference+"\n"+p->limitations:"No model profile")+"\nPhysical verification: consult the profile qualification register; completeness is not a hardware measurement.\n\n"+found->inspection;
    inspection_text_.text(wide(text).c_str());const auto extent=inspection_text_.text_extent(630);inspection_text_.bounds({10,10,640,std::max(50.f,extent.y+14)});float y=extent.y+28;
    auto control=[&](unsigned kind,unsigned id,const std::wstring& label){ag_control_desc d{};d.kind=kind;d.id=id;d.text=label.c_str();d.bounds={10,y,640,y+30};d.maximum=1;d.step=1;association_controls_.emplace_back(*context_,inspection_pane_.hwnd(),d,event,this);y+=38;};
    if(p && saved!=configuration_.rows.end() && !saved->second.applied.profile.empty()) {
        control(AG_LABEL,0,L"Explicit assembly profile and separate USB child-board associations");control(AG_CHOICE,9000,L"");int selected{};
        for(const auto& profile:runtime_->module_library().profiles)if(profile.model==found->model && profile.backend==found->backend && profile.category==p->category && std::find(profile.roles.begin(),profile.roles.end(),saved->second.applied.role)!=profile.roles.end()){if(profile.id==p->id)selected=static_cast<int>(inspection_profiles_.size());inspection_profiles_.push_back(profile.id);association_controls_.back().add(wide(profile.name).c_str());}association_controls_.back().value(selected);association_controls_.back().events(AG_CONTROL_NOTIFY_CHOICE_DECISION);
        for(const auto& association:p->associations){control(AG_LABEL,0,wide(association.role+" (model "+association.model+")"));control(AG_CHOICE,9001+static_cast<unsigned>(association_choices_.size()),L"");std::vector<std::string> choices{""};association_controls_.back().add(L"Unassociated");int current{};const auto previous=saved->second.applied.associations.find(association.id);
            for(const auto& child:view_.modules)if(child.device!=key && child.model==association.model && child.backend==p->backend){if(previous!=saved->second.applied.associations.end() && previous->second==child.device)current=static_cast<int>(choices.size());choices.push_back(child.device);association_controls_.back().add(wide(child.device+(child.connected?"":" (unavailable)")).c_str());}
            if(previous!=saved->second.applied.associations.end() && !current){current=static_cast<int>(choices.size());choices.push_back(previous->second);association_controls_.back().add(wide(previous->second+" (saved; unavailable)").c_str());}association_controls_.back().value(current);association_choices_.push_back(std::move(choices));
        }control(AG_BUTTON,9090,L"Apply reviewed profile / associations");
    }
    inspection_pane_.extent(y+10);inspection_window_->fit();inspection_window_->show(true);}

void ModulePanel::refresh_phidget(){
    if(!inspection_window_ || !inspected_.starts_with("phidget:"))return;
    std::ostringstream text;text<<inspected_<<"\nNative readings are separate from assignment counts (0–16383).\nServo control and assignments are deferred.\n\n";
    for(const auto& c:view_.phidgets.channels)if(c.address.device==inspected_ && c.channel_class==6){text<<"Digital outputs: configured source failure while owned; Off on release/shutdown.\n\n";break;}
    for(const auto& c:view_.phidgets.channels)if(c.address.device==inspected_ && c.address.model==42){text<<"1065: PWM drive; native braking/back-EMF capability. No hardware watchdog or current limiting. Back-EMF is not measured shaft speed.\n\n";break;}
    for(const auto& r:view_.phidget_readings)if(r.channel.address.device==inspected_){text<<r.channel.name<<" / channel "<<r.channel.address.channel<<": ";if(r.count)text<<*r.count;else if(r.value)text<<*r.value;else text<<"Unavailable";text<<' '<<r.unit<<" / "<<r.status;if(r.interval_ms)text<<" / effective "<<r.interval_ms<<" ms (supported "<<r.minimum_interval_ms<<"–"<<r.maximum_interval_ms<<" ms)";text<<'\n';}
    const auto caption=wide(text.str());if(inspection_text_.text()!=caption){
        inspection_text_.text(caption.c_str());const float height=std::max(60.f,inspection_text_.text_extent(630).y+14);inspection_text_.bounds({10,10,640,height});float y=height+20;
        for(auto& control:association_controls_){control.bounds({10,y,640,y+30});y+=38;}y+=transfer_.arrange(10,y,630);inspection_pane_.extent(y+10);
    }
}
void ModulePanel::inspect_generic(const std::string& key){
    const auto card=std::find_if(view_.hardware.begin(),view_.hardware.end(),[&](const auto& d){return d.key==key;});if(card==view_.hardware.end())return;
    if(!inspection_window_){inspection_window_=std::make_unique<gfx::Window>(*context_,L"HWIO — Properties",700,650,event,this);inspection_icons_.emplace(static_cast<HWND>(inspection_window_->hwnd()));inspection_window_->defer_close(true);ag_control_desc d{};d.kind=AG_PANEL;d.bounds={10,10,678,628};inspection_pane_=gfx::Control(*context_,inspection_window_->hwnd(),d);d.kind=AG_LABEL;d.bounds={10,10,640,360};inspection_text_=gfx::Control(*context_,inspection_pane_.hwnd(),d);}
    inspected_=key;association_controls_.clear();association_choices_.clear();inspection_profiles_.clear();
    const auto text=wide(card->name+"\n"+card->key+"\n"+card->status+"\nFirmware: "+card->firmware);inspection_text_.text(text.c_str());const auto height=std::max(60.f,inspection_text_.text_extent(630).y+14);inspection_text_.bounds({10,10,640,height});
    transfer_.attach(*context_,*inspection_window_,inspection_pane_.hwnd(),*runtime_);transfer_.reflow([this]{if(inspected_.starts_with("phidget:"))inspect_phidget(inspected_);else inspect_generic(inspected_);});transfer_.visible(true);transfer_.device(key);const auto y=height+20+transfer_.arrange(10,height+20,630);inspection_pane_.extent(y+10);inspection_window_->fit();inspection_window_->show(true);
}
void ModulePanel::inspect_phidget(const std::string& key){
    if(!inspection_window_){inspection_window_=std::make_unique<gfx::Window>(*context_,L"HWIO — Properties",700,650,event,this);inspection_icons_.emplace(static_cast<HWND>(inspection_window_->hwnd()));inspection_window_->defer_close(true);ag_control_desc d{};d.kind=AG_PANEL;d.text=L"";d.bounds={10,10,678,628};inspection_pane_=gfx::Control(*context_,inspection_window_->hwnd(),d);d.kind=AG_LABEL;d.bounds={10,10,640,360};inspection_text_=gfx::Control(*context_,inspection_pane_.hwnd(),d);}
    transfer_.attach(*context_,*inspection_window_,inspection_pane_.hwnd(),*runtime_);transfer_.reflow([this]{if(inspected_.starts_with("phidget:"))inspect_phidget(inspected_);else inspect_generic(inspected_);});transfer_.visible(true);transfer_.device(key);inspected_=key;association_controls_.clear();association_choices_.clear();inspection_profiles_.clear();refresh_phidget();const auto extent=inspection_text_.text_extent(630);inspection_text_.bounds({10,10,640,std::max(60.f,extent.y+14)});float y=std::max(60.f,extent.y+14)+20;
    std::set<int> ports;for(const auto& c:view_.phidgets.channels)if(c.address.device==key && c.channel_class==31)ports.insert(c.address.channel);
    const auto settings=runtime_->hardware();for(const auto port:ports){bool voltage=false;for(const auto& p:settings.phidget_ports)if(p.device==key && p.channel==unsigned(port))voltage=p.voltage;ag_control_desc d{};d.kind=AG_BUTTON;d.id=8100+static_cast<unsigned>(port);const auto caption=L"Analogue port "+std::to_wstring(port)+(voltage?L": Voltage — change to Ratio":L": Ratio — change to Voltage");d.text=caption.c_str();d.bounds={10,y,640,y+30};association_controls_.emplace_back(*context_,inspection_pane_.hwnd(),d,event,this);y+=38;}
    y+=transfer_.arrange(10,y,630);inspection_pane_.extent(y+10);inspection_window_->fit();inspection_window_->show(true);
}
void ModulePanel::dispatch(){
// Acceptance can precede native close. Refresh drains retained actions before
// rebuilding their captured row mapping, once the choice is available.
if(choice_open())return;
auto events=std::move(events_);events_.clear();for(const auto& action:events){const auto id=action.id;if(action.generation!=generation_)continue;
if(id==50){show_flightillusion();continue;}
if(id==15){std::string error;if(!runtime_->enable_flightillusion(groups_[5].value()!=0,error))set_message(wide(error));if(groups_[5].value()==0)hide_flightillusion();continue;}
if(immediate_choice(id) && action.kind==AG_CHOICE_CANCELLED){
    // Native preview is not a model change. Restore the captured opening index
    // only after the library reports that the choice is safe to mutate.
    if(id>=100 && id<8000){const auto row=(id-100)/32;if(row<rows_.size() && rows_[row].key==action.key)rows_[row].controls[1].value(action.selection);}
    else if(id==5)output_cards_.value(action.selection);
    else if(id==20)fdstq_choice_.value(action.selection);
    else if(id==30)trim_port_choice_.value(action.selection);
    else if(id==40)cp_port_.value(action.selection);
    else if(id==32 || id==33)trim_roles_[id-32][1].value(action.selection);
    else if(id==9000 && action.key==inspected_ && association_controls_.size()>1)association_controls_[1].value(action.selection);
    continue;
}
if(immediate_choice(id) && (action.kind!=AG_CHOICE_ACCEPTED || action.selection<0))continue;
if(id>=8100 && id<8132){if(action.key!=inspected_ || !inspected_.starts_with("phidget:"))continue;auto next=runtime_->hardware();const unsigned port=id-8100;auto found=std::find_if(next.phidget_ports.begin(),next.phidget_ports.end(),[&](const auto& p){return p.device==inspected_ && p.channel==port;});if(found==next.phidget_ports.end())next.phidget_ports.push_back({inspected_,port,true});else found->voltage=!found->voltage;std::string error;if(!runtime_->save_hardware(next,error))set_message(wide(error));else inspect_phidget(inspected_);continue;}
if(id==5){const auto selected=static_cast<std::size_t>(action.selection);if(selected<output_card_keys_.size())output_card_=output_card_keys_[selected];continue;}
if(id==4){std::string error;if(output_card_.empty())set_message(L"Select an output card above first");else if(!output_wizard_.show(*context_,*runtime_,output_card_,error))set_message(wide("Output identification: "+error));continue;}
if(id==40){const auto selected=static_cast<std::size_t>(action.selection);std::string error;if(selected<=cp_ports_.size() && !runtime_->select_cpflight_port(selected?cp_ports_[selected-1]:psx::hardware::SerialPort{},error))set_message(wide(error));signature_.clear();continue;}
if(id==41){runtime_->refresh_cpflight();continue;}
if(id==42){auto settings=runtime_->settings();settings.cpflight.enabled=!settings.cpflight.enabled;std::string error;if(!runtime_->save_settings(settings,error))set_message(wide(error));signature_.clear();continue;}
if(id==30){const auto selected=static_cast<std::size_t>(action.selection);trim_selected_port_=selected && selected<=trim_ports_.size()?trim_ports_[selected-1].com_name:L"";continue;}
if(id==31){const auto selected=static_cast<std::size_t>(trim_port_choice_.value());std::string error;if(!selected || selected>trim_ports_.size())set_message(L"Select the trim display's COM port first");else if(!runtime_->identify_trim_display(trim_ports_[selected-1],error))set_message(wide(error));else set_message(L"Identifying only the selected display port…");continue;}
if(id==32 || id==33){const auto role=id-32;const auto selected=static_cast<std::size_t>(action.selection);std::string error;if(selected<trim_choices_[role].size() && !runtime_->assign_trim_display(role+1,trim_choices_[role][selected],error))set_message(wide(error));signature_.clear();continue;}
if(id==20){const auto selected=static_cast<std::size_t>(action.selection);if(selected<fdstq_serials_.size()){auto settings=runtime_->settings();settings.fdstq.serial=fdstq_serials_[selected];if(!settings.fdstq.serial)settings.fdstq.enabled=false;std::string error;if(!runtime_->save_settings(settings,error))set_message(wide(error).c_str());signature_.clear();}continue;}
if(id==21){configure_fdstq();continue;}if(id==22){runtime_->identify_fdstq(!view_.fdstq_testing);continue;}
if(id==60 || id==61){auto settings=runtime_->settings();auto& c=settings.fdstq;int serial{},port{};std::string error;
if(!integer(utf8(association_controls_[2].text()),serial) || !integer(utf8(association_controls_[4].text()),port) || port<1){inspection_text_.text(L"Enter a positive exact serial and valid server port");continue;}
c.serial=serial;c.port=static_cast<unsigned>(port);c.host=utf8(association_controls_[3].text());c.enabled=association_controls_[0].value()!=0;c.remote=association_controls_[1].value()!=0;c.paired=association_controls_[5].value()!=0;c.inverted_switches=0;
for(unsigned n=0;n<8;++n)if(association_controls_[6+n].value()!=0)c.inverted_switches|=1u<<n;
for(unsigned n=0;n<5;++n){const auto selected=static_cast<std::size_t>(association_controls_[14+n].value());if(selected<fdstq_output_ids_.size())c.outputs[n]=fdstq_output_ids_[selected];}
if(!integer(utf8(association_controls_[19].text()),c.hub_serial) || !integer(utf8(association_controls_[40].text()),c.servo_serial)){inspection_text_.text(L"Enter exact integer motor hub and servo serials (0 when unused)");continue;}c.pair_follow=static_cast<unsigned>(association_controls_[20].value());
auto number=[&](unsigned index,double& value){const auto text=utf8(association_controls_[index].text());char* end{};value=strtod(text.c_str(),&end);return end!=text.c_str() && *end==0 && std::isfinite(value);};bool numbers=true;
for(unsigned n=0;n<3;++n){auto& m=c.steppers[n];const unsigned at=21+n*5;m.enabled=association_controls_[at].value()!=0;m.reversed=association_controls_[at+1].value()!=0;numbers=number(at+2,m.current) && number(at+3,m.acceleration) && number(at+4,m.velocity) && numbers;}
c.trim_enabled=association_controls_[36].value()!=0;c.servo_enabled=association_controls_[41].value()!=0;numbers=number(37,c.trim_counts_per_degree) && number(38,c.trim_velocity) && number(39,c.trim_acceleration) && number(42,c.servo_zero) && number(43,c.servo_full) && number(44,c.servo_velocity) && number(45,c.servo_acceleration) && numbers;
if(!numbers){inspection_text_.text(L"Enter finite numeric motor/servo calibration values");continue;}
if(!runtime_->save_settings(settings,error) || (id==61 && !runtime_->assign_fdstq(error)))inspection_text_.text(wide(error).c_str());
else {signature_.clear();inspection_text_.text(id==61?L"Switch assignments applied. Flight axes staged: capture physical endpoints/detents and Apply in Flight axes.":L"FDSTQ settings saved. Assign and calibrate before routing axes.");}continue;}
if(id==1 || id==2){std::size_t added{};std::string error;const auto ok=id==1?runtime_->merge_output_catalogue(added,error):runtime_->merge_module_library(added,error);set_message(wide(ok?"Added "+std::to_string(added)+" definitions. Restart HWIO to load them.":error).c_str());continue;}if(id==3){if(export_.valid())continue;std::wstring path;std::uint32_t accepted{};ag_file_dialog(context_->handle(),owner_->hwnd(),2,L"Choose diagnostic bundle folder",L"",L"",L"",[](void* user,const wchar_t* selected){*static_cast<std::wstring*>(user)=selected;},&path,&accepted);if(accepted){const auto directory=std::filesystem::path(path)/(L"fds-diagnostics-"+std::to_wstring(GetTickCount64()));diagnostics_enabled_=false;set_message(L"Saving diagnostics…");export_=std::async(std::launch::async,[runtime=runtime_,directory]{std::string error;return runtime->export_fds_diagnostics(directory,error)?"Saved "+utf8(directory.wstring()):error;});}continue;}if(id>=10 && id<14){auto settings=runtime_->settings();if(id==10)settings.fds_enabled=groups_[0].value()!=0;if(id==11)settings.pokeys_enabled=groups_[1].value()!=0;if(id==12)settings.phidgets_enabled=groups_[2].value()!=0;if(id==13)settings.trim_displays.enabled=groups_[3].value()!=0;settings.provider_selection_initialized=true;std::string error;if(!runtime_->save_settings(settings,error))set_message(wide(error).c_str());signature_.clear();continue;}if(id>=9000 && id<10000){const auto stored=configuration_.rows.find(inspected_);if(stored==configuration_.rows.end())continue;const std::set<std::string> applied_rows{inspected_};
        if(id==9000 && action.key==inspected_ && association_controls_.size()>1){const auto selected=static_cast<std::size_t>(action.selection);if(selected<inspection_profiles_.size()){auto updated=runtime_->modules();auto& row=updated.rows.at(inspected_);row.draft=row.applied;row.draft.profile=inspection_profiles_[selected];row.draft.associations.clear();row.pending="apply";std::string error;if(runtime_->save_modules(updated,error,&applied_rows)){configuration_=runtime_->modules();inspect(inspected_);signature_.clear();}else set_message(wide(error).c_str());}}
        else if(id==9090){const auto* profile=module_profile(runtime_->module_library(),stored->second.applied.profile);if(!profile)continue;auto updated=runtime_->modules();auto& row=updated.rows.at(inspected_);row.draft=row.applied;row.draft.associations.clear();for(std::size_t n=0;n<association_choices_.size();++n){const auto selected=static_cast<std::size_t>(association_controls_[3+2*n].value());if(selected<association_choices_[n].size() && !association_choices_[n][selected].empty())row.draft.associations[profile->associations[n].id]=association_choices_[n][selected];}row.pending="apply";std::string error;if(!runtime_->save_modules(updated,error,&applied_rows))set_message(wide(error).c_str());else{configuration_=runtime_->modules();signature_.clear();inspect(inspected_);}}
        continue;}if(id>=20000){const auto n=id-20000;if(n<inventory_.size()){std::string error;if(!output_wizard_.show(*context_,*runtime_,inventory_[n].key,error))set_message(wide("Output identification: "+error));}continue;}if(id>=10000){const auto n=id-10000;if(n<inventory_.size())inspect(inventory_[n].key);continue;}if(id<100)continue;const auto index=(id-100)/32,field=(id-100)%32;if(index>=rows_.size() || rows_[index].key!=action.key)continue;auto& row=rows_[index];const auto selected=static_cast<std::size_t>(field==1?action.selection:row.controls[1].value());if(selected>=row.choices.size())continue;const auto choice=row.choices[selected];std::string error;if(field==4 && row.category=="CDU" && !row.additional_cdu){if(!runtime_->additional_cdu(row.role,row.controls[4].value()!=0,error)){role_errors_[row.key]=wide(error);rebuild();set_message(wide(error));layout();}else{role_errors_.erase(row.key);set_message(L"");signature_.clear();}}else if(field==1){runtime_->clear_module_tests();const bool automatic=choice.first=="@auto";if(!runtime_->assign_module_role(row.category,row.role,automatic?"":choice.first,choice.second,automatic,error,row.additional_cdu)){role_errors_[row.key]=wide(error);rebuild();set_message(wide(error));layout();}else {role_errors_.erase(row.key);set_message(L"");signature_.clear();}}else if(field==2 && visible_){if(!runtime_->identify_module(choice.first,choice.second,error))set_message(wide(error).c_str());}}apply_visuals();}

void ModulePanel::clear_tests(){hide_flightillusion();output_wizard_.hide();if(runtime_)runtime_->clear_module_tests();view_.identification.clear();view_.fdstq_testing=false;}

void ModulePanel::configure_fdstq(bool show){
    clear_tests();if(!inspection_window_){inspection_window_=std::make_unique<gfx::Window>(*context_,L"HWIO — Properties",700,650,event,this);inspection_icons_.emplace(static_cast<HWND>(inspection_window_->hwnd()));inspection_window_->defer_close(true);ag_control_desc d{};d.kind=AG_PANEL;d.text=L"";d.bounds={10,10,678,628};inspection_pane_=gfx::Control(*context_,inspection_window_->hwnd(),d);d.kind=AG_LABEL;d.bounds={10,10,640,92};inspection_text_=gfx::Control(*context_,inspection_pane_.hwnd(),d);}
    transfer_.visible(false);inspected_="fdstq";association_controls_.clear();association_choices_.clear();inspection_profiles_.clear();
    inspection_text_.text(L"FDSTQ uses Phidget22, independently of interfaceIT. Enter the exact InterfaceKit 8/8/8 serial. An empty network host uses Phidget server discovery. Setup does not calibrate physical travel. Configure exact motor hub/servo serials and physical tuning below. Continuous motion requires device watchdog support. Flight-axis calibration remains in Flight axes.");inspection_text_.bounds({10,10,640,106});
    const auto c=runtime_->settings().fdstq;float y=116;
    auto control=[&](unsigned kind,unsigned id,const std::wstring& text){ag_control_desc d{};d.kind=kind;d.id=id;d.text=text.c_str();d.bounds={280,y,640,y+30};d.maximum=1;d.step=1;association_controls_.emplace_back(*context_,inspection_pane_.hwnd(),d,event,this);y+=38;};
    auto label=[&](const wchar_t* text){ag_control_desc d{};d.kind=AG_LABEL;d.text=text;d.bounds={10,y,270,y+30};association_controls_.emplace_back(*context_,inspection_pane_.hwnd(),d);const auto height=std::max(30.f,association_controls_.back().text_extent(260).y+8);association_controls_.back().bounds({10,y,270,y+height});};
    // Keep editable fields first so status refresh never replaces an open choice.
    std::vector<std::pair<float,std::wstring>> captions;
    auto field=[&](unsigned kind,const std::wstring& caption,const std::wstring& text){ag_control_desc measure_desc{};measure_desc.kind=AG_LABEL;measure_desc.text=caption.c_str();measure_desc.bounds={10,0,270,30};gfx::Control measure(*context_,inspection_pane_.hwnd(),measure_desc);const auto height=std::max(30.f,measure.text_extent(260).y+8);captions.emplace_back(y,caption);control(kind,0,text);y+=height-30;};
    field(AG_CHECKBOX,L"FDSTQ support",L"Enable assembly");association_controls_.back().value(c.enabled);
    field(AG_CHECKBOX,L"Connection",L"Network Phidget server");association_controls_.back().value(c.remote);
    field(AG_EDIT,L"Exact InterfaceKit serial",std::to_wstring(c.serial));field(AG_EDIT,L"Server host (empty = discover)",wide(c.host));field(AG_EDIT,L"Server port",std::to_wstring(c.port));
    field(AG_CHECKBOX,L"737 → 747 compatibility",L"Pair left 1–2, right 3–4");association_controls_.back().value(c.paired);
    const auto device=fdstq_device(c.serial);for(unsigned n=0;n<8;++n){field(AG_CHECKBOX,wide(device.controls[n].name),L"Invert contact (active low)");association_controls_.back().value((c.inverted_switches&(1u<<n))!=0);}
    fdstq_output_ids_={""};std::vector<std::wstring> output_labels{L"Unassigned (off)"};for(const auto& [id,definition]:active_catalogue().digital_outputs)if(definition->qualified && definition->unavailable.empty()){fdstq_output_ids_.push_back(id);output_labels.push_back(wide(definition->name));}
    static constexpr const wchar_t* outputs[]{L"DO0 Parking brake lamp",L"DO1 Parking solenoid",L"DO2 Left fuel lamp",L"DO3 Right fuel lamp",L"DO4 Backlight"};
    for(unsigned n=0;n<5;++n){field(AG_CHOICE,outputs[n],L"");int selected{};for(std::size_t i=0;i<fdstq_output_ids_.size();++i){association_controls_.back().add(output_labels[i].c_str());if(fdstq_output_ids_[i]==c.outputs[n])selected=static_cast<int>(i);}if(!c.outputs[n].empty() && selected==0){fdstq_output_ids_.push_back(c.outputs[n]);output_labels.push_back(wide(c.outputs[n]+" (unavailable)"));association_controls_.back().add(output_labels.back().c_str());selected=static_cast<int>(fdstq_output_ids_.size()-1);}association_controls_.back().value(selected);}
    field(AG_EDIT,L"Exact VINT motor hub serial",std::to_wstring(c.hub_serial));
    field(AG_CHOICE,L"Paired lever follows",L"");association_controls_.back().add(L"First engine (1 / 3)");association_controls_.back().add(L"Average of engine pair");association_controls_.back().add(L"Only when pair agrees");association_controls_.back().value(c.pair_follow);
    static constexpr const wchar_t* motors[]{L"Left throttle",L"Right throttle",L"Speedbrake"};
    for(unsigned n=0;n<3;++n){const auto& m=c.steppers[n];const std::wstring title=motors[n];field(AG_CHECKBOX,title+L" motor",L"Enable");association_controls_.back().value(m.enabled);field(AG_CHECKBOX,title+L" motor direction",L"Reverse motor direction");association_controls_.back().value(m.reversed);field(AG_EDIT,title+L" current limit (A)",std::to_wstring(m.current));field(AG_EDIT,title+L" acceleration (raw step units/s²)",std::to_wstring(m.acceleration));field(AG_EDIT,title+L" maximum velocity (raw step units/s)",std::to_wstring(m.velocity));}
    field(AG_CHECKBOX,L"Trim wheel motor",L"Enable");association_controls_.back().value(c.trim_enabled);field(AG_EDIT,L"Signed Hall counts / PSX trim degree",std::to_wstring(c.trim_counts_per_degree));field(AG_EDIT,L"Trim maximum duty (0..1)",std::to_wstring(c.trim_velocity));field(AG_EDIT,L"Trim acceleration (duty/s)",std::to_wstring(c.trim_acceleration));
    field(AG_EDIT,L"Exact trim indicator servo serial",std::to_wstring(c.servo_serial));field(AG_CHECKBOX,L"Trim indicator",L"Enable");association_controls_.back().value(c.servo_enabled);field(AG_EDIT,L"Servo position at PSX 0° trim (0..180)",std::to_wstring(c.servo_zero));field(AG_EDIT,L"Servo position at PSX 15° trim (0..180)",std::to_wstring(c.servo_full));field(AG_EDIT,L"Servo maximum velocity",std::to_wstring(c.servo_velocity));field(AG_EDIT,L"Servo acceleration",std::to_wstring(c.servo_acceleration));
    control(AG_BUTTON,60,L"Save FDSTQ settings");control(AG_BUTTON,61,L"Assign switches and prepare Flight axes");
    for(const auto& [at,text]:captions){const auto bottom=y;y=at;label(text.c_str());y=bottom;}
    inspection_pane_.extent(y+12);inspection_window_->fit();if(show)inspection_window_->show(true);
}

}


namespace hwio {
void __cdecl ModulePanel::fi_event(void* raw,std::uint32_t id,std::uint32_t kind,double) noexcept {
    auto& self=*static_cast<ModulePanel*>(raw);if(!self.fi_window_)return;
    try {if(kind==AG_CLOSE){self.hide_flightillusion();return;}
        if(kind==AG_DISPATCH&&id==flightillusion::Panel::dispatch_id)self.fi_panel_.dispatch();
        if(kind==AG_TIMER&&self.fi_visible_)self.fi_panel_.refresh();
        if(kind==AG_RESIZE&&self.fi_visible_){const auto r=self.fi_window_->client_bounds();self.fi_panel_.arrange(0,0,int(r.right),int(r.bottom),true);}
    }catch(...){self.hide_flightillusion();}
}
void ModulePanel::show_flightillusion(){
    if(!fi_window_){fi_window_=std::make_unique<gfx::Window>(*context_,L"HWIO - Flight Illusion",1000,800,fi_event,this);fi_icons_.emplace(static_cast<HWND>(fi_window_->hwnd()));fi_window_->defer_close(true);fi_panel_.attach(*context_,*fi_window_,runtime_->flightillusion(),fi_window_->hwnd());fi_panel_.connection_handler([this](bool enabled,std::string& error){return runtime_->enable_flightillusion(enabled,error);},[this]{return runtime_->settings().flightillusion_enabled;});fi_window_->timer(1,100);}
    fi_visible_=true;const auto r=fi_window_->client_bounds();fi_panel_.arrange(0,0,int(r.right),int(r.bottom),true);fi_panel_.refresh();fi_window_->show();
}
void ModulePanel::hide_flightillusion(){fi_panel_.hide();fi_visible_=false;if(fi_window_)fi_window_->show(false);}
void ModulePanel::hide_inspection(){hide_flightillusion();output_wizard_.hide();card_.hide();transfer_.hide();if(inspection_window_)inspection_window_->show(false);}
}
