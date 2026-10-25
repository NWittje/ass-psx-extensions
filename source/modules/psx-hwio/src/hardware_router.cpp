#include "hwio/hardware_router.hpp"
#include "hwio/fds.hpp"
#include "hwio/phidget_io.hpp"
#include "psx/diagnostics/journal.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace hwio {
bool observation_value(const CatalogueObservation& o,std::string_view payload,double& number,std::string_view& text) noexcept {
    if(o.encoding==ObservationEncoding::decimal_slice) {
        if(payload.size()<o.payload_minimum || payload.size()>o.payload_maximum ||
           o.character>payload.size() || o.width>payload.size()-o.character ||
           (o.guard_value && (o.guard_character>=payload.size() || static_cast<unsigned char>(payload[o.guard_character])!=o.guard_value)))return false;
        const auto slice=payload.substr(o.character,o.width);
        if(slice.empty() || slice.find_first_not_of("0123456789")!=slice.npos)return false;
        int value{};if(!integer(slice,value) || value<o.minimum || value>o.maximum)return false;
        number=value;return true;
    }
    unsigned count=0;std::string_view selected;
    for(;;){const auto end=payload.find(';');if(count++==o.field)selected=payload.substr(0,end);
        if(end==payload.npos)break;payload.remove_prefix(end+1);}
    if(count!=o.fields)return false;
    if(o.encoding==ObservationEncoding::text){text=selected;return text.size()<128;}
    if(o.encoding==ObservationEncoding::character){if(o.character>=selected.size())return false;number=static_cast<unsigned char>(selected[o.character]);}
    else {const auto r=std::from_chars(selected.data(),selected.data()+selected.size(),number);
        if(r.ec!=std::errc{} || r.ptr!=selected.data()+selected.size() || !std::isfinite(number))return false;}
    if(o.mask){if(number<0 || number>UINT32_MAX || std::floor(number)!=number)return false;number=(static_cast<std::uint32_t>(number)&o.mask)>>o.shift;}
    return number>=o.minimum && number<=o.maximum;
}
void HardwareRouter::configure(const HardwareConfiguration& c,const std::vector<DeviceInfo>& devices,const Catalogue& catalogue,const std::vector<EthernetHealth>& inventory){
    for(const auto& route:routes_)if(route.initialized && route.slot>=first_fds_device){auto intent=route.previous;intent.value=route.binding.failure;releases_.push_back({route.slot,intent,route.binding.failure});}
    routes_.clear();routes_.reserve(c.outputs.size());digital_.configure(catalogue);
    for(const auto& b:c.outputs)if(b.enabled){
        Route r;r.binding=b;r.view.id=b.id;
        std::sort(r.binding.table.begin(),r.binding.table.end());
        const auto definition=catalogue.observations.find(b.observation);
        r.digital=catalogue.digital_outputs.count(b.observation)!=0;
        if(definition==catalogue.observations.end() && !r.digital)r.view.reason="Observation ID missing from catalogue.xml";
        else if(!r.digital){r.observation=definition->second;r.record=record_name(r.observation->record);}
        unsigned found=0;
        for(const auto& d:devices)if(d.connected && d.identity.backend!=BackendKind::usb_hid && d.identity.hardware_key==b.device){r.slot=d.slot;r.generation=d.generation;++found;}
        if(found!=1){r.generation=0;r.view.reason=found?"Ambiguous hardware identity":"Hardware unavailable";}
        const auto enabled=std::find_if(c.devices.begin(),c.devices.end(),[&](const auto& card){return card.key==b.device;});
        if(enabled!=c.devices.end() && !enabled->enabled){r.generation=0;r.view.reason="Card disabled in HWIO Hardware options";}
        if(!inventory.empty() && r.generation){const auto card=std::find_if(inventory.begin(),inventory.end(),[&](const auto& h){return h.key==b.device && h.configured;});
            if(card==inventory.end()){r.generation=0;r.view.reason="Waiting for configuration read from card";}
            else {const auto issue=std::find_if(card->inhibited_outputs.begin(),card->inhibited_outputs.end(),[&](const auto& entry){return entry.first==b.id;});
                const auto target=std::find_if(card->resources.begin(),card->resources.end(),[&](const auto& entry){return entry.output && entry.kind==b.resource && entry.channel==b.channel;});
                if(issue!=card->inhibited_outputs.end()){r.generation=0;r.view.reason=issue->second;}
                else if(target==card->resources.end()){r.generation=0;r.view.reason="Output function unavailable in current card configuration";}
                if(r.generation && b.device.starts_with("fds-") && b.resource==ResourceKind::boolean_output){
                    // interfaceIT_LED_Set sends an eight-way interleaved bank
                    // from its local cache. Without output readback, every
                    // declared bit in that bank must have a HWIO owner.
                    const auto bank=(b.channel-1)%8;
                    if(std::any_of(card->resources.begin(),card->resources.end(),[](const auto& resource){return resource.output && resource.kind==ResourceKind::integral_display;})){
                        r.generation=0;r.view.reason="FDS LED bank shares unowned integral readouts; assign this complete module instead";
                    }
                    for(const auto& resource:card->resources)if(resource.output && resource.kind==ResourceKind::boolean_output && (resource.channel-1)%8==bank)
                        if(std::none_of(c.outputs.begin(),c.outputs.end(),[&](const auto& owner){return owner.enabled && owner.device==b.device && owner.resource==resource.kind && owner.channel==resource.channel;})){
                            r.generation=0;r.view.reason="FDS LED bank requires ownership of every declared bit; unowned output readback is unavailable";break;
                        }
                }
                if(r.generation && b.resource==ResourceKind::dataline_output)for(const auto& resource:card->resources)if(resource.output && resource.kind==ResourceKind::dataline_output)
                    if(std::none_of(c.outputs.begin(),c.outputs.end(),[&](const auto& owner){return owner.enabled && owner.device==b.device && owner.resource==resource.kind && owner.channel==resource.channel;})){
                        r.generation=0;r.view.reason="FDS dataline writes share the complete bank; every declared dataline requires a HWIO owner";break;
                    }
            }
        }
        if(b.resource==pokeys::Channel::ponet && b.bit!=UINT16_MAX){r.generation=0;r.view.reason="PoNET bit output unavailable: unowned output-bank readback not established";}
        if(!hardware_resource_enabled(c,b.device,b.resource)){r.generation=0;r.view.reason="PoExtBus outputs disabled in HWIO Hardware options";}
        r.testable=r.generation && r.digital && catalogue.digital_outputs.at(b.observation)->qualified &&
            b.minimum==0 && b.maximum==1 && b.transform==OutputTransform::boolean &&
            (b.resource==pokeys::Channel::digital || b.resource==pokeys::Channel::led || b.resource==pokeys::Channel::poext || b.resource==ResourceKind::boolean_output || b.resource==ResourceKind::dataline_output);
        const auto token=b.format.find("{value}");
        r.embed_value=token!=b.format.npos;
        if(token==b.format.npos)r.prefix=b.format;
        else {r.prefix=b.format.substr(0,token);r.suffix=b.format.substr(token+7);}
        routes_.push_back(std::move(r));
    }
}
bool HardwareRouter::set_test(const std::string& id,bool on) {
    Route* selected=nullptr;
    for(auto& route:routes_)if(route.binding.observation==id){if(selected)return false;selected=&route;}
    if(!selected || !selected->testable)return false;
    selected->test=on;return true;
}
void HardwareRouter::clear_tests(){for(auto& route:routes_)route.test.reset();}
void HardwareRouter::update(const Mapper& mapper,EthernetInput& ethernet,fds::Provider* fds,PhidgetIo* phidgets){
    if(fds || phidgets){std::size_t accepted{};while(accepted<releases_.size()){const auto& r=releases_[accepted];if(!(r.slot>=first_generic_phidget_device?phidgets && phidgets->submit({r.slot,r.intent}):fds && fds->submit({r.slot,r.intent,r.failure})))break;++accepted;}releases_.erase(releases_.begin(),releases_.begin()+static_cast<std::ptrdiff_t>(accepted));if(!releases_.empty())return;}
    digital_.update(mapper);
    for(auto& r:routes_){
        if(!r.generation)continue;
        HardwareOutputIntent intent;intent.session_epoch=mapper.epoch;intent.device_generation=r.generation;
        intent.resource=static_cast<std::uint16_t>(r.binding.resource);intent.channel=r.binding.channel;intent.velocity=r.binding.velocity;
        double source{};std::string_view text;
        const auto state=r.digital?mapper.records.end():mapper.records.find(r.record);
        const auto record_sequence=state==mapper.records.end()?0:state->second.sequence;
        if(!r.digital && r.initialized && r.view.submitted && r.epoch==mapper.epoch && r.record_sequence==record_sequence && r.ready==mapper.ready && r.authority==mapper.at)continue;
        r.epoch=mapper.epoch;r.record_sequence=record_sequence;r.ready=mapper.ready;r.authority=mapper.at;
        intent.available=mapper.ready && r.observation && state!=mapper.records.end() &&
            observation_value(*r.observation,state->second.observed,source,text);
        const auto* digital=r.digital?digital_.find(r.binding.observation):nullptr;
        if(digital){intent.available=digital->available;source=digital->on?1:0;intent.source_qpc=digital->qpc;}
        if(intent.available && r.binding.resource==pokeys::Channel::motion &&
           (!r.observation || !r.observation->motor_target || mapper.at!=AtAuthority::servo))intent.available=false;
        intent.stop=r.binding.resource==pokeys::Channel::motion && !intent.available;
        if(intent.available){
            if(!r.digital)intent.source_qpc=state->second.qpc;
            if(r.binding.mask){if(source<0 || source>UINT32_MAX || std::floor(source)!=source)intent.available=false;
                else source=static_cast<std::uint32_t>(source)&r.binding.mask;}
            if(r.binding.transform==OutputTransform::boolean)source=source>=r.binding.threshold?1:0;
            if(r.binding.transform==OutputTransform::lookup){
                const auto it=std::lower_bound(r.binding.table.begin(),r.binding.table.end(),source,[](const auto& p,double v){return p.first<v;});
                if(it==r.binding.table.end() || it->first!=source)intent.available=false;else source=it->second;
            }
            source=source*r.binding.scale+r.binding.offset;
            if(!std::isfinite(source))intent.available=false;
        }
        if(r.test){intent.available=true;source=*r.test?1:0;intent.source_qpc=psx::diagnostics::qpc_now();}
        intent.value=intent.available?std::clamp(source,r.binding.minimum,r.binding.maximum):r.binding.failure;
        intent.stop=intent.stop || (r.binding.resource==pokeys::Channel::motion && !intent.available);
        if(r.binding.transform==OutputTransform::text){
            if(!intent.available)text=r.binding.failure_text;
            char number[48]{};
            if(intent.available && (!r.observation || r.observation->encoding!=ObservationEncoding::text)){
                const auto formatted=std::to_chars(number,number+sizeof(number),intent.value,std::chars_format::general,8);
                text={number,static_cast<std::size_t>(formatted.ptr-number)};
            }
            std::size_t n=0;
            auto append=[&](std::string_view s){for(char ch:s)if(n+1<intent.text.size())intent.text[n++]=ch;};
            if(intent.available){append(r.prefix);if(r.embed_value)append(text);append(r.suffix);}else append(text);
        }
        const bool changed=!r.initialized || intent.session_epoch!=r.previous.session_epoch || intent.available!=r.previous.available ||
            intent.value!=r.previous.value || intent.stop!=r.previous.stop || intent.text!=r.previous.text;
        if(changed){intent.sequence=++sequence_;
            if(r.slot<first_fds_device?ethernet.submit({r.slot,intent}):r.slot>=first_generic_phidget_device?phidgets && phidgets->submit({r.slot,intent}):fds && fds->submit({r.slot,intent,r.binding.failure})){r.previous=intent;r.initialized=true;r.view.submitted=true;}
            else r.view.submitted=false;
        }
        r.view.source_available=intent.available;r.view.value=intent.value;r.view.testing=r.test.has_value();
        r.view.reason=r.test?(r.view.submitted?"Test value submitted to hardware transport":"Test value waiting: hardware queue full"):
            (!r.observation && !digital)?"Observation ID missing; failure value active":!intent.available?(digital?digital->reason:"Source/authority unavailable; failure value active"):r.view.submitted?"Submitted to hardware transport":"Hardware queue full";
    }
}
std::vector<HardwareOutputView> HardwareRouter::snapshot() const {
    std::vector<HardwareOutputView> out;out.reserve(routes_.size());for(const auto& r:routes_)out.push_back(r.view);return out;
}
} // namespace hwio
