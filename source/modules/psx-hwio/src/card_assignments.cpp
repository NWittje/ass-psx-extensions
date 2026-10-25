#include "hwio/card_assignments.hpp"
#include <Windows.h>
#include <Shlwapi.h>
#include <xmllite.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace hwio {
namespace {
constexpr std::size_t bound=32*1024*1024;
template<class T>struct Com {T* p{};~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->()const{return p;}};
void ok(HRESULT h,const char* what){if(FAILED(h))throw std::runtime_error(std::string(what)+" ("+std::to_string(static_cast<unsigned>(h))+")");}
void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
struct File {HANDLE h{INVALID_HANDLE_VALUE};~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
std::string read_xml_bytes(const std::filesystem::path& path){
    File file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);require(file.h!=INVALID_HANDLE_VALUE,"Cannot open card XML; Windows error "+std::to_string(GetLastError()));
    LARGE_INTEGER size{};require(GetFileSizeEx(file.h,&size)&&size.QuadPart>0&&size.QuadPart<=static_cast<LONGLONG>(bound),"Card XML is empty or exceeds 32 MiB");std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD got{};require(ReadFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size(),"Cannot read complete card XML");return bytes;
}
void write_payload(IXmlWriter* writer,const std::wstring& text){
    require(text.find(L'\0')==text.npos,"Assignment text contains an unsupported NUL");std::size_t first{};
    // Character references preserve literal CR in LCD formats and names across
    // XML 1.0 newline normalization; ordinary line feeds remain readable.
    while(first<text.size()){const auto end=text.find(L'\r',first);const auto count=(end==text.npos?text.size():end)-first;if(count)ok(writer->WriteChars(text.data()+first,static_cast<UINT>(count)),"Write assignment text");if(end==text.npos)break;ok(writer->WriteCharEntity(L'\r'),"Preserve carriage return");first=end+1;}
}
bool io_binding(const Binding& b){
    // Flight axes have their own calibrated assignment pages and are outside a
    // card-I/O transfer, including legacy bindings targeting those same fields.
    for(std::size_t n=0;n<flight_slots;++n)if(cockpit_destination_overlap(b.target,axis_target(n)))return false;
    return true;
}
bool io_output(const HardwareBinding& b){
    switch(b.resource){case ResourceKind::digital:case ResourceKind::boolean_output:case ResourceKind::dataline_output:case ResourceKind::led:case ResourceKind::poext:case ResourceKind::ponet:case ResourceKind::poil:case ResourceKind::pwm:return true;default:return false;}
}
NetworkDevice identity_device(const NetworkDevice& d){NetworkDevice id;id.key=d.key;id.name=d.name;id.serial=d.serial;id.model=d.model;id.backend=d.backend;id.provider_model=d.provider_model;id.provider_revision=d.provider_revision;return id;}
template<class F>void profiles(CardAssignments& c,F f){for(auto& p:c.profiles)f(p);for(auto& r:c.axes.rows)for(auto* s:{&r.applied,&r.draft})for(auto* p:{&s->source,&s->reverse,&s->inactive_device})f(*p);f(c.axes.override_source);for(auto& [id,r]:c.buttons.rows){(void)id;f(r.applied);f(r.draft);}}
std::string encode(const CardAssignments& c){
    std::ostringstream out;out<<"[hwio-card-assignments-v1]\n"<<c.profiles.size()<<'\n';
    for(const auto& p:c.profiles)out<<std::quoted(profile_encode(p))<<'\n';
    for(const auto& r:c.display)out<<unsigned(r.choice)<<' '<<r.binding<<' '<<std::quoted(r.profile)<<'\n';
    out<<std::quoted(axis_configuration_encode(c.axes))<<'\n'<<std::quoted(button_configuration_encode(c.buttons))<<'\n'<<std::quoted(hardware_encode(c.hardware))<<'\n'<<std::quoted(cockpit_encode(c.cockpit))<<'\n';return out.str();
}
std::string encode_io(const CardAssignments& c){
    std::ostringstream out;out<<"[hwio-card-io-v2]\n"<<c.profiles.size()<<'\n';
    for(const auto& p:c.profiles)out<<std::quoted(profile_encode(p))<<'\n';
    out<<std::quoted(button_configuration_encode(c.buttons))<<'\n'<<std::quoted(hardware_encode(c.hardware))<<'\n'<<std::quoted(cockpit_encode(c.cockpit))<<'\n';return out.str();
}
std::unique_ptr<CardAssignments> decode(const std::string& text){
    require(text.size()<=bound,"Card assignments exceed 32 MiB");auto c=std::make_unique<CardAssignments>();std::istringstream in(text);std::string header,value,error;std::size_t count{};
    std::getline(in,header);require(header=="[hwio-card-io-v2]" && bool(in>>count) && count<=256,"Invalid card I/O header/profile count");
    std::set<std::string> ids;for(std::size_t n=0;n<count;++n){Profile p;require(bool(in>>std::quoted(value)) && profile_decode(value,p,error),"Invalid card profile: "+error);require(ids.insert(p.id).second,"Duplicate card profile ID");c->profiles.push_back(std::move(p));}
    require(bool(in>>std::quoted(value)) && button_configuration_decode(value,c->buttons,error),"Invalid card inputs: "+error);
    require(bool(in>>std::quoted(value)) && hardware_decode(value,c->hardware,error),"Invalid card I/O addresses: "+error);
    require(bool(in>>std::quoted(value)) && cockpit_decode(value,c->cockpit,error),"Invalid card encoders/analogue inputs: "+error);
    in>>std::ws;require(in.eof(),"Unexpected card assignment data");require(card_assignments_valid(*c,error),error);return c;
}
std::string reader_value(IXmlReader* r){const wchar_t* p{};UINT n{};ok(r->GetValue(&p,&n),"Read XML value");return utf8(std::wstring_view(p,n));}
std::string reader_name(IXmlReader* r){const wchar_t* p{};UINT n{};ok(r->GetLocalName(&p,&n),"Read XML name");require(n<=64,"XML name exceeds bound");return utf8(std::wstring_view(p,n));}
void check_identity(const CardArchive& archive){
    const auto& id=archive.identity;const auto& devices=archive.assignments->hardware.devices;
    require(devices.size()==1 && devices[0].key==id.key && devices[0].backend==id.provider,"Card XML requires exactly its declared controller");const auto& d=devices[0];
    if(id.provider==BackendKind::pokeys_ethernet)require(id.model=="PoKeys:"+std::to_string(d.model) && id.serial==std::to_string(d.serial),"PoKeys model/serial disagrees with its assignments");
    else if(id.provider==BackendKind::phidget22){require(id.serial==std::to_string(d.serial),"Phidget serial disagrees with its assignments");if(!d.provider_model.empty())require(id.model=="Phidget:"+d.provider_model,"Phidget model disagrees with its assignments");}
    else require(id.model=="FDS:"+d.provider_model && fds_card_serial(id.key,id.provider)==id.serial,"FDS model/serial disagrees with its assignments");
}
struct Destination {TargetDescriptor target;std::string owner;std::optional<std::size_t> character{};};
bool intersects(const Destination& a,const Destination& b){if(a.character||b.character)return a.target.record==b.target.record&&(!a.character||!b.character||a.character==b.character);return cockpit_destination_overlap(a.target,b.target);}
std::vector<Destination> destinations(const CardAssignments& c){
    std::vector<Destination> result;
    for(const auto& p:c.profiles)for(const auto& b:p.bindings)if(b.enabled && !axis_replaces(c.axes,p.id,b.id) && !button_replaces(c.buttons,p.id,b.id))result.push_back({b.target,"profile "+p.id+"/"+std::to_string(b.id)});
    for(std::size_t n=0;n<flight_slots;++n)if(c.axes.rows[n].managed && c.axes.rows[n].active && !c.axes.rows[n].applied.source.id.empty()){const auto partner=axis_partner(n);const auto owner=partner>=0?std::min(n,std::size_t(partner)):n;result.push_back({axis_target(n),"flight axis "+std::to_string(owner)});}
    for(const auto& [id,row]:c.buttons.rows)if(row.managed && !row.applied.id.empty()){const auto* a=button_action(id);if(!a)continue;result.push_back({a->target,"switch "+button_owner(*a),a->encoding==ButtonEncoding::character?std::optional<std::size_t>(a->character):std::nullopt});}
    for(const auto& [id,row]:c.cockpit.rows)if(row.applied.enabled){const auto it=active_catalogue().cockpit.find(id);if(it==active_catalogue().cockpit.end())continue;result.push_back({it->second->target,"cockpit "+id});}
    return result;
}
bool related_source(const CardAssignments& current,const CardIdentity& source,const CardIdentity& target){
    if(source.key==target.key)return false;
    for(const auto& d:current.hardware.devices)if(d.key==source.key){if(d.backend!=source.provider)return false;
        if(d.backend==BackendKind::pokeys_ethernet)return source.model=="PoKeys:"+std::to_string(d.model)&&source.serial==std::to_string(d.serial);
        if(d.backend==BackendKind::phidget22)return source.serial==std::to_string(d.serial)&&(d.provider_model.empty()||d.provider_model=="0"||source.model=="Phidget:"+d.provider_model);
        return source.model=="FDS:"+d.provider_model;
    }
    return true; // An absent old card can still own stored input assignments.
}
void migrate_inputs(CardAssignments& current,const CardAssignments& incoming,const CardIdentity& source,const CardAssignments& archive){
    const auto targets=destinations(incoming);const auto& key=source.key;std::set<std::string> groups;
    for(const auto& [id,row]:archive.buttons.rows){(void)row;if(const auto* a=button_action(id))groups.insert(a->group);}
    auto overlap=[&](const Destination& d){return std::any_of(targets.begin(),targets.end(),[&](const auto& t){return intersects(d,t);});};
    for(auto& p:current.profiles)if(card_profile(p,key))for(auto& b:p.bindings)if(io_binding(b)&&b.enabled&&overlap({b.target,"legacy",{}}))b.enabled=false;
    for(auto& [id,row]:current.buttons.rows){const auto* a=button_action(id);bool covered=archive.buttons.rows.contains(id);if(a)covered=covered||groups.contains(a->group)||overlap({a->target,"switch",a->encoding==ButtonEncoding::character?std::optional<std::size_t>(a->character):std::nullopt});if(covered){if(card_profile(row.applied,key))row.applied={};if(card_profile(row.draft,key))row.draft={};}}
    for(auto& [id,row]:current.cockpit.rows){bool covered=archive.cockpit.rows.contains(id);if(const auto d=active_catalogue().cockpit.find(id);d!=active_catalogue().cockpit.end())covered=covered||overlap({d->second->target,"cockpit",{}});if(covered){if(row.applied.device==key)row.applied={};if(row.draft.device==key)row.draft={};}}
}

}
bool card_profile(const Profile& p,std::string_view key)noexcept{return !p.id.empty() && p.identity.hardware_key==key;}
std::string fds_card_serial(std::string_view key,BackendKind provider){const auto first=key.find(':');if(first==key.npos)return {};const auto last=provider==BackendKind::fds_ethernet?key.rfind('/'):key.size();return last!=key.npos&&last>first?std::string(key.substr(first+1,last-first-1)):std::string{};}
std::string card_assignments_signature(const CardAssignments& c){return encode(c)+modules_encode(c.modules);}
CardMatch card_match(const CardIdentity& source,const CardIdentity& target)noexcept{
    if(source.provider!=target.provider || source.model.empty() || source.model!=target.model || source.serial.empty() || target.serial.empty())return CardMatch::incompatible;
    return source.serial==target.serial?CardMatch::exact:CardMatch::model_only;
}
std::unique_ptr<CardAssignments> card_slice(const CardAssignments& all,const CardIdentity& identity){
    auto c=std::make_unique<CardAssignments>();const auto& key=identity.key;
    for(const auto& p:all.profiles)if(card_profile(p,key)){auto io=p;std::erase_if(io.bindings,[](const auto& b){return !io_binding(b);});if(!io.bindings.empty())c->profiles.push_back(std::move(io));}
    for(const auto& [id,r]:all.buttons.rows)if(card_profile(r.applied,key)||card_profile(r.draft,key)){auto row=r;if(!card_profile(row.applied,key))row.applied={};if(!card_profile(row.draft,key))row.draft={};row.module_sources.clear();row.module_contacts.clear();row.module_states.clear();c->buttons.rows.emplace(id,std::move(row));const auto* action=button_action(id);if(action){if(all.buttons.toggles.contains(action->group))c->buttons.toggles.insert(action->group);if(const auto it=all.buttons.toggle_drafts.find(action->group);it!=all.buttons.toggle_drafts.end())c->buttons.toggle_drafts.insert(*it);}}
    for(const auto& d:all.hardware.devices)if(d.key==key)c->hardware.devices.push_back(identity_device(d));
    for(const auto& b:all.hardware.outputs)if(b.device==key&&io_output(b))c->hardware.outputs.push_back(b);
    for(const auto& [id,r]:all.cockpit.rows)if(r.applied.device==key || r.draft.device==key){auto row=r;if(row.applied.device!=key)row.applied={};if(row.draft.device!=key)row.draft={};c->cockpit.rows.emplace(id,std::move(row));}
    return c;
}
bool card_assignments_valid(const CardAssignments& c,std::string& error){
    if(c.profiles.size()>256 || !hardware_valid(c.hardware,error) || !button_configuration_valid(c.buttons,error) || !cockpit_valid(c.cockpit,error))return false;
    std::size_t enabled{};std::set<std::string> profile_ids;for(const auto& p:c.profiles){enabled+=std::count_if(p.bindings.begin(),p.bindings.end(),[](const auto& b){return b.enabled;});if(enabled>max_bindings){error="Legacy mapping count exceeds 512";return false;}if(!validate(p,error))return false;if(!profile_ids.insert(p.id).second){error="Duplicate profile ID";return false;}}
    const auto owners=destinations(c);
    for(std::size_t a=0;a<owners.size();++a)for(std::size_t b=0;b<a;++b)if(owners[a].owner!=owners[b].owner&&intersects(owners[a],owners[b])){error="Assignment conflict: "+owners[a].owner+" and "+owners[b].owner;return false;}
    for(const auto& [id,row]:c.cockpit.rows)if(row.applied.enabled){const auto definition=active_catalogue().cockpit.find(id);if(definition==active_catalogue().cockpit.end())continue;const auto& binding=row.applied;
        auto source_conflict=[&](const Profile& p,const Source& source){const Profile* owner=source.profile.empty()||source.profile==p.id?&p:nullptr;if(!owner)for(const auto& candidate:c.profiles)if(candidate.id==source.profile)owner=&candidate;return owner&&owner->identity.hardware_key==binding.device&&cockpit_source_overlap(source,definition->second->encoder,binding);};
        auto profile_conflict=[&](const Profile& p,bool enabled_only=true){for(const auto& b:p.bindings)if(!enabled_only||b.enabled)for(const auto& source:b.sources)if(source_conflict(p,source))return true;return false;};
        for(const auto& p:c.profiles)if(profile_conflict(p)){error="Encoder/analogue source is owned by another input profile: "+id;return false;}
        for(const auto& axis:c.axes.rows)if(axis.managed&&axis.active){if(profile_conflict(axis.applied.source,false)||(!axis.applied.source.bindings.empty()&&axis.applied.source.bindings.front().throttle_layout==ThrottleLayout::separate_forward_reverse&&source_conflict(axis.applied.reverse,axis.applied.source.bindings.front().reverse_source))){error="Encoder/analogue source is owned by a flight axis: "+id;return false;}}
        if(c.axes.override_assigned&&profile_conflict(c.axes.override_source,false)){error="Encoder/analogue source is owned by the seat override: "+id;return false;}
        for(const auto& [action,button]:c.buttons.rows){(void)action;if(button.managed&&profile_conflict(button.applied,false)){error="Encoder/analogue source is owned by Digital inputs: "+id;return false;}}
        for(const auto& [other_id,other]:c.cockpit.rows)if(other_id!=id&&other.applied.enabled&&other.applied.device==binding.device){const auto d=active_catalogue().cockpit.find(other_id);if(d==active_catalogue().cockpit.end())continue;const auto kind=definition->second->encoder?(binding.software?1:2):0;const auto other_kind=d->second->encoder?(other.applied.software?1:2):0;if(kind==other_kind&&(binding.a==other.applied.a||(binding.software&&(binding.b==other.applied.a||binding.a==other.applied.b||binding.b==other.applied.b)))){error="Encoder/analogue resource has two assignments: "+id+" and "+other_id;return false;}}
    }
    error.clear();return true;
}
std::unique_ptr<CardAssignments> card_merge(const CardAssignments& all,const CardArchive& archive,const CardIdentity& target,std::string& error){
    try{
        require(card_match(archive.identity,target)!=CardMatch::incompatible,"Card type/provider does not match this XML");check_identity(archive);
        auto incoming=card_slice(*archive.assignments,archive.identity);require(encode(*incoming)==encode(*archive.assignments),"XML contains assignments outside its declared card");
        const auto source_key=archive.identity.key;const auto& key=target.key;
        const auto actual_card=std::find_if(all.hardware.devices.begin(),all.hardware.devices.end(),[&](const auto& d){return d.key==key;});require(actual_card!=all.hardware.devices.end(),"Selected card identity is no longer present");
        std::map<std::string,std::string> renamed;std::set<std::string> retained;
        for(const auto& p:all.profiles)if(!card_profile(p,key))retained.insert(p.id);
        profiles(*incoming,[&](Profile& p){if(p.id.empty())return;require(card_profile(p,source_key),"XML contains an unrelated input identity");auto id=p.id;if(source_key!=key){std::uint64_t hash=14695981039346656037ull;for(const unsigned char ch:key+"/"+p.id){hash^=ch;hash*=1099511628211ull;}id="card_"+std::to_string(hash);}require(!retained.contains(id),"Imported profile ID belongs to another card");renamed[p.id]=id;p.id=id;p.identity.hardware_key=key;p.identity.serial=target.serial;p.identity.name=target.name;p.identity.instance_path.clear();p.identity.container_id.clear();p.explicit_path=false;});
        auto ref=[&](std::string& id){if(const auto it=renamed.find(id);it!=renamed.end())id=it->second;};
        profiles(*incoming,[&](Profile& p){for(auto& b:p.bindings){for(auto& s:b.sources)ref(s.profile);ref(b.reverse_source.profile);ref(b.inactive_source.profile);}});
        for(auto& [id,r]:incoming->buttons.rows){(void)id;for(auto& replacement:r.replaces)ref(replacement.first);}
        for(auto& b:incoming->hardware.outputs)b.device=key;
        for(auto& [id,r]:incoming->cockpit.rows){(void)id;if(r.applied.device==source_key)r.applied.device=key;if(r.draft.device==source_key)r.draft.device=key;}
        auto next=std::make_unique<CardAssignments>(all);const bool migrating=related_source(all,archive.identity,target);if(migrating)migrate_inputs(*next,*incoming,archive.identity,*archive.assignments);
        for(const auto& [id,row]:incoming->buttons.rows){(void)id;for(const auto& replacement:row.replaces){const auto p=std::find_if(all.profiles.begin(),all.profiles.end(),[&](const auto& profile){return profile.id==replacement.first;});if(p==all.profiles.end())continue;require(card_profile(*p,key)||(migrating&&card_profile(*p,source_key)),"Import would replace a legacy assignment belonging to another card");for(const auto& b:p->bindings)if(b.id==replacement.second)require(io_binding(b),"Card I/O cannot replace a flight-axis binding");}}
        for(auto& p:next->profiles)if(card_profile(p,key))std::erase_if(p.bindings,io_binding);
        for(auto& p:incoming->profiles){const auto old=std::find_if(next->profiles.begin(),next->profiles.end(),[&](const auto& candidate){return candidate.id==p.id;});if(old==next->profiles.end())next->profiles.push_back(std::move(p));else{for(auto& b:p.bindings){require(std::none_of(old->bindings.begin(),old->bindings.end(),[&](const auto& existing){return existing.id==b.id;}),"Imported binding ID conflicts with a retained flight-axis binding");old->bindings.push_back(std::move(b));}}}
        for(auto& [id,r]:next->buttons.rows){(void)id;if(card_profile(r.applied,key))r.applied={};if(card_profile(r.draft,key))r.draft={};}
        for(const auto& [id,s]:incoming->buttons.rows){auto& r=next->buttons.rows[id];const bool applied=!s.applied.id.empty();if(applied){require(r.applied.id.empty(),"Input destination belongs to another card: "+id);r.applied=s.applied;}if(!s.draft.id.empty()){require(r.draft.id.empty(),"Pending input destination belongs to another card: "+id);r.draft=s.draft;r.pending=s.pending;}else if(r.draft.id.empty())r.pending.clear();if(applied||r.applied.id.empty()){r.managed=s.managed;r.replaces=s.replaces;}}
        std::erase_if(next->buttons.rows,[](const auto& item){return item.second.applied.id.empty()&&item.second.draft.id.empty();});
        for(const auto& [id,r]:archive.assignments->buttons.rows){(void)r;const auto* a=button_action(id);if(!a)continue;const auto& group=a->group;for(const auto& [other_id,other]:all.buttons.rows){const auto* other_action=button_action(other_id);if(other_action && other_action->group==group && !other.applied.id.empty() && !card_profile(other.applied,key)&&!(migrating&&card_profile(other.applied,source_key)))require(all.buttons.toggles.contains(group)==incoming->buttons.toggles.contains(group),"Toggle mode is shared with another card");}if(incoming->buttons.toggles.contains(group))next->buttons.toggles.insert(group);else next->buttons.toggles.erase(group);if(const auto i=incoming->buttons.toggle_drafts.find(group);i!=incoming->buttons.toggle_drafts.end())next->buttons.toggle_drafts[group]=i->second;else next->buttons.toggle_drafts.erase(group);}
        std::erase_if(next->hardware.outputs,[&](const auto& b){return b.device==key&&io_output(b);});std::uint64_t free_id=1;std::set<std::uint64_t> output_ids;for(const auto& b:next->hardware.outputs)output_ids.insert(b.id);for(auto b:incoming->hardware.outputs){if(output_ids.contains(b.id)){while(output_ids.contains(free_id))++free_id;b.id=free_id;}output_ids.insert(b.id);next->hardware.outputs.push_back(std::move(b));}
        for(auto& [id,r]:next->cockpit.rows){(void)id;if(r.applied.device==key)r.applied={};if(r.draft.device==key)r.draft={};}for(const auto& [id,s]:incoming->cockpit.rows){auto& r=next->cockpit.rows[id];if(!s.applied.device.empty()){require(r.applied.device.empty(),"Encoder/analogue destination belongs to another card: "+id);r.applied=s.applied;}if(!s.draft.device.empty()){require(r.draft.device.empty(),"Pending encoder/analogue destination belongs to another card: "+id);r.draft=s.draft;}r.pending=s.pending;}
        std::erase_if(next->cockpit.rows,[](const auto& item){return item.second.applied.device.empty()&&item.second.draft.device.empty();});
        require(card_assignments_valid(*next,error),error);error.clear();return next;
    }catch(const std::exception& e){error=e.what();return {};}
}
bool write_card_archive(const std::filesystem::path& path,const CardArchive& archive,std::string& error){
    try{
        const auto& id=archive.identity;require(!id.model.empty()&&id.model.size()<=128&&!id.serial.empty()&&id.serial.size()<=256&&!id.key.empty()&&id.key.size()<=128&&id.name.size()<=256,"Card model, serial or name is missing/too long");check_identity(archive);require(card_assignments_valid(*archive.assignments,error),error);require(encode(*archive.assignments)==encode(*card_slice(*archive.assignments,id)),"Export contains data outside the selected card I/O");const auto payload=encode_io(*archive.assignments);require(payload.size()<=bound,"Card export exceeds 32 MiB");const auto unicode=wide(payload);require(!unicode.empty(),"Assignment text is not valid UTF-8");
        Com<IStream> stream;ok(CreateStreamOnHGlobal(nullptr,TRUE,stream.out()),"Create XML stream");Com<IXmlWriter> xml;ok(CreateXmlWriter(__uuidof(IXmlWriter),reinterpret_cast<void**>(xml.out()),nullptr),"Create XML writer");ok(xml->SetOutput(stream.p),"Set XML output");ok(xml->SetProperty(XmlWriterProperty_Indent,TRUE),"Set XML indentation");ok(xml->WriteStartDocument(XmlStandalone_Omit),"Write XML declaration");ok(xml->WriteStartElement(nullptr,L"hwio-card",nullptr),"Write card");
        auto attr=[&](const wchar_t* name,const std::string& value){ok(xml->WriteAttributeString(nullptr,name,nullptr,wide(value).c_str()),"Write card identity");};attr(L"version","2");attr(L"provider",std::to_string(unsigned(id.provider)));attr(L"model",id.model);attr(L"serial",id.serial);attr(L"key",id.key);attr(L"name",id.name);
        ok(xml->WriteStartElement(nullptr,L"assignments",nullptr),"Write assignments");write_payload(xml.p,unicode);ok(xml->WriteEndElement(),"Close assignments");ok(xml->WriteEndElement(),"Close card");ok(xml->WriteEndDocument(),"Finish XML");ok(xml->Flush(),"Flush XML");
        STATSTG stat{};ok(stream->Stat(&stat,STATFLAG_NONAME),"Read XML size");require(stat.cbSize.QuadPart<=bound,"Card XML exceeds 32 MiB");LARGE_INTEGER zero{};ok(stream->Seek(zero,STREAM_SEEK_SET,nullptr),"Rewind XML");std::string bytes(static_cast<std::size_t>(stat.cbSize.QuadPart),'\0');ULONG got{};ok(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&got),"Read XML bytes");require(got==bytes.size(),"Incomplete XML output");
        const auto temporary=path.wstring()+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
        {File file;file.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(file.h!=INVALID_HANDLE_VALUE,"Cannot create card XML staging file");DWORD written{};const bool saved=WriteFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file.h);if(!saved){CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;DeleteFileW(temporary.c_str());throw std::runtime_error("Cannot write complete card XML");}}
        const bool committed=std::filesystem::exists(path)?bool(ReplaceFileW(path.c_str(),temporary.c_str(),(path.wstring()+L".bak").c_str(),0,nullptr,nullptr)):bool(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH));const auto code=GetLastError();if(!committed){DeleteFileW(temporary.c_str());throw std::runtime_error("Cannot commit card XML; Windows error "+std::to_string(code));}error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool read_card_archive(const std::filesystem::path& path,CardArchive& archive,std::string& error){
    try{
        const auto bytes=read_xml_bytes(path);Com<IStream> stream;stream.p=SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),static_cast<UINT>(bytes.size()));require(stream.p!=nullptr,"Cannot create XML input");Com<IXmlReader> xml;ok(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(xml.out()),nullptr),"Create XML reader");ok(xml->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit),"Prohibit DTD");ok(xml->SetProperty(XmlReaderProperty_MaxElementDepth,3),"Bound XML depth");ok(xml->SetInput(stream.p),"Set XML input");
        CardArchive parsed;std::string payload;bool root{},child{},closed{},child_closed{};XmlNodeType kind{};HRESULT result{};std::map<std::string,std::string> attrs;unsigned nodes{};
        while((result=xml->Read(&kind))==S_OK){require(++nodes<=128,"Too many card XML nodes");UINT depth{};ok(xml->GetDepth(&depth),"Read XML depth");if(kind==XmlNodeType_Element){const wchar_t* uri{};UINT uri_length{};ok(xml->GetNamespaceUri(&uri,&uri_length),"Read namespace");require(!uri_length,"Card XML namespaces are unsupported");const auto name=reader_name(xml.p);if(!depth){require(!root&&!closed&&name=="hwio-card"&&!xml->IsEmptyElement(),"Expected one hwio-card root");root=true;if(xml->MoveToFirstAttribute()==S_OK)do{auto attribute=reader_name(xml.p);require(attribute=="version"||attribute=="provider"||attribute=="model"||attribute=="serial"||attribute=="key"||attribute=="name","Unknown card XML attribute");require(attrs.emplace(attribute,reader_value(xml.p)).second,"Duplicate card XML attribute");}while(xml->MoveToNextAttribute()==S_OK);xml->MoveToElement();}
            else {require(depth==1 && root&&!closed&&!child&&name=="assignments"&&!xml->IsEmptyElement(),"Unexpected card XML element");require(xml->MoveToFirstAttribute()!=S_OK,"Assignments cannot have attributes");xml->MoveToElement();child=true;}}
            else if(kind==XmlNodeType_Text||kind==XmlNodeType_CDATA||kind==XmlNodeType_Whitespace){const auto text=reader_value(xml.p);if(child&&!child_closed&&depth==2){require(payload.size()+text.size()<=bound,"Assignment payload exceeds bound");payload+=text;}else require(text.find_first_not_of(" \t\r\n")==text.npos,"Unexpected card XML text");}
            else if(kind==XmlNodeType_EndElement){if(depth==2){require(child&&!child_closed&&reader_name(xml.p)=="assignments","Unexpected assignments close");child_closed=true;}else{require(depth==1&&root&&child_closed&&!closed&&reader_name(xml.p)=="hwio-card","Unexpected card close");closed=true;}}
            else require(kind==XmlNodeType_XmlDeclaration||kind==XmlNodeType_Comment,"Unsupported XML node");
        }
        ok(result,"Parse card XML");require(closed&&attrs.size()==6&&attrs.at("version")=="2","Incomplete or unsupported card I/O XML; export it with the current HWIO version");std::int32_t provider{};require(integer(attrs.at("provider"),provider)&&provider>=1&&provider<=4,"Unsupported card provider");auto& id=parsed.identity;id.provider=static_cast<BackendKind>(provider);id.model=attrs.at("model");id.serial=attrs.at("serial");id.key=attrs.at("key");id.name=attrs.at("name");require(!id.model.empty()&&id.model.size()<=128&&!id.serial.empty()&&id.serial.size()<=256&&!id.key.empty()&&id.key.size()<=128&&id.name.size()<=256,"Invalid card identity");parsed.assignments=decode(payload);check_identity(parsed);auto only=card_slice(*parsed.assignments,id);require(encode(*only)==encode(*parsed.assignments),"XML contains data outside its declared card I/O");archive=std::move(parsed);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
} // namespace hwio
