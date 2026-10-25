#include "hwio/phidget_io.hpp"
#include <phidget22.h>
#include <psx/diagnostics/journal.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>
#include <condition_variable>

namespace hwio {
namespace {
template<class F> F function(const PhidgetSession& s,const char* name){const auto p=s.symbol(name);F f{};static_assert(sizeof(f)==sizeof(p));memcpy(&f,&p,sizeof(f));return f;}
void checked(PhidgetReturnCode rc,const char* name){if(rc!=EPHIDGET_OK)throw std::runtime_error(std::string(name)+": "+phidget_error_text(int(rc)));}
using Create=PhidgetReturnCode(CCONV*)(PhidgetHandle*);
using GetDouble=PhidgetReturnCode(CCONV*)(PhidgetHandle,double*);
using GetInt=PhidgetReturnCode(CCONV*)(PhidgetHandle,int*);
using GetUInt=PhidgetReturnCode(CCONV*)(PhidgetHandle,uint32_t*);
using SetUInt=PhidgetReturnCode(CCONV*)(PhidgetHandle,uint32_t);
using SetDouble=PhidgetReturnCode(CCONV*)(PhidgetHandle,double);
void address_channel(const PhidgetSession& s,PhidgetHandle h,const MotorDeviceAddress& a,const PhidgetLease& server){
    checked(function<decltype(&Phidget_setDeviceSerialNumber)>(s,"Phidget_setDeviceSerialNumber")(h,a.serial),"exact serial");
    checked(function<decltype(&Phidget_setChannel)>(s,"Phidget_setChannel")(h,a.channel),"exact channel");
    if(a.hub_port>=0){checked(function<decltype(&Phidget_setHubPort)>(s,"Phidget_setHubPort")(h,a.hub_port),"hub port");checked(function<decltype(&Phidget_setIsHubPortDevice)>(s,"Phidget_setIsHubPortDevice")(h,0),"VINT child");}
    checked(function<decltype(&Phidget_setIsLocal)>(s,"Phidget_setIsLocal")(h,!a.remote),"local transport");
    checked(function<decltype(&Phidget_setIsRemote)>(s,"Phidget_setIsRemote")(h,a.remote),"remote transport");
    if(a.remote)checked(function<decltype(&Phidget_setServerName)>(s,"Phidget_setServerName")(h,server.server_name.c_str()),"exact network server");
}
void validate_channel(const PhidgetSession& s,PhidgetHandle h,const MotorDeviceAddress& a,int expected){
    int serial{},channel{};Phidget_ChannelClass cls{};Phidget_DeviceID model{};
    checked(function<decltype(&Phidget_getDeviceSerialNumber)>(s,"Phidget_getDeviceSerialNumber")(h,&serial),"read serial");
    checked(function<decltype(&Phidget_getChannel)>(s,"Phidget_getChannel")(h,&channel),"read channel");
    checked(function<decltype(&Phidget_getChannelClass)>(s,"Phidget_getChannelClass")(h,&cls),"read class");
    checked(function<decltype(&Phidget_getDeviceID)>(s,"Phidget_getDeviceID")(h,&model),"read model");
    if(serial!=a.serial || channel!=a.channel || int(cls)!=expected || (a.model && a.model!=int(model)))throw std::runtime_error("Phidget attachment differs from assigned physical identity");
    if(a.hub_port>=0){int port{};checked(function<decltype(&Phidget_getHubPort)>(s,"Phidget_getHubPort")(h,&port),"read hub port");if(port!=a.hub_port)throw std::runtime_error("Phidget hub port changed");}
}
}
int phidget_analogue_counts(double value,bool voltage)noexcept{
    const double ratio=voltage?value/5.:value;
    if(!std::isfinite(ratio) || ratio<0 || ratio>1)return -1;
    const double scaled=ratio*16383.,lower=std::floor(scaled),fraction=scaled-lower;
    return static_cast<int>(lower+(fraction>.5 || (fraction==.5 && std::fmod(lower,2.)!=0)));
}
struct PhidgetEncoderReader::Impl {
    std::shared_ptr<PhidgetSession> session;MotorDeviceAddress address;PhidgetLease server,claim;
    mutable std::mutex mutex;PhidgetReading value;std::thread worker;std::atomic<bool> stop{},lost{};std::atomic<std::uint64_t> events{},observed{};
    PhidgetEncoderHandle handle{};
    static void CCONV changed(PhidgetEncoderHandle,void* p,int,double,int){auto& x=*static_cast<Impl*>(p);++x.events;x.observed=GetTickCount64();}
    static void CCONV detached(PhidgetHandle,void* p){static_cast<Impl*>(p)->lost=true;}
    static void CCONV error(PhidgetHandle,void* p,Phidget_ErrorEventCode,const char*){static_cast<Impl*>(p)->lost=true;}
    void run(){
        try{claim=session->claim({address.serial,address.hub_port,address.channel,1008},"shared Phidget encoder");}
        catch(const std::exception& e){std::lock_guard lock(mutex);value.status=e.what();return;}
        while(!stop){
            lost=false;
            try{
                checked(function<decltype(&PhidgetEncoder_create)>(*session,"PhidgetEncoder_create")(&handle),"create encoder");
                const auto h=reinterpret_cast<PhidgetHandle>(handle);address_channel(*session,h,address,server);
                checked(function<decltype(&Phidget_setOnDetachHandler)>(*session,"Phidget_setOnDetachHandler")(h,detached,this),"encoder detach");
                checked(function<decltype(&Phidget_setOnErrorHandler)>(*session,"Phidget_setOnErrorHandler")(h,error,this),"encoder errors");
                checked(function<decltype(&PhidgetEncoder_setOnPositionChangeHandler)>(*session,"PhidgetEncoder_setOnPositionChangeHandler")(handle,changed,this),"encoder callback");
                checked(function<decltype(&Phidget_openWaitForAttachment)>(*session,"Phidget_openWaitForAttachment")(h,500),"attach encoder");validate_channel(*session,h,address,PHIDCHCLASS_ENCODER);
                unsigned minimum{},maximum{},effective{};
                checked(function<decltype(&PhidgetEncoder_getMinDataInterval)>(*session,"PhidgetEncoder_getMinDataInterval")(handle,&minimum),"minimum encoder interval");
                checked(function<decltype(&PhidgetEncoder_getMaxDataInterval)>(*session,"PhidgetEncoder_getMaxDataInterval")(handle,&maximum),"maximum encoder interval");
                checked(function<decltype(&PhidgetEncoder_setDataInterval)>(*session,"PhidgetEncoder_setDataInterval")(handle,minimum),"set encoder interval");
                checked(function<decltype(&PhidgetEncoder_getDataInterval)>(*session,"PhidgetEncoder_getDataInterval")(handle,&effective),"effective encoder interval");
                {std::lock_guard lock(mutex);value.minimum_interval_ms=minimum;value.maximum_interval_ms=maximum;value.interval_ms=effective;}
                const auto get=function<decltype(&PhidgetEncoder_getPosition)>(*session,"PhidgetEncoder_getPosition");
                while(!stop && !lost){
                    std::int64_t count{};const auto before=events.load();const auto rc=get(handle,&count);const auto after=events.load();
                    {std::lock_guard lock(mutex);const bool initial=!value.count;value.valid=rc==EPHIDGET_OK && !lost && before==after;value.count=value.valid?std::optional{count}:std::nullopt;value.checked_ms=GetTickCount64();value.observed_ms=initial?value.checked_ms:observed.load();value.sequence=after;value.status=value.valid?"Encoder counts":"Encoder unavailable";}
                    Sleep(5);
                }
                {std::lock_guard lock(mutex);value.valid=false;value.count.reset();value.status="Encoder detached; fresh baseline required";++value.generation;}
            }catch(const std::exception& e){std::lock_guard lock(mutex);value.valid=false;value.count.reset();value.status=e.what();++value.generation;}
            if(handle){const auto h=reinterpret_cast<PhidgetHandle>(handle);function<decltype(&Phidget_close)>(*session,"Phidget_close")(h);function<decltype(&Phidget_delete)>(*session,"Phidget_delete")(reinterpret_cast<PhidgetHandle*>(&handle));}
            // Retry inputs only. Motor owners still require their own explicit recovery.
            for(unsigned n=0;n<20 && !stop;++n)Sleep(5);
        }
        claim={};
    }
};
PhidgetEncoderReader::PhidgetEncoderReader(std::shared_ptr<PhidgetSession> s,MotorDeviceAddress a,std::optional<PhidgetServer> endpoint):impl_(std::make_unique<Impl>()){
    impl_->session=std::move(s);impl_->address=a;impl_->value.channel={a,PHIDCHCLASS_ENCODER,0,"Encoder"};impl_->value.unit="counts";impl_->value.generation=std::uint64_t(psx::diagnostics::qpc_now());
    if(a.remote){if(!endpoint)throw std::runtime_error("Encoder requires its configured Network Server");impl_->server=impl_->session->server(*endpoint);}
    impl_->worker=std::thread([this]{impl_->run();});
}
PhidgetEncoderReader::~PhidgetEncoderReader(){impl_->stop=true;if(impl_->worker.joinable())impl_->worker.join();}
std::shared_ptr<PhidgetEncoderReader> PhidgetEncoderReader::acquire(std::shared_ptr<PhidgetSession> s,MotorDeviceAddress a,std::optional<PhidgetServer> endpoint){
    struct Slot {std::weak_ptr<PhidgetEncoderReader> reader;std::condition_variable ready;bool live{};};
    static std::mutex mutex;static std::map<std::tuple<const PhidgetSession*,int,int,int>,std::shared_ptr<Slot>> readers;
    std::unique_lock lock(mutex);
    std::erase_if(readers,[](const auto& x){return !x.second->live && x.second->reader.expired();});
    const auto key=std::make_tuple(s.get(),a.serial,a.hub_port,a.channel);auto& stored=readers[key];if(!stored)stored=std::make_shared<Slot>();const auto slot=stored;
    for(;;){if(auto existing=slot->reader.lock())return existing;if(!slot->live)break;slot->ready.wait(lock);}
    slot->live=true;
    try{
        auto reader=std::shared_ptr<PhidgetEncoderReader>(new PhidgetEncoderReader(std::move(s),std::move(a),std::move(endpoint)),[slot](PhidgetEncoderReader* reader){
            delete reader;{std::lock_guard guard(mutex);slot->reader.reset();slot->live=false;}slot->ready.notify_all();
        });slot->reader=reader;return reader;
    }catch(...){slot->live=false;slot->ready.notify_all();throw;}
}
PhidgetReading PhidgetEncoderReader::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->value;}

struct PhidgetIo::Impl {
    PhidgetIo& owner;std::wstring library;std::shared_ptr<PhidgetSession> session;
    mutable std::mutex mutex;std::thread worker;bool stopping{},enabled{};std::atomic<bool> reconnect{},output_lost{};std::uint64_t revision{},applied{},generation{},sequence{};
    std::vector<PhidgetChannel> pending;HardwareConfiguration configuration;std::vector<std::string> excluded;
    std::vector<DeviceInfo> visible_metadata,metadata;std::vector<EthernetHealth> visible_inventory,inventory;std::vector<PhidgetReading> published;
    std::array<std::array<AtomicSample,32>,max_phidget_devices> latest;
    Spsc<EthernetOutput,1024> outputs;
    IdentificationPort identification;IdentificationLease identifying;
    std::vector<PhidgetChannel> active_source;
    HANDLE wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    struct Event {std::uint64_t token{};double value{};bool lost{};std::int64_t qpc{};};
    std::array<Event,4096> events{};std::size_t read{},write{};bool lost_events{};
    struct Channel {Impl* parent{};PhidgetReading reading;PhidgetHandle handle{};PhidgetLease server,claim;std::shared_ptr<PhidgetEncoderReader> encoder;std::optional<HardwareOutputIntent> command;std::uint64_t command_epoch{},command_sequence{};std::uint64_t token{},binding_id{};std::int64_t previous_count{};std::uint64_t encoder_sequence{};unsigned slot{},control{};bool input{},output{},baseline{},failure{},normal{},wizard_only{};std::atomic<bool> detached{};};
    std::vector<std::unique_ptr<Channel>> channels;
    explicit Impl(PhidgetIo& p,std::wstring path):owner(p),library(std::move(path)){}
    static void CCONV analog(PhidgetHandle,void* p,double v){auto& c=*static_cast<Channel*>(p);c.parent->enqueue(c.token,v,false);}
    static void CCONV digital(PhidgetHandle,void* p,int v){analog(nullptr,p,double(v));}
    static void CCONV attached(PhidgetHandle,void* p){auto& c=*static_cast<Channel*>(p);if(c.detached.exchange(false)){c.parent->reconnect=true;SetEvent(c.parent->wake);}}
    static void CCONV detached(PhidgetHandle,void* p){auto& c=*static_cast<Channel*>(p);c.detached=true;c.parent->enqueue(c.token,0,true);}
    static void CCONV error(PhidgetHandle,void* p,Phidget_ErrorEventCode,const char*){detached(nullptr,p);}
    void enqueue(std::uint64_t token,double value,bool lost)noexcept{
        std::lock_guard lock(mutex);if(write-read==events.size()){lost_events=true;++owner.overflow;}else events[write++%events.size()]={token,value,lost,psx::diagnostics::qpc_now()};SetEvent(wake);
    }
    void publish_sample(Channel& c,int value,bool invalid,bool relative=false,std::int64_t qpc=0){
        if(!c.input)return;Sample s;s.device=static_cast<std::uint16_t>(c.slot);s.control=static_cast<std::uint16_t>(c.control);s.generation=generation;s.sequence=++sequence;s.qpc=qpc?qpc:psx::diagnostics::qpc_now();s.value=value;s.invalid=invalid;s.relative=relative;s.baseline=!c.baseline;c.baseline=!invalid;
        latest[c.slot-first_generic_phidget_device][c.control].store(s);
        if(relative || c.reading.channel.channel_class==PHIDCHCLASS_DIGITALINPUT || invalid || s.baseline){if(!owner.edges.push(s))++owner.overflow;}
        ++owner.reports;SetEvent(owner.available_event);
    }
    void accept(Channel& c,double value,bool lost,std::int64_t qpc){
        auto& r=c.reading;r.sequence=++sequence;r.generation=generation;r.observed_ms=r.checked_ms=GetTickCount64();
        const auto cls=r.channel.channel_class;const bool analog_input=cls==PHIDCHCLASS_VOLTAGEINPUT || cls==PHIDCHCLASS_VOLTAGERATIOINPUT;
        const int count=analog_input && c.input?phidget_analogue_counts(value,cls==PHIDCHCLASS_VOLTAGEINPUT):0;
        r.valid=!lost && std::isfinite(value) && (cls!=PHIDCHCLASS_DIGITALINPUT || value==0 || value==1) && (!c.input || !analog_input || count>=0);r.value=r.valid?std::optional{value}:std::nullopt;r.status=r.valid?"Available":"Input unavailable; fresh baseline required";
        SetEvent(owner.available_event);if(c.input)publish_sample(c,analog_input?std::max(0,count):value!=0,!r.valid,false,qpc);
    }
    void close_channels(){
        restore_identification();
        for(auto& c:channels){if(c->input)publish_sample(*c,0,true);if(c->handle){if(c->output){try{checked(function<decltype(&PhidgetDigitalOutput_setState)>(*session,"PhidgetDigitalOutput_setState")(reinterpret_cast<PhidgetDigitalOutputHandle>(c->handle),0),"output Off on release");}catch(...) {}}
                function<decltype(&Phidget_close)>(*session,"Phidget_close")(c->handle);function<decltype(&Phidget_delete)>(*session,"Phidget_delete")(&c->handle);}}
        channels.clear();session.reset();{std::lock_guard lock(mutex);read=write=0;lost_events=false;}
    }
    void close_temporary_outputs(){
        for(auto i=channels.begin();i!=channels.end();)if((*i)->wizard_only){auto& c=**i;if(c.handle){function<decltype(&Phidget_close)>(*session,"Phidget_close")(c.handle);function<decltype(&Phidget_delete)>(*session,"Phidget_delete")(&c.handle);}i=channels.erase(i);}else ++i;
    }
    Channel* output_channel(std::string_view key,unsigned channel){for(auto& c:channels)if(c->output && c->reading.channel.address.device==key && unsigned(c->reading.channel.address.channel)==channel)return c.get();return nullptr;}
    void write_identification(){
        // Each SDK call completes on this worker. Publication never means that
        // a callback/queue alone established physical illumination.
        for(unsigned n=0;n<1025;++n){const auto change=identifying.next();if(!change)break;auto* c=output_channel(identifying.card().device,change->endpoint.channel);
            if(!c || !c->handle || c->detached){identifying.complete(change->ticket,false,"Phidget output channel unavailable");break;}
            const auto rc=function<decltype(&PhidgetDigitalOutput_setState)>(*session,"PhidgetDigitalOutput_setState")(reinterpret_cast<PhidgetDigitalOutputHandle>(c->handle),change->on);
            identifying.complete(change->ticket,rc==EPHIDGET_OK,rc==EPHIDGET_OK?"":"Phidget output write: "+phidget_error_text(int(rc)));
            if(rc!=EPHIDGET_OK)break;
            c->reading.value=double(change->on);c->reading.valid=true;c->reading.checked_ms=GetTickCount64();
        }
    }
    void restore_identification(){
        if(identifying.active()){identifying.release();try{write_identification();}catch(...){}identifying.lost("Phidget session retired before output restoration completed");}
        if(session)close_temporary_outputs();
    }
    void identification_step(){
        if(auto cmd=identification.take()){
            const auto& r=cmd->request;
            if(r.kind==IdentificationRequestKind::acquire){std::string error;std::vector<std::uint8_t> initial;initial.reserve(cmd->card.endpoints.size());
                const auto card=std::find_if(inventory.begin(),inventory.end(),[&](const auto& h){return h.key==cmd->card.device;});
                if(identifying.active())error="Another output-identification session owns this provider";
                else if(!session || card==inventory.end() || card->revision!=r.resource_generation || generation!=r.connection_generation || card->resources.size()!=cmd->card.endpoints.size())error="Phidget output inventory changed";
                else try{for(const auto& endpoint:cmd->card.endpoints){
                    if(endpoint.resource!=unsigned(ResourceKind::boolean_output) || endpoint.bit!=UINT16_MAX || std::none_of(card->resources.begin(),card->resources.end(),[&](const auto& resource){return resource.output && resource.kind==ResourceKind::boolean_output && resource.channel==endpoint.channel;}))throw std::runtime_error("Unknown Phidget Boolean output");
                    auto* c=output_channel(cmd->card.device,endpoint.channel);
                    if(!c){const auto descriptor=std::find_if(active_source.begin(),active_source.end(),[&](const auto& d){return d.address.device==cmd->card.device && d.channel_class==PHIDCHCLASS_DIGITALOUTPUT && d.address.channel==endpoint.channel && !d.hub_port_device;});if(descriptor==active_source.end())throw std::runtime_error("Phidget output descriptor unavailable");
                        auto extra=std::make_unique<Channel>();extra->parent=this;extra->reading.channel=*descriptor;extra->reading.generation=generation;extra->token=++sequence;extra->output=extra->wizard_only=true;extra->slot=first_generic_phidget_device+unsigned(card-inventory.begin());c=extra.get();channels.push_back(std::move(extra));
                        if(descriptor->address.remote)c->server=session->server({descriptor->address.server,{},descriptor->address.port});
                        c->claim=session->claim({descriptor->address.serial,descriptor->address.hub_port,descriptor->address.channel,1000+PHIDCHCLASS_DIGITALOUTPUT},"Phidget output identification");
                        checked(function<Create>(*session,"PhidgetDigitalOutput_create")(&c->handle),"create identification output");address_channel(*session,c->handle,descriptor->address,c->server);
                        checked(function<decltype(&Phidget_setOnDetachHandler)>(*session,"Phidget_setOnDetachHandler")(c->handle,detached,c),"identification detach");checked(function<decltype(&Phidget_setOnErrorHandler)>(*session,"Phidget_setOnErrorHandler")(c->handle,Impl::error,c),"identification errors");
                        checked(function<decltype(&Phidget_openWaitForAttachment)>(*session,"Phidget_openWaitForAttachment")(c->handle,500),"attach identification output");validate_channel(*session,c->handle,descriptor->address,PHIDCHCLASS_DIGITALOUTPUT);
                    }
                    if(!c->handle || c->detached || (!c->wizard_only && !c->reading.valid))throw std::runtime_error("Assigned Phidget output unavailable");initial.push_back(c->normal);
                }}catch(const std::exception& e){error=e.what();}
                if(!error.empty()){if(!identifying.active())close_temporary_outputs();initial.resize(cmd->card.endpoints.size());}
                identifying.acquire(identification,std::move(*cmd),initial,error);
            }else if(identifying.active())identifying.request(r);
            else identification.complete({r,r.kind==IdentificationRequestKind::release,true,"Phidget output-identification session no longer owns the card"});
        }
        if(identifying.active()){for(const auto& c:channels)if(c->output && c->reading.channel.address.device==identifying.card().device && c->detached){identifying.lost("Phidget output detached; restoration unconfirmed");break;}write_identification();}
        if(!identifying.active())close_temporary_outputs();
    }
    void build(const std::vector<PhidgetChannel>& source,const HardwareConfiguration& config,const std::vector<std::string>& reserved,bool on){
        close_channels();active_source=source;++generation;metadata.clear();inventory.clear();{std::lock_guard lock(mutex);published.clear();}
        if(!on){{std::lock_guard lock(mutex);visible_metadata=metadata;visible_inventory=inventory;}++owner.metadata_revision;return;}session=PhidgetSession::acquire(library);
        std::map<std::string,unsigned> slots;
        for(const auto& descriptor:source){
            {std::lock_guard lock(mutex);if(stopping || applied!=revision)break;}
            // An advertised alternative hub mode is not an attached input.
            // Selecting a mode requires explicit configuration, never discovery.
            if(descriptor.hub_port_device)continue;
            if(std::find(reserved.begin(),reserved.end(),descriptor.address.device)!=reserved.end())continue;
            const auto card=std::find_if(config.devices.begin(),config.devices.end(),[&](const auto& d){return d.key==descriptor.address.device;});if(card!=config.devices.end() && !card->enabled)continue;
            auto found=slots.find(descriptor.address.device);if(found==slots.end()){
                if(slots.size()>=max_phidget_devices)continue;const auto slot=static_cast<unsigned>(slots.size());found=slots.emplace(descriptor.address.device,slot).first;
                DeviceInfo d;d.slot=static_cast<std::uint16_t>(first_generic_phidget_device+slot);d.generation=generation;d.connected=true;d.coherent_reports=false;d.identity.backend=BackendKind::phidget22;d.identity.hardware_key=descriptor.address.device;d.identity.serial=std::to_string(descriptor.address.serial);d.identity.name="Phidget "+d.identity.serial;d.identity.vendor=0x06c2;d.status="Native Phidget22";metadata.push_back(d);
                EthernetHealth h;h.key=descriptor.address.device;h.name=d.identity.name;h.serial=static_cast<std::uint32_t>(descriptor.address.serial);h.connected=h.configured=true;h.firmware=std::to_string(descriptor.firmware);h.status="Native Phidget22 I/O";h.revision=generation;inventory.push_back(h);
            }
            const unsigned slot=found->second;const int cls=descriptor.channel_class;const auto index=descriptor.address.channel;
            const bool analog=cls==PHIDCHCLASS_VOLTAGEINPUT || cls==PHIDCHCLASS_VOLTAGERATIOINPUT;
            const bool sensor=analog && !(descriptor.address.model==PHIDID_1065 && cls==PHIDCHCLASS_VOLTAGEINPUT && index==2);
            bool voltage=false;for(const auto& p:config.phidget_ports)if(p.device==descriptor.address.device && p.channel==unsigned(index))voltage=p.voltage;
            if(sensor && (cls==PHIDCHCLASS_VOLTAGEINPUT)!=voltage)continue;
            if(cls!=PHIDCHCLASS_DIGITALINPUT && cls!=PHIDCHCLASS_DIGITALOUTPUT && cls!=PHIDCHCLASS_ENCODER && cls!=PHIDCHCLASS_CURRENTINPUT && !analog && cls!=PHIDCHCLASS_RCSERVO)continue;
            if(cls==PHIDCHCLASS_DIGITALOUTPUT){inventory[slot].resources.push_back({ResourceKind::boolean_output,static_cast<std::uint16_t>(index),"Digital output "+std::to_string(index),"Phidget channel",false,true,false,0,1});}
            auto c=std::make_unique<Channel>();c->parent=this;c->reading.channel=descriptor;c->reading.generation=generation;c->slot=static_cast<unsigned>(first_generic_phidget_device)+slot;c->token=++sequence;
            c->input=sensor || cls==PHIDCHCLASS_DIGITALINPUT || cls==PHIDCHCLASS_ENCODER;c->control=static_cast<unsigned>(metadata[slot].controls.size());
            c->reading.unit=cls==PHIDCHCLASS_CURRENTINPUT?"A":cls==PHIDCHCLASS_VOLTAGEINPUT?"V":cls==PHIDCHCLASS_VOLTAGERATIOINPUT?"ratio":cls==PHIDCHCLASS_ENCODER?"counts":"Boolean";
            if(c->input){if(c->control>=32)continue;ControlInfo control;control.key.backend=BackendKind::phidget22;control.key.kind=cls==PHIDCHCLASS_DIGITALINPUT?ControlKind::button:ControlKind::axis;control.key.channel_kind=static_cast<std::uint16_t>(cls);control.key.channel=static_cast<std::uint16_t>(index);control.minimum=cls==PHIDCHCLASS_ENCODER?INT32_MIN:0;control.maximum=sensor?16383:cls==PHIDCHCLASS_ENCODER?INT32_MAX:1;control.bits=sensor?14:cls==PHIDCHCLASS_DIGITALINPUT?1:64;control.relative=cls==PHIDCHCLASS_ENCODER;control.name=descriptor.name+" "+std::to_string(index)+(sensor?" (software counts 0-16383)":"");metadata[slot].controls.push_back(control);}
            if(cls==PHIDCHCLASS_RCSERVO){c->reading.status="Servo control deferred; controller profile is not physical feedback";c->reading.unit="servo descriptor";channels.push_back(std::move(c));continue;}
            if(cls==PHIDCHCLASS_DIGITALOUTPUT){const auto binding=std::find_if(config.outputs.begin(),config.outputs.end(),[&](const auto& b){return b.enabled && b.device==descriptor.address.device && b.channel==index && b.resource==ResourceKind::boolean_output;});if(binding==config.outputs.end())continue;c->output=true;c->binding_id=binding->id;c->normal=c->failure=binding->failure!=0;}
            try{
                std::optional<PhidgetServer> endpoint;if(descriptor.address.remote)endpoint=PhidgetServer{descriptor.address.server,{},descriptor.address.port};
                if(cls==PHIDCHCLASS_ENCODER)c->encoder=PhidgetEncoderReader::acquire(session,descriptor.address,endpoint);
                else {
                    if(endpoint)c->server=session->server(*endpoint);
                    c->claim=session->claim({descriptor.address.serial,descriptor.address.hub_port,index,1000+(sensor?29:cls)},"generic Phidget I/O");
                    const char* prefix=cls==PHIDCHCLASS_DIGITALINPUT?"PhidgetDigitalInput":cls==PHIDCHCLASS_DIGITALOUTPUT?"PhidgetDigitalOutput":cls==PHIDCHCLASS_CURRENTINPUT?"PhidgetCurrentInput":cls==PHIDCHCLASS_VOLTAGEINPUT?"PhidgetVoltageInput":"PhidgetVoltageRatioInput";
                    auto symbol=[&](const char* suffix){return std::string(prefix)+suffix;};checked(function<Create>(*session,symbol("_create").c_str())(&c->handle),"create I/O channel");address_channel(*session,c->handle,descriptor.address,c->server);
                    checked(function<decltype(&Phidget_setOnAttachHandler)>(*session,"Phidget_setOnAttachHandler")(c->handle,attached,c.get()),"I/O attach callback");checked(function<decltype(&Phidget_setOnDetachHandler)>(*session,"Phidget_setOnDetachHandler")(c->handle,detached,c.get()),"I/O detach callback");checked(function<decltype(&Phidget_setOnErrorHandler)>(*session,"Phidget_setOnErrorHandler")(c->handle,error,c.get()),"I/O errors");
                    if(cls==PHIDCHCLASS_DIGITALINPUT){using Callback=PhidgetReturnCode(CCONV*)(PhidgetHandle,void(CCONV*)(PhidgetHandle,void*,int),void*);checked(function<Callback>(*session,symbol("_setOnStateChangeHandler").c_str())(c->handle,digital,c.get()),"digital callback");}
                    else if(!c->output){using Callback=PhidgetReturnCode(CCONV*)(PhidgetHandle,void(CCONV*)(PhidgetHandle,void*,double),void*);const char* event=cls==PHIDCHCLASS_CURRENTINPUT?"_setOnCurrentChangeHandler":cls==PHIDCHCLASS_VOLTAGEINPUT?"_setOnVoltageChangeHandler":"_setOnVoltageRatioChangeHandler";checked(function<Callback>(*session,symbol(event).c_str())(c->handle,Impl::analog,c.get()),"analogue callback");}
                    checked(function<decltype(&Phidget_openWaitForAttachment)>(*session,"Phidget_openWaitForAttachment")(c->handle,500),"attach I/O channel");validate_channel(*session,c->handle,descriptor.address,cls);
                    if(!c->output && cls!=PHIDCHCLASS_DIGITALINPUT){unsigned interval{};checked(function<GetUInt>(*session,symbol("_getMinDataInterval").c_str())(c->handle,&interval),"minimum interval");c->reading.minimum_interval_ms=interval;checked(function<GetUInt>(*session,symbol("_getMaxDataInterval").c_str())(c->handle,&c->reading.maximum_interval_ms),"maximum interval");checked(function<SetUInt>(*session,symbol("_setDataInterval").c_str())(c->handle,interval),"set interval");checked(function<GetUInt>(*session,symbol("_getDataInterval").c_str())(c->handle,&c->reading.interval_ms),"effective interval");const char* trigger=cls==PHIDCHCLASS_CURRENTINPUT?"_setCurrentChangeTrigger":cls==PHIDCHCLASS_VOLTAGEINPUT?"_setVoltageChangeTrigger":"_setVoltageRatioChangeTrigger";checked(function<SetDouble>(*session,symbol(trigger).c_str())(c->handle,0.),"zero change trigger");}
                    if(cls==PHIDCHCLASS_DIGITALINPUT){int state{};const auto rc=function<GetInt>(*session,symbol("_getState").c_str())(c->handle,&state);accept(*c,double(state),rc!=EPHIDGET_OK,0);}
                    else if(c->output){{std::lock_guard lock(mutex);if(stopping || applied!=revision)throw std::runtime_error("Output ownership superseded during attachment");}checked(function<decltype(&PhidgetDigitalOutput_setState)>(*session,"PhidgetDigitalOutput_setState")(reinterpret_cast<PhidgetDigitalOutputHandle>(c->handle),c->failure),"initial output failure value");c->reading.value=double(c->failure);c->reading.valid=true;}
                    else {double value{};const char* getter=cls==PHIDCHCLASS_CURRENTINPUT?"_getCurrent":cls==PHIDCHCLASS_VOLTAGEINPUT?"_getVoltage":"_getVoltageRatio";const auto rc=function<GetDouble>(*session,symbol(getter).c_str())(c->handle,&value);accept(*c,value,rc!=EPHIDGET_OK,0);}
                }
            }catch(const std::exception& e){c->reading.status=e.what();c->reading.valid=false;if(c->output)inventory[slot].inhibited_outputs.emplace_back(c->binding_id,c->reading.status);publish_sample(*c,0,true);}
            channels.push_back(std::move(c));
        }
        {std::lock_guard lock(mutex);visible_metadata=metadata;visible_inventory=inventory;}++owner.metadata_revision;SetEvent(owner.available_event);
    }
    void run(){
        for(;;){std::vector<PhidgetChannel> source;HardwareConfiguration config;std::vector<std::string> reserved;bool rebuild{},on{};
            {std::lock_guard lock(mutex);if(stopping)break;if(reconnect.exchange(false))++revision;if(applied!=revision){applied=revision;source=pending;config=configuration;reserved=excluded;on=enabled;rebuild=true;}}
            if(rebuild){try{build(source,config,reserved,on);}catch(const std::exception& e){close_channels();metadata.clear();inventory.clear();{std::lock_guard lock(mutex);published.clear();}EthernetHealth h;h.status=e.what();inventory.push_back(h);{std::lock_guard lock(mutex);visible_metadata=metadata;visible_inventory=inventory;}++owner.metadata_revision;}}
            bool queue_lost{};{std::lock_guard lock(mutex);queue_lost=lost_events;if(queue_lost){lost_events=false;read=write;}}
            if(queue_lost)for(auto& c:channels)if(c->input){
                c->reading.valid=false;c->reading.value.reset();c->reading.count.reset();c->reading.status="Callback queue overflow; fresh baseline required";publish_sample(*c,0,true,c->reading.channel.channel_class==PHIDCHCLASS_ENCODER);
                // Quiet contacts still need a fresh getter baseline after a gap;
                // waiting for their next edge would consume that operator action.
                if(c->handle && c->reading.channel.channel_class==PHIDCHCLASS_DIGITALINPUT){int state{};const auto rc=function<GetInt>(*session,"PhidgetDigitalInput_getState")(c->handle,&state);accept(*c,double(state),rc!=EPHIDGET_OK,0);}
            }
            for(;;){Event event;bool have{};{std::lock_guard lock(mutex);if(read!=write){event=events[read++%events.size()];have=true;}}if(!have)break;for(auto& c:channels)if(c->token==event.token){accept(*c,event.value,event.lost,event.qpc);break;}}
            for(auto& c:channels)if(c->encoder){auto value=c->encoder->snapshot();const bool first=!c->baseline;const bool changed=value.sequence!=c->encoder_sequence;const auto old_generation=c->reading.generation;const bool continuous=first || old_generation==value.generation;const auto descriptor=c->reading.channel;c->reading=value;c->reading.channel=descriptor;if(!value.valid){publish_sample(*c,0,true,true);continue;}if(first || changed || !continuous){const auto count=*value.count;const bool positive=count>=c->previous_count;const auto magnitude=positive?std::uint64_t(count)-std::uint64_t(c->previous_count):std::uint64_t(c->previous_count)-std::uint64_t(count);const bool fits=magnitude<=std::uint64_t(INT32_MAX);const int delta=fits?(positive?static_cast<int>(magnitude):-static_cast<int>(magnitude)):0;c->previous_count=count;c->encoder_sequence=value.sequence;if(!continuous || !fits)publish_sample(*c,0,true,true);else publish_sample(*c,first?0:delta,false,true);}}
            const bool lost_output=output_lost.exchange(false);
            EthernetOutput output;
            while(outputs.pop(output))for(auto& c:channels)if(c->output && c->handle && c->slot==output.device && c->reading.channel.address.channel==output.intent.channel && output.intent.device_generation==generation){
                if(!lost_output && (output.intent.session_epoch>c->command_epoch || (output.intent.session_epoch==c->command_epoch && output.intent.sequence>=c->command_sequence))){c->command=output.intent;c->command_epoch=output.intent.session_epoch;c->command_sequence=output.intent.sequence;}
            }
            // Coalesce queued Boolean states before touching hardware. A lost queue
            // first applies failure values; Runtime advances its source epoch.
            for(auto& c:channels)if(c->output && c->handle && (c->command || lost_output)){
                const bool state=!lost_output && c->command->available?c->command->value!=0:c->failure;
                c->normal=state;if(identifying.active() && c->reading.channel.address.device==identifying.card().device && identifying.normal(unsigned(ResourceKind::boolean_output),static_cast<std::uint16_t>(c->reading.channel.address.channel),state)){c->command.reset();continue;}
                const auto rc=function<decltype(&PhidgetDigitalOutput_setState)>(*session,"PhidgetDigitalOutput_setState")(reinterpret_cast<PhidgetDigitalOutputHandle>(c->handle),state);
                c->reading.valid=rc==EPHIDGET_OK;c->reading.value=c->reading.valid?std::optional{double(state)}:std::nullopt;c->reading.status=c->reading.valid?"Output accepted by SDK":"Output write: "+phidget_error_text(int(rc));c->reading.checked_ms=GetTickCount64();c->command.reset();
            }
            identification_step();
            {std::lock_guard lock(mutex);published.resize(channels.size());for(std::size_t n=0;n<channels.size();++n)published[n]=channels[n]->reading;}
            const bool encoders=std::any_of(channels.begin(),channels.end(),[](const auto& c){return bool(c->encoder);});WaitForSingleObject(wake,encoders?5:100);
        }
        close_channels();
    }
};
PhidgetIo::PhidgetIo(std::wstring library):impl_(std::make_unique<Impl>(*this,std::move(library))){impl_->worker=std::thread([this]{impl_->run();});}
PhidgetIo::~PhidgetIo(){stop();CloseHandle(impl_->wake);CloseHandle(available_event);}
void PhidgetIo::configure(std::vector<PhidgetChannel> channels,HardwareConfiguration config,bool enabled,std::vector<std::string> excluded){std::lock_guard lock(impl_->mutex);impl_->pending=std::move(channels);impl_->configuration=std::move(config);impl_->enabled=enabled;impl_->excluded=std::move(excluded);++impl_->revision;SetEvent(impl_->wake);}
void PhidgetIo::stop(){{std::lock_guard lock(impl_->mutex);impl_->stopping=true;SetEvent(impl_->wake);}if(impl_->worker.joinable())impl_->worker.join();}
std::vector<DeviceInfo> PhidgetIo::devices()const{std::lock_guard lock(impl_->mutex);return impl_->visible_metadata;}
std::vector<EthernetHealth> PhidgetIo::health()const{std::lock_guard lock(impl_->mutex);return impl_->visible_inventory;}
std::vector<PhidgetReading> PhidgetIo::readings()const{std::lock_guard lock(impl_->mutex);return impl_->published;}
bool PhidgetIo::latest(std::size_t d,std::size_t c,Sample& value)const noexcept{
    if(d<first_generic_phidget_device || d>=first_generic_phidget_device+max_phidget_devices || c>=32)return false;
    value={};value.device=static_cast<std::uint16_t>(d);value.control=static_cast<std::uint16_t>(c);
    return impl_->latest[d-first_generic_phidget_device][c].read(value);
}
bool PhidgetIo::submit(const EthernetOutput& value)noexcept{const bool ok=impl_->outputs.push(value);if(!ok){++overflow;impl_->output_lost=true;}SetEvent(impl_->wake);return ok;}
bool PhidgetIo::identify_outputs(IdentificationRequest r,const IdentificationCard& c){const bool ok=impl_->identification.submit(r,c);if(ok)SetEvent(impl_->wake);return ok;}
std::optional<IdentificationCompletion> PhidgetIo::identification_completion(std::uint64_t session){return impl_->identification.poll(session);}
}
