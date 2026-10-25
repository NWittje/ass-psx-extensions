#include "hwio/cpflight.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace hwio {
std::unique_ptr<CpflightPort> native_cpflight_port();
namespace {
bool same_endpoint(const psx::hardware::SerialPort& a,const psx::hardware::SerialPort& b){return !a.instance_id.empty() && a.instance_id==b.instance_id?true:a.com_name==b.com_name && a.instance_id==b.instance_id;}
std::int64_t receipt(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
ControlInfo control(unsigned channel,std::string name,bool rotary=false){ControlInfo c;c.key.backend=BackendKind::cpflight_serial;c.key.kind=rotary?ControlKind::axis:ControlKind::button;c.key.channel_kind=unsigned(rotary?ResourceKind::rotary_delta:ResourceKind::boolean_input);c.key.channel=static_cast<std::uint16_t>(channel);c.minimum=rotary?-32768:0;c.maximum=rotary?32767:1;c.bits=rotary?16:1;c.relative=rotary;c.name=std::move(name);return c;}
}
bool valid_cpflight_configuration(const CpflightConfiguration& c,std::string& error){
    const auto& n=c.port.com_name;
    if((c.enabled && n.empty()) || (!n.empty() && (n.size()<4 || n.size()>8 || n.substr(0,3)!=L"COM" || n.substr(3).find_first_not_of(L"0123456789")!=n.npos || n[3]==L'0'))){error="Select a valid CPflight COM port";return false;}
    if(c.port.instance_id.size()>1024 || c.port.interface_path.size()>2048 || c.serial.size()>32 || c.serial.find_first_not_of("0123456789")!=c.serial.npos){error="Invalid saved CPflight identity";return false;}
    return true;
}
struct Cpflight::Impl {
    Cpflight& owner;CpflightDriver driver;
    mutable std::mutex mutex;std::condition_variable change;std::thread worker;
    bool stopping{},refresh_requested{true},ready{};unsigned roles{};
    std::uint64_t revision{},epoch{},sequence{},output_revision{};
    CpflightConfiguration configuration;CpflightView view;
    std::vector<ModuleDevice> inventory;
    std::array<std::vector<Sample>,3> latest;
    std::map<unsigned,ModuleOutput> outputs;
    explicit Impl(Cpflight& o,CpflightDriver d):owner(o),driver(std::move(d)){
        if(!driver.enumerate)driver.enumerate=psx::hardware::enumerate_serial_ports;
        if(!driver.create)driver.create=native_cpflight_port;
    }
    void status(std::string_view text,bool connected=false){std::lock_guard lock(mutex);if(view.status!=text || view.connected!=connected){view.status.assign(text);view.connected=connected;++view.revision;SetEvent(owner.available_event);}}
    void disconnect(){std::lock_guard lock(mutex);outputs.clear();++output_revision;for(auto& d:inventory){d.input.connected=false;d.baseline=false;}for(auto& bank:latest)bank.clear();view.connected=false;view.vs_ready=false;++view.revision;++owner.metadata_revision;SetEvent(owner.available_event);}
    void discovered(const cpflight::Identity& id,const psx::hardware::SerialPort& port){
        std::lock_guard lock(mutex);inventory.clear();++view.generation;view.serial=id.serial;view.model=id.model;view.com_name=port.com_name;view.captain=id.captain;view.first_officer=id.first_officer;view.connected=true;view.vs_ready=false;outputs.clear();++output_revision;
        for(unsigned role=0;role<3;++role){latest[role].clear();if((role==1 && !id.captain) || (role==2 && !id.first_officer))continue;
            ModuleDevice d;d.key="cpflight:"+id.serial+(role==0?":mcp":role==1?":efi1":":efi2");d.model=role==0?id.model:role==1?"CP747_EFI1":"CP747_EFI2";d.update=105;d.baseline=true;d.status="Connected / "+utf8(port.com_name);d.input.identity.backend=BackendKind::cpflight_serial;d.input.identity.hardware_key=d.key;d.input.identity.serial=id.serial;d.input.identity.name=role==0?"CPflight Boeing 747 MCP":role==1?"CPflight Captain EFIS":"CPflight First Officer EFIS";d.input.slot=static_cast<std::uint16_t>(first_cpflight_device+role);d.input.generation=view.generation;d.input.connected=true;d.input.complete_module=true;d.input.coherent_reports=false;
            for(const auto& i:cpflight::inputs())if(i.role==role && i.kind!=cpflight::InputKind::unavailable)d.input.controls.push_back(control(i.channel,std::string(i.name)));
            if(!role){for(unsigned c=2;c<=5;++c)d.input.controls.push_back(control(1000+c,"MCP numeric "+std::to_string(c),true));for(unsigned c=21;c<=34;++c)if(c!=23){HardwareResource r;r.kind=ResourceKind::boolean_output;r.channel=static_cast<std::uint16_t>(c);r.output=true;d.resources.push_back(r);}for(unsigned c=2;c<=5;++c){HardwareResource r;r.kind=ResourceKind::integral_display;r.channel=static_cast<std::uint16_t>(c);r.output=true;d.resources.push_back(r);}}
            d.addressed_windows=true;
            latest[role].resize(d.input.controls.size());
            for(std::size_t c=0;c<latest[role].size();++c){auto& s=latest[role][c];s.device=d.input.slot;s.control=static_cast<std::uint16_t>(c);s.generation=view.generation;s.sequence=++sequence;s.qpc=receipt();s.baseline=true;s.relative=d.input.controls[c].relative;s.invalid=true;}
            inventory.push_back(std::move(d));
        }
        ++view.revision;++owner.metadata_revision;SetEvent(owner.available_event);
    }
    void sample(unsigned role,unsigned channel,int value,bool baseline,bool relative=false){
        std::lock_guard lock(mutex);const auto d=std::find_if(inventory.begin(),inventory.end(),[&](const auto& x){return x.input.slot==first_cpflight_device+role && x.input.connected;});if(d==inventory.end())return;
        const auto c=std::find_if(d->input.controls.begin(),d->input.controls.end(),[&](const auto& x){return x.key.channel==channel;});if(c==d->input.controls.end())return;const auto index=static_cast<std::size_t>(c-d->input.controls.begin());auto& s=latest[role][index];s={d->input.slot,static_cast<std::uint16_t>(index),d->input.generation,++sequence,receipt(),value,baseline,relative,false,false};
        if(!ready || !(roles&(1u<<role)))s.baseline=true;
        // Never enqueue offline moves. The latest sample remains a baseline only.
        if(ready && (roles&(1u<<role))){if(!owner.edges.push(s)){++owner.overflow;ready=false;outputs.clear();++output_revision;view.status="Input overflow; resynchronizing";}SetEvent(owner.available_event);}
    }
    void run(){
        std::unique_ptr<CpflightPort> port;std::uint64_t seen=UINT64_MAX,enumerate_at{},retry_at{},opened_at{},publication_epoch{};unsigned backoff{},publication_roles{};bool operational{},permanent{},vs_enabled{},vs_seen{},vs_first_done{},vs_sync{};std::uint64_t vs_at{};cpflight::Identity identity;cpflight::Framer framer;
        std::uint64_t output_seen=UINT64_MAX,last_receive_at{},identified_at{};bool startup_settled{};std::map<unsigned,ModuleOutput> pending;
        std::map<std::string,unsigned> states;std::array<std::optional<cpflight::Numeric>,6> numeric_baseline;std::map<unsigned,ModuleOutput> sent;
        auto close=[&]{if(port){std::string error;port->write(std::string_view("K999\0",5),error);port->close();port.reset();}operational=false;disconnect();};
        auto write=[&](std::string_view bytes){std::string error;if(port && port->write(bytes,error))return true;status(error.empty()?"Serial write failed":error);return false;};
        while(true){
            CpflightConfiguration config;std::uint64_t current_revision,current_epoch;bool publish;unsigned role_mask;bool refresh;
            {std::lock_guard lock(mutex);if(stopping)break;config=configuration;current_revision=revision;publish=ready;current_epoch=epoch;role_mask=roles;refresh=refresh_requested;refresh_requested=false;}
            const auto now=GetTickCount64();
            if(refresh || now>=enumerate_at){try {auto ports=driver.enumerate();std::lock_guard lock(mutex);view.ports=std::move(ports);++view.revision;}catch(...){status("Serial inventory unavailable");}enumerate_at=now+2000;}
            if(seen!=current_revision){close();seen=current_revision;permanent=false;retry_at=0;backoff=0;identity={};framer={};states.clear();numeric_baseline={};sent.clear();vs_enabled=false;vs_sync=false;vs_first_done=false;vs_seen=false;}
            if(!config.enabled){status("Support disabled");std::unique_lock lock(mutex);change.wait_for(lock,std::chrono::milliseconds(100));continue;}
            if(!port && !permanent && now>=retry_at){
                auto candidate=config.port;std::vector<psx::hardware::SerialPort> ports;{std::lock_guard lock(mutex);ports=view.ports;}
                if(!candidate.instance_id.empty()){std::vector<psx::hardware::SerialPort> matching;for(const auto& p:ports)if(p.instance_id==candidate.instance_id)matching.push_back(p);if(matching.size()==1)candidate=matching.front();else {status(matching.empty()?"Saved port unavailable — retrying":"Ambiguous saved port identity");retry_at=now+2000;continue;}}
                status("Connecting");port=driver.create();std::string error;
                if(!port || !port->open(candidate,error)){if(port)port->close();port.reset();status(error.empty()?"Port unavailable — retrying":error);retry_at=now+(1000ull<<std::min(backoff++,3u));}
                else {identity={};framer={};states.clear();numeric_baseline={};sent.clear();vs_enabled=false;vs_sync=false;vs_seen=false;vs_first_done=false;startup_settled=false;opened_at=now;operational=false;{std::lock_guard lock(mutex);view.com_name=candidate.com_name;}status("Identifying");if(!write(std::string_view("Q001\0",5))){close();retry_at=now+1000;}}
            }
            if(!port){std::unique_lock lock(mutex);change.wait_for(lock,std::chrono::milliseconds(50));continue;}
            std::string error;std::array<std::uint8_t,1024> bytes{};const int count=port->read(bytes,error);bool failed=count<0;
            if(count>0){last_receive_at=GetTickCount64();++owner.reports;framer.feed(std::span(bytes.data(),static_cast<std::size_t>(count)),[&](std::string_view record){
                if(identity.receive(record)){
                    // An identity and its first selector states may share a read.
                    if(!operational && identity.ready() && (config.serial.empty() || config.serial==identity.serial)){
                        psx::hardware::SerialPort candidate=config.port;{std::lock_guard lock(mutex);candidate.com_name=view.com_name;}
                        discovered(identity,candidate);identified_at=GetTickCount64();operational=true;backoff=0;status("Connected — waiting for PSX",true);
                    }
                    return;
                }
                if(!operational)return;
                if(const auto* i=cpflight::input(record)){
                    if(i->kind==cpflight::InputKind::unavailable)return;
                    if(i->kind==cpflight::InputKind::state){const auto previous=states.find(std::string(i->group));const bool baseline=previous==states.end();for(const auto& state:cpflight::inputs())if(state.role==i->role && state.kind==cpflight::InputKind::state && state.group==i->group)sample(i->role,state.channel,state.channel==i->channel?1:0,baseline);states[std::string(i->group)]=i->channel;}
                    else {sample(i->role,i->channel,0,true);sample(i->role,i->channel,1,false);sample(i->role,i->channel,0,false);}return;
                }
                if(const auto value=cpflight::numeric(record)){
                    const unsigned c=value->channel;if(c==5 && (!vs_enabled || !vs_sync)){if(vs_enabled){vs_seen=true;vs_at=std::max(vs_at,GetTickCount64()+200);}return;}
                    if(!publish || !(role_mask&1u)){numeric_baseline[c].reset();return;}
                    if(numeric_baseline[c] && numeric_baseline[c]->mach==value->mach){int delta=value->value-numeric_baseline[c]->value;if(c==3){if(delta>180)delta-=360;if(delta<-180)delta+=360;}if(c==4 || c==5)delta/=100;if(delta && std::abs(delta)<=1000)sample(0,1000+c,delta,false,true);}numeric_baseline[c]=value;return;
                }
                {std::lock_guard lock(mutex);++view.unknown;}++owner.rejected;
            });}
            if(!identity.problem.empty()){error=identity.problem;failed=true;permanent=true;}
            if(!operational && identity.ready()){
                if(!config.serial.empty() && config.serial!=identity.serial){error="Identity changed — reselect port to bind replacement";failed=true;permanent=true;}
                else {psx::hardware::SerialPort candidate=config.port;{std::lock_guard lock(mutex);candidate.com_name=view.com_name;}discovered(identity,candidate);identified_at=GetTickCount64();operational=true;backoff=0;status("Connected — waiting for PSX",true);}
            }
            if(!operational && now-opened_at>=15000){error="CPflight identity timed out — retrying";failed=true;}
            if(failed){close();status(error.empty()?"Disconnected — retrying":error);retry_at=now+(1000ull<<std::min(backoff++,3u));continue;}
            if(!operational)continue;
            if(current_epoch!=publication_epoch || !publish || ((publication_roles&1u) && !(role_mask&1u))){
                bool released=true;for(const auto& [key,o]:sent){(void)key;if(o.intent.resource==unsigned(ResourceKind::boolean_output)){const auto command=cpflight::lamp(o.intent.channel,false);if(!command.empty() && !write(command)){released=false;break;}}}
                sent.clear();numeric_baseline={};publication_epoch=current_epoch;publication_roles=role_mask;
                if(vs_enabled){released=write("X1005") && released;vs_enabled=false;vs_sync=false;std::lock_guard lock(mutex);view.vs_ready=false;}
                if(!released){close();retry_at=now+1000;continue;}if(!publish){status("Connected — waiting for PSX",true);continue;}
            }
            publication_roles=role_mask;
            // Firmware 105 identifies before its startup selector stream ends.
            // Allow 6.5 s from open and 4 s after identity, plus receive quiet:
            // a delayed identity must not consume the selector settling phase.
            // This gates output only, never validity for an unseen input.
            if(!startup_settled){if(GetTickCount64()-opened_at<6500 || GetTickCount64()-identified_at<4000 || GetTickCount64()-last_receive_at<200){status("Synchronizing panel startup",true);continue;}startup_settled=true;}
            {std::lock_guard lock(mutex);if(output_seen!=output_revision){pending=outputs;output_seen=output_revision;}}
            bool io_ok=true,blanking=false;
            for(const auto& [key,o]:pending){(void)key;if(o.intent.resource==unsigned(ResourceKind::integral_display) && o.intent.channel!=5 && (!o.intent.available || std::all_of(o.intent.text.begin(),o.intent.text.end(),[](char c){return !c || c==' ';})))blanking=true;}
            for(const auto& [key,o]:pending){
                if(o.intent.session_epoch!=current_epoch || o.intent.device_generation!=snapshot_generation())continue;
                if(o.release){const auto previous=sent.find(key);if(previous!=sent.end()){
                    if(o.intent.resource==unsigned(ResourceKind::boolean_output))io_ok=write(cpflight::lamp(o.intent.channel,false));
                    else {numeric_baseline[o.intent.channel].reset();if(o.intent.channel==5 && vs_enabled){io_ok=write("X1005");vs_enabled=false;vs_sync=false;std::lock_guard lock(mutex);view.vs_ready=false;}}
                    sent.erase(previous);if(!io_ok)break;
                }continue;}
                const auto old=sent.find(key);if(old!=sent.end() && old->second.intent.sequence==o.intent.sequence)continue;
                const unsigned c=o.intent.channel;std::string command;
                if(o.intent.resource==unsigned(ResourceKind::boolean_output))command=cpflight::lamp(c,o.intent.available && o.intent.value!=0);
                else if(o.intent.resource==unsigned(ResourceKind::integral_display)){
                    const std::string text(o.intent.text.data(),std::find(o.intent.text.begin(),o.intent.text.end(),'\0')-o.intent.text.begin());command=o.intent.available?cpflight::display(c,text):std::string{};
                    if(c==5){
                        if(command.empty()){if(vs_enabled)io_ok=write("X1005");vs_enabled=false;vs_sync=false;{std::lock_guard lock(mutex);view.vs_ready=false;}sent[key]=o;continue;}
                        if(!vs_enabled){io_ok=write("X1105");vs_enabled=true;vs_sync=false;vs_seen=false;vs_at=GetTickCount64()+600;{std::lock_guard lock(mutex);view.vs_ready=false;}if(!io_ok)break;}
                        if(GetTickCount64()<vs_at || (!vs_first_done && !vs_seen)){if(GetTickCount64()>vs_at+1400){status("VS activation baseline unavailable",true);}continue;}
                    }
                    if(command.empty()){blanking=true;sent[key]=o;continue;}
                    if(c==2){const bool mach=command.find('.')!=command.npos;io_ok=write(cpflight::lamp(55,mach));if(!io_ok)break;}
                }
                if(!command.empty()){io_ok=write(command);if(!io_ok)break;if(o.intent.resource==unsigned(ResourceKind::integral_display)){numeric_baseline[c]=cpflight::numeric(std::string_view(command.data(),command.size()-1));if(c==5){vs_first_done=true;vs_sync=true;std::lock_guard lock(mutex);view.vs_ready=true;}}}sent[key]=o;
            }
            if(!io_ok){close();retry_at=now+1000;continue;}
            status(!role_mask?"Connected — roles unassigned or conflicting":vs_enabled && !vs_first_done && !vs_seen && GetTickCount64()>vs_at+1400?"VS activation baseline unavailable":blanking?"Connected — display blanking unavailable":vs_enabled && !vs_sync?"Synchronizing vertical speed":"Active",true);
        }
        close();
    }
    std::uint64_t snapshot_generation(){std::lock_guard lock(mutex);return view.generation;}
};
Cpflight::Cpflight(CpflightDriver d):impl_(std::make_unique<Impl>(*this,std::move(d))){}
Cpflight::~Cpflight(){stop();CloseHandle(available_event);}
void Cpflight::start(){std::lock_guard lock(impl_->mutex);if(impl_->worker.joinable())return;impl_->stopping=false;impl_->worker=std::thread([this]{impl_->run();});}
void Cpflight::stop(){{std::lock_guard lock(impl_->mutex);impl_->stopping=true;impl_->change.notify_all();}if(impl_->worker.joinable())impl_->worker.join();}
void Cpflight::configure(const CpflightConfiguration& c){std::lock_guard lock(impl_->mutex);const bool changed=c.enabled!=impl_->configuration.enabled || !same_endpoint(c.port,impl_->configuration.port) || (!impl_->configuration.serial.empty() && c.serial!=impl_->configuration.serial);impl_->configuration=c;if(changed)++impl_->revision;impl_->change.notify_all();}
void Cpflight::refresh(){std::lock_guard lock(impl_->mutex);impl_->refresh_requested=true;impl_->change.notify_all();}
void Cpflight::publication(bool ready,std::uint64_t epoch,unsigned roles){std::lock_guard lock(impl_->mutex);if(impl_->epoch!=epoch || impl_->ready!=ready || impl_->roles!=roles){impl_->outputs.clear();++impl_->output_revision;impl_->epoch=epoch;impl_->ready=ready;impl_->roles=roles;impl_->change.notify_all();}}
CpflightView Cpflight::snapshot() const{std::lock_guard lock(impl_->mutex);return impl_->view;}
std::vector<ModuleDevice> Cpflight::inventory() const{std::lock_guard lock(impl_->mutex);return impl_->inventory;}
std::vector<DeviceInfo> Cpflight::devices() const{std::lock_guard lock(impl_->mutex);std::vector<DeviceInfo> out;for(const auto& d:impl_->inventory)out.push_back(d.input);return out;}
bool Cpflight::latest(std::size_t slot,std::size_t control,Sample& s) const{if(slot<first_cpflight_device || slot>=max_devices)return false;std::lock_guard lock(impl_->mutex);const auto& bank=impl_->latest[slot-first_cpflight_device];if(control>=bank.size())return false;s=bank[control];return !s.invalid;}
bool Cpflight::submit(const ModuleOutput& o){
    std::lock_guard lock(impl_->mutex);const unsigned key=(unsigned(o.intent.resource)<<16)|o.intent.channel;
    if(o.release){
        if(o.slot<first_cpflight_device || o.slot>=max_devices)return false;
        // A retired route cannot withdraw a newer owner or block another provider.
        const auto previous=impl_->outputs.find(key);
        if(o.slot==first_cpflight_device && o.intent.device_generation==impl_->view.generation && previous!=impl_->outputs.end() && previous->second.intent.session_epoch==o.intent.session_epoch && previous->second.intent.sequence==o.intent.sequence){previous->second=o;++impl_->output_revision;impl_->change.notify_all();}
        return true;
    }
    if(o.slot!=first_cpflight_device || !impl_->view.connected || o.intent.device_generation!=impl_->view.generation)return false;
    if(!impl_->ready || !(impl_->roles&1u) || o.intent.session_epoch!=impl_->epoch)return false;
    if(o.intent.resource==unsigned(ResourceKind::boolean_output)){if(o.intent.channel<21 || o.intent.channel>34 || o.intent.channel==23)return false;}
    else if(o.intent.resource==unsigned(ResourceKind::integral_display)){if(o.intent.channel<2 || o.intent.channel>5)return false;}else return false;
    impl_->outputs[key]=o;++impl_->output_revision;impl_->change.notify_all();return true;
}
}
