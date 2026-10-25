#include "hwio/hardware_config.hpp"
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace hwio {
HardwareConfiguration initial_hardware_configuration(){
    return {}; // Every supported card on the selected adapter is discovered automatically.
}
bool hardware_valid(const HardwareConfiguration& c,std::string& error){
    if(c.devices.size()>max_ethernet_devices+max_fds_devices+max_phidget_devices || c.outputs.size()>512 || c.phidget_ports.size()>max_phidget_devices*32){error="Hardware device/output limit exceeded";return false;}
    std::set<std::string> keys;std::set<std::uint64_t> ids;std::set<std::tuple<std::string,pokeys::Channel,unsigned,unsigned>> owners;
    for(const auto& d:c.devices){
        if(d.key.empty() || d.key.size()>128 || d.name.size()>256 || !keys.insert(d.key).second || !std::isfinite(d.fixed_hz) || d.fixed_hz<0 || d.fixed_hz>10000 || (d.fixed_hz>0 && d.fixed_hz<1)){error="Invalid/duplicate network device or acquisition rate";return false;}
        pokeys::Identity i;i.serial=d.serial;i.hardware=d.model;
        if(d.backend==BackendKind::pokeys_ethernet){if(!i.supported() || !d.serial || d.key!=i.key()){error="PoKeys requires a verified nonzero serial and matching model identity";return false;}}
        else if(d.backend==BackendKind::phidget22){if(!d.key.starts_with("phidget:") || !d.serial){error="Invalid Phidget identity";return false;}}
        else if((d.backend!=BackendKind::fds_usb && d.backend!=BackendKind::fds_ethernet) ||
            !d.key.starts_with(d.backend==BackendKind::fds_usb?"fds-usb:":"fds-net:") || d.provider_model.size()!=4 || d.provider_revision>65535){error="Invalid provider-qualified hardware identity";return false;}
    }
    for(const auto& b:c.outputs){
        if(!b.id || !ids.insert(b.id).second || b.name.size()>256 || b.observation.size()>256 || b.format.size()>120 || b.failure_text.size()>120 || b.table.size()>128){error="Invalid hardware output identity or text length";return false;}
        if(!std::isfinite(b.scale)||!std::isfinite(b.offset)||!std::isfinite(b.minimum)||!std::isfinite(b.maximum)||b.minimum>b.maximum||!std::isfinite(b.failure)||b.failure<b.minimum||b.failure>b.maximum||!std::isfinite(b.threshold)){error="Invalid hardware output scale/range/failure value";return false;}
        if(b.device.starts_with("phidget:") && b.resource!=ResourceKind::boolean_output){error="Phidget output requires a Boolean digital channel";return false;}
        const bool neutral=b.resource==ResourceKind::boolean_output || b.resource==ResourceKind::dataline_output;
        if(static_cast<unsigned>(b.transform)>3 || static_cast<unsigned>(b.resource)<1 || (!neutral && static_cast<unsigned>(b.resource)>static_cast<unsigned>(pokeys::Channel::motion))){error="Unknown output transform/resource";return false;}
        if(b.bit!=UINT16_MAX && (b.resource!=pokeys::Channel::ponet || b.bit>=128 || b.minimum!=0 || b.maximum!=1 || b.transform!=OutputTransform::boolean)){error="Invalid Boolean PoNET bit address";return false;}
        if(b.enabled && (!keys.count(b.device) || b.observation.empty() || !owners.emplace(b.device,b.resource,b.channel,b.bit).second)){error="Missing device/observation or duplicate physical output owner";return false;}
        if(b.enabled && b.resource==pokeys::Channel::ponet && std::any_of(c.outputs.begin(),c.outputs.end(),[&](const auto& other){return other.enabled && other.id!=b.id && other.device==b.device && other.resource==b.resource && other.channel==b.channel && (other.bit==UINT16_MAX || b.bit==UINT16_MAX);})){error="PoNET whole-bank and bit ownership overlap";return false;}
        if(b.enabled){
            const auto device=std::find_if(c.devices.begin(),c.devices.end(),[&](const auto& d){return d.key==b.device;});
            if(device!=c.devices.end() && neutral!=(device->backend!=BackendKind::pokeys_ethernet)){error="Output resource does not belong to the selected hardware provider";return false;}
            // Configuration declares an assignment, not controller capabilities.
            // Actual resource eligibility is checked against each session's readback.
            const bool resource=b.resource==pokeys::Channel::digital?b.channel<55:
                b.resource==pokeys::Channel::pwm?b.channel<6 && b.minimum>=0 && b.maximum<=1:
                b.resource==pokeys::Channel::lcd?b.channel<4 && b.transform==OutputTransform::text:
                b.resource==pokeys::Channel::led?b.channel<128:
                b.resource==pokeys::Channel::poext?b.channel<80:
                b.resource==pokeys::Channel::poil?b.channel<128:
                b.resource==pokeys::Channel::ponet?b.channel<16 && b.minimum>=0 && b.maximum<=UINT32_MAX:
                b.resource==pokeys::Channel::motion?b.channel<8 && !b.unit.empty():
                neutral?(b.device.starts_with("phidget:")?b.channel<8:b.channel>=1 && b.channel<=(b.resource==ResourceKind::dataline_output?32:512)) && b.minimum==0 && b.maximum==1 && b.transform==OutputTransform::boolean:false;
            if(!resource){error="Invalid physical output function, channel or range";return false;}
            if(b.resource==pokeys::Channel::poil && (b.minimum<INT32_MIN || b.maximum>INT32_MAX)){error="PoIL output must fit a signed 32-bit slot";return false;}
            if(b.format.find("{value}")!=b.format.rfind("{value}")){error="Text format accepts one {value} placeholder";return false;}
            if(b.transform==OutputTransform::lookup && b.table.empty()){error="Enabled lookup output requires at least one value";return false;}
        }
        std::set<double> lookup;
        for(const auto& [key,value]:b.table)if(!std::isfinite(key)||!std::isfinite(value)||!lookup.insert(key).second || value<b.minimum || value>b.maximum){error="Invalid/duplicate output lookup table value";return false;}
    }
    std::set<std::pair<std::string,unsigned>> ports;for(const auto& p:c.phidget_ports)if(!p.device.starts_with("phidget:") || p.device.size()>128 || p.channel>31 || !ports.emplace(p.device,p.channel).second){error="Invalid/duplicate Phidget analogue port";return false;}
    return true;
}
std::string hardware_encode(const HardwareConfiguration& c){
    std::ostringstream o;o<<std::setprecision(17)<<"hardware-v8\n"<<c.devices.size()<<'\n';
    for(const auto& d:c.devices){
        o<<std::quoted(d.key)<<' '<<std::quoted(d.name)<<' '<<d.enabled<<' '<<d.serial<<' '<<unsigned(d.model)<<' '<<d.fixed_hz<<' '<<d.use_poext<<' '<<unsigned(d.backend)<<' '<<std::quoted(d.provider_model)<<' '<<d.provider_revision<<'\n';

    }
    o<<c.outputs.size()<<'\n';for(const auto& b:c.outputs){
        o<<b.id<<' '<<std::quoted(b.name)<<' '<<std::quoted(b.device)<<' '<<std::quoted(b.observation)<<' '<<std::quoted(b.unit)<<' '<<std::quoted(b.format)<<' '<<std::quoted(b.failure_text)<<' '<<unsigned(b.resource)<<' '<<b.channel<<' '<<b.enabled<<' '<<b.velocity<<' '<<unsigned(b.transform)<<' '<<b.scale<<' '<<b.offset<<' '<<b.minimum<<' '<<b.maximum<<' '<<b.failure<<' '<<b.threshold<<' '<<b.mask<<' '<<b.table.size();
        for(const auto& [key,value]:b.table)o<<' '<<key<<' '<<value;o<<' '<<b.bit<<'\n';
    }o<<c.phidget_ports.size()<<'\n';for(const auto& p:c.phidget_ports)o<<std::quoted(p.device)<<' '<<p.channel<<' '<<p.voltage<<'\n';return o.str();
}
bool hardware_decode(std::string_view text,HardwareConfiguration& out,std::string& error){
    if(text.size()>4*1024*1024){error="Hardware configuration exceeds bound";return false;}
    std::istringstream in{std::string(text)};std::string header;std::size_t n{};HardwareConfiguration c;
    auto byte=[&](std::uint8_t& v){unsigned x{};if(!(in>>x)||x>255)in.setstate(std::ios::failbit);v=static_cast<std::uint8_t>(x);};
    if(!(in>>header>>n)||(header!="hardware-v1" && header!="hardware-v2" && header!="hardware-v3" && header!="hardware-v4" && header!="hardware-v5" && header!="hardware-v6" && header!="hardware-v7" && header!="hardware-v8")||n>max_ethernet_devices+max_fds_devices+max_phidget_devices){error="Invalid hardware configuration header";return false;}c.devices.resize(n);
    for(auto& d:c.devices){
        in>>std::quoted(d.key)>>std::quoted(d.name);
        if(header=="hardware-v1" || header=="hardware-v2"){std::string obsolete_address;in>>std::quoted(obsolete_address);} // Consume, never retain/use legacy IP.
        in>>d.enabled>>d.serial;byte(d.model);in>>d.fixed_hz;
        if(header=="hardware-v6" || (header=="hardware-v7" || header=="hardware-v8"))in>>d.use_poext;
        if((header=="hardware-v7" || header=="hardware-v8")){unsigned backend{};in>>backend>>std::quoted(d.provider_model)>>d.provider_revision;if(backend>4){error="Invalid hardware backend";return false;}d.backend=static_cast<BackendKind>(backend);}
        if(header=="hardware-v4" || header=="hardware-v5" || header=="hardware-v6" || (header=="hardware-v7" || header=="hardware-v8"))continue;
        pokeys::Configuration a; // Consume legacy setup, never retain or apply it.
        in>>a.watchdog_ms;
        for(auto& p:a.pins){in>>p.owned;byte(p.mode);byte(p.counter_options);}
        for(auto& e:a.encoders){in>>e.owned>>e.enabled;byte(e.a);byte(e.b);byte(e.options);in>>e.counts_per_detent;}
        auto& m=a.matrix;in>>m.owned>>m.enabled;byte(m.rows);byte(m.columns);byte(m.decimation);for(auto& p:m.row_pins)byte(p);for(auto& p:m.column_pins)byte(p);
        byte(a.pwm.owned);byte(a.pwm.enabled);in>>a.pwm.period;
        for(auto* p:{&a.lcd,&a.led[0],&a.led[1]}){in>>p->owned>>p->enabled;byte(p->rows);byte(p->columns);byte(p->pins);}
        if(!(in>>n)||n>128){error="Invalid peripheral count";return false;}a.peripherals.resize(n);
        for(auto& p:a.peripherals){unsigned kind{};std::size_t pins{};in>>kind>>p.channel>>p.address>>p.length>>p.rate>>p.options>>p.enabled>>pins;if(!in||kind<1||kind>unsigned(pokeys::Channel::motion)||pins>55){error="Invalid peripheral definition";return false;}p.kind=static_cast<pokeys::Channel>(kind);p.pins.resize(pins);for(auto& pin:p.pins)byte(pin);if(header!="hardware-v1"){std::size_t size{};if(!(in>>size)||size>55){error="Invalid peripheral payload length";return false;}p.data.resize(size);for(auto& value:p.data)byte(value);}}
        if(!(in>>n)||n>8){error="Invalid motion axis count";return false;}a.motion.resize(n);
        for(auto& p:a.motion){in>>p.owned>>p.enabled>>p.require_home>>p.axis>>std::quoted(p.unit)>>p.steps_per_unit>>p.minimum>>p.maximum>>p.maximum_speed>>p.acceleration;byte(p.step_pin);byte(p.direction_pin);byte(p.home_pin);byte(p.negative_limit);byte(p.positive_limit);}
    }
    if(!(in>>n)||n>512){error="Invalid output count";return false;}c.outputs.resize(n);
    for(auto& b:c.outputs){unsigned kind{},transform{};std::size_t count{};
        in>>b.id>>std::quoted(b.name)>>std::quoted(b.device)>>std::quoted(b.observation)>>std::quoted(b.unit)>>std::quoted(b.format)>>std::quoted(b.failure_text)>>kind>>b.channel>>b.enabled>>b.velocity>>transform>>b.scale>>b.offset>>b.minimum>>b.maximum>>b.failure>>b.threshold>>b.mask>>count;
        if(!in||kind<1||(kind>unsigned(pokeys::Channel::motion) && !((header=="hardware-v7" || header=="hardware-v8") && (kind==unsigned(ResourceKind::boolean_output) || kind==unsigned(ResourceKind::dataline_output))))||transform>3||count>128){error="Invalid hardware output definition";return false;}b.resource=static_cast<pokeys::Channel>(kind);b.transform=static_cast<OutputTransform>(transform);b.table.resize(count);for(auto& [key,value]:b.table)in>>key>>value;
        if(header=="hardware-v5" || header=="hardware-v6" || (header=="hardware-v7" || header=="hardware-v8")){unsigned bit{};if(!(in>>bit) || bit>UINT16_MAX){error="Invalid output bit address";return false;}b.bit=static_cast<std::uint16_t>(bit);}
    }
    if(header=="hardware-v8"){if(!(in>>n) || n>max_phidget_devices*32){error="Invalid Phidget port count";return false;}c.phidget_ports.resize(n);for(auto& p:c.phidget_ports){unsigned mode{};in>>std::quoted(p.device)>>p.channel>>mode;if(mode>1){error="Invalid Phidget analogue mode";return false;}p.voltage=mode!=0;}}
    if(!in){error="Incomplete hardware configuration";return false;}in>>std::ws;if(!in.eof()){error="Unexpected hardware configuration data";return false;}
    if(header!="hardware-v6" && header!="hardware-v7" && header!="hardware-v8")for(auto& d:c.devices)d.use_poext=std::any_of(c.outputs.begin(),c.outputs.end(),[&](const auto& b){return b.enabled && b.device==d.key && b.resource==pokeys::Channel::poext;});
    if(!hardware_valid(c,error))return false;out=std::move(c);return true;
}
bool hardware_resource_enabled(const HardwareConfiguration& c,std::string_view device,pokeys::Channel kind) noexcept {
    if(kind!=pokeys::Channel::poext)return true;
    for(const auto& d:c.devices)if(d.key==device)return d.enabled && d.use_poext;
    return false;
}
} // namespace hwio
