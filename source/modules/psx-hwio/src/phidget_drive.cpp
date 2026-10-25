#include "hwio/software_motor.hpp"
#include "hwio/phidget_io.hpp"
#include <phidget22.h>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace hwio {
namespace {
template<class T>T load(const PhidgetSession& session,const char* name,bool required=true){const auto symbol=session.symbol(name,required);T fn{};static_assert(sizeof(fn)==sizeof(symbol));memcpy(&fn,&symbol,sizeof(fn));return fn;}
void check(PhidgetReturnCode rc,const char* operation){if(rc!=EPHIDGET_OK)throw MotorDriveFailure(int(rc),std::string(operation)+" / "+phidget_error_text(int(rc)));}
struct PhidgetDrive final:MotorDrive {
    std::shared_ptr<PhidgetSession> session;MotorDeviceAddress address;std::optional<PhidgetServer> endpoint;std::vector<PhidgetServer> servers;PhidgetLease claim,server;
    std::shared_ptr<PhidgetEncoderReader> shared_encoder;
    PhidgetHandle handle{};MotorFeedbackDefinition encoder_selection;
    MotorCapabilities capabilities;MotorParameters parameters;double fastest_ramp{},configured_ramp{},native_ceiling{1};bool driving{},configured{},qualified{},watchdog_active{};std::uint64_t watchdog_ms{};
    std::atomic<bool> detached{};std::atomic<int> error{};
    #define F(name) decltype(&name) name##_{};
    F(PhidgetDCMotor_create) F(PhidgetMotorVelocityController_create) F(Phidget_openWaitForAttachment) F(Phidget_close) F(Phidget_delete)
    F(Phidget_setDeviceSerialNumber) F(Phidget_setChannel) F(Phidget_setHubPort) F(Phidget_setIsHubPortDevice) F(Phidget_setIsLocal) F(Phidget_setIsRemote) F(Phidget_setServerName)
    F(Phidget_getDeviceID) F(Phidget_getDeviceVersion) F(Phidget_getChannelClass) F(Phidget_getDeviceSerialNumber) F(Phidget_getChannel) F(Phidget_getHubPort) F(Phidget_getAttached)
    F(Phidget_setOnDetachHandler) F(Phidget_setOnErrorHandler)
    #define BOTH(suffix) F(PhidgetDCMotor_##suffix) F(PhidgetMotorVelocityController_##suffix)
    BOTH(setAcceleration) BOTH(getAcceleration) BOTH(getMinAcceleration) BOTH(getMaxAcceleration)
    BOTH(setCurrentLimit) BOTH(getCurrentLimit) BOTH(getMinCurrentLimit) BOTH(getMaxCurrentLimit)
    BOTH(setDataInterval) BOTH(getMinDataInterval) BOTH(getMaxDataInterval)
    BOTH(enableFailsafe) BOTH(resetFailsafe) BOTH(getMinFailsafeTime) BOTH(getMaxFailsafeTime)
    BOTH(setTargetVelocity) BOTH(getVelocity)
    F(PhidgetDCMotor_setBrakingEnabled) F(PhidgetDCMotor_setTargetBrakingStrength)
    F(PhidgetMotorVelocityController_setEngaged) F(PhidgetMotorVelocityController_getEngaged)
    F(PhidgetMotorVelocityController_getMinTargetVelocity) F(PhidgetMotorVelocityController_getMaxTargetVelocity)
    F(PhidgetMotorVelocityController_setKp) F(PhidgetMotorVelocityController_getKp) F(PhidgetMotorVelocityController_setKi) F(PhidgetMotorVelocityController_getKi) F(PhidgetMotorVelocityController_setKd) F(PhidgetMotorVelocityController_getKd)
    F(PhidgetMotorVelocityController_setDeadBand) F(PhidgetMotorVelocityController_getDeadBand) F(PhidgetMotorVelocityController_setPositionType) F(PhidgetMotorVelocityController_getPositionType)
    #undef BOTH
    #undef F
    PhidgetDrive(std::shared_ptr<PhidgetSession> s,MotorDeviceAddress a,std::optional<PhidgetServer> e,std::vector<PhidgetServer> configured_servers):session(std::move(s)),address(std::move(a)),endpoint(std::move(e)),servers(std::move(configured_servers)){
        #define L(name) name##_=load<decltype(&name)>(*session,#name);
        L(Phidget_openWaitForAttachment) L(Phidget_close) L(Phidget_delete)
        L(Phidget_setDeviceSerialNumber) L(Phidget_setChannel) L(Phidget_setHubPort) L(Phidget_setIsHubPortDevice) L(Phidget_setIsLocal) L(Phidget_setIsRemote) L(Phidget_setServerName)
        L(Phidget_getDeviceID) L(Phidget_getDeviceVersion) L(Phidget_getChannelClass) L(Phidget_getDeviceSerialNumber) L(Phidget_getChannel) L(Phidget_getHubPort) L(Phidget_getAttached)
        L(Phidget_setOnDetachHandler) L(Phidget_setOnErrorHandler)
        #undef L
        #define O(name) name##_=load<decltype(&name)>(*session,#name,false);
        O(PhidgetDCMotor_create) O(PhidgetMotorVelocityController_create)
        #define BOTH(suffix) O(PhidgetDCMotor_##suffix) O(PhidgetMotorVelocityController_##suffix)
        BOTH(setAcceleration) BOTH(getAcceleration) BOTH(getMinAcceleration) BOTH(getMaxAcceleration)
        BOTH(setCurrentLimit) BOTH(getCurrentLimit) BOTH(getMinCurrentLimit) BOTH(getMaxCurrentLimit)
        BOTH(setDataInterval) BOTH(getMinDataInterval) BOTH(getMaxDataInterval)
        BOTH(enableFailsafe) BOTH(resetFailsafe) BOTH(getMinFailsafeTime) BOTH(getMaxFailsafeTime)
        BOTH(setTargetVelocity) BOTH(getVelocity)
        O(PhidgetDCMotor_setBrakingEnabled) O(PhidgetDCMotor_setTargetBrakingStrength)
        O(PhidgetMotorVelocityController_setEngaged) O(PhidgetMotorVelocityController_getEngaged)
        O(PhidgetMotorVelocityController_getMinTargetVelocity) O(PhidgetMotorVelocityController_getMaxTargetVelocity)
        O(PhidgetMotorVelocityController_setKp) O(PhidgetMotorVelocityController_getKp) O(PhidgetMotorVelocityController_setKi) O(PhidgetMotorVelocityController_getKi) O(PhidgetMotorVelocityController_setKd) O(PhidgetMotorVelocityController_getKd)
        O(PhidgetMotorVelocityController_setDeadBand) O(PhidgetMotorVelocityController_getDeadBand) O(PhidgetMotorVelocityController_setPositionType) O(PhidgetMotorVelocityController_getPositionType)
        #undef BOTH
        #undef O
    }
    ~PhidgetDrive(){close();}
    bool pwm()const noexcept{return address.mode==MotorDriveMode::pwm;}
    static void CCONV detach(PhidgetHandle,void* p){static_cast<PhidgetDrive*>(p)->detached=true;}
    static void CCONV sdk_error(PhidgetHandle,void* p,Phidget_ErrorEventCode rc,const char*){static_cast<PhidgetDrive*>(p)->error=int(rc);}
    void watchdog(bool force=false){
        const auto now=GetTickCount64();if(!watchdog_active || (!force && now-watchdog_ms<100))return;
        if(pwm()){if(!PhidgetDCMotor_resetFailsafe_)throw std::runtime_error("PWM watchdog unavailable");check(PhidgetDCMotor_resetFailsafe_(reinterpret_cast<PhidgetDCMotorHandle>(handle)),"reset PWM failsafe");}
        else {if(!PhidgetMotorVelocityController_resetFailsafe_)throw std::runtime_error("Velocity watchdog unavailable");check(PhidgetMotorVelocityController_resetFailsafe_(reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle)),"reset velocity failsafe");}watchdog_ms=now;
    }
    template<class P,class V,class... Args>PhidgetReturnCode call(P p,V v,Args... args){
        watchdog();
        if(pwm()){if(!p)throw std::runtime_error("Installed SDK lacks required DCMotor operation");return p(reinterpret_cast<PhidgetDCMotorHandle>(handle),args...);}
        if(!v)throw std::runtime_error("Installed SDK lacks required MotorVelocityController operation");return v(reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle),args...);
    }
    void address_handle(PhidgetHandle h,const MotorDeviceAddress& a,const PhidgetLease& registration){
        check(Phidget_setDeviceSerialNumber_(h,a.serial),"set exact serial");check(Phidget_setChannel_(h,a.channel),"set exact channel");
        if(a.hub_port>=0){check(Phidget_setHubPort_(h,a.hub_port),"set hub port");check(Phidget_setIsHubPortDevice_(h,0),"select VINT child");}
        check(Phidget_setIsLocal_(h,a.remote?0:1),"select local");check(Phidget_setIsRemote_(h,a.remote?1:0),"select remote");if(a.remote)check(Phidget_setServerName_(h,registration.server_name.c_str()),"select exact server");
    }
    MotorCapabilities open()override{
        close();claim=session->claim({address.serial,address.hub_port,address.channel},"HWIO software position motor");if(address.remote){if(!endpoint)throw std::runtime_error("Configure exact Phidget Network Server first");server=session->server(*endpoint);}
        if(pwm()){if(!PhidgetDCMotor_create_)throw std::runtime_error("SDK lacks DCMotor");check(PhidgetDCMotor_create_(reinterpret_cast<PhidgetDCMotorHandle*>(&handle)),"create DCMotor");}
        else {if(!PhidgetMotorVelocityController_create_)throw std::runtime_error("SDK lacks MotorVelocityController");check(PhidgetMotorVelocityController_create_(reinterpret_cast<PhidgetMotorVelocityControllerHandle*>(&handle)),"create MotorVelocityController");}
        address_handle(handle,address,server);detached=false;error=0;check(Phidget_setOnDetachHandler_(handle,detach,this),"install detach handler");check(Phidget_setOnErrorHandler_(handle,sdk_error,this),"install error handler");
        check(Phidget_openWaitForAttachment_(handle,500),"attach exact motor");Phidget_ChannelClass cls{};Phidget_DeviceID model{};int firmware{},channel{};
        check(Phidget_getChannelClass_(handle,&cls),"read channel class");check(Phidget_getDeviceID_(handle,&model),"read model");check(Phidget_getDeviceVersion_(handle,&firmware),"read firmware");check(Phidget_getChannel_(handle,&channel),"read channel");
        if(cls!=(pwm()?PHIDCHCLASS_DCMOTOR:PHIDCHCLASS_MOTORVELOCITYCONTROLLER) || (address.model && int(model)!=address.model) || channel!=address.channel)throw std::runtime_error("Attached motor model/channel class differs from exact assignment");
        int identity{};if(address.hub_port<0){check(Phidget_getDeviceSerialNumber_(handle,&identity),"read exact direct serial");if(identity!=address.serial)throw std::runtime_error("Attached motor serial differs from assignment");}
        else {check(Phidget_getHubPort_(handle,&identity),"read exact VINT port");if(identity!=address.hub_port)throw std::runtime_error("Attached motor hub port differs from assignment");}
        qualified=true;
        capabilities={};capabilities.model=int(model);capabilities.firmware=firmware;capabilities.software_position=capabilities.position_controller=capabilities.feed_forward=true;capabilities.pwm_drive=pwm();
        capabilities.minimum_position=-9e12;capabilities.maximum_position=9e12;capabilities.maximum_velocity=capabilities.maximum_acceleration=1e9;
        #define CALL(name,...) call(PhidgetDCMotor_##name##_,PhidgetMotorVelocityController_##name##_,__VA_ARGS__)
        check(CALL(getMinDataInterval,&capabilities.minimum_interval_ms),"minimum feedback interval");check(CALL(getMaxDataInterval,&capabilities.maximum_interval_ms),"maximum feedback interval");
        const auto low_failsafe=CALL(getMinFailsafeTime,&capabilities.minimum_failsafe_ms);
        const auto high_failsafe=CALL(getMaxFailsafeTime,&capabilities.maximum_failsafe_ms);
        capabilities.failsafe=low_failsafe==EPHIDGET_OK && high_failsafe==EPHIDGET_OK;
        if(!capabilities.failsafe && !(low_failsafe==EPHIDGET_UNSUPPORTED && high_failsafe==EPHIDGET_UNSUPPORTED)){
            check(low_failsafe,"minimum failsafe");check(high_failsafe,"maximum failsafe");
        }
        if(capabilities.failsafe && (!capabilities.minimum_failsafe_ms || capabilities.minimum_failsafe_ms>capabilities.maximum_failsafe_ms))throw std::runtime_error("Inconsistent native failsafe limits");
        capabilities.braking=pwm() && model==PHIDID_1065;capabilities.back_emf=capabilities.braking;
        const auto low_current=CALL(getMinCurrentLimit,&capabilities.minimum_current);
        const auto high_current=CALL(getMaxCurrentLimit,&capabilities.maximum_current);
        capabilities.current_limit=low_current==EPHIDGET_OK && high_current==EPHIDGET_OK;
        if(!capabilities.current_limit && !(low_current==EPHIDGET_UNSUPPORTED && high_current==EPHIDGET_UNSUPPORTED)){check(low_current,"minimum current limit");check(high_current,"maximum current limit");}
        check(CALL(getMaxAcceleration,&fastest_ramp),"maximum native ramp");release();return capabilities;
    }
    void renew()override{watchdog(true);}
    bool hardware_watchdog_active()const noexcept override{return watchdog_active;}
    template<class Set,class Get>void verified(Set set,Get get,double value,const char* label){if(!set || !get)throw std::runtime_error(std::string("Unsupported ")+label);const auto h=reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle);watchdog();check(set(h,value),label);double actual{};watchdog();check(get(h,&actual),label);if(std::abs(actual-value)>1e-6*std::max(1.,std::abs(value)))throw std::runtime_error(std::string(label)+" readback differs");}
    void configure(const MotorParameters& p)override{
        configured=false;release();double minimum_ramp{};check(CALL(getMinAcceleration,&minimum_ramp),"minimum native acceleration");
        const double ramp=pwm()?p.drive.pwm_ramp:p.drive.native_acceleration.value_or(0);
        if(!std::isfinite(ramp) || ramp<minimum_ramp || ramp>fastest_ramp)throw std::runtime_error("Native drive acceleration/ramp outside supported range");
        if(p.drive.maximum_duty<=0 || p.drive.maximum_duty>1)throw std::runtime_error("Maximum PWM duty must be greater than zero and at most one");
        if(capabilities.current_limit){if(!p.current_limit || *p.current_limit<capabilities.minimum_current || *p.current_limit>capabilities.maximum_current)throw std::runtime_error("Set supported motor current limit");check(CALL(setCurrentLimit,*p.current_limit),"set current limit");double actual{};check(CALL(getCurrentLimit,&actual),"read current limit");if(std::abs(actual-*p.current_limit)>1e-4)throw std::runtime_error("Current-limit readback differs");}
        check(CALL(setAcceleration,ramp),"set native ramp/acceleration");double actual{};check(CALL(getAcceleration,&actual),"read native acceleration");if(std::abs(actual-ramp)>1e-5*std::max(1.,ramp))throw std::runtime_error("Native acceleration readback differs");
        if(!pwm()){
            const auto h=reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle);double low{},high{};
            if(!PhidgetMotorVelocityController_getMaxTargetVelocity_ || !PhidgetMotorVelocityController_getMinTargetVelocity_)throw std::runtime_error("SDK lacks native velocity bounds");
            check(PhidgetMotorVelocityController_getMinTargetVelocity_(h,&low),"minimum native velocity");check(PhidgetMotorVelocityController_getMaxTargetVelocity_(h,&high),"maximum native velocity");native_ceiling=p.drive.native_velocity.value_or(0);
            if(!std::isfinite(native_ceiling) || native_ceiling<=0 || native_ceiling>std::min(high,-low))throw std::runtime_error("Configure supported native velocity ceiling");
            if(p.drive.speed_kp)verified(PhidgetMotorVelocityController_setKp_,PhidgetMotorVelocityController_getKp_,*p.drive.speed_kp,"controller speed Kp");
            if(p.drive.speed_ki)verified(PhidgetMotorVelocityController_setKi_,PhidgetMotorVelocityController_getKi_,*p.drive.speed_ki,"controller speed Ki");
            if(p.drive.speed_kd)verified(PhidgetMotorVelocityController_setKd_,PhidgetMotorVelocityController_getKd_,*p.drive.speed_kd,"controller speed Kd");
            if(p.drive.speed_deadband)verified(PhidgetMotorVelocityController_setDeadBand_,PhidgetMotorVelocityController_getDeadBand_,*p.drive.speed_deadband,"controller speed deadband");
            if(p.drive.speed_feedback){if(!PhidgetMotorVelocityController_setPositionType_ || !PhidgetMotorVelocityController_getPositionType_)throw std::runtime_error("Native feedback selection unsupported");const auto wanted=static_cast<Phidget_PositionType>(*p.drive.speed_feedback);check(PhidgetMotorVelocityController_setPositionType_(h,wanted),"set speed feedback source");Phidget_PositionType readback{};check(PhidgetMotorVelocityController_getPositionType_(h,&readback),"read speed feedback source");if(readback!=wanted)throw std::runtime_error("Speed feedback readback differs");}
        }
        check(CALL(setDataInterval,std::clamp(50u,capabilities.minimum_interval_ms,capabilities.maximum_interval_ms)),"set drive interval");
        if(capabilities.failsafe && (p.failsafe_ms<capabilities.minimum_failsafe_ms || p.failsafe_ms>capabilities.maximum_failsafe_ms))throw std::runtime_error("Failsafe timeout outside supported range");
        if(capabilities.failsafe)check(CALL(enableFailsafe,p.failsafe_ms),"enable motor failsafe");watchdog_active=capabilities.failsafe;watchdog_ms=GetTickCount64();parameters=p;configured_ramp=ramp;configured=true;renew();
    }
    void command(double normalized)override{
        if(!configured || detached || error)throw std::runtime_error("Motor drive unavailable; native event "+std::to_string(error.load()));
        if(!driving){check(CALL(setAcceleration,configured_ramp),"restore configured ramp");if(!pwm()){if(!PhidgetMotorVelocityController_setEngaged_)throw std::runtime_error("Velocity engagement unsupported");check(PhidgetMotorVelocityController_setEngaged_(reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle),1),"engage native velocity loop");}driving=true;}
        check(CALL(setTargetVelocity,std::clamp(normalized,-1.,1.)*(pwm()?parameters.drive.maximum_duty:native_ceiling)),pwm()?"set PWM duty":"set native velocity");
    }
    void release()override{
        driving=false;if(!handle || !qualified)return;
        if(pwm()){
            if(fastest_ramp>0)check(CALL(setAcceleration,fastest_ramp),"set fastest PWM release ramp");
            check(CALL(setTargetVelocity,0.),"request PWM zero");
            const auto h=reinterpret_cast<PhidgetDCMotorHandle>(handle);auto rc=PhidgetDCMotor_setBrakingEnabled_?PhidgetDCMotor_setBrakingEnabled_(h,0):EPHIDGET_UNSUPPORTED;
            if(rc==EPHIDGET_UNSUPPORTED && PhidgetDCMotor_setTargetBrakingStrength_)rc=PhidgetDCMotor_setTargetBrakingStrength_(h,0.);
            check(rc,"disable holding brake");
        }else {if(!PhidgetMotorVelocityController_setEngaged_)throw std::runtime_error("Velocity disengagement unsupported");check(PhidgetMotorVelocityController_setEngaged_(reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle),0),"disengage velocity controller");check(CALL(setTargetVelocity,0.),"clear native velocity target");}
    }
    MotorDriveObservation observation()override{
        int attached{};if(!handle || detached || Phidget_getAttached_(handle,&attached)!=EPHIDGET_OK || !attached)return {};
        if(error)throw MotorDriveFailure(error.load(),"Motor native error event");
        double reported{};const bool valid=CALL(getVelocity,&reported)==EPHIDGET_OK;bool released=false;
        if(pwm())released=valid && std::abs(reported)<1e-6;
        else {int engaged{};if(PhidgetMotorVelocityController_getEngaged_ && PhidgetMotorVelocityController_getEngaged_(reinterpret_cast<PhidgetMotorVelocityControllerHandle>(handle),&engaged)==EPHIDGET_OK)released=!engaged;}
        return {true,released,valid?std::optional{reported}:std::nullopt};
    }
    MotorPositionSample encoder(const MotorFeedbackDefinition& feedback)override{
        if(encoder_selection!=feedback){shared_encoder.reset();encoder_selection=feedback;}
        if(!shared_encoder){std::optional<PhidgetServer> selected;
            if(feedback.encoder.remote){const auto found=std::find_if(servers.begin(),servers.end(),[&](const auto& x){return x.host==feedback.encoder.server && x.port==feedback.encoder.port;});if(found!=servers.end())selected=*found;else if(endpoint && endpoint->host==feedback.encoder.server && endpoint->port==feedback.encoder.port)selected=endpoint;else throw std::runtime_error("Configure the selected encoder Network Server");}
            shared_encoder=PhidgetEncoderReader::acquire(session,feedback.encoder,selected);
        }
        const auto value=shared_encoder->snapshot();const bool valid=value.valid && value.count && *value.count>=-9007199254740991LL && *value.count<=9007199254740991LL;
        return {valid?double(*value.count)*(feedback.inverted?-1.:1.):0,value.generation,value.sequence,value.observed_ms,value.checked_ms,valid};
    }
    void close()noexcept override{
        if(handle){try{release();}catch(...){}Phidget_setOnDetachHandler_(handle,nullptr,nullptr);Phidget_setOnErrorHandler_(handle,nullptr,nullptr);Phidget_close_(handle);Phidget_delete_(&handle);}
        shared_encoder.reset();configured=driving=qualified=watchdog_active=false;claim={};server={};encoder_selection={};
    }
    #undef CALL
};
}
std::unique_ptr<MotorDrive> phidget_motor_drive(std::shared_ptr<PhidgetSession> session,MotorDeviceAddress a,std::optional<PhidgetServer> s,std::vector<PhidgetServer> servers){return std::make_unique<PhidgetDrive>(std::move(session),std::move(a),std::move(s),std::move(servers));}
}
