#include "hwio/runtime.hpp"
#include <algorithm>

namespace hwio {
std::unique_ptr<CardAssignments> Runtime::card_configuration(){
    auto c=std::make_unique<CardAssignments>();
    {std::lock_guard lock(mutex_);c->profiles=profiles_;c->display=display_assignments_;c->axes=axes_;c->buttons=buttons_;c->cockpit=cockpit_;c->modules=modules_;}
    c->hardware=hardware();return c;
}
std::optional<CardIdentity> Runtime::card_identity(const std::string& key,std::string& error){
    const auto h=hardware();const auto device=std::find_if(h.devices.begin(),h.devices.end(),[&](const auto& d){return d.key==key;});
    if(device==h.devices.end()){error="This card is not available in the device inventory";return {};}
    CardIdentity id;id.key=key;id.name=device->name;id.provider=device->backend;
    if(device->backend==BackendKind::pokeys_ethernet){id.model="PoKeys:"+std::to_string(device->model);id.serial=std::to_string(device->serial);}
    else if(device->backend==BackendKind::phidget22){id.serial=std::to_string(device->serial);if(!device->provider_model.empty())id.model="Phidget:"+device->provider_model;const auto inventory=phidgets_.inventory();for(const auto& channel:inventory.channels)if(channel.address.device==key){const auto model="Phidget:"+std::to_string(channel.address.model);if(id.model.empty())id.model=model;else if(id.model!=model){error="The device inventory reports different card models for this address";return {};}}}
    else if(device->backend==BackendKind::fds_usb || device->backend==BackendKind::fds_ethernet){id.model="FDS:"+device->provider_model;id.serial=fds_card_serial(key,device->backend);}
    if(id.model.empty()||id.serial.empty()||id.serial=="0"){error="Card model and serial must be known before transferring assignments";return {};}
    error.clear();return id;
}
bool Runtime::export_card_assignments(const std::string& key,const std::filesystem::path& path,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);const auto id=card_identity(key,error);if(!id)return false;
    CardArchive archive;archive.identity=*id;archive.assignments=card_slice(*card_configuration(),*id);return write_card_archive(path,archive,error);
}
bool Runtime::prepare_card_import(const std::string& key,const std::filesystem::path& path,CardImport& prepared,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);const auto id=card_identity(key,error);if(!id)return false;CardArchive archive;if(!read_card_archive(path,archive,error))return false;
    const auto match=card_match(archive.identity,*id);if(match==CardMatch::incompatible){error="The XML card model/provider does not match the selected card";return false;}
    auto current=card_configuration();auto next=card_merge(*current,archive,*id,error);if(!next)return false;
    if(!module_conflicts(module_library_,next->modules,next->buttons,next->cockpit,next->hardware,next->profiles,error,&next->axes))return false;
    prepared.source=archive.identity;prepared.target=*id;prepared.match=match;prepared.preimage=card_assignments_signature(*current);prepared.replacement=std::move(next);error.clear();return true;
}
bool Runtime::import_card_assignments(const CardImport& prepared,bool confirmed,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    if(!prepared.replacement || prepared.match==CardMatch::incompatible || (prepared.match==CardMatch::model_only&&!confirmed)){error="Confirm replacing this model's assignments from a different serial first";return false;}
    const auto identity=card_identity(prepared.target.key,error);if(!identity)return false;if(*identity!=prepared.target){error="The selected card identity changed; choose the XML again";return false;}
    const auto current=card_configuration();if(card_assignments_signature(*current)!=prepared.preimage){error="Assignments changed while the import was open; choose the XML again";return false;}
    {std::lock_guard lock(mutex_);if(output_identification_card_.device==prepared.target.key){error="Finish output identification for this card before importing its assignments";return false;}}
    const auto& c=*prepared.replacement;if(!card_assignments_valid(c,error) || !module_conflicts(module_library_,c.modules,c.buttons,c.cockpit,c.hardware,c.profiles,error,&c.axes))return false;
    if(!save_assignments(options_.data_dir+L"\\assignments.cfg",c.profiles,c.display,c.axes,error,&c.buttons,&c.hardware,&c.cockpit,&c.modules))return false;
    {std::lock_guard lock(mutex_);std::set<std::string> changed_outputs;for(const auto* h:{static_cast<const HardwareConfiguration*>(&hardware_),&c.hardware})for(const auto& b:h->outputs)if(b.device==prepared.target.key)changed_outputs.insert(b.observation);bool tests_changed{};for(const auto& id:changed_outputs)tests_changed=output_tests_.erase(id)!=0||tests_changed;profiles_=c.profiles;buttons_=c.buttons;hardware_=c.hardware;cockpit_=c.cockpit;++revision_;++button_revision_;++hardware_revision_;++cockpit_revision_;if(tests_changed)++output_test_revision_;}
    SetEvent(change_);error.clear();return true;
}
} // namespace hwio
