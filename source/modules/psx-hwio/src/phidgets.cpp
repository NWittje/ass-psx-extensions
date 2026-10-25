#include <cmath>
#include "hwio/phidgets.hpp"
#include "hwio/software_motor.hpp"
#include "hwio/storage.hpp"
#include <phidget22.h>
#include <filesystem>
#include <map>
#include <array>
#include <algorithm>
#include <cctype>
namespace hwio {
bool phidget_motor_shaft_available(const PhidgetInventory& inventory,const MotorDeviceAddress& motor) noexcept {
    if(motor.provider!="phidget22" || !motor.assigned())return false;
    return std::any_of(inventory.encoders.begin(),inventory.encoders.end(),[&](const auto& entry){const auto& a=entry.address;
        return a.provider==motor.provider && a.device==motor.device && a.serial==motor.serial && a.hub_port==motor.hub_port && a.channel==motor.channel && a.model==motor.model && a.remote==motor.remote && (!a.remote || (a.server==motor.server && a.port==motor.port));});
}
struct PhidgetDiscovery::Impl {
    std::mutex mutex;PhidgetInventory published;std::thread worker;std::atomic<bool> stop{false};
    std::wstring library;std::shared_ptr<PhidgetSession> session;HMODULE dll{};PhidgetManagerHandle manager{};
    bool support_enabled{};std::vector<std::weak_ptr<PhidgetMotor>> motors;std::vector<std::weak_ptr<SoftwarePositionMotor>> software_motors;std::vector<std::weak_ptr<ServoBackend>> servos;
    std::vector<PhidgetServer> configured_servers;std::vector<PhidgetLease> server_leases;
    decltype(&PhidgetManager_create) create{};decltype(&PhidgetManager_delete) destroy{};
    decltype(&PhidgetManager_open) open{};decltype(&PhidgetManager_close) close{};
    decltype(&PhidgetManager_setOnAttachHandler) attach{};decltype(&PhidgetManager_setOnDetachHandler) detach{};
    decltype(&Phidget_retain) retain{};decltype(&Phidget_release) release{};
    decltype(&Phidget_getDeviceSerialNumber) serial{};decltype(&Phidget_getDeviceID) model{};
    decltype(&Phidget_getDeviceVersion) version{};decltype(&Phidget_getDeviceName) name{};
    decltype(&Phidget_getChannel) channel{};decltype(&Phidget_getChannelClass) channel_class{};
    decltype(&Phidget_getIsRemote) remote{};decltype(&Phidget_getParent) parent{};
    decltype(&Phidget_getHubPort) hub_port{};decltype(&Phidget_getServerName) server_name{};
    decltype(&Phidget_getIsHubPortDevice) hub_port_device{};
    struct Event{PhidgetHandle handle{};bool attached{};};
    std::array<Event,2048> queue{};std::size_t read{},write{};bool overflow{};
    HANDLE wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    using Key=std::tuple<std::string,int,int>;
    std::map<Key,PhidgetDevice> channels;std::map<Key,PhidgetChannel> native_channels;std::map<PhidgetHandle,Key> identities;std::map<Key,MotorDeviceEntry> position_channels;
    template<class T>void symbol(T& target,const char* key){const auto value=GetProcAddress(dll,key);if(!value)throw std::runtime_error(std::string("Missing Phidget22 export: ")+key);static_assert(sizeof(T)==sizeof(value));memcpy(&target,&value,sizeof(value));}
    static void CCONV attached(PhidgetManagerHandle,void* ctx,PhidgetHandle ch){static_cast<Impl*>(ctx)->enqueue(ch,true);}
    static void CCONV detached(PhidgetManagerHandle,void* ctx,PhidgetHandle ch){static_cast<Impl*>(ctx)->enqueue(ch,false);}
    void enqueue(PhidgetHandle ch,bool added) noexcept {
        std::lock_guard lock(mutex);if(write-read==queue.size()){overflow=true;SetEvent(wake);return;}
        if(retain(ch)==EPHIDGET_OK)queue[write++%queue.size()]={ch,added};else overflow=true;SetEvent(wake);
    }
    void publish(std::string status){std::vector<PhidgetDevice> devices;std::map<std::string,unsigned> roots;
        for(const auto& [key,c]:channels){auto [found,added]=roots.emplace(std::get<0>(key),static_cast<unsigned>(devices.size()));if(added){devices.push_back(c);devices.back().channels=1;}else ++devices[found->second].channels;}
        std::vector<MotorDeviceEntry> available,velocity,encoders;for(const auto& [key,value]:position_channels){if(std::get<1>(key)==int(PHIDCHCLASS_ENCODER))encoders.push_back(value);else if(value.address.mode==MotorDriveMode::position)available.push_back(value);else velocity.push_back(value);}
        std::vector<PhidgetChannel> native;for(const auto& [key,c]:native_channels){(void)key;native.push_back(c);}
        std::lock_guard lock(mutex);if(published.channels!=native || published.devices!=devices || published.position_motors!=available || published.velocity_motors!=velocity || published.encoders!=encoders || published.status!=status){published.channels=std::move(native);published.devices=std::move(devices);published.position_motors=std::move(available);published.velocity_motors=std::move(velocity);published.encoders=std::move(encoders);published.status=std::move(status);++published.revision;}}
    void run(){try{
        session=PhidgetSession::acquire(library);dll=session->module();
        symbol(create,"PhidgetManager_create");symbol(destroy,"PhidgetManager_delete");symbol(open,"PhidgetManager_open");symbol(close,"PhidgetManager_close");symbol(attach,"PhidgetManager_setOnAttachHandler");symbol(detach,"PhidgetManager_setOnDetachHandler");symbol(retain,"Phidget_retain");symbol(release,"Phidget_release");symbol(serial,"Phidget_getDeviceSerialNumber");symbol(model,"Phidget_getDeviceID");symbol(version,"Phidget_getDeviceVersion");symbol(name,"Phidget_getDeviceName");symbol(channel,"Phidget_getChannel");symbol(channel_class,"Phidget_getChannelClass");symbol(remote,"Phidget_getIsRemote");symbol(parent,"Phidget_getParent");
        if(auto proc=session->symbol("Phidget_getHubPort",false))memcpy(&hub_port,&proc,sizeof(proc));
        if(auto proc=session->symbol("Phidget_getServerName",false))memcpy(&server_name,&proc,sizeof(proc));
        symbol(hub_port_device,"Phidget_getIsHubPortDevice");
        {std::lock_guard lock(mutex);for(const auto& endpoint:configured_servers)server_leases.push_back(session->server(endpoint));}
        if(create(&manager) || attach(manager,attached,this) || detach(manager,detached,this) || open(manager))throw std::runtime_error("Phidget22 Manager could not start");
        publish("USB/VINT and configured network discovery; motors open only on assignment");
        while(!stop){Event event;bool ready{},lost{};{std::lock_guard lock(mutex);lost=overflow;overflow=false;if(read!=write){event=queue[read++%queue.size()];ready=true;}}
            if(lost)throw std::runtime_error("Phidget22 discovery queue overflow; disable and enable support to rediscover");
            if(!ready){WaitForSingleObject(wake,100);continue;}
            if(!event.attached){const auto previous=identities.find(event.handle);if(previous!=identities.end()){channels.erase(previous->second);native_channels.erase(previous->second);position_channels.erase(previous->second);identities.erase(previous);publish("USB/VINT and configured network discovery; motors open only on assignment");}release(&event.handle);continue;}
            int number{},index{},is_remote{},rev{},port_mode{};Phidget_DeviceID id{};Phidget_ChannelClass cls{};const char* label{};PhidgetHandle up{};
            const bool valid=!serial(event.handle,&number) && !channel(event.handle,&index) && !channel_class(event.handle,&cls) && !remote(event.handle,&is_remote) && !model(event.handle,&id) && !version(event.handle,&rev) && !name(event.handle,&label) && !hub_port_device(event.handle,&port_mode);
            // Channel parent is its device; a VINT device has a further hub parent.
            const auto parent_result=parent(event.handle,&up);PhidgetHandle hub{};const bool direct=parent_result==EPHIDGET_OK && up && parent(up,&hub)==EPHIDGET_OK && !hub;
            MotorDeviceAddress address;address.serial=number;address.channel=index;address.model=int(id);address.remote=is_remote!=0;
            bool addressed=direct;std::optional<PhidgetServer> endpoint;
            if(!direct && hub && hub_port){int root{},port{};if(!serial(hub,&root) && !hub_port(event.handle,&port) && root>0 && port>=0){address.serial=root;address.hub_port=port;addressed=true;}}
            if(is_remote){const char* server{};if(server_name && !server_name(event.handle,&server) && server)endpoint=session->server_address(server);addressed=addressed && endpoint.has_value();if(endpoint){address.server=endpoint->host;address.port=endpoint->port;}}
            if(hub)release(&hub);if(up)release(&up);
            if(valid && addressed){address.device="phidget:"+(address.remote?address.server+":"+std::to_string(address.port):"local")+":"+std::to_string(address.serial)+":"+std::to_string(address.hub_port);const auto key=std::make_tuple(address.device,int(cls),index);if(channels.size()>=2048 && !channels.count(key)){release(&event.handle);throw std::runtime_error("Phidget22 channel inventory bound exceeded");}channels[key]={address.serial,int(id),rev,0,label?label:"Phidget",address.device};native_channels[key]={address,int(cls),rev,label?label:"Phidget channel",port_mode!=0};identities[event.handle]=key;if(cls==PHIDCHCLASS_MOTORPOSITIONCONTROLLER || cls==PHIDCHCLASS_DCMOTOR || cls==PHIDCHCLASS_MOTORVELOCITYCONTROLLER || cls==PHIDCHCLASS_ENCODER){address.mode=cls==PHIDCHCLASS_DCMOTOR?MotorDriveMode::pwm:cls==PHIDCHCLASS_MOTORVELOCITYCONTROLLER?MotorDriveMode::velocity:MotorDriveMode::position;position_channels[key]={address,label?label:"Phidget channel",true};}publish("USB/VINT and configured network discovery; motors open only on assignment");}
            release(&event.handle);
        }
    }catch(const std::exception& e){channels.clear();native_channels.clear();position_channels.clear();identities.clear();publish(e.what());}
        if(manager){if(close)close(manager);if(destroy)destroy(&manager);}for(;;){Event event;{std::lock_guard lock(mutex);if(read==write)break;event=queue[read++%queue.size()];}if(release)release(&event.handle);}server_leases.clear();dll=nullptr;session.reset();
    }
    ~Impl(){stop=true;SetEvent(wake);if(worker.joinable())worker.join();CloseHandle(wake);}
};
PhidgetDiscovery::PhidgetDiscovery(std::wstring library):impl_(std::make_unique<Impl>()){impl_->library=std::move(library);}
PhidgetDiscovery::~PhidgetDiscovery(){enable(false);}
void PhidgetDiscovery::enable(bool enabled){
    {std::lock_guard lock(impl_->mutex);std::erase_if(impl_->servos,[](const auto& w){return w.expired();});for(const auto& w:impl_->servos)if(auto s=w.lock())s->provider_enabled(enabled);}
    {std::lock_guard lock(impl_->mutex);impl_->support_enabled=enabled;for(auto it=impl_->software_motors.begin();it!=impl_->software_motors.end();){if(auto motor=it->lock()){motor->provider_enabled(enabled);++it;}else it=impl_->software_motors.erase(it);}for(auto it=impl_->motors.begin();it!=impl_->motors.end();){if(auto motor=it->lock()){motor->provider_enabled(enabled);++it;}else it=impl_->motors.erase(it);}}
    if(enabled){if(!impl_->worker.joinable()){impl_->stop=false;impl_->worker=std::thread([this]{impl_->run();});}}else {impl_->stop=true;SetEvent(impl_->wake);if(impl_->worker.joinable())impl_->worker.join();impl_->channels.clear();impl_->native_channels.clear();impl_->position_channels.clear();impl_->identities.clear();impl_->publish("Disabled");}
}
std::shared_ptr<PhidgetMotor> PhidgetDiscovery::create_motor(MotorAddress address,MotorParameters parameters,std::shared_ptr<MotorClock> clock){
    auto motor=std::make_shared<PhidgetMotor>(PhidgetSession::acquire(impl_->library),std::move(address),std::move(parameters),std::move(clock));
    std::lock_guard lock(impl_->mutex);
    // Initial disengaged inspection may be retried while support is disabled.
    std::erase_if(impl_->motors,[](const auto& weak){return weak.expired();});
    motor->provider_enabled(impl_->support_enabled);impl_->motors.push_back(motor);return motor;
}
PhidgetInventory PhidgetDiscovery::inventory(){std::lock_guard lock(impl_->mutex);return impl_->published;}
std::shared_ptr<PositionMotorBackend> PhidgetDiscovery::stepper_backend(const StepperDefinition& d,std::function<MotorPositionSample(const MotorFeedbackDefinition&)> feedback){
    std::optional<PhidgetServer> endpoint;{std::lock_guard lock(impl_->mutex);if(!impl_->support_enabled)throw std::runtime_error("Phidgets support disabled");if(d.address.remote){const auto found=std::find_if(impl_->configured_servers.begin(),impl_->configured_servers.end(),[&](const auto& server){return server.host==d.address.server && server.port==d.address.port;});if(found==impl_->configured_servers.end())throw std::runtime_error("Configure exact Phidget Network Server");endpoint=*found;}}
    return phidget_stepper_backend(PhidgetSession::acquire(impl_->library),d,endpoint,std::move(feedback));
}
std::shared_ptr<ServoBackend> PhidgetDiscovery::servo_backend(const MotorDeviceAddress& a){
    std::optional<PhidgetServer> endpoint;bool enabled{};
    {std::lock_guard lock(impl_->mutex);enabled=impl_->support_enabled;if(a.remote){const auto found=std::find_if(impl_->configured_servers.begin(),impl_->configured_servers.end(),[&](const auto& s){return s.host==a.server && s.port==a.port;});if(found==impl_->configured_servers.end())throw std::runtime_error("Configure this Phidget Network Server before opening its servo");endpoint=*found;}}
    auto servo=std::make_shared<PhidgetServo>(PhidgetSession::acquire(impl_->library),a,endpoint);servo->provider_enabled(enabled);
    {std::lock_guard lock(impl_->mutex);std::erase_if(impl_->servos,[](const auto& weak){return weak.expired();});impl_->servos.push_back(servo);}return servo;
}
namespace {
struct PhidgetPositionBackend final:PositionMotorBackend {
    std::shared_ptr<PhidgetMotor> motor;explicit PhidgetPositionBackend(std::shared_ptr<PhidgetMotor> m):motor(std::move(m)){}
    MotorCommandResult inspect()override{return motor->inspect();}
    MotorCommandResult configure(MotorParameters p)override{return motor->update(std::move(p));}
    MotorCommandResult tuning(double p,double i,double d)override{return motor->update_tuning(p,i,d);}
    MotorCommandResult limits(double v,double a)override{return motor->update_limits(v,a);}
    MotorCommandResult target(double p)override{return motor->move_to(p);}
    MotorCommandResult reference(double zero,double min,double max)override{return motor->set_reference(zero,min,max);}
    MotorCommandResult engage()override{return motor->engage();}
    MotorCommandResult jog(double v)override{return motor->jog(v);}
    MotorCommandResult wave(MotorWaveform w,double a)override{return motor->start_wave(w,a);}
    MotorCommandResult renew()override{return motor->renew_jog();}
    MotorCommandResult stop()override{return motor->stop();}
    MotorCommandResult disengage()override{return motor->disengage();}
    MotorCommandResult close()override{return motor->close();}
    void guard(MotorGuardSnapshot g)override{motor->guard(g);}
    bool quiescent()const override{return motor->snapshot().state==MotorState::closed;}
    MotorSnapshot snapshot()const override{
        auto s=motor->snapshot();
        // The legacy SDK stores raw gains. The tuning page uses standardized
        // PID units; inspection converts without changing the device or SDK.
        // Vendor motorpositioncontroller.c; independently checked on DCC1002.
        auto normalize=[](MotorParameters& p){if(p.normalize_pid==false){const auto scale=std::abs(p.rescale);p.kp/=2097151.*scale;p.ki/=2097.151*scale;p.kd/=2097151000.*scale;p.normalize_pid=true;}};
        if(s.capabilities.normalize_pid){normalize(s.applied);normalize(s.requested);}return s;
    }
    std::optional<MotorCommandResult> result(std::uint64_t n)const override{return motor->result(n);}
};
}
std::shared_ptr<PositionMotorBackend> PhidgetDiscovery::position_backend(const MotorDeviceAddress& a){
    if(a.provider!="phidget22" || !a.assigned())throw std::runtime_error("Unsupported motor backend/address");
    // A VINT child inherits the hub serial. Its saved model must still match:
    // replacing a DCC with an STC on the same port is not the same motor.
    {std::lock_guard lock(impl_->mutex);if(a.model)for(const auto& channel:impl_->published.channels){const auto& found=channel.address;
        if(!channel.hub_port_device && found.serial==a.serial && found.hub_port==a.hub_port && found.remote==a.remote && (!a.remote || (found.server==a.server && found.port==a.port)) && found.model && found.model!=a.model)
            throw std::runtime_error("This port contains "+channel.name+", not the saved position controller");}}
    MotorAddress address;address.resource={a.serial,a.hub_port,a.channel};address.model=a.model;address.remote=a.remote;
    if(a.remote){std::lock_guard lock(impl_->mutex);const auto found=std::find_if(impl_->configured_servers.begin(),impl_->configured_servers.end(),[&](const auto& s){return s.host==a.server && s.port==a.port;});if(found==impl_->configured_servers.end())throw std::runtime_error("Configure this Phidget Network Server before opening its motor");address.server=*found;}
    return std::make_shared<PhidgetPositionBackend>(create_motor(std::move(address),{}));
}

std::shared_ptr<PositionMotorBackend> PhidgetDiscovery::software_backend(const MotorDeviceAddress& a,std::function<MotorPositionSample(const MotorFeedbackDefinition&)> sample){
    if(a.provider!="phidget22" || !a.assigned() || a.mode==MotorDriveMode::position)throw std::runtime_error("Unsupported software motor address/mode");
    std::optional<PhidgetServer> endpoint;std::vector<PhidgetServer> servers;bool enabled{};
    {std::lock_guard lock(impl_->mutex);enabled=impl_->support_enabled;servers=impl_->configured_servers;if(a.remote){const auto found=std::find_if(impl_->configured_servers.begin(),impl_->configured_servers.end(),[&](const auto& s){return s.host==a.server && s.port==a.port;});if(found==impl_->configured_servers.end())throw std::runtime_error("Configure this Phidget Network Server first");endpoint=*found;}}
    auto motor=std::make_shared<SoftwarePositionMotor>(phidget_motor_drive(PhidgetSession::acquire(impl_->library),a,endpoint,std::move(servers)),std::move(sample));motor->provider_enabled(enabled);
    {std::lock_guard lock(impl_->mutex);std::erase_if(impl_->software_motors,[](const auto& weak){return weak.expired();});impl_->software_motors.push_back(motor);}return motor;
}
MotorBench::ShaftReader PhidgetDiscovery::shaft_reader(const MotorDeviceAddress& a){
    if(a.provider!="phidget22" || !a.assigned())throw std::runtime_error("No qualified motor-shaft counter for this controller");
    std::optional<PhidgetServer> endpoint;
    {std::lock_guard lock(impl_->mutex);if(!impl_->support_enabled)throw std::runtime_error("Phidgets support disabled");
        if(!phidget_motor_shaft_available(impl_->published,a))throw std::runtime_error("Find backlash requires a motor-shaft counter; potentiometer-only movement and Auto tune remain available");
        if(a.remote){const auto found=std::find_if(impl_->configured_servers.begin(),impl_->configured_servers.end(),[&](const auto& s){return s.host==a.server && s.port==a.port;});if(found==impl_->configured_servers.end())throw std::runtime_error("Configure this Phidget Network Server first");endpoint=*found;}}
    auto reader=PhidgetEncoderReader::acquire(PhidgetSession::acquire(impl_->library),a,endpoint);
    return [reader=std::move(reader)]{const auto s=reader->snapshot();MotorPositionSample out;out.position=s.count?double(*s.count):0;out.generation=s.generation;out.sequence=s.sequence;out.observed_ms=s.observed_ms;out.checked_ms=s.checked_ms;out.valid=s.valid && s.count.has_value();return out;};
}
void PhidgetDiscovery::servers(const std::vector<PhidgetServer>& value){std::lock_guard lock(impl_->mutex);if(impl_->worker.joinable())throw std::runtime_error("Disable Phidgets before changing configured servers");impl_->configured_servers=value;for(auto& endpoint:impl_->configured_servers)for(auto& c:endpoint.host)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
}
