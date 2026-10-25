#include "hwio/fds_modules.hpp"
#include "hwio/hardware_router.hpp"
#include "hwio/axis_setup.hpp"
#include <objbase.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hwio {
std::pair<std::size_t,std::size_t> merge_fds_display_definitions(Catalogue& current,ModuleLibrary& modules,const Catalogue& defaults,const ModuleLibrary& library){
    constexpr std::string_view id="fds.usb.331a.747.cdu.psx747";
    const auto* profile=module_profile(library,id);if(!profile || profile->input_only)throw std::runtime_error("Unified 747 CDU profile missing from defaults");
    const std::set<std::string> names{"out.cdu.l.lcdpower","out.cdu.c.lcdpower","out.cdu.r.lcdpower"};
    for(const auto& name:names)if(!defaults.digital_outputs.count(name))throw std::runtime_error("CDU LCD definition missing from defaults: "+name);
    auto additions=defaults;
    // Merge reads only groups, chapters and schema from this narrowed value;
    // derived lookup pointers are deliberately not used as runtime definitions.
    for(auto& group:additions.groups){group.actions.clear();group.targets.clear();group.observations.clear();group.cockpit.clear();group.displays.clear();std::erase_if(group.digital_outputs,[&](const auto& d){return !names.count(d.id);});}
    std::erase_if(additions.groups,[](const auto& g){return g.digital_outputs.empty();});
    const auto definitions=merge_digital_catalogue(current,additions);
    std::size_t profiles{};
    // Compare operational fields before upgrading an existing stock profile.
    // Operator mappings survive unchanged; their owner can review them explicitly.
    const auto wiring=[](const ModuleProfile& p){
        std::ostringstream out;out<<p.model<<' '<<unsigned(p.backend)<<' '<<p.minimum_update<<' '<<p.maximum_update<<' '<<p.input_only<<' '<<p.identification_brightness<<'\n';
        for(const auto& r:p.roles)out<<"role "<<std::quoted(r)<<'\n';
        for(const auto& i:p.inputs){out<<"key "<<i.channel<<' '<<std::quoted(i.action)<<' '<<std::quoted(i.association);for(auto n:i.contacts)out<<" c"<<n;for(auto n:i.states)out<<" s"<<n;out<<'\n';}
        for(const auto& i:p.analogue)out<<"adc "<<i.channel<<' '<<i.minimum<<' '<<i.maximum<<' '<<i.bits<<' '<<i.definition<<' '<<std::quoted(i.association)<<' '<<i.inverted<<' '<<i.optional<<' '<<i.rotary<<'\n';
        for(const auto& i:p.lamps)out<<"led "<<i.channel<<' '<<i.output<<' '<<std::quoted(i.association)<<'\n';
        for(const auto& i:p.actuators)out<<"line "<<i.channel<<' '<<i.output<<' '<<std::quoted(i.association)<<'\n';
        for(const auto& i:p.displays)out<<"display "<<i.channel<<' '<<i.width<<' '<<i.display<<' '<<std::quoted(i.association)<<' '<<i.loss_pattern<<'\n';
        for(const auto& i:p.associations)out<<"child "<<i.id<<' '<<i.model<<' '<<std::quoted(i.role)<<' '<<i.required<<'\n';
        for(auto n:p.identification_lamps)out<<"identify "<<n<<'\n';return out.str();
    };
    auto* existing=const_cast<ModuleProfile*>(module_profile(modules,id));
    if(!existing){auto selected=library;selected.profiles={*profile};profiles=merge_modules(modules,selected);}
    else if(existing->version==1 && existing->unavailable.empty()){
        auto old=*profile;old.input_only=true;old.lamps.clear();old.actuators.clear();old.displays.clear();
        if(wiring(*existing)==wiring(old)){*existing=*profile;++profiles;}
    }
    for(const auto old_id:{"fds.usb.331a.boeing.psx747","fds.usb.331a.747.cdu.update4.psx747"}){
        auto* old=const_cast<ModuleProfile*>(module_profile(modules,old_id));const auto* replacement=module_profile(library,old_id);
        if(old && replacement && old->version==replacement->version && old->unavailable.empty() && !replacement->unavailable.empty() && wiring(*old)==wiring(*replacement)){old->unavailable=replacement->unavailable;++profiles;}
    }
    const auto* update1=module_profile(library,"fds.usb.3305.update1.xpndr.psx747");
    const auto* update2=module_profile(library,"fds.usb.3305.xpndr.psx747");
    if(!update1 || !update2)throw std::runtime_error("Qualified transponder revision profiles missing from defaults");
    if(!module_profile(modules,update1->id)){auto selected=library;selected.profiles={*update1};profiles+=merge_modules(modules,selected);}
    else if(auto* old=const_cast<ModuleProfile*>(module_profile(modules,update1->id));old->version==1 && update1->version==2 && old->unavailable.empty()){
        auto legacy=*update1;legacy.displays.front().channel=1;
        for(unsigned n=33;n<=40;++n){legacy.lamps.push_back({n,"out.lighting.supply.15","",{}});legacy.identification_lamps.push_back(n);}
        if(wiring(*old)==wiring(legacy)){*old=*update1;++profiles;}
    }
    auto* old_transponder=const_cast<ModuleProfile*>(module_profile(modules,update2->id));
    if(old_transponder && old_transponder->version==4 && old_transponder->unavailable.empty()){
        auto old=*update2;old.minimum_update=0;old.maximum_update=UINT16_MAX;
        if(wiring(*old_transponder)==wiring(old)){*old_transponder=*update2;++profiles;}
    }
    if(profiles){modules.version=std::max(modules.version,library.version);modules.revision+=" + unified 747 CDU";}
    return {definitions,profiles};
}
namespace {
using Attributes=std::map<std::string,std::string>;
std::string take(Attributes& a,const char* key,const char* fallback=nullptr) {
    const auto it=a.find(key);if(it==a.end()){if(fallback)return fallback;throw std::runtime_error(std::string("Missing FDS profile attribute: ")+key);}
    auto text=std::move(it->second);a.erase(it);return text;
}
unsigned num(Attributes& a,const char* key,unsigned maximum,const char* fallback=nullptr) {
    const auto text=take(a,key,fallback);unsigned value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || value>maximum)throw std::runtime_error(std::string("Invalid FDS integer: ")+key);
    return value;
}
std::vector<unsigned> numbers(Attributes& attributes,const char* key,unsigned maximum) {
    const auto text=take(attributes,key,"");std::vector<unsigned> result;
    if(text.empty())return result;
    std::size_t first{};
    while(first<text.size()){
        const auto end=text.find(';',first);const auto last=end==text.npos?text.size():end;unsigned value{};
        const auto parsed=std::from_chars(text.data()+first,text.data()+last,value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+last || value>maximum || result.size()==16 ||
           std::find(result.begin(),result.end(),value)!=result.end())throw std::runtime_error(std::string("Invalid FDS contact table: ")+key);
        result.push_back(value);if(end==text.npos)break;first=end+1;if(first==text.size())throw std::runtime_error("Trailing contact separator");
    }
    return result;
}
std::string number_list(const std::vector<unsigned>& values){std::string text;for(const auto value:values){if(!text.empty())text+=';';text+=std::to_string(value);}return text;}
bool flag(Attributes& a,const char* key,const char* fallback="false") {
    const auto s=take(a,key,fallback);if(s!="true" && s!="false")throw std::runtime_error("Invalid FDS Boolean");return s=="true";
}
bool bounded(std::string_view s,std::size_t maximum=256) {return s.size()<=maximum && s.find_first_of("\r\n\0",0,3)==s.npos;}
bool identifier(std::string_view s,std::size_t maximum=128){return !s.empty() && bounded(s,maximum) && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-{}")==s.npos;}
bool model_code(std::string_view s){unsigned value{};const auto parsed=std::from_chars(s.data(),s.data()+s.size(),value,16);return s.size()==4 && parsed.ec==std::errc{} && parsed.ptr==s.data()+s.size();}
std::string role_ref(std::string value,std::string_view role) {
    const std::string_view letter=role=="Left"?"401":role=="Centre"?"402":role=="Right"?"403":"";
    const auto at=value.find("{cdu}");if(at!=value.npos)value.replace(at,5,letter);
    const auto brightness=value.find("{cdu-brightness}");
    if(brightness!=value.npos)value.replace(brightness,16,role=="Left"?"404":role=="Centre"?"405":role=="Right"?"406":"");
    const std::string_view brightness_keys="{cdu-brightness-keys}";
    const auto keys=value.find(brightness_keys);
    if(keys!=value.npos)value.replace(keys,brightness_keys.size(),role=="Left"?"428":role=="Centre"?"429":role=="Right"?"430":"");
    const unsigned radio=role=="Left"?0:role=="Centre"?1:role=="Right"?2:3;
    if(radio<3)for(const auto& [token,replacement]:std::array<std::pair<std::string_view,std::string>,6>{{
        {"{rcp}",std::to_string(407+radio)},{"{radio}",std::to_string(109+radio)},{"{radio-index}",std::to_string(radio)},
        {"{rcp-lighting}",radio==0?"13":"14"},{"{side}",radio==0?"l":radio==1?"c":"r"},{"{cdu-lighting}",radio==0?"15":"12"}}}) {
        const auto position=value.find(token);if(position!=value.npos)value.replace(position,token.size(),replacement);
    }
    const auto efis=value.find("{efis");
    if(efis!=value.npos && (role=="Captain" || role=="First Officer")) {
        const auto end=value.find('}',efis);unsigned number{};
        if(end!=value.npos){const auto token=std::string_view(value).substr(efis+5,end-efis-5);const auto parsed=std::from_chars(token.data(),token.data()+token.size(),number);
            if(parsed.ec==std::errc{} && parsed.ptr==token.data()+token.size() && number>=13 && number<=33)value.replace(efis,end-efis+1,std::to_string(number+(role=="First Officer"?21:0)));}
    }
    return value;
}
std::string_view input_role(const ModuleProfile& profile,const ModuleChoice& choice,std::string_view association) {
    if(association.empty())return choice.role;
    const auto found=std::find_if(profile.associations.begin(),profile.associations.end(),[&](const auto& a){return a.id==association;});
    return found==profile.associations.end()?std::string_view{}:std::string_view(found->role);
}
std::string association_device(const ModuleRow& row,std::string_view association) {
    if(association.empty())return row.device;const auto found=row.applied.associations.find(std::string(association));
    return found==row.applied.associations.end()?std::string{}:found->second;
}
std::vector<ButtonAction> actions(const ModuleProfile& profile,const ModuleChoice& choice,const Catalogue& catalogue) {
    std::vector<ButtonAction> result;result.reserve(profile.inputs.size()+profile.analogue.size());
    auto append=[&](const auto& definition){ButtonAction proxy;proxy.target=definition.target;proxy.applicability=definition.applicability;result.push_back(std::move(proxy));};
    for(const auto& input:profile.inputs){
        if(!input.association.empty()){const auto source=choice.associations.find(input.association);if(source==choice.associations.end() || source->second.empty())continue;}
        if(const auto* a=catalogue.action(role_ref(input.action,input_role(profile,choice,input.association)))){append(*a);result.back().encoding=a->encoding;}
    }
    for(const auto& input:profile.analogue){
        if(!input.association.empty()){const auto source=choice.associations.find(input.association);if(source==choice.associations.end() || source->second.empty())continue;}
        const auto found=catalogue.cockpit.find(role_ref(input.definition,input_role(profile,choice,input.association)));
        if(found!=catalogue.cockpit.end())append(*found->second);
    }
    return result;
}
bool compatible(const ModuleProfile& profile,const ModuleDevice& device) {
    return profile.backend==device.input.identity.backend && profile.model==device.model &&
        device.update>=profile.minimum_update && device.update<=profile.maximum_update;
}
std::optional<double> observed(const Catalogue& catalogue,const Mapper& mapper,const std::string& id) {
    const auto definition=catalogue.observations.find(id);if(definition==catalogue.observations.end())return {};
    const auto& d=*definition->second;const auto record=mapper.records.find(record_name(d.record));
    if(record==mapper.records.end())return {};double value{};std::string_view text;
    return observation_value(d,record->second.observed,value,text)?std::optional<double>(value):std::nullopt;
}
} // namespace
std::string module_reference(const ModuleProfile& profile,const ModuleChoice& choice,std::string reference,std::string_view association) {
    return role_ref(std::move(reference),input_role(profile,choice,association));
}
ModuleLibrary read_modules(const std::filesystem::path& path) {
    ModuleLibrary library;IXmlReader* reader{};IStream* stream{};
    try {
        if(std::filesystem::file_size(path)>4*1024*1024)throw std::runtime_error("fds-modules.xml exceeds 4 MiB");
        if(FAILED(SHCreateStreamOnFileEx(path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream)) ||
           FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr)))throw std::runtime_error("Cannot open fds-modules.xml");
        reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);reader->SetProperty(XmlReaderProperty_MaxElementDepth,8);
        if(FAILED(reader->SetInput(stream)))throw std::runtime_error("Cannot read FDS module library");
        XmlNodeType type;HRESULT status;std::vector<std::string> stack;std::set<std::string> ids;bool root{};
        while((status=reader->Read(&type))==S_OK) {
            if(type==XmlNodeType_Element) {
                const wchar_t* name{};reader->GetLocalName(&name,nullptr);const auto tag=utf8(name);const auto parent=stack.empty()?std::string{}:stack.back();
                Attributes attributes;
                if(reader->MoveToFirstAttribute()==S_OK)do {
                    const wchar_t *key{},*value{};reader->GetLocalName(&key,nullptr);reader->GetValue(&value,nullptr);
                    if(!attributes.emplace(utf8(key),utf8(value)).second || wcslen(value)>1024)throw std::runtime_error("Duplicate/oversized FDS attribute");
                }while(reader->MoveToNextAttribute()==S_OK);
                reader->MoveToElement();
                if(tag=="fds-modules" && parent.empty() && !root) {root=true;library.version=num(attributes,"version",5);if(library.version<1)throw std::runtime_error("Unsupported FDS module library version");library.revision=take(attributes,"revision");}
                else if(tag=="profile" && parent=="fds-modules") {
                    if(library.profiles.size()>=128)throw std::runtime_error("Too many FDS profiles");ModuleProfile p;
                    p.id=take(attributes,"id");p.name=take(attributes,"name");p.model=take(attributes,"model");p.reference=take(attributes,"reference");p.version=num(attributes,"version",65535);
                    p.minimum_update=num(attributes,"min-update",65535,"0");p.maximum_update=num(attributes,"max-update",65535,"65535");
                    const auto transport=take(attributes,"transport");if(transport!="usb" && transport!="ethernet" && transport!="phidget22")throw std::runtime_error("Unknown FDS transport");p.backend=transport=="usb"?BackendKind::fds_usb:transport=="ethernet"?BackendKind::fds_ethernet:BackendKind::phidget22;
                    p.unavailable=take(attributes,"unavailable","");p.input_only=flag(attributes,"input-only");p.category=take(attributes,"category",p.id.find("cdu")!=p.id.npos?"CDU":p.id.find("efis")!=p.id.npos?"EFIS":p.id.find("rcp")!=p.id.npos?"RMP":"MCP");p.confidence=take(attributes,"confidence",library.version<3?"partial":"complete");p.limitations=take(attributes,"limitations",library.version<3?"Legacy profile has no support-confidence metadata; review against current defaults":"");if(p.confidence!="complete" && p.confidence!="tentative" && p.confidence!="partial")throw std::runtime_error("Invalid support confidence");
                    if(p.id.empty() || !ids.insert(p.id).second || p.model.size()!=4 || !p.version || p.minimum_update>p.maximum_update)throw std::runtime_error("Invalid/duplicate FDS profile");
                    library.profiles.push_back(std::move(p));
                } else if(parent=="profile" && !library.profiles.empty()) {
                    auto& p=library.profiles.back();
                    if(tag=="role")p.roles.push_back(take(attributes,"id"));
                    else if(tag=="input"){
                        ModuleInput input{num(attributes,"channel",512),take(attributes,"action"),take(attributes,"name"),take(attributes,"association","")};
                        if(library.version>=5){input.contacts=numbers(attributes,"contacts",512);input.states=numbers(attributes,"states",65535);}
                        p.inputs.push_back(std::move(input));
                    }
                    else if(tag=="analogue-input" && library.version>=2)p.analogue.push_back({num(attributes,"channel",512),num(attributes,"min",4095),num(attributes,"max",4095),num(attributes,"bits",16),take(attributes,"definition"),take(attributes,"name"),take(attributes,"association",""),flag(attributes,"inverted"),flag(attributes,"optional")});
                    else if(tag=="encoder-input" && library.version>=3)p.analogue.push_back({num(attributes,"channel",512),0,1,1,take(attributes,"definition"),take(attributes,"name"),take(attributes,"association",""),flag(attributes,"inverted"),flag(attributes,"optional"),true});
                    else if(tag=="identify" && library.version>=3){const auto channel=num(attributes,"lamp",512,"0");p.identification_brightness=flag(attributes,"brightness") || p.identification_brightness;if(channel)p.identification_lamps.push_back(channel);}
                    else if(tag=="lamp")p.lamps.push_back({num(attributes,"channel",512),take(attributes,"output"),take(attributes,"name"),take(attributes,"association","")});
                    else if(tag=="actuator" && library.version>=4 && p.backend==BackendKind::fds_usb)p.actuators.push_back({num(attributes,"channel",32),take(attributes,"output"),take(attributes,"name"),take(attributes,"association","")});
                    else if(tag=="display")p.displays.push_back({num(attributes,"channel",512),num(attributes,"width",12),take(attributes,"definition"),take(attributes,"name"),take(attributes,"association",""),flag(attributes,"loss-pattern")});
                    else if(tag=="unused")p.unused.push_back({num(attributes,"channel",512,"0"),take(attributes,"name"),take(attributes,"reason")});
                    else if(tag=="association")p.associations.push_back({take(attributes,"id"),take(attributes,"model"),take(attributes,"role"),flag(attributes,"required")});
                    else throw std::runtime_error("Unknown FDS profile element: "+tag);
                    if(p.inputs.size()>512 || p.analogue.size()>16 || p.lamps.size()>256 || p.actuators.size()>32 || p.displays.size()>16 || p.associations.size()>8 || p.roles.size()>8 || p.unused.size()>512)throw std::runtime_error("FDS profile bounds exceeded");
                } else throw std::runtime_error("Unexpected FDS library element: "+tag);
                if(!attributes.empty())throw std::runtime_error("Unknown FDS attribute: "+attributes.begin()->first);
                if(!reader->IsEmptyElement())stack.push_back(tag);
            } else if(type==XmlNodeType_EndElement){if(stack.empty())throw std::runtime_error("Unbalanced FDS XML");stack.pop_back();}
            else if(type==XmlNodeType_Text || type==XmlNodeType_CDATA)throw std::runtime_error("Unexpected FDS XML text");
        }
        if(FAILED(status) || !root || !stack.empty())throw std::runtime_error("Invalid FDS module XML");
        // These qualified EFIS selectors have exactly two contacts and a real
        // centre detent. Express all three positions as explicit truth tables,
        // including old installed profiles, without treating callbacks as reports.
        for(auto& p:library.profiles)if(p.backend==BackendKind::fds_usb || p.backend==BackendKind::fds_ethernet){
            std::vector<std::string> sources{""};for(const auto& a:p.associations)sources.push_back(a.id);
            bool bearing_tables{};
            for(const auto& source:sources)for(const std::string stem:{"qh{efis13}.","qh{efis14}.","qh13.","qh14.","qh34.","qh35."}){
                auto vor=std::find_if(p.inputs.begin(),p.inputs.end(),[&](const auto& i){return i.association==source && i.action==stem+"1";});
                auto adf=std::find_if(p.inputs.begin(),p.inputs.end(),[&](const auto& i){return i.association==source && i.action==stem+"-1";});
                if(vor==p.inputs.end() || adf==p.inputs.end())continue;
                const auto channel=vor->channel;const std::vector<unsigned> contacts{channel,adf->channel};if(channel==adf->channel)continue;
                if((!vor->contacts.empty() && (vor->contacts!=contacts || vor->states!=std::vector<unsigned>{1})) || (!adf->contacts.empty() && (adf->contacts!=contacts || adf->states!=std::vector<unsigned>{2})))continue;
                const auto off=std::find_if(p.inputs.begin(),p.inputs.end(),[&](const auto& i){return i.association==source && i.action==stem+"0";});
                if(off!=p.inputs.end() && (off->contacts!=contacts || off->states!=std::vector<unsigned>{0}))continue;
                vor->contacts=adf->contacts=contacts;vor->states={1};adf->states={2};
                bearing_tables=true;if(off==p.inputs.end())p.inputs.push_back({channel,stem+"0","Bearing selector OFF",source,contacts,{0}});
            }
            if(bearing_tables){library.version=std::max(5u,library.version);p.unused.erase(std::remove_if(p.unused.begin(),p.unused.end(),[](const auto& u){return u.name.find("Bearing selectors OFF inference")!=std::string::npos;}),p.unused.end());}
        }
        for(const auto& p:library.profiles) {
            if(!identifier(p.id) || !bounded(p.name) || p.name.empty() || (!model_code(p.model) && !(p.backend==BackendKind::phidget22 && p.model=="TQTQ")) || !bounded(p.reference,1024) || !bounded(p.unavailable,1024))throw std::runtime_error("Invalid FDS profile identity/text");
            if(!identifier(p.category,64) || !bounded(p.limitations,1024) || p.identification_lamps.size()>64)throw std::runtime_error("Invalid FDS confidence/identification metadata");std::set<unsigned> identification;for(const auto channel:p.identification_lamps)if(!channel || !identification.insert(channel).second)throw std::runtime_error("Invalid/duplicate identification lamp");
            if(p.roles.empty() || (p.input_only && (!p.lamps.empty() || !p.actuators.empty() || !p.displays.empty())))throw std::runtime_error("Invalid FDS profile roles/output scope");
            std::set<std::string> roles,associations;
            for(const auto& role:p.roles)if(role.empty() || !bounded(role,64) || !roles.insert(role).second)throw std::runtime_error("Invalid/duplicate FDS role");
            for(const auto& association:p.associations)if(!identifier(association.id,64) || !model_code(association.model) || !bounded(association.role,64) || !associations.insert(association.id).second)throw std::runtime_error("Invalid/duplicate FDS child association");
            auto address=[&](const auto& item){if(!bounded(item.name) || item.name.empty() || (!item.association.empty() && !associations.count(item.association)))throw std::runtime_error("Invalid FDS mapping name/association");};
            std::set<std::pair<std::string,unsigned>> channels;
            std::set<std::string> phidget_actions;
            std::map<std::pair<std::string,unsigned>,std::vector<const ModuleInput*>> representatives;
            for(const auto& i:p.inputs){address(i);if(!identifier(i.action) || (p.backend==BackendKind::phidget22?(i.channel>=8 || !phidget_actions.insert(i.action).second):!i.channel))throw std::runtime_error("Invalid FDS input address");
                if(p.backend!=BackendKind::phidget22){auto& previous=representatives[{i.association,i.channel}];for(const auto* old:previous)if(i.contacts.empty() || i.contacts!=old->contacts || std::any_of(i.states.begin(),i.states.end(),[&](auto state){return std::find(old->states.begin(),old->states.end(),state)!=old->states.end();}))throw std::runtime_error("Duplicate FDS input address");previous.push_back(&i);}
            }
            for(const auto& i:p.inputs)if(!i.contacts.empty() || !i.states.empty()){
                if(library.version<5 || p.backend==BackendKind::phidget22 || i.contacts.empty() || i.contacts.size()>16 || i.states.empty() || i.states.size()>16 ||
                   std::find(i.contacts.begin(),i.contacts.end(),i.channel)==i.contacts.end() ||
                   std::any_of(i.contacts.begin(),i.contacts.end(),[](auto c){return !c || c>512;}) ||
                   std::any_of(i.states.begin(),i.states.end(),[&](auto s){return s>=(1u<<i.contacts.size());}))throw std::runtime_error("Invalid FDS selector table bounds");
            }
            channels.clear();for(const auto& i:p.analogue){address(i);if(!identifier(i.definition) || !i.bits || i.minimum>=i.maximum || i.maximum>=(1u<<i.bits) || !channels.emplace(i.association,i.channel).second)throw std::runtime_error("Invalid/duplicate FDS analogue address or calibration");}
            channels.clear();for(const auto& lamp:p.lamps){address(lamp);if(!identifier(lamp.output) || !lamp.channel || !channels.emplace(lamp.association,lamp.channel).second)throw std::runtime_error("Invalid/duplicate FDS lamp address");}
            channels.clear();for(const auto& a:p.actuators){address(a);if(library.version<4 || p.backend!=BackendKind::fds_usb || !identifier(a.output) || !a.channel || a.channel>32 || !channels.emplace(a.association,a.channel).second)throw std::runtime_error("Invalid/duplicate FDS actuator dataline address");}
            channels.clear();for(const auto& display:p.displays){address(display);if((!display.display.empty() && !identifier(display.display)) || !display.channel || !display.width || display.channel+display.width>513 || (display.display.empty() && display.loss_pattern))throw std::runtime_error("Invalid FDS display window");for(unsigned n=0;n<display.width;++n)if(!channels.emplace(display.association,display.channel+n).second)throw std::runtime_error("Overlapping FDS display windows");}
            for(const auto& unused:p.unused)if(unused.name.empty() || !bounded(unused.name) || unused.reason.empty() || !bounded(unused.reason,1024))throw std::runtime_error("Invalid FDS unused-function description");
        }
    } catch(const std::exception& e){library.profiles.clear();library.problem=e.what();}
    if(reader)reader->Release();if(stream)stream->Release();return library;
}
const ModuleProfile* module_profile(const ModuleLibrary& library,std::string_view id) noexcept {for(const auto& p:library.profiles)if(p.id==id)return &p;return nullptr;}
std::vector<ModuleAutoChoice> module_device_auto_choices(const ModuleLibrary& library,const ModuleConfiguration& configuration,const std::vector<ModuleDevice>& devices){
    std::vector<ModuleAutoChoice> result;std::set<std::pair<std::string,std::string>> roles;
    for(const auto& p:library.profiles)if(p.roles.size()==1 && p.associations.empty() && p.unavailable.empty())roles.emplace(p.category,p.roles.front());
    for(const auto& [category,role]:roles){const auto automatic=configuration.automatic.find(category+"/"+role);if(automatic!=configuration.automatic.end() && !automatic->second)continue;
        bool occupied=false;for(const auto& [id,row]:configuration.rows){(void)id;const auto* p=module_profile(library,row.applied.profile);if(p && p->category==category && row.applied.role==role)occupied=true;const auto* draft=module_profile(library,row.draft.profile);if((row.draft!=row.applied || !row.pending.empty()) && draft && draft->category==category && row.draft.role==role)occupied=true;}if(occupied)continue;
        std::map<std::string,const ModuleProfile*> candidates;for(const auto& d:devices)if(d.input.connected)for(const auto& p:library.profiles)if(p.category==category && p.roles.size()==1 && p.roles.front()==role && p.associations.empty() && p.unavailable.empty() && p.model==d.model && p.backend==d.input.identity.backend && unsigned(d.update)>=p.minimum_update && unsigned(d.update)<=p.maximum_update){candidates.emplace(d.key,&p);break;}
        if(candidates.size()==1)result.push_back({category,role,candidates.begin()->first,candidates.begin()->second->id});
    }return result;
}
std::size_t merge_modules(ModuleLibrary& current,const ModuleLibrary& defaults) {
    if(!current.problem.empty() || !defaults.problem.empty())throw std::runtime_error("Repair invalid module XML before merging definitions");
    std::size_t added{};
    for(const auto& profile:defaults.profiles){
        auto existing=std::find_if(current.profiles.begin(),current.profiles.end(),[&](const auto& p){return p.id==profile.id;});
        if(existing==current.profiles.end()) {if(current.profiles.size()>=128)throw std::runtime_error("Module library capacity exceeded");current.profiles.push_back(profile);++added;}
        else if(!existing->input_only && existing->model==profile.model && existing->backend==profile.backend && existing->version<profile.version){
            // Explicit additive merge: add only absent typed actuator addresses.
            // All operator maps stay intact; the version change requires Apply.
            // A default may explicitly fan one function out to several addresses.
            // Add only when every retained address for that function remains in
            // the proposed set; never reinterpret a custom address as an extra coil.
            const bool correction=std::any_of(existing->actuators.begin(),existing->actuators.end(),[&](const auto& own){return std::any_of(profile.actuators.begin(),profile.actuators.end(),[&](const auto& a){return own.association==a.association && own.output==a.output;}) && std::none_of(profile.actuators.begin(),profile.actuators.end(),[&](const auto& a){return own.association==a.association && own.output==a.output && own.channel==a.channel;});});
            if(correction)continue;
            bool changed=false;for(const auto& a:profile.actuators)if(std::none_of(existing->actuators.begin(),existing->actuators.end(),[&](const auto& own){return own.association==a.association && own.channel==a.channel;})){existing->actuators.push_back(a);++added;changed=true;}
            if(changed)existing->version=profile.version;
        }
    }
    if(added){current.version=std::max(current.version,defaults.version);current.revision+=" + missing defaults";}
    return added;
}
void write_modules(const ModuleLibrary& library,const std::filesystem::path& file) {
    if(!library.problem.empty())throw std::runtime_error("Cannot write an invalid module library");
    const auto temporary=std::filesystem::path(file.wstring()+L".tmp."+std::to_wstring(GetCurrentProcessId()));
    IStream* stream{};IXmlWriter* xml{};
    auto ok=[](HRESULT status){if(FAILED(status))throw std::runtime_error("Cannot write module XML: HRESULT "+std::to_string(static_cast<unsigned>(status)));};
    try {
        ok(SHCreateStreamOnFileEx(temporary.c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_EXCLUSIVE,FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,&stream));
        ok(CreateXmlWriter(__uuidof(IXmlWriter),reinterpret_cast<void**>(&xml),nullptr));ok(xml->SetOutput(stream));ok(xml->SetProperty(XmlWriterProperty_Indent,TRUE));ok(xml->WriteStartDocument(XmlStandalone_Omit));
        auto start=[&](const wchar_t* tag){ok(xml->WriteStartElement(nullptr,tag,nullptr));};
        auto attr=[&](const wchar_t* key,std::string_view value){ok(xml->WriteAttributeString(nullptr,key,nullptr,wide(value).c_str()));};
        auto integer_attr=[&](const wchar_t* key,unsigned value){attr(key,std::to_string(value));};
        auto end=[&]{ok(xml->WriteEndElement());};
        start(L"fds-modules");integer_attr(L"version",library.version);attr(L"revision",library.revision);
        for(const auto& p:library.profiles) {
            if(p.backend!=BackendKind::fds_usb && p.backend!=BackendKind::fds_ethernet && p.backend!=BackendKind::phidget22)throw std::runtime_error("Invalid module transport");
            start(L"profile");attr(L"id",p.id);attr(L"name",p.name);attr(L"model",p.model);attr(L"reference",p.reference);integer_attr(L"version",p.version);
            attr(L"transport",p.backend==BackendKind::fds_usb?"usb":p.backend==BackendKind::fds_ethernet?"ethernet":"phidget22");integer_attr(L"min-update",p.minimum_update);integer_attr(L"max-update",p.maximum_update);
            if(p.input_only)attr(L"input-only","true");if(!p.unavailable.empty())attr(L"unavailable",p.unavailable);
            if(library.version>=3){attr(L"category",p.category);attr(L"confidence",p.confidence);if(!p.limitations.empty())attr(L"limitations",p.limitations);for(const auto channel:p.identification_lamps){start(L"identify");integer_attr(L"lamp",channel);end();}if(p.identification_brightness){start(L"identify");attr(L"brightness","true");end();}}
            for(const auto& role:p.roles){start(L"role");attr(L"id",role);end();}
            for(const auto& a:p.associations){start(L"association");attr(L"id",a.id);attr(L"model",a.model);attr(L"role",a.role);attr(L"required",a.required?"true":"false");end();}
            for(const auto& i:p.inputs){start(L"input");integer_attr(L"channel",i.channel);attr(L"action",i.action);attr(L"name",i.name);if(!i.association.empty())attr(L"association",i.association);if(!i.contacts.empty()){attr(L"contacts",number_list(i.contacts));attr(L"states",number_list(i.states));}end();}
            for(const auto& i:p.analogue){start(i.rotary?L"encoder-input":L"analogue-input");integer_attr(L"channel",i.channel);if(!i.rotary){integer_attr(L"min",i.minimum);integer_attr(L"max",i.maximum);integer_attr(L"bits",i.bits);}attr(L"definition",i.definition);attr(L"name",i.name);if(!i.association.empty())attr(L"association",i.association);if(i.inverted)attr(L"inverted","true");if(i.optional)attr(L"optional","true");end();}
            for(const auto& l:p.lamps){start(L"lamp");integer_attr(L"channel",l.channel);attr(L"output",l.output);attr(L"name",l.name);if(!l.association.empty())attr(L"association",l.association);end();}
            for(const auto& a:p.actuators){if(library.version<4)throw std::runtime_error("Actuators require module schema 4");start(L"actuator");integer_attr(L"channel",a.channel);attr(L"output",a.output);attr(L"name",a.name);if(!a.association.empty())attr(L"association",a.association);end();}
            for(const auto& d:p.displays){start(L"display");integer_attr(L"channel",d.channel);integer_attr(L"width",d.width);attr(L"definition",d.display);attr(L"name",d.name);if(!d.association.empty())attr(L"association",d.association);if(d.loss_pattern)attr(L"loss-pattern","true");end();}
            for(const auto& u:p.unused){start(L"unused");integer_attr(L"channel",u.channel);attr(L"name",u.name);attr(L"reason",u.reason);end();}
            end();
        }
        end();ok(xml->WriteEndDocument());ok(xml->Flush());ok(stream->Commit(STGC_DEFAULT));xml->Release();xml=nullptr;stream->Release();stream=nullptr;
        const auto checked=read_modules(temporary);if(!checked.problem.empty())throw std::runtime_error(checked.problem);
        // Native atomic commit/backup preserves the existing configuration transaction.
        if(!(std::filesystem::exists(file)?ReplaceFileW(file.c_str(),temporary.c_str(),(file.wstring()+L".bak").c_str(),REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr):MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH)))
            throw std::runtime_error("Cannot atomically replace module XML: "+std::to_string(GetLastError()));
    } catch(...) {if(xml)xml->Release();if(stream)stream->Release();DeleteFileW(temporary.c_str());throw;}
}
namespace {
bool cdu_role(std::string_view role){return role=="Left" || role=="Centre" || role=="Right";}
bool shared_cdu_target(const ButtonAction& action,std::string_view role){
    const unsigned side=role=="Left"?0:role=="Centre"?1:role=="Right"?2:3;
    return side<3 && action.target.record.kind==RecordKind::qh &&
        (action.target.record.index==401+side || action.target.record.index==404+side || action.target.record.index==428+side);
}
}
bool modules_valid(const ModuleConfiguration& configuration,std::string& error) {
    if(configuration.rows.size()>64 || configuration.automatic.size()>64){error="Too many module assignments";return false;}
    if(configuration.additional_cdu_roles.size()>3 || std::any_of(configuration.additional_cdu_roles.begin(),configuration.additional_cdu_roles.end(),[](const auto& role){return !cdu_role(role);})){error="Invalid additional CDU role";return false;}
    std::set<std::string> devices;for(const auto& [key,on]:configuration.automatic){(void)on;if(!bounded(key,128) || key.empty()){error="Invalid automatic module role";return false;}}
    for(const auto& [id,row]:configuration.rows) {
        if(id!=row.device || !bounded(id,128) || id.empty() || !bounded(row.model,id.starts_with("cpflight:")?32:4) || row.update>65535 || !bounded(row.pending,1024)){error="Invalid module identity";return false;}
        for(const auto* choice:{&row.applied,&row.draft}) {
            if(!bounded(choice->profile) || !bounded(choice->role,64) || choice->version>65535 || choice->associations.size()>8 ||
                (choice->profile.empty()!=choice->role.empty()) || (!choice->profile.empty() && !choice->version)){error="Invalid module profile/role/version";return false;}
            for(const auto& [association,device]:choice->associations)if(!bounded(association,64) || !bounded(device,128) || association.empty() || device.empty()){error="Invalid module child association";return false;}
            if(choice->additional_cdu && (choice->profile.empty() || !configuration.additional_cdu_roles.count(choice->role))){error="Additional CDU assignment is not enabled for this role";return false;}
        }
        if(!row.applied.profile.empty()) {
            if(!devices.insert(id).second){error="Duplicate applied module device";return false;}
            for(const auto& [association,device]:row.applied.associations){(void)association;if(!devices.insert(device).second){error="Module child is already owned";return false;}}
        }
    }return true;
}
std::string modules_encode(const ModuleConfiguration& configuration) {
    std::ostringstream out;out<<"modules-v3\n"<<configuration.rows.size()<<'\n';
    for(const auto& [id,row]:configuration.rows){out<<std::quoted(id)<<' '<<std::quoted(row.model)<<' '<<row.update<<' '<<std::quoted(row.pending)<<'\n';
        for(const auto* choice:{&row.applied,&row.draft}){out<<std::quoted(choice->profile)<<' '<<choice->version<<' '<<std::quoted(choice->role)<<' '<<choice->associations.size()<<' '<<choice->additional_cdu<<'\n';for(const auto& [key,value]:choice->associations)out<<std::quoted(key)<<' '<<std::quoted(value)<<'\n';}}
    out<<configuration.automatic.size()<<'\n';for(const auto& [role,automatic]:configuration.automatic)out<<std::quoted(role)<<' '<<automatic<<'\n';
    out<<configuration.additional_cdu_roles.size()<<'\n';for(const auto& role:configuration.additional_cdu_roles)out<<std::quoted(role)<<'\n';
    return out.str();
}
bool modules_decode(std::string_view text,ModuleConfiguration& result,std::string& error) {
    std::istringstream in{std::string(text)};std::string header;std::size_t count{};ModuleConfiguration configuration;
    if(text.size()>1024*1024 || !(in>>header>>count) || (header!="modules-v1" && header!="modules-v2" && header!="modules-v3") || count>64){error="Invalid module configuration header";return false;}
    for(std::size_t n=0;n<count;++n){ModuleRow row;in>>std::quoted(row.device)>>std::quoted(row.model)>>row.update>>std::quoted(row.pending);
        for(auto* choice:{&row.applied,&row.draft}){std::size_t children{};in>>std::quoted(choice->profile)>>choice->version>>std::quoted(choice->role)>>children;if(header=="modules-v3"){int additional{};if(!(in>>additional) || (additional!=0 && additional!=1)){error="Invalid additional CDU choice";return false;}choice->additional_cdu=additional!=0;}if(children>8){error="Too many module associations";return false;}for(std::size_t c=0;c<children;++c){std::string key,value;in>>std::quoted(key)>>std::quoted(value);if(!choice->associations.emplace(key,value).second){error="Duplicate module association";return false;}}}
        if(!in || !configuration.rows.emplace(row.device,row).second){error="Incomplete/duplicate module configuration";return false;}}
    if(header!="modules-v1"){std::size_t choices{};if(!(in>>choices) || choices>64){error="Invalid automatic role count";return false;}for(std::size_t n=0;n<choices;++n){std::string role;int enabled{};if(!(in>>std::quoted(role)>>enabled) || (enabled!=0 && enabled!=1) || !bounded(role,128) || !configuration.automatic.emplace(role,enabled!=0).second){error="Invalid automatic role choice";return false;}}}
    if(header=="modules-v3"){std::size_t choices{};if(!(in>>choices) || choices>3){error="Invalid additional CDU role count";return false;}for(std::size_t n=0;n<choices;++n){std::string role;if(!(in>>std::quoted(role)) || !cdu_role(role) || !configuration.additional_cdu_roles.insert(role).second){error="Invalid/duplicate additional CDU role";return false;}}}
    in>>std::ws;if(!in.eof() || !modules_valid(configuration,error))return false;result=std::move(configuration);return true;
}
bool module_conflicts(const ModuleLibrary& library,const ModuleConfiguration& configuration,const ButtonConfiguration& buttons,
                      const CockpitConfiguration& cockpit,const HardwareConfiguration& hardware,const std::vector<Profile>& profiles,std::string& error,const AxisConfiguration* axes) {
    if(!modules_valid(configuration,error))return false;std::vector<ButtonAction> targets;std::vector<std::string> shared_roles;std::set<std::string> owned;std::set<std::pair<std::string,bool>> cdu_slots;
    for(const auto& [id,row]:configuration.rows)if(!row.applied.profile.empty()) {
        owned.insert(id);for(const auto& [association,device]:row.applied.associations){(void)association;owned.insert(device);}
        const auto* profile=module_profile(library,row.applied.profile);if(!profile)continue;
        if(row.applied.additional_cdu && profile->category!="CDU"){error="Additional assignments require a CDU profile";return false;}
        const auto shared=profile->category=="CDU"?row.applied.role:std::string{};
        if(!shared.empty() && !cdu_slots.emplace(shared,row.applied.additional_cdu).second){error="CDU role already has this primary/additional assignment";return false;}
        const auto destinations=actions(*profile,row.applied,active_catalogue());
        for(const auto& a:destinations)for(std::size_t n=0;n<targets.size();++n)if(button_overlap(a,targets[n].target) && !(configuration.additional_cdu_roles.count(shared) && shared_roles[n]==shared && shared_cdu_target(a,shared) && shared_cdu_target(targets[n],shared))){error="PSX destination occupied by another module";return false;}
        targets.insert(targets.end(),destinations.begin(),destinations.end());
        shared_roles.insert(shared_roles.end(),destinations.size(),shared);
    }
    for(const auto& [id,row]:buttons.rows)if(row.managed && !row.applied.id.empty()) {
        if(owned.count(row.applied.identity.hardware_key)){error="Physical input is assigned in Digital inputs; unassign it first";return false;}
        if(const auto* b=active_catalogue().action(id))for(const auto& a:targets)if(button_overlap(a,b->target)){error="PSX destination is assigned in Digital inputs; unassign it first";return false;}
    }
    for(const auto& [id,row]:cockpit.rows)if(row.applied.enabled){if(owned.count(row.applied.device)){error="Device is assigned in Encoders/Analogue inputs ("+id+"); unassign it first";return false;}const auto found=active_catalogue().cockpit.find(id);if(found!=active_catalogue().cockpit.end())for(const auto& a:targets)if(button_overlap(a,found->second->target)){error="Conflicts with "+id+" on "+row.applied.device+" in Encoders/Analogue inputs; unassign that control first";return false;}}
    for(const auto& output:hardware.outputs)if(output.enabled && owned.count(output.device)){error="Device outputs are assigned in Digital outputs; unassign them first";return false;}
    for(const auto& p:profiles)for(const auto& binding:p.bindings)if(binding.enabled){if(owned.count(p.identity.hardware_key)){error="Device is assigned in an input profile";return false;}for(const auto& a:targets)if(button_overlap(a,binding.target)){error="PSX destination is assigned in an input profile";return false;}}
    if(axes) {
        for(std::size_t slot=0;slot<axes->rows.size();++slot) {
            const auto& row=axes->rows[slot];if(!row.managed || !row.active || row.applied.source.bindings.empty())continue;
            const auto& setup=row.applied;
            if(owned.count(setup.source.identity.hardware_key) ||
               (setup.source.bindings.front().throttle_layout==ThrottleLayout::separate_forward_reverse && owned.count(setup.reverse.identity.hardware_key))) {
                error="Physical module device is assigned in flight controls; unassign it first";return false;
            }
            for(const auto& action:targets)if(button_overlap(action,axis_target(slot))){error="PSX destination is assigned in flight controls; unassign it first";return false;}
        }
        if(axes->override_assigned && !axes->override_source.bindings.empty() && owned.count(axes->override_source.identity.hardware_key)) {
            error="Physical module device is assigned to the flight-control override; unassign it first";return false;
        }
    }
    return true;
}
ModuleCompilation compile_module_devices(const ModuleLibrary& library,const ModuleConfiguration& configuration,const std::vector<ModuleDevice>& devices,
                                 const Catalogue& catalogue,const ButtonConfiguration& buttons,const CockpitConfiguration& cockpit,
                                 const HardwareConfiguration& hardware,const std::vector<Profile>& profiles,const AxisConfiguration* axes) {
    ModuleCompilation result;result.buttons=buttons;result.cockpit=cockpit;std::string conflict;
    const bool ownership_valid=module_conflicts(library,configuration,buttons,cockpit,hardware,profiles,conflict,axes);
    std::set<std::string> seen;
    auto compile=[&](const std::string& id,const ModuleDevice* device,const ModuleRow* row){
        ModuleView view;view.device=id;view.connected=device && device->input.connected;view.readouts_known=device && device->readouts_known;view.model=device?device->model:row?row->model:"";view.update=device?static_cast<unsigned>(device->update):row?row->update:0;
        view.backend=device?device->input.identity.backend:id.starts_with("cpflight:")?BackendKind::cpflight_serial:id.starts_with("fds-net:")?BackendKind::fds_ethernet:BackendKind::fds_usb;
        view.name=device?device->input.identity.name:id;view.connection=device?device->status:"Hardware absent";view.pending=row && (row->draft!=row->applied || !row->pending.empty());
        for(const auto& [owner,assigned]:configuration.rows)if(!assigned.applied.profile.empty())for(const auto& [association,child]:assigned.applied.associations)if(child==id) {
            view.parent=owner;view.applied=true;view.profile=assigned.applied.profile;
            if(const auto* parent=module_profile(library,assigned.applied.profile)){view.profile=parent->name;const auto a=std::find_if(parent->associations.begin(),parent->associations.end(),[&](const auto& entry){return entry.id==association;});if(a!=parent->associations.end())view.role=a->role;}
            view.completeness="Reserved as "+view.role+" by module "+owner;
            view.inspection="This board participates in the applied parent module. Inspect, test or change its mapping through "+owner+".\n";
            result.views.push_back(std::move(view));seen.insert(id);return;
        }
        const auto* p=row?module_profile(library,row->applied.profile):nullptr;
        view.profile=p?p->name:row?row->applied.profile:"";view.role=row?row->applied.role:"";view.applied=row && !row->applied.profile.empty();
        std::string reason=library.problem;
        if(reason.empty() && !view.applied)reason="Unassigned";
        if(reason.empty() && !p)reason="Applied profile is missing; review library and Apply";
        if(reason.empty() && p->version!=row->applied.version)reason="Profile version changed; review and Apply";
        if(reason.empty() && !p->unavailable.empty())reason=p->unavailable;
        if(reason.empty() && (!device || !compatible(*p,*device)))reason="Exact model/revision unavailable";
        if(reason.empty() && !device->baseline && p->backend!=BackendKind::fds_ethernet)reason="Awaiting actual startup replay baseline";
        if(reason.empty() && std::find(p->roles.begin(),p->roles.end(),row->applied.role)==p->roles.end())reason="Role is not supported by this profile";
        if(reason.empty() && !ownership_valid)reason=conflict;
        if(reason.empty())for(const auto& [association,key]:row->applied.associations){(void)key;if(std::none_of(p->associations.begin(),p->associations.end(),[&](const auto& a){return a.id==association;})){reason="Applied profile has an unknown child association; review and Apply";break;}}
        if(reason.empty())for(const auto& association:p->associations){const auto key=association_device(*row,association.id);if(key.empty() && !association.required)continue;
            const auto child=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.key==key;});
            if(child==devices.end() || child->model!=association.model || child->input.identity.backend!=p->backend || !child->baseline){reason="Explicit associated "+association.id+" model/baseline unavailable";break;}
        }
        if(!p)for(const auto& candidate:library.profiles)if(candidate.model==view.model && candidate.backend==view.backend && !candidate.unavailable.empty())
            view.inspection+=candidate.name+"\n"+candidate.unavailable+"\n"+candidate.reference+"\n"+candidate.limitations+"\n";
        if(p){for(const auto& unused:p->unused)view.inspection+=unused.name+": unused — "+unused.reason+"\n";}
        ButtonConfiguration additions;CockpitConfiguration analogue;unsigned mapped{},pots{};
        auto optional_absent=[&](std::string_view association){if(association.empty() || !association_device(*row,association).empty())return false;const auto found=std::find_if(p->associations.begin(),p->associations.end(),[&](const auto& a){return a.id==association;});return found!=p->associations.end() && !found->required;};
        auto output_source=[&](std::string_view association){const auto key=association_device(*row,association);return std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.key==key && d.input.connected;});};
        if(reason.empty())for(const auto& input:p->inputs) {
            if(optional_absent(input.association))continue;
            const auto key=association_device(*row,input.association);const auto source=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.key==key;});
            if(source==devices.end() || !source->input.connected){reason="Associated input device unavailable: "+key;break;}
            const auto action=role_ref(input.action,input_role(*p,row->applied,input.association));if(!catalogue.action(action)){reason="Missing catalogue action: "+action;break;}
            const auto control=std::find_if(source->input.controls.begin(),source->input.controls.end(),[&](const auto& c){return c.key.kind==ControlKind::button && c.key.channel_kind==static_cast<unsigned>(ResourceKind::boolean_input) && c.key.channel==input.channel;});
            if(control==source->input.controls.end()){if(p->confidence!="complete" && input.contacts.empty()){view.inspection+=input.name+": not advertised; partial profile\n";continue;}reason="Input outside declared board capabilities: "+input.name;break;}
            auto& assignment=additions.rows[action];const auto* definition=catalogue.action(action);
            if(!input.contacts.empty()){
                if(definition->operation!=ButtonOperation::position || assignment.managed || input.contacts.size()>16 || input.states.empty() || input.states.size()>16 ||
                   std::any_of(input.states.begin(),input.states.end(),[&](auto s){return s>=(1u<<input.contacts.size());})){reason="Contact tables require one bounded maintained action: "+action;break;}
                for(const auto channel:input.contacts){
                    const auto contact=std::find_if(source->input.controls.begin(),source->input.controls.end(),[&](const auto& c){return c.key.kind==ControlKind::button && c.key.channel_kind==unsigned(ResourceKind::boolean_input) && c.key.channel==channel;});
                    if(contact==source->input.controls.end()){reason="Selector contact outside declared board capabilities: "+std::to_string(channel);break;}
                    assignment.module_contacts.push_back(button_source(source->input,*contact));
                }
                if(!reason.empty())break;assignment.module_states=input.states;
            }
            if(assignment.managed){
                if(definition->operation!=ButtonOperation::pulse && definition->operation!=ButtonOperation::step){reason="Multiple sources require a pulse/step action: "+action;break;}
                assignment.module_sources.push_back(button_source(source->input,*control));
            } else {assignment.applied=button_source(source->input,*control);assignment.draft=assignment.applied;assignment.managed=true;}
            view.inspection+=input.name+" → "+catalogue.action(action)->name+" / "+catalogue.action(action)->position;
            if(!input.contacts.empty())view.inspection+=" (explicit contact states "+number_list(input.states)+" over channels "+number_list(input.contacts)+"; incomplete/contradictory banks hold the last PSX mode)";
            else if(definition->operation==ButtonOperation::position && !source->input.coherent_reports)view.inspection+=" (implicit unassigned positions unavailable: no qualified report boundary)";
            view.inspection+='\n';++mapped;
        }
        if(reason.empty())for(const auto& lamp:p->lamps){if(optional_absent(lamp.association))continue;const auto source=output_source(lamp.association);if(source==devices.end() || std::none_of(source->resources.begin(),source->resources.end(),[&](const auto& resource){return resource.output && resource.kind==ResourceKind::boolean_output && resource.channel==lamp.channel;})){reason="Lamp outside declared board capabilities: "+lamp.name;break;}const auto output=module_reference(*p,row->applied,lamp.output,lamp.association);const auto found=catalogue.digital_outputs.find(output);if(found==catalogue.digital_outputs.end() || !found->second->qualified){reason="Missing/unavailable lamp definition: "+output;break;}view.inspection+=lamp.name+" → "+found->second->name+" (output "+std::to_string(lamp.channel)+")\n";}
        if(reason.empty())for(const auto& a:p->actuators){if(optional_absent(a.association))continue;const auto source=output_source(a.association);if(source==devices.end() || std::none_of(source->resources.begin(),source->resources.end(),[&](const auto& resource){return resource.output && resource.kind==ResourceKind::dataline_output && resource.channel==a.channel;})){reason="Actuator dataline outside declared board capabilities: "+a.name;break;}const auto output=module_reference(*p,row->applied,a.output,a.association);const auto found=catalogue.digital_outputs.find(output);if(found==catalogue.digital_outputs.end() || !found->second->qualified || found->second->kind!="actuator"){reason="Missing/unavailable actuator definition: "+output;break;}view.inspection+=a.name+" → "+found->second->name+" (dataline "+std::to_string(a.channel)+")\n";}
        if(reason.empty())for(const auto& input:p->analogue) {
            if(optional_absent(input.association))continue;const auto source=output_source(input.association);
            const auto definition=module_reference(*p,row->applied,input.definition,input.association);const auto found=catalogue.cockpit.find(definition);
            if(found==catalogue.cockpit.end() || found->second->encoder!=input.rotary || !found->second->target.writable){reason="Missing/incompatible analogue definition: "+definition;break;}
            if(source==devices.end()){reason="Associated analogue device unavailable";break;}
            const auto control=std::find_if(source->input.controls.begin(),source->input.controls.end(),[&](const auto& c){return c.key.channel_kind==static_cast<unsigned>(input.rotary?ResourceKind::rotary_delta:ResourceKind::analogue_input) && c.key.channel==input.channel;});
            if(control==source->input.controls.end()) {
                if(input.optional || p->confidence!="complete"){view.inspection+=input.name+": optional input not advertised by this board\n";continue;}
                reason="Analogue input outside declared board capabilities: "+input.name;break;
            }
            if(!input.rotary && (control->key.kind!=ControlKind::axis || control->minimum!=static_cast<int>(input.minimum) || control->maximum!=static_cast<int>(input.maximum) || control->bits!=input.bits)){reason="Analogue input resolution/calibration differs from qualified profile: "+input.name;break;}
            if(std::any_of(analogue.rows.begin(),analogue.rows.end(),[&](const auto& a){return cockpit_destination_overlap(catalogue.cockpit.at(a.first)->target,found->second->target);}) || std::any_of(additions.rows.begin(),additions.rows.end(),[&](const auto& a){return button_overlap(*catalogue.action(a.first),found->second->target);})){reason="Analogue destination overlaps another module input: "+definition;break;}
            auto& binding=analogue.rows[definition].applied;binding.device=source->key;binding.a=input.channel;binding.enabled=true;binding.minimum=static_cast<int>(input.minimum);binding.maximum=static_cast<int>(input.maximum);binding.inverted=input.inverted;analogue.rows[definition].draft=binding;
            view.inspection+=input.name+" → "+found->second->name+" ("+std::to_string(input.minimum)+".."+std::to_string(input.maximum)+", "+std::to_string(input.bits)+" bits)\n";++pots;
        }
        if(reason.empty())for(const auto& display:p->displays){
            if(optional_absent(display.association))continue;const auto source=output_source(display.association);
            auto digit=[&](unsigned channel){return source!=devices.end() && std::any_of(source->resources.begin(),source->resources.end(),[&](const auto& resource){return resource.output && resource.kind==ResourceKind::integral_display && resource.channel==channel;});};
            const unsigned resources=source!=devices.end() && source->addressed_windows?1:display.width;
            bool valid=true;for(unsigned n=0;n<resources && valid;++n)valid=digit(display.channel+n);
            if(!valid){reason="Display outside declared board capabilities: "+display.name;break;}
            if(p->backend==BackendKind::fds_usb && std::any_of(p->lamps.begin(),p->lamps.end(),[&](const auto& lamp){
                const auto column=(lamp.channel-1)/8+1;return lamp.association==display.association && column>=display.channel && column<display.channel+display.width;
            })){reason="Integral display overlaps an assigned physical LED bank: "+display.name;break;}
            const auto definition=module_reference(*p,row->applied,display.display,display.association);
            if(!definition.empty() && (!catalogue.displays.count(definition) || catalogue.displays.at(definition)->width!=display.width)){reason="Missing/incompatible display definition: "+definition;break;}
            view.inspection+=display.name+" → "+(definition.empty()?"blank (no PSX counterpart)":definition)+" (start "+std::to_string(display.channel)+")\n";
        }
        if(reason.empty() && !p->input_only && p->confidence=="complete")for(const auto& source:devices)if(source.key==id || std::any_of(row->applied.associations.begin(),row->applied.associations.end(),[&](const auto& a){return a.second==source.key;}))for(const auto& resource:source.resources)if(resource.output && resource.kind==ResourceKind::integral_display && std::none_of(p->displays.begin(),p->displays.end(),[&](const auto& display){return association_device(*row,display.association)==source.key && resource.channel>=display.channel && resource.channel<display.channel+display.width;})){reason="Complete module profile does not own every integral display digit";break;}
        if(reason.empty()) {
            for(auto& [action,assignment]:additions.rows){auto existing=result.buttons.rows.find(action);if(existing!=result.buttons.rows.end() && p->category=="CDU" && configuration.additional_cdu_roles.count(row->applied.role)){
                    existing->second.module_sources.push_back(std::move(assignment.applied));for(auto& source:assignment.module_sources)existing->second.module_sources.push_back(std::move(source));
                }else result.buttons.rows[action]=std::move(assignment);}
            result.devices.insert(id);
            for(auto& [definition,assignment]:analogue.rows){auto existing=result.cockpit.rows.find(definition);if(existing!=result.cockpit.rows.end() && p->category=="CDU" && configuration.additional_cdu_roles.count(row->applied.role)){
                    if(row->applied.additional_cdu)existing->second.module_sources.push_back(std::move(assignment.applied));
                    else {existing->second.module_sources.push_back(std::move(existing->second.applied));existing->second.applied=std::move(assignment.applied);existing->second.draft=existing->second.applied;}
                }else result.cockpit.rows[definition]=std::move(assignment);}
            for(const auto& [association,key]:row->applied.associations){(void)association;result.devices.insert(key);}
            view.eligible=true;view.completeness=std::to_string(mapped)+" mapped inputs; "+std::to_string(pots)+" analogue inputs; "+std::to_string(p->lamps.size())+" lamps; "+std::to_string(p->displays.size())+" windows; "+std::to_string(p->unused.size())+" unused";
        } else view.completeness=reason;
        result.views.push_back(std::move(view));seen.insert(id);
    };
    for(const auto& device:devices)if(!device.generic){const auto row=configuration.rows.find(device.key);compile(device.key,&device,row==configuration.rows.end()?nullptr:&row->second);}
    for(const auto& [id,row]:configuration.rows)if(!seen.count(id))compile(id,nullptr,&row);
    return result;
}
bool format_display(const CatalogueDisplay& definition,const Catalogue& catalogue,const Mapper& mapper,unsigned width,std::string& text,std::uint64_t test_seconds) {
    text.assign(width,' ');if(!mapper.ready || width<1 || width>12)return false;
    if(definition.style=="transponder-lcd") {
        if(width!=4)return false;const auto found=catalogue.observations.find(definition.observation);if(found==catalogue.observations.end())return false;
        const auto& observation=*found->second;const auto record=mapper.records.find(record_name(observation.record));if(record==mapper.records.end())return false;
        double number{};std::string_view value;if(!observation_value(observation,record->second.observed,number,value) || value.size()!=11)return false;
        if(value.substr(0,4).find_first_not_of("01234567")!=value.npos || value.substr(4,4).find_first_not_of("0123456789_")!=value.npos ||
           (value[8]!='N' && value[8]!='L') || (value[9]!='N' && value[9]!='B' && value[9]!='P' && value[9]!='E') || (value[10]!='N' && value[10]!='F'))return false;
        // Qs118 carries the actual LCD, including power blanking. PSX's renderer
        // checks B before lamp test; the old vendor client does this in reverse.
        if(value[9]=='B')return true;
        if(value[8]=='L'){text="8.8.8.8.";return true;}
        if(value[9]=='P'){text="PASS";return true;}
        text.assign(value.substr(4,4));std::replace(text.begin(),text.end(),'_',' ');return true;
    }
    if(definition.style=="rcp-active" || definition.style=="rcp-standby") {
        if(width!=6)return false;const auto found=catalogue.observations.find(definition.observation);if(found==catalogue.observations.end())return false;
        const auto& observation=*found->second;const auto record=mapper.records.find(record_name(observation.record));if(record==mapper.records.end())return false;
        const auto payload=std::string_view(record->second.observed);
        if(payload.size()!=15 || payload.back()!=';' || payload.substr(0,14).find_first_not_of(" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=payload.npos)return false;
        double number{};std::string_view value;if(!observation_value(observation,payload,number,value) || value.size()!=14)return false;
        const auto window=value.substr(definition.style=="rcp-active"?0:7,7);
        if(window=="XXXXXXX"){text.clear();for(unsigned n=0;n<width;++n)text+="8.";return true;}
        // The native RMP host fits decimal-attached glyphs, left-aligned for
        // active and right-aligned for standby. Spaces are real display cells.
        std::array<std::string_view,7> glyphs{};unsigned count{};
        for(std::size_t n=0;n<window.size();++n) {
            if(window[n]=='.')return false;
            const bool decimal=n+1<window.size() && window[n+1]=='.';
            if(decimal && (window[n]<'0' || window[n]>'9'))return false;
            glyphs[count++]=window.substr(n,decimal?2:1);if(decimal)++n;
        }
        const unsigned first=definition.style=="rcp-standby" && count>width?count-width:0;
        text.clear();if(definition.style=="rcp-standby" && count<width)text.assign(width-count,' ');
        for(unsigned n=first;n<count && n<first+width;++n)text+=glyphs[n];
        if(definition.style=="rcp-active" && count<width)text.append(width-count,' ');
        return true;
    }
    auto power=observed(catalogue,mapper,definition.power);
    if(!power || std::floor(*power)!=*power)return false;
    const auto power_definition=catalogue.observations.find(definition.power);
    // Masked supplies describe powered sides, not the legacy 0/1/2 LCD state.
    // Neither a single right supply nor an analogue MCP option means TEST/OFF.
    const bool supplies=power_definition!=catalogue.observations.end() && power_definition->second->mask;
    if(supplies)*power=*power>0?1:0;
    if(*power==0)return true;if(*power==2){text.assign(width,'8');return true;}if(*power!=1)return false;
    if(supplies && definition.power=="obs.mcp.power"){
        const auto test=observed(catalogue,mapper,"obs.mcp.display-test");
        if(!test || std::floor(*test)!=*test || *test<-1 || *test>1)return false;
        // The analogue option does not publish an LCD flash phase. Use one
        // hardware test pattern for both options, independent of stale Qi36.
        if(*test==1){if((test_seconds/2)%2==0)text.assign(width,'8');return true;}
    }
    if(definition.style=="nav-course") {
        const auto found=catalogue.observations.find(definition.observation);if(found==catalogue.observations.end())return false;
        const auto& o=*found->second;const auto record=mapper.records.find(record_name(o.record));if(record==mapper.records.end())return false;
        const auto payload=std::string_view(record->second.observed);
        if(width!=3 || payload.size()<o.payload_minimum || payload.size()>o.payload_maximum || o.character+3>payload.size() || o.guard_character>=payload.size())return false;
        const auto digits=payload.substr(o.character,3);if(digits.find_first_not_of("0123456789")!=digits.npos)return false;
        const char mode=payload[o.guard_character];if(mode!='M' && mode!='A' && mode!='R' && mode!='P')return false;
        const unsigned number=static_cast<unsigned>((digits[0]-'0')*100+(digits[1]-'0')*10+digits[2]-'0');
        if(number>360 && number!=720)return false;
        // Automatic tuning and deleted CRS are valid blanks, not source loss.
        // This compatibility window never switches to an ILS course.
        if(mode!='M' || number==720)return true;
        text.assign(digits);return true;
    }
    std::optional<double> mode;if(!definition.mode.empty()){mode=observed(catalogue,mapper,definition.mode);if(!mode || std::floor(*mode)!=*mode || *mode<0 || *mode>8207 || (static_cast<unsigned>(*mode)&~8207u))return false;}
    if(definition.style=="mcp-vs" && mode && (static_cast<unsigned>(*mode)&8)==0)return true;
    const auto value=observed(catalogue,mapper,definition.observation);if(!value || std::floor(*value)!=*value)return false;
    if(definition.style=="mcp-speed" && *value>950)return true;
    // Catalogue observations permit all int32 values. Expand hundreds before
    // the physical digit-window check, without wrapping into a false readout.
    std::int64_t number=static_cast<std::int64_t>(*value);if(definition.style=="mcp-vs" || definition.style=="mcp-altitude")number*=100;
    std::string formatted=std::to_string(number);
    if(definition.style=="mcp-speed") {
        if(number>=400)formatted="0."+std::to_string(number);
        else {if(formatted.size()>3)return false;formatted=std::string(3-formatted.size(),'0')+formatted;}
    } else if(definition.style=="integer"){if(formatted.size()>width)return false;formatted=std::string(width-formatted.size(),'0')+formatted;}
    else if(!number)formatted.assign(width,'0');
    // The audited SDK's seven-segment alphabet has minus but no plus glyph.
    // A positive vertical speed therefore retains a leading blank.
    const auto digits=formatted.size()-static_cast<std::size_t>(std::count(formatted.begin(),formatted.end(),'.'));
    if(digits>width)return false;
    text.assign(width-digits,' ');text+=formatted;return true;
}
void ModuleOutputs::release_to(const ModuleSink& sink) {
    for(const auto& route:routes_)if(route.submitted){auto intent=route.previous;intent.value=0;if(route.display){intent.text.fill(0);std::fill_n(intent.text.begin(),route.width,' ');}releases_.push_back({route.slot,intent,0,true});}
    std::size_t accepted{};while(accepted<releases_.size() && sink(releases_[accepted]))++accepted;releases_.erase(releases_.begin(),releases_.begin()+static_cast<std::ptrdiff_t>(accepted));routes_.clear();tests_.clear();
}
void ModuleOutputs::configure_devices(const ModuleLibrary& library,const ModuleConfiguration& configuration,const ModuleCompilation& compilation,
                              const std::vector<ModuleDevice>& devices,const Catalogue& catalogue,const ModuleSink& sink) {
    release_to(sink);catalogue_=&catalogue;digital_.configure(catalogue);
    for(const auto& [id,row]:configuration.rows)if(compilation.devices.count(id)) {
        const auto* p=module_profile(library,row.applied.profile);if(!p || p->input_only)continue;
        auto add=[&](std::string association,std::string ref,unsigned channel,unsigned width,bool display,bool pattern,ResourceKind kind=ResourceKind::boolean_output){
            const auto key=association_device(row,association);const auto device=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.key==key && d.input.connected;});if(device==devices.end())return;
            if(display && std::none_of(device->resources.begin(),device->resources.end(),[&](const auto& resource){return resource.output && resource.kind==ResourceKind::integral_display && resource.channel==channel;}))return;
            Route route;route.device=id;route.id=module_reference(*p,row.applied,std::move(ref),association);route.slot=device->input.slot;route.generation=device->input.generation;
            route.channel=static_cast<std::uint16_t>(channel);route.width=static_cast<std::uint16_t>(width);route.display=display;route.pattern=pattern;route.kind=kind;routes_.push_back(std::move(route));
        };
        std::set<std::pair<std::string,unsigned>> mapped;
        for(const auto& lamp:p->lamps){add(lamp.association,lamp.output,lamp.channel,0,false,false);mapped.emplace(lamp.association,lamp.channel);}
        for(const auto& a:p->actuators)add(a.association,a.output,a.channel,0,false,false,ResourceKind::dataline_output);
        // Only profile-addressed lamps/readouts are serviced; undeclared resources stay untouched.
        for(const auto& display:p->displays)add(display.association,display.display,display.channel,display.width,true,display.loss_pattern);
    }
}
void ModuleOutputs::update_to(const Mapper& mapper,std::uint64_t seconds,const ModuleSink& sink,const DigitalOutputs* evaluated) {
    std::size_t accepted{};while(accepted<releases_.size() && sink(releases_[accepted]))++accepted;releases_.erase(releases_.begin(),releases_.begin()+static_cast<std::ptrdiff_t>(accepted));if(!releases_.empty())return;
    if(!evaluated){digital_.update(mapper);evaluated=&digital_;}
    for(auto& route:routes_) {
        if(route.device==identifying_ && !route.display && route.kind==ResourceKind::boolean_output && std::find(identification_lamps_.begin(),identification_lamps_.end(),route.channel)!=identification_lamps_.end())continue;
        HardwareOutputIntent intent;intent.device_generation=route.generation;intent.session_epoch=mapper.epoch;intent.channel=route.channel;
        intent.resource=static_cast<std::uint16_t>(route.display?ResourceKind::integral_display:route.kind);
        const auto test=tests_.find(route.device);
        if(route.display) {
            std::string text(route.width,' ');bool current=false;
            if(!route.id.empty()){const auto definition=catalogue_->displays.find(route.id);if(definition!=catalogue_->displays.end())current=format_display(*definition->second,*catalogue_,mapper,route.width,text,seconds);}
            if(!current && route.pattern)for(unsigned n=0;n<route.width;++n)text[n]=((n+seconds)&1)?'1':'0';
            if(test!=tests_.end() && !route.id.empty())text.assign(route.width,test->second?'8':' ');
            std::copy(text.begin(),text.end(),intent.text.begin());intent.available=current;
            route.current=current;
        } else {const auto* value=evaluated->find(route.id);intent.value=value && value->available && value->on?1:0;intent.available=value && value->available;route.current=intent.available;if(route.kind==ResourceKind::boolean_output && test!=tests_.end() && !route.id.empty()){intent.value=test->second?1:0;intent.available=true;}}
        if(!route.submitted || intent.device_generation!=route.previous.device_generation || intent.session_epoch!=route.previous.session_epoch || intent.value!=route.previous.value || intent.text!=route.previous.text || intent.available!=route.previous.available) {
            intent.sequence=++sequence_;if(sink({route.slot,intent})){route.previous=intent;route.submitted=true;}
        }
    }
}
std::string ModuleOutputs::source_status(std::string_view device,bool ready) const {
    if(!ready)return "PSX disconnected or awaiting current baselines";
    unsigned missing{},mapped{};for(const auto& route:routes_)if(route.device==device && !route.id.empty()){++mapped;if(!route.current)++missing;}
    return missing?"PSX source data unavailable for "+std::to_string(missing)+" indication(s)":mapped?"PSX connected; current required observations":"PSX connected";
}
bool ModuleOutputs::test(std::string_view device,bool on){if(std::none_of(routes_.begin(),routes_.end(),[&](const auto& r){return r.device==device;}))return false;tests_[std::string(device)]=on;return true;}
} // namespace hwio

namespace hwio {
void ModuleOutputs::identification(std::string device,std::span<const unsigned> lamps){if(device==identifying_ && std::equal(lamps.begin(),lamps.end(),identification_lamps_.begin(),identification_lamps_.end()))return;for(auto& route:routes_)if(route.device==identifying_ || route.device==device)route.submitted=false;identifying_=std::move(device);identification_lamps_.assign(lamps.begin(),lamps.end());}
}

namespace hwio {
std::vector<ModuleDevice> module_devices(const std::vector<fds::Device>& devices){std::vector<ModuleDevice> out;for(const auto& d:devices)out.push_back({d.input,d.key,d.model,d.status,{},unsigned(d.caps.update),d.resources,d.baseline,d.generic,d.readouts_known});return out;}
std::vector<ModuleAutoChoice> module_auto_choices(const ModuleLibrary& l,const ModuleConfiguration& c,const std::vector<fds::Device>& d){return module_device_auto_choices(l,c,module_devices(d));}
ModuleCompilation compile_modules(const ModuleLibrary& l,const ModuleConfiguration& c,const std::vector<fds::Device>& d,const Catalogue& a,const ButtonConfiguration& b,const CockpitConfiguration& k,const HardwareConfiguration& h,const std::vector<Profile>& p,const AxisConfiguration* x){return compile_module_devices(l,c,module_devices(d),a,b,k,h,p,x);}
void ModuleOutputs::release(fds::Provider& p){release_to([&](const ModuleOutput& o){return p.submit(o);});}
void ModuleOutputs::configure(const ModuleLibrary& l,const ModuleConfiguration& c,const ModuleCompilation& m,const std::vector<fds::Device>& d,const Catalogue& a,fds::Provider& p){configure_devices(l,c,m,module_devices(d),a,[&](const ModuleOutput& o){return p.submit(o);});}
void ModuleOutputs::update(const Mapper& m,std::uint64_t t,fds::Provider& p,const DigitalOutputs* d){update_to(m,t,[&](const ModuleOutput& o){return p.submit(o);},d);}
}
