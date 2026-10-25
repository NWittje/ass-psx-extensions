#include "hwio/input_overrides.hpp"
#include "hwio/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hwio {
std::vector<InputOverrideControl> input_override_controls(const Catalogue& catalogue){
    std::map<std::string,InputOverrideControl> result;
    for(const auto& a:catalogue.actions){
        const auto id="button:"+a.group;
        auto& c=result[id];c.id=id;c.chapter=a.chapter;
        if(c.name.empty() || a.primary)c.name=a.name;
        c.actions.push_back(a.id);
    }
    for(const auto& g:catalogue.groups){const auto it=result.find("button:"+g.id);if(it!=result.end())it->second.name=g.name;}
    // Catalogue links, rather than record-name coincidence, establish rotary aliases.
    for(const auto& [id,d]:catalogue.cockpit)if(d->encoder){
        InputOverrideControl c;c.id="encoder:"+id;c.name=d->name;c.chapter=d->chapter;c.encoder=true;
        for(const auto& action:{d->decrease,d->increase})if(const auto* a=catalogue.action(action)){
            const auto found=result.find("button:"+a->group);
            if(found!=result.end())c.actions.insert(c.actions.end(),found->second.actions.begin(),found->second.actions.end());
        }
        std::sort(c.actions.begin(),c.actions.end());c.actions.erase(std::unique(c.actions.begin(),c.actions.end()),c.actions.end());
        result[c.id]=std::move(c);
    }
    std::set<std::string> rotary_actions;
    for(const auto& [id,c]:result)if(c.encoder)rotary_actions.insert(c.actions.begin(),c.actions.end());
    std::vector<InputOverrideControl> out;
    for(auto& [id,c]:result)if(c.encoder || std::none_of(c.actions.begin(),c.actions.end(),[&](const auto& a){return rotary_actions.count(a)!=0;}))out.push_back(std::move(c));
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.chapter,a.name,a.id)<std::tie(b.chapter,b.name,b.id);});return out;
}
std::string input_overrides_encode(const InputOverrideConfiguration& c){
    std::ostringstream out;out<<"input-overrides-v1\n"<<c.controls.size()<<'\n';for(const auto& id:c.controls)out<<std::quoted(id)<<'\n';return out.str();
}
bool input_overrides_decode(std::string_view text,InputOverrideConfiguration& c,std::string& error){
    std::istringstream in{std::string(text)};std::string header,id;std::size_t count{};InputOverrideConfiguration next;
    if(text.size()>1024*1024 || !(in>>header>>count) || header!="input-overrides-v1" || count>8192){error="Invalid input override header or size";return false;}
    for(std::size_t n=0;n<count;++n)if(!(in>>std::quoted(id)) || id.size()>300 || (!id.starts_with("button:") && !id.starts_with("encoder:")) || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:")!=id.npos || !next.controls.insert(id).second){error="Invalid or duplicate input override";return false;}
    in>>std::ws;if(!in.eof()){error="Unexpected input override data";return false;}c=std::move(next);return true;
}
void InputOverrides::load(const std::wstring& directory){
    std::lock_guard lock(mutex_);directory_=directory;view_={};view_.revision=1;
    const auto file=std::filesystem::path(directory)/L"input-overrides.cfg";
    if(!std::filesystem::exists(file) && !std::filesystem::exists(file.wstring()+L".bak"))return;
    std::string error;
    for(const auto& candidate:{file,std::filesystem::path(file.wstring()+L".bak")}){
        std::ifstream in(candidate,std::ios::binary);if(!in)continue;
        if(std::filesystem::file_size(candidate)>1024*1024)continue;
        std::string data{std::istreambuf_iterator<char>(in),{}};
        if(input_overrides_decode(data,view_.configuration,error)){view_.status=candidate==file?"":"Recovered input overrides from backup";return;}
    }
    view_.valid=false;view_.status="Cannot load input overrides; button/encoder routing inhibited. Restore the file or Clear all.";
}
InputOverrideView InputOverrides::snapshot() const {std::lock_guard lock(mutex_);return view_;}
bool InputOverrides::replace(InputOverrideConfiguration configuration,std::string& error,std::uint64_t expected_revision){
    std::lock_guard lock(mutex_);InputOverrideConfiguration validated;
    if(!view_.valid && !configuration.controls.empty()){error="Restore the input override file and restart, or use Clear all first";view_.status=error;return false;}
    if(expected_revision!=UINT64_MAX && expected_revision!=view_.revision){error="Input override list changed; retry the operation";view_.status=error;return false;}
    if(!input_overrides_decode(input_overrides_encode(configuration),validated,error) || !save_input_overrides(directory_,configuration,error)){view_.status=error;return false;}
    view_.configuration=std::move(configuration);view_.valid=true;++view_.revision;view_.status="Saved input overrides";return true;
}
void InputOverrides::capture(bool value){std::lock_guard lock(mutex_);if(value && !view_.valid){view_.status="Restore the input override file and restart, or use Clear all first";return;}view_.capturing=value;view_.status=value?"Press a button or turn an encoder to block its applied control":"Capture cancelled";}
void InputOverrides::status(std::string value){std::lock_guard lock(mutex_);view_.status=std::move(value);}
InputOverridePolicy::InputOverridePolicy(const Catalogue& catalogue):controls_(input_override_controls(catalogue)){
    for(const auto& c:controls_){for(const auto& action:c.actions)actions_[action]=c.id;if(c.encoder)encoders_[c.id.substr(8)]=c.id;}
}
void InputOverridePolicy::configure(const InputOverrideView& view){
    invalid_=!view.valid;blocked_actions_.clear();blocked_encoders_.clear();
    for(const auto& c:controls_)if(invalid_ || view.configuration.controls.count(c.id)){blocked_actions_.insert(c.actions.begin(),c.actions.end());if(c.encoder)blocked_encoders_.insert(c.id.substr(8));}
    for(const auto& c:controls_)if(c.encoder && std::any_of(c.actions.begin(),c.actions.end(),[&](const auto& a){return blocked_actions_.count(a)!=0;}))blocked_encoders_.insert(c.id.substr(8));
}
std::string InputOverridePolicy::button_control(std::string_view id) const {const auto it=actions_.find(std::string(id));return it==actions_.end()?"":it->second;}
std::string InputOverridePolicy::encoder_control(std::string_view id) const {const auto it=encoders_.find(std::string(id));return it==encoders_.end()?"":it->second;}
bool InputOverridePolicy::button_blocked(std::string_view id) const{return invalid_ || blocked_actions_.count(std::string(id));}
bool InputOverridePolicy::encoder_blocked(std::string_view id) const{return invalid_ || blocked_encoders_.count(std::string(id));}
}
