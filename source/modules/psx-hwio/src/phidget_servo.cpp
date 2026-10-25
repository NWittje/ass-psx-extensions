#include "hwio/servo.hpp"
#include <phidget22.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hwio {
struct PhidgetServo::Impl {
    std::shared_ptr<PhidgetSession> session;ServoAddress address;std::optional<PhidgetServer> server;
    PhidgetLease claim,network,supply_claim;PhidgetRCServoHandle channel{};int supply{};bool inspected{};
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    ServoSnapshot state,published;std::array<MotorCommandResult,64> results{};
    enum class Op {inspect,configure,target,engage,hold,release,close};
    struct Request {Op op{};std::uint64_t id{},epoch{};ServoParameters parameters;double target{};};
    std::deque<Request> queue;std::optional<Request> priority;std::uint64_t next{};
    std::atomic<std::uint64_t> epoch{1};std::atomic<bool> enabled{true},quit{},detached{},dead{};
    std::atomic<int> callback_error{};
    std::atomic<std::uint64_t> position_ms{},velocity_ms{};
    std::uint64_t reset_at{};bool failsafe_active{},unsafe{};ServoRelease rest;
    #define FN(name) decltype(&name) name##_{};
    FN(PhidgetRCServo_create) FN(Phidget_delete) FN(Phidget_close) FN(Phidget_openWaitForAttachment)
    FN(Phidget_setDeviceSerialNumber) FN(Phidget_setHubPort) FN(Phidget_setIsHubPortDevice) FN(Phidget_setChannel)
    FN(Phidget_setIsLocal) FN(Phidget_setIsRemote) FN(Phidget_setServerName)
    FN(Phidget_getDeviceID) FN(Phidget_getDeviceVersion) FN(Phidget_getChannelClass) FN(Phidget_getAttached)
    FN(Phidget_getParent) FN(Phidget_release) FN(Phidget_getDeviceSerialNumber) FN(Phidget_getHubPort) FN(Phidget_getIsRemote)
    FN(Phidget_setOnDetachHandler) FN(Phidget_setOnErrorHandler)
    FN(PhidgetRCServo_setOnPositionChangeHandler) FN(PhidgetRCServo_setOnVelocityChangeHandler)
    FN(PhidgetRCServo_setEngaged) FN(PhidgetRCServo_getEngaged)
    FN(PhidgetRCServo_getMinPulseWidthLimit) FN(PhidgetRCServo_getMaxPulseWidthLimit)
    FN(PhidgetRCServo_setMinPulseWidth) FN(PhidgetRCServo_getMinPulseWidth) FN(PhidgetRCServo_setMaxPulseWidth) FN(PhidgetRCServo_getMaxPulseWidth)
    FN(PhidgetRCServo_setMinPosition) FN(PhidgetRCServo_setMaxPosition) FN(PhidgetRCServo_getMinPosition) FN(PhidgetRCServo_getMaxPosition)
    FN(PhidgetRCServo_setTargetPosition) FN(PhidgetRCServo_getTargetPosition)
    FN(PhidgetRCServo_getPosition) FN(PhidgetRCServo_getVelocity) FN(PhidgetRCServo_getIsMoving)
    FN(PhidgetRCServo_setVelocityLimit) FN(PhidgetRCServo_getVelocityLimit) FN(PhidgetRCServo_getMinVelocityLimit) FN(PhidgetRCServo_getMaxVelocityLimit)
    FN(PhidgetRCServo_setAcceleration) FN(PhidgetRCServo_getAcceleration) FN(PhidgetRCServo_getMinAcceleration) FN(PhidgetRCServo_getMaxAcceleration)
    FN(PhidgetRCServo_setSpeedRampingState) FN(PhidgetRCServo_getSpeedRampingState)
    FN(PhidgetRCServo_setDataInterval) FN(PhidgetRCServo_getDataInterval) FN(PhidgetRCServo_getMinDataInterval) FN(PhidgetRCServo_getMaxDataInterval)
    FN(PhidgetRCServo_setTorque) FN(PhidgetRCServo_getTorque) FN(PhidgetRCServo_getMinTorque) FN(PhidgetRCServo_getMaxTorque)
    FN(PhidgetRCServo_setVoltage) FN(PhidgetRCServo_getVoltage)
    FN(PhidgetRCServo_getMinFailsafeTime) FN(PhidgetRCServo_getMaxFailsafeTime) FN(PhidgetRCServo_enableFailsafe) FN(PhidgetRCServo_resetFailsafe)
    #undef FN
    struct Failure {int code;std::string operation;bool cancelled{};};
    void check(PhidgetReturnCode code,const char* operation){
        if(code)throw Failure{int(code),operation};
        // Multi-property configuration may be slower than one worker tick.
        // Renew between successful SDK operations as well as during polling.
        const auto now=GetTickCount64();
        if(failsafe_active && channel && now-reset_at>=std::max(20u,state.applied.failsafe_ms/3)){
            const auto result=PhidgetRCServo_resetFailsafe_(channel);reset_at=now;
            if(result)throw Failure{int(result),"Renew servo failsafe"};
        }
    }
    void valid_epoch(const Request& r){if(r.epoch!=epoch || !enabled || detached)throw Failure{EPHIDGET_INTERRUPTED,"Cancelled servo command",true};}
    template<class T>void load(T& target,const char* name){auto p=session->symbol(name);static_assert(sizeof(p)==sizeof(target));memcpy(&target,&p,sizeof(p));}
    static void CCONV detach(PhidgetHandle,void* ctx){auto& s=*static_cast<Impl*>(ctx);s.detached=true;++s.epoch;s.wake.notify_one();}
    static void CCONV error(PhidgetHandle,void* ctx,Phidget_ErrorEventCode code,const char*){auto& s=*static_cast<Impl*>(ctx);s.callback_error=int(code);++s.epoch;s.wake.notify_one();}
    static void CCONV position(PhidgetRCServoHandle,void* ctx,double){static_cast<Impl*>(ctx)->position_ms=GetTickCount64();}
    static void CCONV velocity(PhidgetRCServoHandle,void* ctx,double){static_cast<Impl*>(ctx)->velocity_ms=GetTickCount64();}
    void load_api(){
        #define LOAD(name) load(name##_,#name);
        LOAD(PhidgetRCServo_create) LOAD(Phidget_delete) LOAD(Phidget_close) LOAD(Phidget_openWaitForAttachment)
        LOAD(Phidget_setDeviceSerialNumber) LOAD(Phidget_setHubPort) LOAD(Phidget_setIsHubPortDevice) LOAD(Phidget_setChannel)
        LOAD(Phidget_setIsLocal) LOAD(Phidget_setIsRemote) LOAD(Phidget_setServerName)
        LOAD(Phidget_getDeviceID) LOAD(Phidget_getDeviceVersion) LOAD(Phidget_getChannelClass) LOAD(Phidget_getAttached)
        LOAD(Phidget_getParent) LOAD(Phidget_release) LOAD(Phidget_getDeviceSerialNumber) LOAD(Phidget_getHubPort) LOAD(Phidget_getIsRemote)
        LOAD(Phidget_setOnDetachHandler) LOAD(Phidget_setOnErrorHandler)
        LOAD(PhidgetRCServo_setOnPositionChangeHandler) LOAD(PhidgetRCServo_setOnVelocityChangeHandler)
        LOAD(PhidgetRCServo_setEngaged) LOAD(PhidgetRCServo_getEngaged)
        LOAD(PhidgetRCServo_getMinPulseWidthLimit) LOAD(PhidgetRCServo_getMaxPulseWidthLimit)
        LOAD(PhidgetRCServo_setMinPulseWidth) LOAD(PhidgetRCServo_getMinPulseWidth) LOAD(PhidgetRCServo_setMaxPulseWidth) LOAD(PhidgetRCServo_getMaxPulseWidth)
        LOAD(PhidgetRCServo_setMinPosition) LOAD(PhidgetRCServo_setMaxPosition) LOAD(PhidgetRCServo_getMinPosition) LOAD(PhidgetRCServo_getMaxPosition)
        LOAD(PhidgetRCServo_setTargetPosition) LOAD(PhidgetRCServo_getTargetPosition)
        LOAD(PhidgetRCServo_getPosition) LOAD(PhidgetRCServo_getVelocity) LOAD(PhidgetRCServo_getIsMoving)
        LOAD(PhidgetRCServo_setVelocityLimit) LOAD(PhidgetRCServo_getVelocityLimit) LOAD(PhidgetRCServo_getMinVelocityLimit) LOAD(PhidgetRCServo_getMaxVelocityLimit)
        LOAD(PhidgetRCServo_setAcceleration) LOAD(PhidgetRCServo_getAcceleration) LOAD(PhidgetRCServo_getMinAcceleration) LOAD(PhidgetRCServo_getMaxAcceleration)
        LOAD(PhidgetRCServo_setSpeedRampingState) LOAD(PhidgetRCServo_getSpeedRampingState)
        LOAD(PhidgetRCServo_setDataInterval) LOAD(PhidgetRCServo_getDataInterval) LOAD(PhidgetRCServo_getMinDataInterval) LOAD(PhidgetRCServo_getMaxDataInterval)
        LOAD(PhidgetRCServo_setTorque) LOAD(PhidgetRCServo_getTorque) LOAD(PhidgetRCServo_getMinTorque) LOAD(PhidgetRCServo_getMaxTorque)
        LOAD(PhidgetRCServo_setVoltage) LOAD(PhidgetRCServo_getVoltage)
        LOAD(PhidgetRCServo_getMinFailsafeTime) LOAD(PhidgetRCServo_getMaxFailsafeTime) LOAD(PhidgetRCServo_enableFailsafe) LOAD(PhidgetRCServo_resetFailsafe)
        #undef LOAD
    }
    void release_drive(){rest.reset();state.released_at_target=false;if(!channel)return;state.drive=MotorDriveState::unknown;check(PhidgetRCServo_setEngaged_(channel,0),"Disengage servo");int on{};check(PhidgetRCServo_getEngaged_(channel,&on),"Read disengagement");if(on)throw Failure{EPHIDGET_UNEXPECTED,"Disengagement readback mismatch"};state.drive=MotorDriveState::off;}
    void cleanup(){
        if(!channel){supply_claim={};claim={};network={};inspected=false;return;}
        // An absent device cannot confirm release, but must still be closed so
        // reattachment gets a new baseline instead of retaining a stale handle.
        try{release_drive();}catch(const Failure& f){state.drive=MotorDriveState::unknown;state.fault=f.operation+": "+phidget_error_text(f.code);state.fault_code=f.code;}
        check(Phidget_setOnDetachHandler_(reinterpret_cast<PhidgetHandle>(channel),nullptr,nullptr),"Remove detach callback");
        check(Phidget_setOnErrorHandler_(reinterpret_cast<PhidgetHandle>(channel),nullptr,nullptr),"Remove error callback");
        if(state.capabilities.profile)check(PhidgetRCServo_setOnPositionChangeHandler_(channel,nullptr,nullptr),"Remove profile callback");
        if(state.capabilities.velocity)check(PhidgetRCServo_setOnVelocityChangeHandler_(channel,nullptr,nullptr),"Remove velocity callback");
        failsafe_active=false;check(Phidget_close_(reinterpret_cast<PhidgetHandle>(channel)),"Close servo");
        auto raw=reinterpret_cast<PhidgetHandle>(channel);check(Phidget_delete_(&raw),"Delete servo");channel=nullptr;
        state.attached=false;state.configured=false;state.accepted.reset();state.desired.reset();failsafe_active=false;
        supply_claim={};claim={};network={};inspected=false;
    }
    bool optional(PhidgetReturnCode code,const char* operation){if(code==EPHIDGET_UNSUPPORTED)return false;check(code,operation);return true;}
    void open(){
        if(channel){if(state.attached && inspected && !detached)return;cleanup();}
        if(!enabled)throw Failure{EPHIDGET_CLOSED,"Phidgets support disabled"};
        claim=session->claim({address.serial,address.hub_port,address.channel},"Servo indicator");
        if(address.remote){if(!server)throw Failure{EPHIDGET_INVALIDARG,"Missing exact network server"};network=session->server(*server);}
        check(PhidgetRCServo_create_(&channel),"Create RCServo");auto raw=reinterpret_cast<PhidgetHandle>(channel);
        check(Phidget_setDeviceSerialNumber_(raw,address.serial),"Select serial");check(Phidget_setChannel_(raw,address.channel),"Select channel");
        check(Phidget_setIsRemote_(raw,address.remote),"Select remote");check(Phidget_setIsLocal_(raw,!address.remote),"Select local");
        if(address.hub_port>=0){check(Phidget_setHubPort_(raw,address.hub_port),"Select VINT port");check(Phidget_setIsHubPortDevice_(raw,0),"Select VINT device");}
        if(address.remote)check(Phidget_setServerName_(raw,network.server_name.c_str()),"Select server");
        check(Phidget_setOnDetachHandler_(raw,detach,this),"Attach detach callback");check(Phidget_setOnErrorHandler_(raw,error,this),"Attach error callback");
        check(Phidget_openWaitForAttachment_(raw,1000),"Open RCServo");
        PhidgetHandle parent{},hub{};int number{},port{},remote{};bool topology{};
        try{
            check(Phidget_getParent_(raw,&parent),"Read servo parent");if(!parent)throw Failure{EPHIDGET_WRONGDEVICE,"Servo device parent unavailable"};
            check(Phidget_getParent_(parent,&hub),"Read servo root");check(Phidget_getIsRemote_(raw,&remote),"Read servo transport");
            if(hub){check(Phidget_getDeviceSerialNumber_(hub,&number),"Read hub identity");check(Phidget_getHubPort_(raw,&port),"Read actual VINT port");topology=address.hub_port>=0 && port==address.hub_port && number==address.serial;}
            else{check(Phidget_getDeviceSerialNumber_(raw,&number),"Read direct identity");topology=address.hub_port<0 && number==address.serial;}
        }catch(...){if(hub)Phidget_release_(&hub);if(parent)Phidget_release_(&parent);throw;}
        if(hub)Phidget_release_(&hub);if(parent)Phidget_release_(&parent);
        if(!topology || bool(remote)!=address.remote)throw Failure{EPHIDGET_WRONGDEVICE,"Attached servo topology differs from exact address"};
        Phidget_DeviceID id{};Phidget_ChannelClass cls{};int firmware{};
        check(Phidget_getDeviceID_(raw,&id),"Read model");check(Phidget_getDeviceVersion_(raw,&firmware),"Read firmware");check(Phidget_getChannelClass_(raw,&cls),"Read channel class");
        if(cls!=PHIDCHCLASS_RCSERVO || (address.model && address.model!=int(id)) || firmware<address.minimum_firmware)throw Failure{EPHIDGET_WRONGDEVICE,"Servo identity/firmware mismatch"};
        state={};state.attached=true;state.generation=++session_generation;detached=false;auto& c=state.capabilities;c.model=int(id);c.firmware=firmware;
        release_drive();
        check(PhidgetRCServo_getMinPulseWidthLimit_(channel,&c.pulse_min),"Read minimum pulse bound");check(PhidgetRCServo_getMaxPulseWidthLimit_(channel,&c.pulse_max),"Read maximum pulse bound");
        check(PhidgetRCServo_getMinPulseWidth_(channel,&state.applied.pulse_min),"Read minimum pulse");check(PhidgetRCServo_getMaxPulseWidth_(channel,&state.applied.pulse_max),"Read maximum pulse");
        c.velocity=optional(PhidgetRCServo_getMinVelocityLimit_(channel,&c.velocity_min),"Read velocity support");if(c.velocity)check(PhidgetRCServo_getMaxVelocityLimit_(channel,&c.velocity_max),"Read velocity bound");
        c.acceleration=optional(PhidgetRCServo_getMinAcceleration_(channel,&c.acceleration_min),"Read acceleration support");if(c.acceleration)check(PhidgetRCServo_getMaxAcceleration_(channel,&c.acceleration_max),"Read acceleration bound");
        double position_min{},position_max{};check(PhidgetRCServo_getMinPosition_(channel,&position_min),"Read current minimum coordinate");check(PhidgetRCServo_getMaxPosition_(channel,&position_max),"Read current maximum coordinate");
        if(!std::isfinite(position_min) || !std::isfinite(position_max) || position_min==position_max)throw Failure{EPHIDGET_UNKNOWNVAL,"Invalid current servo unit mapping"};
        const auto units=std::abs((state.applied.pulse_max-state.applied.pulse_min)/(position_max-position_min));
        c.velocity_min*=units;c.velocity_max*=units;c.acceleration_min*=units;c.acceleration_max*=units;
        c.interval=optional(PhidgetRCServo_getMinDataInterval_(channel,&c.interval_min),"Read interval support");if(c.interval)check(PhidgetRCServo_getMaxDataInterval_(channel,&c.interval_max),"Read interval bound");
        c.failsafe=optional(PhidgetRCServo_getMinFailsafeTime_(channel,&c.failsafe_min),"Read failsafe support");if(c.failsafe)check(PhidgetRCServo_getMaxFailsafeTime_(channel,&c.failsafe_max),"Read failsafe bound");
        c.torque=optional(PhidgetRCServo_getMinTorque_(channel,&c.torque_min),"Read torque support");if(c.torque)check(PhidgetRCServo_getMaxTorque_(channel,&c.torque_max),"Read torque bound");
        PhidgetRCServo_Voltage voltage_value{};c.voltage=optional(PhidgetRCServo_getVoltage_(channel,&voltage_value),"Read voltage support");
        if(c.voltage){supply=int(voltage_value);supply_claim=session->servo_supply({address.serial,address.hub_port,address.channel},supply);}
        int ramp{};c.ramping=optional(PhidgetRCServo_getSpeedRampingState_(channel,&ramp),"Read ramp support");
        c.profile=optional(PhidgetRCServo_setOnPositionChangeHandler_(channel,position,this),"Attach profile callback");
        if(c.velocity)check(PhidgetRCServo_setOnVelocityChangeHandler_(channel,velocity,this),"Attach velocity callback");
        c.startup_pulse_possible=c.model==PHIDID_1061 && firmware<302;
        inspected=true;
        // Configuration and a selected target still require explicit commands.
    }
    std::uint64_t session_generation{};
    void configure(const Request& r){
        open();release_drive();state.configured=false;state.accepted.reset();state.desired.reset();state.requested=r.parameters;const auto& p=r.parameters;const auto& c=state.capabilities;
        auto range=[](double v,double lo,double hi){return std::isfinite(v) && v>=lo && v<=hi;};
        if(!range(p.pulse_min,c.pulse_min,c.pulse_max) || !range(p.pulse_max,c.pulse_min,c.pulse_max) || p.pulse_min>=p.pulse_max)throw Failure{EPHIDGET_INVALIDARG,"Invalid native pulse calibration"};
        if((p.velocity && !c.velocity) || (p.acceleration && !c.acceleration) || (p.torque && (!c.torque || !range(*p.torque,c.torque_min,c.torque_max))) || (p.voltage && !c.voltage) || (p.failsafe_ms && (!c.failsafe || p.failsafe_ms<c.failsafe_min || p.failsafe_ms>c.failsafe_max)) || (c.interval && (p.interval_ms<c.interval_min || p.interval_ms>c.interval_max)))throw Failure{EPHIDGET_INVALIDARG,"Unsupported servo option or value outside device capability"};
        // Program pulse mapping while disengaged. Coordinates are exactly us.
        check(PhidgetRCServo_setMinPulseWidth_(channel,c.pulse_min),"Expand disengaged pulse mapping");check(PhidgetRCServo_setMaxPulseWidth_(channel,p.pulse_max),"Set maximum pulse");check(PhidgetRCServo_setMinPulseWidth_(channel,p.pulse_min),"Set minimum pulse");
        check(PhidgetRCServo_setMinPosition_(channel,c.pulse_min),"Expand disengaged coordinates");check(PhidgetRCServo_setMaxPosition_(channel,p.pulse_max),"Set maximum coordinate");check(PhidgetRCServo_setMinPosition_(channel,p.pulse_min),"Set minimum coordinate");
        // These bounds are expressed in the newly selected us coordinates.
        auto& bounds=state.capabilities;
        if(c.velocity){check(PhidgetRCServo_getMinVelocityLimit_(channel,&bounds.velocity_min),"Read scaled velocity minimum");check(PhidgetRCServo_getMaxVelocityLimit_(channel,&bounds.velocity_max),"Read scaled velocity maximum");}
        if(c.acceleration){check(PhidgetRCServo_getMinAcceleration_(channel,&bounds.acceleration_min),"Read scaled acceleration minimum");check(PhidgetRCServo_getMaxAcceleration_(channel,&bounds.acceleration_max),"Read scaled acceleration maximum");}
        if((p.velocity && !range(*p.velocity,bounds.velocity_min,bounds.velocity_max)) || (p.acceleration && !range(*p.acceleration,bounds.acceleration_min,bounds.acceleration_max)))throw Failure{EPHIDGET_INVALIDARG,"Motion limits outside microsecond-coordinate capabilities"};
        auto verify=[&](auto setter,auto getter,double value,const char* operation){valid_epoch(r);check(setter(channel,value),operation);double actual{};check(getter(channel,&actual),operation);if(std::abs(actual-value)>std::max(1e-6,std::abs(value)*1e-5))throw Failure{EPHIDGET_UNEXPECTED,std::string(operation)+" readback mismatch"};};
        verify(PhidgetRCServo_setMinPulseWidth_,PhidgetRCServo_getMinPulseWidth_,p.pulse_min,"Verify minimum pulse");verify(PhidgetRCServo_setMaxPulseWidth_,PhidgetRCServo_getMaxPulseWidth_,p.pulse_max,"Verify maximum pulse");
        // Seed unspecified motion properties in pulse-us units, independent of
        // an earlier application's settings. Preserve explicitly imported limits.
        auto applied=p;
        if(c.velocity){applied.velocity=p.velocity.value_or(std::clamp((p.pulse_max-p.pulse_min)/2,bounds.velocity_min,bounds.velocity_max));verify(PhidgetRCServo_setVelocityLimit_,PhidgetRCServo_getVelocityLimit_,*applied.velocity,"Set velocity");}
        if(c.acceleration){applied.acceleration=p.acceleration.value_or(std::clamp((p.pulse_max-p.pulse_min)*2,bounds.acceleration_min,bounds.acceleration_max));verify(PhidgetRCServo_setAcceleration_,PhidgetRCServo_getAcceleration_,*applied.acceleration,"Set acceleration");}
        if(p.torque)verify(PhidgetRCServo_setTorque_,PhidgetRCServo_getTorque_,*p.torque,"Set torque");
        if(c.ramping){check(PhidgetRCServo_setSpeedRampingState_(channel,p.ramping),"Set ramping");int actual{};check(PhidgetRCServo_getSpeedRampingState_(channel,&actual),"Read ramping");if(actual!=int(p.ramping))throw Failure{EPHIDGET_UNEXPECTED,"Ramping readback mismatch"};}
        if(p.voltage){const auto v=static_cast<PhidgetRCServo_Voltage>(*p.voltage);session->servo_supply({address.serial,address.hub_port,address.channel},*p.voltage,[&]{check(PhidgetRCServo_setVoltage_(channel,v),"Set board supply");PhidgetRCServo_Voltage actual{};check(PhidgetRCServo_getVoltage_(channel,&actual),"Read board supply");if(v!=actual)throw Failure{EPHIDGET_UNEXPECTED,"Board supply readback mismatch"};});supply=*p.voltage;}
        auto effective_interval=p.interval_ms;
        if(c.interval){
            check(PhidgetRCServo_setDataInterval_(channel,p.interval_ms),"Set reporting interval");
            check(PhidgetRCServo_getDataInterval_(channel,&effective_interval),"Read effective reporting interval");
            // The SDK quantizes reporting intervals to the controller's timebase.
            // Retain the requested value separately; the readback is the applied rate.
            if(!effective_interval || effective_interval<c.interval_min || effective_interval>c.interval_max)
                throw Failure{EPHIDGET_UNEXPECTED,"Invalid reporting interval: requested "+std::to_string(p.interval_ms)+" ms, effective "+std::to_string(effective_interval)+" ms, supported "+std::to_string(c.interval_min)+".."+std::to_string(c.interval_max)+" ms"};
        }
        if(p.failsafe_ms){check(PhidgetRCServo_enableFailsafe_(channel,p.failsafe_ms),"Enable failsafe");failsafe_active=true;reset_at=GetTickCount64();}
        valid_epoch(r);state.applied=applied;state.applied.interval_ms=effective_interval;state.configured=true;++state.revision;state.fault.clear();state.fault_code=0;
    }
    void poll(){if(!channel || !state.attached)return;int attached{};check(Phidget_getAttached_(reinterpret_cast<PhidgetHandle>(channel),&attached),"Read attachment");if(!attached)throw Failure{EPHIDGET_NOTATTACHED,"Servo detached"};
        if(inspected && state.capabilities.voltage){PhidgetRCServo_Voltage voltage{};check(PhidgetRCServo_getVoltage_(channel,&voltage),"Read shared board supply");if(int(voltage)!=supply)throw Failure{EPHIDGET_UNEXPECTED,"Servo board supply changed outside this channel"};}
        if(!state.configured){state.profile_position.reset();state.profile_velocity.reset();state.moving.reset();state.checked_ms=GetTickCount64();return;}
        // Keep the last completed command coordinate while pulses are absent.
        // Its event timestamp remains unchanged; this is not a new shaft sample.
        if(state.released_at_target){state.checked_ms=GetTickCount64();return;}
        double value{};int flag{};
        if(!PhidgetRCServo_getPosition_(channel,&value) && std::isfinite(value))state.profile_position=value;else state.profile_position.reset();
        if(!PhidgetRCServo_getVelocity_(channel,&value) && std::isfinite(value))state.profile_velocity=value;else state.profile_velocity.reset();
        if(!PhidgetRCServo_getIsMoving_(channel,&flag))state.moving=flag!=0;else state.moving.reset();
        state.position_event_ms=position_ms;state.velocity_event_ms=velocity_ms;state.checked_ms=GetTickCount64();
        if(rest.due(state,state.checked_ms)){
            bool idle;{std::lock_guard lock(mutex);idle=queue.empty() && !priority;}
            if(idle){release_drive();state.released_at_target=true;}
        }
    }
    void execute(const Request& r){
        if(r.op==Op::release){release_drive();return;}if(r.op==Op::close){cleanup();return;}
        valid_epoch(r);if(r.op==Op::inspect){open();return;}if(r.op==Op::configure){configure(r);return;}
        if(!channel || !state.configured || !state.attached)throw Failure{EPHIDGET_NOTCONFIGURED,"Servo configuration required"};
        if(r.op==Op::engage){if(!state.accepted)throw Failure{EPHIDGET_NOTCONFIGURED,"Select an initial servo target before engagement"};rest.reset();state.released_at_target=false;check(PhidgetRCServo_setEngaged_(channel,1),"Engage servo");int on{};check(PhidgetRCServo_getEngaged_(channel,&on),"Read engagement");if(!on)throw Failure{EPHIDGET_UNEXPECTED,"Engagement readback mismatch"};state.drive=MotorDriveState::on;valid_epoch(r);return;}
        double target=r.target;
        if(r.op==Op::hold){poll();if(!state.profile_position){release_drive();throw Failure{EPHIDGET_UNKNOWNVAL,"Profile position unavailable; disengagement requested"};}target=*state.profile_position;}
        if(!std::isfinite(target) || target<state.applied.pulse_min || target>state.applied.pulse_max)throw Failure{EPHIDGET_INVALIDARG,"Target outside native configured pulse range"};
        rest.reset();state.released_at_target=false;state.desired=target;check(PhidgetRCServo_setTargetPosition_(channel,target),"Set servo target");double accepted{};check(PhidgetRCServo_getTargetPosition_(channel,&accepted),"Read accepted target");if(!std::isfinite(accepted) || accepted<state.applied.pulse_min || accepted>state.applied.pulse_max)throw Failure{EPHIDGET_UNEXPECTED,"Target readback mismatch"};
        state.accepted=accepted;state.accepted_id=r.id;state.accepted_ms=GetTickCount64();valid_epoch(r);
    }
    void finish(MotorCommandResult result){state.last_result=result;std::lock_guard lock(mutex);results[result.id%results.size()]=std::move(result);published=state;}
    void fault(const Failure& f){state.fault=f.operation+": "+phidget_error_text(f.code);state.fault_code=f.code;state.configured=false;state.accepted.reset();++epoch;
        try{release_drive();}catch(...){state.drive=MotorDriveState::unknown;}
        if(detached || f.code==EPHIDGET_NOTATTACHED || f.code==EPHIDGET_NODEV)state.attached=false;
        if(!state.attached){try{cleanup();}catch(...){unsafe=true;}state.fault=f.operation+": "+phidget_error_text(f.code);state.fault_code=f.code;}
    }
    void run()noexcept{
        try{load_api();while(!quit){
            Request r;bool have{};{std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(50),[&]{return quit || priority || !queue.empty() || detached || callback_error;});if(priority){r=*priority;priority.reset();have=true;}else if(!queue.empty()){r=queue.front();queue.pop_front();have=true;}}
            if(detached || callback_error){const auto code=callback_error.exchange(0);fault({code?code:EPHIDGET_NOTATTACHED,code?"Servo SDK event":"Servo detached"});detached=false;}
            if(have){try{execute(r);finish({r.id,MotorResultStage::sdk_succeeded});}catch(const Failure& f){if(!f.cancelled)fault(f);finish({r.id,f.cancelled?MotorResultStage::cancelled:MotorResultStage::rejected,f.cancelled?MotorError::cancelled:MotorError::sdk_error,f.code,f.operation+": "+phidget_error_text(f.code)});}catch(const std::exception& e){state.fault=e.what();finish({r.id,MotorResultStage::rejected,MotorError::busy,0,e.what()});}}
            try{poll();}catch(const Failure& f){fault(f);}std::lock_guard lock(mutex);published=state;
        }}catch(const std::exception& e){state.fault=e.what();state.configured=false;std::lock_guard lock(mutex);published=state;dead=true;for(const auto& r:queue)results[r.id%results.size()]={r.id,MotorResultStage::rejected,MotorError::sdk_error,0,e.what()};queue.clear();}
        try{cleanup();}catch(const Failure& f){fault(f);unsafe=true;}catch(...){unsafe=true;}
        std::lock_guard lock(mutex);published=state;
    }
    MotorCommandResult submit(Op op,ServoParameters p={},double target=0){std::lock_guard lock(mutex);Request r{op,++next,epoch,std::move(p),target};
        if(quit || dead)return {r.id,MotorResultStage::rejected,MotorError::not_ready,0,published.fault.empty()?"Servo worker stopped":published.fault};
        if(op==Op::release && priority && priority->op==Op::close)return {priority->id,MotorResultStage::queued};
        if(op==Op::release || op==Op::close){r.epoch=++epoch;for(const auto& old:queue)results[old.id%results.size()]={old.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Cancelled by release"};queue.clear();if(priority)results[priority->id%results.size()]={priority->id,MotorResultStage::cancelled,MotorError::cancelled};priority=r;}
        else{if(op==Op::configure){r.epoch=++epoch;for(const auto& old:queue)results[old.id%results.size()]={old.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Configuration cancels earlier commands"};queue.clear();}if(op==Op::target){for(auto it=queue.begin();it!=queue.end();)if(it->op==Op::target){results[it->id%results.size()]={it->id,MotorResultStage::cancelled,MotorError::cancelled,0,"Coalesced target"};it=queue.erase(it);}else ++it;}if(queue.size()>=32)return {r.id,MotorResultStage::rejected,MotorError::queue_full,0,"Servo command queue full"};queue.push_back(r);}
        wake.notify_one();return {r.id,MotorResultStage::queued};
    }
};
PhidgetServo::PhidgetServo(std::shared_ptr<PhidgetSession> s,ServoAddress a,std::optional<PhidgetServer> server):impl_(std::make_unique<Impl>()){
    if(!s || a.provider!="phidget22" || !a.assigned() || a.serial<=0 || a.channel>65535 || a.hub_port<-1 || a.hub_port>255 || a.model<0 || a.minimum_firmware<0 || (a.remote && (!server || a.server!=server->host || a.port!=server->port)))throw std::runtime_error("Exact RCServo address and matching server required");
    impl_->session=std::move(s);impl_->address=std::move(a);impl_->server=std::move(server);impl_->worker=std::thread([this]{impl_->run();});
}
PhidgetServo::~PhidgetServo(){impl_->quit=true;++impl_->epoch;impl_->wake.notify_one();if(impl_->worker.joinable())impl_->worker.join();if(impl_->unsafe)(void)impl_.release(); /* Retain uncertain SDK/callback lifetime rather than unload executing code. */}
MotorCommandResult PhidgetServo::inspect(){return impl_->submit(Impl::Op::inspect);}
MotorCommandResult PhidgetServo::configure(ServoParameters p){return impl_->submit(Impl::Op::configure,std::move(p));}
MotorCommandResult PhidgetServo::target(double p){return impl_->submit(Impl::Op::target,{},p);}
MotorCommandResult PhidgetServo::engage(){return impl_->submit(Impl::Op::engage);}
MotorCommandResult PhidgetServo::hold(){return impl_->submit(Impl::Op::hold);}
MotorCommandResult PhidgetServo::disengage(){return impl_->submit(Impl::Op::release);}
MotorCommandResult PhidgetServo::close(){return impl_->submit(Impl::Op::close);}
ServoSnapshot PhidgetServo::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->published;}
std::optional<MotorCommandResult> PhidgetServo::result(std::uint64_t n)const{std::lock_guard lock(impl_->mutex);if(n && impl_->results[n%impl_->results.size()].id==n)return impl_->results[n%impl_->results.size()];return {};}
void PhidgetServo::provider_enabled(bool on){if(impl_->enabled.exchange(on)!=on && !on)(void)close();}
}
