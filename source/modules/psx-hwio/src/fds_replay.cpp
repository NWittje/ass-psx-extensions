#include "hwio/fds_replay.hpp"
#include <bcrypt.h>
#include <sstream>
#include <iomanip>
#include <fstream>
namespace hwio::fds {
std::string context_encode(const ModuleConfiguration& modules,const ButtonConfiguration& buttons,const CockpitConfiguration& cockpit,std::span<const Device> devices){
    std::ostringstream out;out<<"FDSCTX1 "<<std::quoted(modules_encode(modules))<<' '<<std::quoted(button_configuration_encode(buttons))<<' '<<std::quoted(cockpit_encode(cockpit))<<' '<<devices.size()<<'\n';
    for(const auto& d:devices){const auto& i=d.input;out<<std::quoted(d.key)<<' '<<d.model<<' '<<d.caps.update<<' '<<unsigned(i.identity.backend)<<' '<<i.slot<<' '<<i.generation<<' '<<i.connected<<' '<<d.baseline<<' '<<i.coherent_reports<<' '<<d.generic<<' '<<i.controls.size()<<' '<<d.resources.size()<<'\n';
        for(const auto& c:i.controls)out<<unsigned(c.key.kind)<<' '<<c.key.channel_kind<<' '<<c.key.channel<<' '<<c.minimum<<' '<<c.maximum<<' '<<c.bits<<' '<<c.relative<<'\n';
        for(const auto& r:d.resources)out<<unsigned(r.kind)<<' '<<r.channel<<' '<<r.input<<' '<<r.output<<' '<<r.minimum<<' '<<r.maximum<<'\n';
    }return out.str();
}
namespace {
void context_decode(std::string_view text,ModuleConfiguration& modules,ButtonConfiguration& buttons,CockpitConfiguration& cockpit,std::vector<Device>& devices){
    if(text.size()>4*1024*1024)throw std::runtime_error("Replay context exceeds 4 MiB");std::istringstream in{std::string(text)};std::string header,m,b,c,error;std::size_t count{};
    if(!(in>>header>>std::quoted(m)>>std::quoted(b)>>std::quoted(c)>>count) || header!="FDSCTX1" || count>max_devices-first_fds_device || !modules_decode(m,modules,error) || !button_configuration_decode(b,buttons,error) || !cockpit_decode(c,cockpit,error))throw std::runtime_error("Invalid replay configuration: "+error);
    std::set<unsigned> slots;devices.clear();for(std::size_t n=0;n<count;++n){Device d;unsigned backend{},controls{},resources{};auto& i=d.input;
        if(!(in>>std::quoted(d.key)>>d.model>>d.caps.update>>backend>>i.slot>>i.generation>>i.connected>>d.baseline>>i.coherent_reports>>d.generic>>controls>>resources) || d.key.size()>256 || d.model.size()!=4 || controls>controls_bound || resources>2048 || i.slot<first_fds_device || i.slot>=max_devices || !slots.insert(i.slot).second || (backend!=unsigned(BackendKind::fds_usb) && backend!=unsigned(BackendKind::fds_ethernet) && backend!=unsigned(BackendKind::phidget22)))throw std::runtime_error("Invalid replay device");
        i.identity.backend=static_cast<BackendKind>(backend);i.identity.hardware_key=d.key;i.complete_module=!d.generic;
        for(unsigned k=0;k<controls;++k){ControlInfo v;unsigned kind{};if(!(in>>kind>>v.key.channel_kind>>v.key.channel>>v.minimum>>v.maximum>>v.bits>>v.relative) || kind>unsigned(ControlKind::button) || v.minimum>v.maximum || v.bits>32)throw std::runtime_error("Invalid replay input");v.key.kind=static_cast<ControlKind>(kind);v.key.backend=i.identity.backend;i.controls.push_back(v);}
        for(unsigned k=0;k<resources;++k){HardwareResource r;unsigned kind{};if(!(in>>kind>>r.channel>>r.input>>r.output>>r.minimum>>r.maximum) || kind<100 || kind>107 || r.minimum>r.maximum)throw std::runtime_error("Invalid replay resource");r.kind=static_cast<ResourceKind>(kind);d.resources.push_back(r);}devices.push_back(std::move(d));
    }in>>std::ws;if(!in.eof())throw std::runtime_error("Trailing replay context");
}
}
struct ReplaySession::Impl {
    std::uint64_t frequency;ModuleLibrary library;ReplayResult result;Mapper mapper;ButtonRouter buttons;CockpitRouter cockpit;std::unique_ptr<SourceBank> values{std::make_unique<SourceBank>()};std::vector<Device> devices;
    std::array<Framer,roots_bound> frames;std::array<NetworkInventory,roots_bound> inventories;std::array<std::uint64_t,roots_bound> generations{};std::array<Cdu3302Bitmap,roots_bound> bitmaps;std::array<std::uint64_t,roots_bound> bitmap_generations{};
    double ms{};std::uint64_t seconds{};std::unique_ptr<Provider> provider{std::make_unique<Provider>()};ModuleOutputs outputs;TraceEntry pending{};bool have{};std::string text;
    Impl(std::uint64_t f,const ModuleLibrary& l):frequency(f),library(l){if(!f)throw std::runtime_error("Invalid replay clock");}
    void output_step(){outputs.update(mapper,seconds,*provider);for(const auto& value:provider->offline_outputs()){const auto& i=value.intent;result.outputs.push_back(std::to_string(value.slot)+" "+std::to_string(i.device_generation)+" "+std::to_string(i.resource)+" "+std::to_string(i.channel)+" "+std::to_string(i.value)+" "+i.text.data());}}
    void drain(std::int64_t stamp){buttons.end_report();mapper.tick(ms);cockpit.update(*values,mapper,ms,0);for(unsigned n=0;n<2048;++n){auto command=buttons.next(*values,mapper,ms,stamp);if(!command)command=cockpit.next(mapper,ms,stamp);if(!command)break;result.commands.push_back(command->line);mapper.sent_record(command->line,stamp);buttons.sent(command->line);}}
    void process(const TraceEntry& first,std::string_view bytes){const auto& catalogue=active_catalogue();
        ms=double(first.stamp)*1000/double(frequency);
        if(first.kind==TraceKind::context){ModuleConfiguration m;ButtonConfiguration b;CockpitConfiguration c;context_decode(bytes,m,b,c,devices);const auto compiled=compile_modules(library,m,devices,catalogue,b,c,{},{});std::vector<DeviceInfo> input;for(const auto& d:devices)input.push_back(d.input);for(auto& bank:*values)bank.fill({});buttons.configure(compiled.buttons,input,mapper);cockpit.configure(compiled.cockpit,input,mapper,compiled.devices);outputs.configure(library,m,compiled,devices,catalogue,*provider);++result.contexts;}
        else if(first.kind==TraceKind::psx_record){mapper.receive(bytes,static_cast<std::int64_t>(first.stamp));buttons.received(bytes.substr(0,bytes.find('=')));}
        else if(first.kind==TraceKind::session){if(bytes=="ready")mapper.ready=true;else if(bytes=="boundary" || bytes=="load-boundary"){const bool loading=bytes=="load-boundary";mapper.boundary(first.generation,loading);buttons.boundary(first.generation,loading);cockpit.boundary(first.generation);if(!loading)for(auto& bank:*values)bank.fill({});}else if(bytes.starts_with("sent ")){mapper.sent_record(bytes.substr(5),static_cast<std::int64_t>(first.stamp));buttons.sent(bytes.substr(5));}else if(bytes.starts_with("seconds ")){std::istringstream timer{std::string(bytes.substr(8))};if(!(timer>>seconds))throw std::runtime_error("Invalid captured output timer");}}
        else if(first.kind==TraceKind::input){Sample s;bool edge{};std::istringstream in{std::string(bytes)};if(!(in>>s.device>>s.control>>s.generation>>s.sequence>>s.qpc>>s.value>>s.baseline>>s.relative>>s.invalid>>edge) || s.device>=max_devices || s.control>=max_controls)throw std::runtime_error("Invalid captured sample");const auto d=std::find_if(devices.begin(),devices.end(),[&](const auto& v){return v.input.slot==s.device && v.input.generation==s.generation && v.input.connected;});if(d==devices.end() || s.control>=d->input.controls.size()){++result.rejected;return;}(*values)[s.device][s.control]={s.value,!s.invalid,s.qpc,s.sequence};buttons.input(s,*values,mapper,edge);cockpit.input(s,mapper);++result.inputs;drain(static_cast<std::int64_t>(first.stamp));}
        else if(first.kind==TraceKind::tcp_rx && first.stream<roots_bound){const auto stream=first.stream;if(generations[stream]!=first.generation){frames[stream].reset();inventories[stream].reset();generations[stream]=first.generation;}frames[stream].feed({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},[&](std::string_view line){Message m;std::string error;if(parse_line(line,m,error)){inventories[stream].accept(m);++result.parsed;if(m.kind==Message::Kind::analogue_raw)++result.raw_analogue;}else{++result.rejected;result.faults.push_back(error);}});}
        else if(first.kind==TraceKind::usb_bitmap){
            if(first.stream<first_fds_device || first.stream>=first_fds_device+roots_bound){++result.rejected;return;}
            const auto slot=first.stream-first_fds_device;
            if(bitmap_generations[slot]!=first.generation){bitmaps[slot].reset();bitmap_generations[slot]=first.generation;}
            const auto decoded=bitmaps[slot].accept({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()});
            ++result.bitmap_packets;result.bitmap_edges+=decoded.count;
            if(decoded.kind==Cdu3302Bitmap::Result::Kind::rejected)++result.rejected;
        }
        else if(first.kind==TraceKind::fault)result.faults.emplace_back(bytes);
        output_step();
    }
    void append(const TraceEntry& e){if(e.size>e.bytes.size())throw std::runtime_error("Invalid replay chunk");if(have && (e.event!=pending.event || e.kind!=pending.kind || e.stream!=pending.stream || e.generation!=pending.generation)){process(pending,text);text.clear();have=false;}if(!have){pending=e;have=true;}if(text.size()+e.size>4*1024*1024)throw std::runtime_error("Replay event exceeds 4 MiB");text.append(e.bytes.data(),e.size);++result.entries;}
};
ReplaySession::ReplaySession(std::uint64_t f,const ModuleLibrary& l):impl_(std::make_unique<Impl>(f,l)){}ReplaySession::~ReplaySession()=default;
void ReplaySession::append(const TraceEntry& e){impl_->append(e);}void ReplaySession::finish(){if(impl_->have){impl_->process(impl_->pending,impl_->text);impl_->text.clear();impl_->have=false;}}
ReplayResult ReplaySession::take(){ReplayResult result=std::move(impl_->result);impl_->result={};return result;}
ReplayResult replay(std::span<const TraceEntry> entries,std::uint64_t frequency,const ModuleLibrary& library){if(entries.size()>65536)throw std::runtime_error("Invalid replay bounds");ReplaySession session(frequency,library);for(const auto& e:entries)session.append(e);session.finish();return session.take();}
std::string sha256_file(const std::filesystem::path& path){
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA-256 unavailable");std::array<unsigned char,32> digest{};
    try{if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("SHA-256 allocation failed");std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot hash diagnostic source");std::array<char,65536> block{};while(in){in.read(block.data(),block.size());const auto count=in.gcount();if(count && BCryptHashData(hash,reinterpret_cast<PUCHAR>(block.data()),static_cast<ULONG>(count),0)<0)throw std::runtime_error("SHA-256 read failed");}if(!in.eof() || BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)throw std::runtime_error("SHA-256 finish failed");}
    catch(...){if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);throw;}BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);std::ostringstream out;out<<std::hex<<std::setfill('0');for(const auto byte:digest)out<<std::setw(2)<<unsigned(byte);return out.str();
}
}
