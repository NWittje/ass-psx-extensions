#include "hwio/fdstq.hpp"
#include "hwio/fds_trace.hpp"
#include "hwio/fdstq_motion.hpp"
#include "hwio/motor_motion.hpp"
#include "hwio/phidget_session.hpp"
#include <phidget22.h>
#include <thread>
#include <mutex>
#include <filesystem>
#include <cmath>
#include <algorithm>
namespace hwio {
std::optional<int> fdstq_counts(double ratio) noexcept {if(!std::isfinite(ratio) || ratio<0 || ratio>1)return {};return static_cast<int>(std::nearbyint(ratio*16383.));}
DeviceInfo fdstq_device(int serial,std::uint64_t generation){
    DeviceInfo d;d.slot=static_cast<std::uint16_t>(first_phidget_device);d.generation=generation;d.coherent_reports=false;
    d.identity.backend=BackendKind::phidget22;d.identity.serial=std::to_string(serial);d.identity.hardware_key="fdstq-phidget22:"+d.identity.serial;d.identity.name="FDS FDSTQ / InterfaceKit "+d.identity.serial;
    static constexpr const char* labels[]{"Parking brake","Fuel cutoff left","Fuel cutoff right","TO/GA","A/T disconnect","Horn cutout","Stabilizer trim main electric cutout","Stabilizer trim autopilot cutout","Throttle left","Throttle right","Speedbrake","Flaps"};
    for(unsigned n=0;n<12;++n){ControlInfo c;c.key.backend=BackendKind::phidget22;c.key.kind=n<8?ControlKind::button:ControlKind::axis;c.key.channel_kind=static_cast<std::uint16_t>(n<8?ResourceKind::boolean_input:ResourceKind::analogue_input);c.key.channel=static_cast<std::uint16_t>(n<8?n:n-7);c.minimum=0;c.maximum=n<8?1:16383;c.bits=n<8?1:14;c.name=labels[n];d.controls.push_back(c);}return d;
}
struct FdstqInterface::Impl {
    FdstqInterface& owner;std::wstring library;std::thread worker;std::atomic<bool> stop{false};std::mutex mutex;
    FdstqConfiguration configuration;bool enabled{},test{},test_ready{};fds::Trace* trace{};std::uint64_t test_started{},generation{},sequence{};std::array<bool,5> desired{};
    DeviceInfo published;std::string message{"FDSTQ unassigned"};std::array<AtomicSample,12> latest;FdstqMotion intent;std::uint64_t intent_at{},motion_epoch{};std::atomic<unsigned> moving_mask{},motor_mask{};
    HMODULE dll{};std::shared_ptr<PhidgetSession> session;PhidgetLease network;std::array<PhidgetLease,5> motor_claims;HANDLE wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    #define FN(name) decltype(&name) name##_{};
    FN(PhidgetDigitalInput_create) FN(PhidgetDigitalOutput_create) FN(PhidgetVoltageRatioInput_create)
    FN(Phidget_delete) FN(Phidget_close) FN(Phidget_open) FN(Phidget_getAttached) FN(Phidget_getDeviceID)
    FN(Phidget_setHubPort) FN(Phidget_setIsHubPortDevice) FN(Phidget_getChannelClass)
    FN(Phidget_setDeviceSerialNumber) FN(Phidget_setChannel) FN(Phidget_setIsRemote) FN(Phidget_setIsLocal) FN(Phidget_setServerName)
    FN(Phidget_setOnAttachHandler) FN(Phidget_setOnDetachHandler) FN(Phidget_setOnErrorHandler)
    FN(PhidgetDigitalInput_setOnStateChangeHandler) FN(PhidgetDigitalInput_getState)
    FN(PhidgetVoltageRatioInput_setOnVoltageRatioChangeHandler) FN(PhidgetVoltageRatioInput_getVoltageRatio)
    FN(PhidgetDigitalOutput_setState) FN(PhidgetNet_addServer) FN(PhidgetNet_removeServer)
    FN(PhidgetNet_enableServerDiscovery) FN(PhidgetNet_disableServerDiscovery)
    FN(PhidgetStepper_create) FN(PhidgetBLDCMotor_create) FN(PhidgetRCServo_create) FN(PhidgetStepper_setEngaged) FN(PhidgetStepper_getEngaged) FN(PhidgetStepper_setControlMode) FN(PhidgetStepper_setRescaleFactor) FN(PhidgetStepper_setCurrentLimit) FN(PhidgetStepper_setHoldingCurrentLimit) FN(PhidgetStepper_setAcceleration) FN(PhidgetStepper_setVelocityLimit) FN(PhidgetStepper_getMaxVelocityLimit) FN(PhidgetStepper_getMinAcceleration) FN(PhidgetStepper_getMaxAcceleration) FN(PhidgetStepper_getMinCurrentLimit) FN(PhidgetStepper_getMaxCurrentLimit) FN(PhidgetStepper_getMinFailsafeTime) FN(PhidgetStepper_getMaxFailsafeTime) FN(PhidgetStepper_enableFailsafe) FN(PhidgetStepper_resetFailsafe) FN(PhidgetBLDCMotor_setTargetVelocity) FN(PhidgetBLDCMotor_getTargetVelocity) FN(PhidgetBLDCMotor_getTargetBrakingStrength) FN(PhidgetBLDCMotor_setTargetBrakingStrength) FN(PhidgetBLDCMotor_setAcceleration) FN(PhidgetBLDCMotor_setRescaleFactor) FN(PhidgetBLDCMotor_getPosition) FN(PhidgetBLDCMotor_setOnPositionChangeHandler) FN(PhidgetBLDCMotor_getMinAcceleration) FN(PhidgetBLDCMotor_getMaxAcceleration) FN(PhidgetBLDCMotor_getMinFailsafeTime) FN(PhidgetBLDCMotor_getMaxFailsafeTime) FN(PhidgetBLDCMotor_enableFailsafe) FN(PhidgetBLDCMotor_resetFailsafe) FN(PhidgetRCServo_setEngaged) FN(PhidgetRCServo_getEngaged) FN(PhidgetRCServo_setTargetPosition) FN(PhidgetRCServo_setMinPosition) FN(PhidgetRCServo_setMaxPosition) FN(PhidgetRCServo_setAcceleration) FN(PhidgetRCServo_setVelocityLimit) FN(PhidgetRCServo_getMinAcceleration) FN(PhidgetRCServo_getMaxAcceleration) FN(PhidgetRCServo_getMinVelocityLimit) FN(PhidgetRCServo_getMaxVelocityLimit) FN(PhidgetRCServo_getMinFailsafeTime) FN(PhidgetRCServo_getMaxFailsafeTime) FN(PhidgetRCServo_enableFailsafe) FN(PhidgetRCServo_resetFailsafe)
    #undef FN
    struct Channel {Impl* self{};unsigned index{};std::uint64_t callback_generation{1};PhidgetHandle handle{};MotorDriveState drive{MotorDriveState::off};unsigned release_attempts{};bool recover{},recovery_used{},wait_demand{},off_seen{};std::uint64_t recover_at{};bool attached{},known{},failed{},attempted{},engaged{},active{},target_known{},anchor{},watchdog{},paused{};int sent{-1};double position{},velocity{},target{},anchor_position{},anchor_trim{},last_feedback{},paused_feedback{},paused_target{},commanded_travel{};std::uint64_t progress_at{},reset_at{},motion_at{};};std::array<Channel,22> channels;
    struct Event{unsigned channel{},kind{};double value{};std::uint64_t generation{};};std::array<Event,4096> queue{};std::size_t read{},write{};bool lost{};std::array<std::string,22> faults;
    std::array<FdstqDriveStatus,2> published_thrust;std::array<std::uint64_t,2> settled_since{};
    std::uint64_t thrust_token{},thrust_operation{};bool ios_thrust{};
    Impl(FdstqInterface& o,std::wstring path):owner(o),library(std::move(path)){}
    template<class T>void symbol(T& t,const char* key){const auto p=GetProcAddress(dll,key);if(!p)throw std::runtime_error(std::string("Missing Phidget22 FDSTQ export: ")+key);static_assert(sizeof(t)==sizeof(p));memcpy(&t,&p,sizeof(t));}
    void enqueue(Channel& c,unsigned kind,double value=0) noexcept {std::lock_guard lock(mutex);if(write-read==queue.size()){lost=true;++owner.overflow;}else queue[write++%queue.size()]={c.index,kind,value,c.callback_generation};SetEvent(wake);}
    static void CCONV attach(PhidgetHandle,void* ctx){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,1);}
    static void CCONV detach(PhidgetHandle,void* ctx){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,2);}
    static void CCONV error(PhidgetHandle,void* ctx,Phidget_ErrorEventCode code,const char*){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,3,static_cast<double>(code));}
    static void CCONV digital(PhidgetDigitalInputHandle,void* ctx,int value){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,0,static_cast<double>(value));}
    static void CCONV analogue(PhidgetVoltageRatioInputHandle,void* ctx,double value){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,0,value);}
    static void CCONV trim_position(PhidgetBLDCMotorHandle,void* ctx,double value){auto& c=*static_cast<Channel*>(ctx);c.self->enqueue(c,4,value);}
    void motor_result(Channel& c,PhidgetReturnCode result,const char* operation){
        if(trace)trace->record(fds::TraceKind::api_result,static_cast<std::uint32_t>(first_phidget_device),generation,"FDSTQ motor="+std::to_string(c.index-17)+" "+operation+" result="+std::to_string(static_cast<int>(result)));
        if(result){c.failed=true;motor_stop(c);faults[c.index]="FDSTQ motor "+std::to_string(c.index-17)+" "+operation+" failed ("+std::to_string(static_cast<int>(result))+"); stopped until reattach";publish(faults[c.index]);}
    }
    void motor_stop(Channel& c){
        moving_mask.fetch_and(~(1u<<(c.index-17)));
        if(!c.handle || !c.attached){if(c.drive!=MotorDriveState::off){c.drive=MotorDriveState::unknown;faults[c.index]="FDSTQ motor detached; release unconfirmed";}c.active=c.engaged=c.anchor=false;return;}
        if(c.drive==MotorDriveState::unknown && c.release_attempts>=2)return;
        PhidgetReturnCode first{},second{};int engaged{};double velocity{},braking{};++c.release_attempts;
        if(c.index<20){auto h=reinterpret_cast<PhidgetStepperHandle>(c.handle);first=PhidgetStepper_setVelocityLimit_(h,0);second=PhidgetStepper_setEngaged_(h,0);if(!first && !second){second=PhidgetStepper_getEngaged_(h,&engaged);if(!second && engaged)second=EPHIDGET_UNEXPECTED;}}
        else if(c.index==20){auto h=reinterpret_cast<PhidgetBLDCMotorHandle>(c.handle);first=PhidgetBLDCMotor_setTargetVelocity_(h,0);second=PhidgetBLDCMotor_setTargetBrakingStrength_(h,0);if(!first && !second){first=PhidgetBLDCMotor_getTargetVelocity_(h,&velocity);second=PhidgetBLDCMotor_getTargetBrakingStrength_(h,&braking);if(!first && !second && (velocity!=0 || braking!=0))second=EPHIDGET_UNEXPECTED;}}
        else {auto h=reinterpret_cast<PhidgetRCServoHandle>(c.handle);first=PhidgetRCServo_setEngaged_(h,0);if(!first){first=PhidgetRCServo_getEngaged_(h,&engaged);if(!first && engaged)first=EPHIDGET_UNEXPECTED;}}
        if(trace)trace->record(fds::TraceKind::api_result,static_cast<std::uint32_t>(first_phidget_device),generation,"FDSTQ motor="+std::to_string(c.index-17)+" release results="+std::to_string(static_cast<int>(first))+","+std::to_string(static_cast<int>(second)));
        if(first || second){c.drive=MotorDriveState::unknown;c.failed=true;faults[c.index]="FDSTQ release failed; drive state unknown; native "+std::to_string(first)+","+std::to_string(second);if(c.release_attempts<2){motor_stop(c);return;}if(!c.recovery_used){c.recover=true;c.recover_at=GetTickCount64()+1000;}publish(faults[c.index]);return;}
        c.drive=MotorDriveState::off;c.release_attempts=0;c.engaged=c.active=c.anchor=false;c.velocity=0;c.progress_at=0;c.commanded_travel=0;c.motion_at=0;
    }
    void remember_demand(Channel& c){
        std::lock_guard lock(mutex);if(intent.generation!=generation)return;
        if(c.index<20){if(intent.enabled[c.index-17])c.target=intent.target[c.index-17];}
        else if(c.index==20?intent.trim:intent.servo)c.anchor_trim=intent.trim_degrees;
    }
    void service_channel(Channel& c,std::uint64_t now){
        if(c.recover){if(now<c.recover_at)return;c.recover=false;c.recovery_used=true;c.wait_demand=true;c.off_seen=false;c.watchdog=false;remember_demand(c);Phidget_close_(c.handle);++c.callback_generation;c.attached=false;c.failed=false;c.drive=MotorDriveState::unknown;c.release_attempts=0;const auto code=Phidget_open_(c.handle);if(code){c.failed=true;faults[c.index]="FDSTQ automatic reopen failed; native "+std::to_string(code);}return;}
        if(c.failed || !c.watchdog || c.drive==MotorDriveState::unknown || now-c.reset_at<100)return;
        const auto code=c.index<20?PhidgetStepper_resetFailsafe_(reinterpret_cast<PhidgetStepperHandle>(c.handle)):c.index==20?PhidgetBLDCMotor_resetFailsafe_(reinterpret_cast<PhidgetBLDCMotorHandle>(c.handle)):PhidgetRCServo_resetFailsafe_(reinterpret_cast<PhidgetRCServoHandle>(c.handle));
        c.reset_at=now;if(code!=EPHIDGET_UNSUPPORTED)motor_result(c,code,"hardware watchdog");
    }
    void motor_attach(Channel& c){
        motor_stop(c);if(c.failed){publish(faults[c.index]);return;}if(c.wait_demand)remember_demand(c);c.target_known=false;c.known=false;c.reset_at=0;c.watchdog=false;
        Phidget_ChannelClass cls{};const auto expected=c.index<20?PHIDCHCLASS_STEPPER:c.index==20?PHIDCHCLASS_BLDCMOTOR:PHIDCHCLASS_RCSERVO;
        auto result=Phidget_getChannelClass_(c.handle,&cls);if(result || cls!=expected){motor_result(c,EPHIDGET_WRONGDEVICE,"channel class");return;}
        auto bound=[](double v,double lo,double hi){return std::isfinite(v) && v>=lo && v<=hi;};double lo{},hi{};uint32_t min_time{},max_time{};
        if(c.index<20){auto h=reinterpret_cast<PhidgetStepperHandle>(c.handle);const auto& cfg=configuration.steppers[c.index-17];
            result=PhidgetStepper_setControlMode_(h,CONTROL_MODE_RUN);if(!result)result=PhidgetStepper_setRescaleFactor_(h,1);
            if(!result)result=PhidgetStepper_getMinCurrentLimit_(h,&lo);if(!result)result=PhidgetStepper_getMaxCurrentLimit_(h,&hi);if(!result && !bound(cfg.current,lo,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetStepper_setCurrentLimit_(h,cfg.current);if(!result)result=PhidgetStepper_setHoldingCurrentLimit_(h,0);
            if(!result)result=PhidgetStepper_getMinAcceleration_(h,&lo);if(!result)result=PhidgetStepper_getMaxAcceleration_(h,&hi);if(!result && !bound(cfg.acceleration,lo,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetStepper_setAcceleration_(h,cfg.acceleration);if(!result)result=PhidgetStepper_getMaxVelocityLimit_(h,&hi);if(!result && !bound(cfg.velocity,0,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetStepper_getMinFailsafeTime_(h,&min_time);if(!result)result=PhidgetStepper_getMaxFailsafeTime_(h,&max_time);if(!result && (min_time>1000 || max_time<1000))result=EPHIDGET_UNSUPPORTED;
            if(!result){result=PhidgetStepper_enableFailsafe_(h,1000);c.watchdog=!result;}
        }else if(c.index==20){auto h=reinterpret_cast<PhidgetBLDCMotorHandle>(c.handle);
            result=PhidgetBLDCMotor_setRescaleFactor_(h,1);if(!result)result=PhidgetBLDCMotor_getMinAcceleration_(h,&lo);if(!result)result=PhidgetBLDCMotor_getMaxAcceleration_(h,&hi);if(!result && !bound(configuration.trim_acceleration,lo,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetBLDCMotor_setAcceleration_(h,configuration.trim_acceleration);if(!result)result=PhidgetBLDCMotor_getMinFailsafeTime_(h,&min_time);if(!result)result=PhidgetBLDCMotor_getMaxFailsafeTime_(h,&max_time);if(!result && (min_time>1000 || max_time<1000))result=EPHIDGET_UNSUPPORTED;
            if(!result){result=PhidgetBLDCMotor_enableFailsafe_(h,1000);c.watchdog=!result;}if(!result && !PhidgetBLDCMotor_getPosition_(h,&c.position))c.known=std::isfinite(c.position);
        }else {auto h=reinterpret_cast<PhidgetRCServoHandle>(c.handle);
            result=PhidgetRCServo_setMinPosition_(h,0);if(!result)result=PhidgetRCServo_setMaxPosition_(h,180);
            if(!result)result=PhidgetRCServo_getMinAcceleration_(h,&lo);if(!result)result=PhidgetRCServo_getMaxAcceleration_(h,&hi);if(!result && !bound(configuration.servo_acceleration,lo,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetRCServo_setAcceleration_(h,configuration.servo_acceleration);if(!result)result=PhidgetRCServo_getMinVelocityLimit_(h,&lo);if(!result)result=PhidgetRCServo_getMaxVelocityLimit_(h,&hi);if(!result && !bound(configuration.servo_velocity,lo,hi))result=EPHIDGET_INVALIDARG;
            if(!result)result=PhidgetRCServo_setVelocityLimit_(h,configuration.servo_velocity);
            // Older 1061 servos have bounded position commands but no hardware failsafe.
            if(!result){const auto supported=PhidgetRCServo_getMinFailsafeTime_(h,&min_time);if(supported!=EPHIDGET_UNSUPPORTED){result=supported;if(!result)result=PhidgetRCServo_getMaxFailsafeTime_(h,&max_time);if(!result && (min_time>1000 || max_time<1000))result=EPHIDGET_UNSUPPORTED;if(!result){result=PhidgetRCServo_enableFailsafe_(h,1000);c.watchdog=!result;}}}
        }
        motor_result(c,result,"initialize/bounds/failsafe");
    }
    void service_motion(){
        FdstqMotion requested;std::uint64_t at{};{std::lock_guard lock(mutex);requested=intent;at=intent_at;}
        const auto now=GetTickCount64();const bool valid=at && now-at<=250 && requested.generation==generation;
        if(valid && motion_epoch!=requested.psx_epoch){for(auto& c:channels)if(c.index>=17 && (!requested.ios_thrust || c.index>=19) && (c.engaged || c.active || c.anchor))motor_stop(c);motion_epoch=requested.psx_epoch;}
        if(valid && (ios_thrust!=requested.ios_thrust || (requested.ios_thrust && thrust_token!=requested.ios_token))){for(unsigned n=17;n<19;++n)motor_stop(channels[n]);settled_since={};ios_thrust=requested.ios_thrust;thrust_token=requested.ios_token;}
        if(thrust_operation!=requested.ios_operation){settled_since={};thrust_operation=requested.ios_operation;}
        for(unsigned n=0;n<5;++n){auto& c=channels[17+n];if(!c.handle || (!c.attached && !c.recover))continue;service_channel(c,now);if(!c.attached || c.recover)continue;
            if(c.failed || !valid){if(c.engaged || c.active || c.anchor)motor_stop(c);continue;}
            if(n<3){const auto& cfg=configuration.steppers[n];auto& input=channels[8+n];double raw{};
                auto result=PhidgetVoltageRatioInput_getVoltageRatio_(reinterpret_cast<PhidgetVoltageRatioInputHandle>(input.handle),&raw);const auto counts=result?std::optional<int>{}:fdstq_counts(raw);
                // An attached channel, valid baseline and successful current getter are all required.
                Sample last;const bool known=!result && counts && input.attached && input.known && latest[8+n].read(last) && !last.invalid;
                if(!known && input.known)sample(input,0,true);
                const double feedback=counts?*counts:known?last.value:0;
                bool enable=requested.enabled[n] && known;
                if(c.wait_demand){if(!requested.enabled[n])c.off_seen=true;if((c.off_seen && requested.enabled[n]) || requested.target[n]!=c.target){c.wait_demand=false;c.recovery_used=false;}else enable=false;}
                if(n==2)enable=enable && requested.speed_follow;
                if(requested.target[n]!=c.target && trace)trace->record(fds::TraceKind::routing,static_cast<std::uint32_t>(first_phidget_device),generation,"FDSTQ motor="+std::to_string(n)+" target="+std::to_string(requested.target[n])+" feedback="+std::to_string(feedback));
                if(!enable){if(c.engaged)motor_stop(c);if(!requested.enabled[n]){c.target_known=false;c.paused=false;}c.target=requested.target[n];continue;}
                const auto range=std::abs(requested.full[n]-requested.zero[n]);
                const int edge=feedback<=std::min(requested.zero[n],requested.full[n])?-1:feedback>=std::max(requested.zero[n],requested.full[n])?1:0;if(edge && !motor_inward(feedback,requested.target[n],edge)){if(c.engaged)motor_stop(c);faults[c.index]="FDSTQ calibrated limit; outward movement blocked; calibration retained";continue;}
                double velocity=fdstq_velocity(requested.target[n],feedback,requested.zero[n],requested.full[n],cfg.reversed,cfg.velocity);
                if(!c.progress_at || (std::abs(feedback-c.last_feedback)>=std::max(2.,range*.0003) && (feedback-c.last_feedback)*(requested.target[n]-c.last_feedback)>0)){c.last_feedback=feedback;c.progress_at=now;c.commanded_travel=0;}
                if(c.paused && (requested.target[n]!=c.paused_target || ((feedback-c.paused_feedback)*(requested.target[n]-c.paused_feedback)>0 && std::abs(feedback-c.paused_feedback)>=2))){c.paused=false;c.progress_at=now;c.commanded_travel=0;}
                if(c.paused && !(n<2 && requested.ios_thrust))continue;
                if(c.motion_at)c.commanded_travel+=std::abs(c.velocity)*std::min(.1,double(now-c.motion_at)/1000.);c.motion_at=now;
                if(!(n<2 && requested.ios_thrust) && velocity!=0 && now-c.progress_at>2000 && c.commanded_travel>=2){motor_stop(c);c.paused=true;c.paused_feedback=feedback;c.paused_target=requested.target[n];faults[c.index]="FDSTQ tracking paused; no directed lever progress; calibration retained";publish(faults[c.index]);continue;}
                auto h=reinterpret_cast<PhidgetStepperHandle>(c.handle);
                if(velocity==0){if(c.engaged)motor_stop(c);c.target=requested.target[n];continue;}
                if(!c.engaged){result=PhidgetStepper_setVelocityLimit_(h,0);if(!result)result=PhidgetStepper_setEngaged_(h,1);if(result){motor_result(c,result,"engage");continue;}c.engaged=true;c.drive=MotorDriveState::on;}
                if(c.velocity!=velocity){result=PhidgetStepper_setVelocityLimit_(h,velocity);motor_result(c,result,("velocity="+std::to_string(velocity)).c_str());if(result)continue;c.velocity=velocity;}
                c.active=true;c.target=requested.target[n];moving_mask.fetch_or(1u<<n);
                if(now-c.reset_at>=100){motor_result(c,PhidgetStepper_resetFailsafe_(h),"lease");c.reset_at=now;}
            }else if(n==3){auto h=reinterpret_cast<PhidgetBLDCMotorHandle>(c.handle);double position{};const auto getter_result=PhidgetBLDCMotor_getPosition_(h,&position);c.known=!getter_result && std::isfinite(position);if(c.known)c.position=position;
                if(c.wait_demand){if(!requested.trim)c.off_seen=true;if((c.off_seen && requested.trim) || requested.trim_degrees!=c.anchor_trim){c.wait_demand=false;c.recovery_used=false;}}
                if(c.wait_demand || !requested.trim || !c.known){if(c.active || c.anchor)motor_stop(c);if(!requested.trim)c.paused=false;continue;}
                if(!c.anchor){c.anchor=true;c.anchor_position=c.position;c.anchor_trim=requested.trim_degrees;}
                const auto target=c.anchor_position+(requested.trim_degrees-c.anchor_trim)*configuration.trim_counts_per_degree;
                const double normalized=(target-c.position)/(std::abs(configuration.trim_counts_per_degree)*15.);
                const double velocity=std::abs(normalized)<=.001?0:std::clamp(normalized*10.,-1.,1.)*configuration.trim_velocity;
                if(!std::isfinite(target) || !std::isfinite(normalized)){motor_result(c,EPHIDGET_INVALIDARG,"non-finite trim calibration arithmetic");continue;}
                if(!c.progress_at || (std::abs(c.position-c.last_feedback)>=1 && (c.position-c.last_feedback)*(target-c.last_feedback)>0)){c.last_feedback=c.position;c.progress_at=now;}
                if(c.paused && (target!=c.paused_target || std::abs(c.position-c.paused_feedback)>=1)){c.paused=false;c.progress_at=now;}if(c.paused)continue;
                if(velocity!=0 && now-c.progress_at>2000){motor_stop(c);c.paused=true;c.paused_feedback=c.position;c.paused_target=target;faults[c.index]="FDSTQ trim tracking paused; no Hall feedback progress";continue;}
                if(c.velocity!=velocity){const auto result=PhidgetBLDCMotor_setTargetVelocity_(h,velocity);motor_result(c,result,("velocity="+std::to_string(velocity)).c_str());if(result)continue;c.velocity=velocity;}
                c.active=velocity!=0;c.drive=c.active?MotorDriveState::on:MotorDriveState::off;if(c.active)moving_mask.fetch_or(1u<<n);else moving_mask.fetch_and(~(1u<<n));
                if(now-c.reset_at>=100){motor_result(c,PhidgetBLDCMotor_resetFailsafe_(h),"lease");c.reset_at=now;}
            }else {auto h=reinterpret_cast<PhidgetRCServoHandle>(c.handle);if(c.wait_demand){if(!requested.servo)c.off_seen=true;if((c.off_seen && requested.servo) || requested.trim_degrees!=c.anchor_trim){c.wait_demand=false;c.recovery_used=false;}}if(c.wait_demand || !requested.servo){if(c.engaged)motor_stop(c);continue;}
                const auto target=configuration.servo_zero+(configuration.servo_full-configuration.servo_zero)*requested.trim_degrees/15.;
                if(!c.engaged || target!=c.target){auto result=PhidgetRCServo_setTargetPosition_(h,target);if(!result && !c.engaged)result=PhidgetRCServo_setEngaged_(h,1);motor_result(c,result,"position/engage");if(result)continue;c.engaged=true;c.drive=MotorDriveState::on;c.target=target;}
                if(c.watchdog && now-c.reset_at>=100){const auto result=PhidgetRCServo_resetFailsafe_(h);if(result!=EPHIDGET_UNSUPPORTED)motor_result(c,result,"lease");c.reset_at=now;}
            }
        }
        std::array<FdstqDriveStatus,2> status;
        for(unsigned n=0;n<2;++n){auto& s=status[n];const auto& c=channels[17+n];Sample sensor;const bool known=latest[8+n].read(sensor) && !sensor.invalid && channels[8+n].attached && channels[8+n].known;
            s.generation=generation;s.operation=requested.ios_operation;s.available=c.attached && !c.failed;s.valid=known;s.drive=c.drive;s.raw=known?sensor.value:0;s.target=c.target;if(c.failed || !c.attached)s.reason=faults[17+n];
            const int edge=s.raw<=std::min(requested.zero[n],requested.full[n])?-1:s.raw>=std::max(requested.zero[n],requested.full[n])?1:0;s.limited=known && requested.enabled[n] && edge && !motor_inward(s.raw,requested.target[n],edge);
            const bool inside=valid && requested.ios_thrust && requested.enabled[n] && known && s.available && std::abs(s.raw-requested.target[n])<=std::abs(requested.full[n]-requested.zero[n])*.003;
            if(!inside)settled_since[n]=0;else if(!settled_since[n])settled_since[n]=now;s.held=inside && now-settled_since[n]>=200;
        }
        {std::lock_guard lock(mutex);published_thrust=std::move(status);}
    }
    void publish(std::string reason={}){unsigned attached_count{};for(const auto& c:channels)if(c.attached)++attached_count;
        unsigned motors{};for(unsigned n=0;n<5;++n)if(channels[17+n].attached && !channels[17+n].failed)motors|=1u<<n;motor_mask=motors;
        std::lock_guard lock(mutex);test_ready=channels[16].attached && !channels[16].failed;const bool connected=attached_count!=0;if(published.connected!=connected){published.connected=connected;++owner.metadata_revision;}
        message=reason.empty()?std::to_string(attached_count)+"/"+std::to_string(12+static_cast<unsigned>(std::count_if(channels.begin()+12,channels.end(),[](const auto& c){return c.handle!=nullptr;})))+" channels attached; "+(configuration.remote?"Phidget22 network":"direct USB")+"; motion mask "+std::to_string(moving_mask.load()):std::move(reason);for(const auto& fault:faults)if(!fault.empty() && message.find(fault)==message.npos)message+="; "+fault;published.status=message;
    }
    void sample(Channel& c,double raw,bool invalid=false){if(c.index>=12)return;const auto value=c.index<8?(raw==0 || raw==1?std::optional<int>(static_cast<int>(raw)):std::nullopt):fdstq_counts(raw);
        Sample s;s.device=static_cast<std::uint16_t>(first_phidget_device);s.control=static_cast<std::uint16_t>(c.index);s.generation=generation;s.sequence=++sequence;LARGE_INTEGER q{};QueryPerformanceCounter(&q);s.qpc=q.QuadPart;s.invalid=invalid || !value;s.value=value.value_or(0);if(c.index<8 && (configuration.inverted_switches&(1u<<c.index)))s.value=1-s.value;s.baseline=!c.known;c.known=!s.invalid;latest[c.index].store(s);if(c.index<8 && !owner.edges.push(s))++owner.overflow;++owner.reports;SetEvent(owner.available_event);
    }
    void cleanup(){if(dll){for(auto& c:channels)if(c.handle){if(c.index>=12 && c.index<17 && c.attached && (!configuration.outputs[c.index-12].empty() || c.attempted))PhidgetDigitalOutput_setState_(reinterpret_cast<PhidgetDigitalOutputHandle>(c.handle),0);if(c.index>=17 && c.attached)motor_stop(c);Phidget_close_(c.handle);Phidget_delete_(&c.handle);c.attached=false;}
        for(auto& claim:motor_claims)claim={};network={};session.reset();dll=nullptr;moving_mask=0;motor_mask=0;
        std::lock_guard lock(mutex);for(unsigned n=0;n<2;++n){published_thrust[n].available=published_thrust[n].valid=published_thrust[n].held=false;published_thrust[n].drive=channels[17+n].drive;}
    }}
    void run(){try{
        session=PhidgetSession::acquire(library);dll=session->module();
        #define LOAD(name) symbol(name##_,#name);
        LOAD(PhidgetDigitalInput_create) LOAD(PhidgetDigitalOutput_create) LOAD(PhidgetVoltageRatioInput_create)
        LOAD(Phidget_delete) LOAD(Phidget_close) LOAD(Phidget_open) LOAD(Phidget_getAttached) LOAD(Phidget_getDeviceID)
        LOAD(Phidget_setDeviceSerialNumber) LOAD(Phidget_setChannel) LOAD(Phidget_setIsRemote) LOAD(Phidget_setIsLocal) LOAD(Phidget_setServerName)
        LOAD(Phidget_setOnAttachHandler) LOAD(Phidget_setOnDetachHandler) LOAD(Phidget_setOnErrorHandler)
        LOAD(PhidgetDigitalInput_setOnStateChangeHandler) LOAD(PhidgetDigitalInput_getState)
        LOAD(PhidgetVoltageRatioInput_setOnVoltageRatioChangeHandler) LOAD(PhidgetVoltageRatioInput_getVoltageRatio)
        LOAD(PhidgetDigitalOutput_setState) LOAD(PhidgetNet_addServer) LOAD(PhidgetNet_removeServer) LOAD(PhidgetNet_enableServerDiscovery) LOAD(PhidgetNet_disableServerDiscovery)
        LOAD(PhidgetStepper_create) LOAD(PhidgetBLDCMotor_create) LOAD(PhidgetRCServo_create) LOAD(PhidgetStepper_setEngaged) LOAD(PhidgetStepper_getEngaged) LOAD(PhidgetStepper_setControlMode) LOAD(PhidgetStepper_setRescaleFactor) LOAD(PhidgetStepper_setCurrentLimit) LOAD(PhidgetStepper_setHoldingCurrentLimit) LOAD(PhidgetStepper_setAcceleration) LOAD(PhidgetStepper_setVelocityLimit) LOAD(PhidgetStepper_getMaxVelocityLimit) LOAD(PhidgetStepper_getMinAcceleration) LOAD(PhidgetStepper_getMaxAcceleration) LOAD(PhidgetStepper_getMinCurrentLimit) LOAD(PhidgetStepper_getMaxCurrentLimit) LOAD(PhidgetStepper_getMinFailsafeTime) LOAD(PhidgetStepper_getMaxFailsafeTime) LOAD(PhidgetStepper_enableFailsafe) LOAD(PhidgetStepper_resetFailsafe) LOAD(PhidgetBLDCMotor_setTargetVelocity) LOAD(PhidgetBLDCMotor_getTargetVelocity) LOAD(PhidgetBLDCMotor_getTargetBrakingStrength) LOAD(PhidgetBLDCMotor_setTargetBrakingStrength) LOAD(PhidgetBLDCMotor_setAcceleration) LOAD(PhidgetBLDCMotor_setRescaleFactor) LOAD(PhidgetBLDCMotor_getPosition) LOAD(PhidgetBLDCMotor_setOnPositionChangeHandler) LOAD(PhidgetBLDCMotor_getMinAcceleration) LOAD(PhidgetBLDCMotor_getMaxAcceleration) LOAD(PhidgetBLDCMotor_getMinFailsafeTime) LOAD(PhidgetBLDCMotor_getMaxFailsafeTime) LOAD(PhidgetBLDCMotor_enableFailsafe) LOAD(PhidgetBLDCMotor_resetFailsafe) LOAD(PhidgetRCServo_setEngaged) LOAD(PhidgetRCServo_getEngaged) LOAD(PhidgetRCServo_setTargetPosition) LOAD(PhidgetRCServo_setMinPosition) LOAD(PhidgetRCServo_setMaxPosition) LOAD(PhidgetRCServo_setAcceleration) LOAD(PhidgetRCServo_setVelocityLimit) LOAD(PhidgetRCServo_getMinAcceleration) LOAD(PhidgetRCServo_getMaxAcceleration) LOAD(PhidgetRCServo_getMinVelocityLimit) LOAD(PhidgetRCServo_getMaxVelocityLimit) LOAD(PhidgetRCServo_getMinFailsafeTime) LOAD(PhidgetRCServo_getMaxFailsafeTime) LOAD(PhidgetRCServo_enableFailsafe) LOAD(PhidgetRCServo_resetFailsafe)
        LOAD(Phidget_setHubPort) LOAD(Phidget_setIsHubPortDevice) LOAD(Phidget_getChannelClass)
        #undef LOAD
        if(configuration.remote)network=session->server({configuration.host,{},configuration.port},configuration.host.empty());
        // Reserve all configured drives before opening any of them. Class names
        // cannot permit a second consumer to control the same physical motor.
        for(unsigned n=0;n<5;++n)if(n<3?configuration.steppers[n].enabled:n==3?configuration.trim_enabled:configuration.servo_enabled)
            motor_claims[n]=session->claim({n==4?configuration.servo_serial:configuration.hub_serial,n==4?-1:n==3?0:static_cast<int>(n+1),0},"FDSTQ");
        for(unsigned n=0;n<channels.size();++n){auto& c=channels[n];c={this,n};if(n>=12 && n<17 && configuration.outputs[n-12].empty() && n!=16)continue;if(n>=17 && n<20 && !configuration.steppers[n-17].enabled)continue;if(n==20 && !configuration.trim_enabled)continue;if(n==21 && !configuration.servo_enabled)continue;
            PhidgetReturnCode result{};if(n<8){PhidgetDigitalInputHandle h{};result=PhidgetDigitalInput_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}else if(n<12){PhidgetVoltageRatioInputHandle h{};result=PhidgetVoltageRatioInput_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}else if(n<17){PhidgetDigitalOutputHandle h{};result=PhidgetDigitalOutput_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}else if(n<20){PhidgetStepperHandle h{};result=PhidgetStepper_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}else if(n==20){PhidgetBLDCMotorHandle h{};result=PhidgetBLDCMotor_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}else{PhidgetRCServoHandle h{};result=PhidgetRCServo_create_(&h);c.handle=reinterpret_cast<PhidgetHandle>(h);}
            if(result || !c.handle)throw std::runtime_error("Cannot create FDSTQ channel");
            if(Phidget_setDeviceSerialNumber_(c.handle,n<17?configuration.serial:n==21?configuration.servo_serial:configuration.hub_serial) || Phidget_setChannel_(c.handle,static_cast<int>(n<8?n:n<12?n-7:n<17?n-12:0)) || (n>=17 && n<21 && Phidget_setHubPort_(c.handle,n==20?0:static_cast<int>(n-16))) || (n>=17 && Phidget_setIsHubPortDevice_(c.handle,0)) || (configuration.remote?Phidget_setIsRemote_(c.handle,1):Phidget_setIsLocal_(c.handle,1)) || (configuration.remote && !configuration.host.empty() && Phidget_setServerName_(c.handle,network.server_name.c_str())) || Phidget_setOnAttachHandler_(c.handle,attach,&c) || Phidget_setOnDetachHandler_(c.handle,detach,&c) || Phidget_setOnErrorHandler_(c.handle,error,&c))throw std::runtime_error("Cannot address exact FDSTQ channel");
            if(n<8)result=PhidgetDigitalInput_setOnStateChangeHandler_(reinterpret_cast<PhidgetDigitalInputHandle>(c.handle),digital,&c);else if(n<12)result=PhidgetVoltageRatioInput_setOnVoltageRatioChangeHandler_(reinterpret_cast<PhidgetVoltageRatioInputHandle>(c.handle),analogue,&c);
            if(n==20)result=PhidgetBLDCMotor_setOnPositionChangeHandler_(reinterpret_cast<PhidgetBLDCMotorHandle>(c.handle),trim_position,&c);
            if(result || Phidget_open_(c.handle))throw std::runtime_error("Cannot open FDSTQ channel");
        }
        publish();while(!stop){Event event;bool present{},overflowed{};std::array<bool,5> output;bool testing{};std::uint64_t started{};
            {std::lock_guard lock(mutex);overflowed=lost;if(read!=write){event=queue[read++%queue.size()];present=true;}output=desired;testing=test;started=test_started;}
            if(overflowed)throw std::runtime_error("FDSTQ callback overflow: disable/re-enable to acquire fresh baselines");
            if(present){auto& c=channels[event.channel];if(event.generation!=c.callback_generation)continue;if(event.kind==1){Phidget_DeviceID model{};if(c.index<17 && (Phidget_getDeviceID_(c.handle,&model) || model!=PHIDID_1010_1013_1018_1019))throw std::runtime_error("FDSTQ serial is not the qualified InterfaceKit 8/8/8 family");c.attached=true;c.known=false;c.failed=false;c.sent=-1;faults[c.index].clear();if(c.index<8){int v{};if(!PhidgetDigitalInput_getState_(reinterpret_cast<PhidgetDigitalInputHandle>(c.handle),&v))sample(c,v);}else if(c.index<12){double v{};if(!PhidgetVoltageRatioInput_getVoltageRatio_(reinterpret_cast<PhidgetVoltageRatioInputHandle>(c.handle),&v))sample(c,v);}if(c.index>=17)motor_attach(c);publish();}
                else if(event.kind==2 || event.kind==3){c.known=false;if(event.kind==2)c.attached=false;c.sent=-1;{std::lock_guard lock(mutex);test=false;}testing=false;if(c.index>=12)c.failed=true;if(c.index>=17){c.wait_demand=true;c.off_seen=false;motor_stop(c);}sample(c,0,true);publish(event.kind==3?"FDSTQ channel "+std::to_string(c.index)+" error "+std::to_string(static_cast<int>(event.value))+"; source invalid":"");}
                else if(c.attached){if(event.kind==4 && c.index==20){c.position=event.value;c.known=std::isfinite(event.value);}else sample(c,event.value);}
            }
            if(testing)output[4]=((GetTickCount64()-started)/1000)%2==0;
            for(unsigned n=0;n<5;++n){auto& c=channels[12+n];if(!c.handle || !c.attached || c.failed)continue;if(n==4 && configuration.outputs[4].empty() && !testing && !c.attempted)continue;const int wanted=output[n];if(c.sent!=wanted){c.attempted=true;const auto result=PhidgetDigitalOutput_setState_(reinterpret_cast<PhidgetDigitalOutputHandle>(c.handle),wanted);if(trace)trace->record(fds::TraceKind::api_result,static_cast<std::uint32_t>(first_phidget_device),generation,"FDSTQ serial="+std::to_string(configuration.serial)+" DO"+std::to_string(n)+" state="+std::to_string(wanted)+" result="+std::to_string(static_cast<int>(result)));if(result){c.failed=true;c.sent=-1;{std::lock_guard lock(mutex);test=false;}publish("FDSTQ output "+std::to_string(n)+" write failed");}else c.sent=wanted;}}
            service_motion();
            if(!present)WaitForSingleObject(wake,20);
        }
    }catch(const std::exception& e){publish(e.what());if(trace)trace->record(fds::TraceKind::api_result,static_cast<std::uint32_t>(first_phidget_device),generation,e.what());}cleanup();{std::lock_guard lock(mutex);published.connected=false;test=false;test_ready=false;++owner.metadata_revision;}SetEvent(owner.available_event);}
    void halt(){stop=true;SetEvent(wake);if(worker.joinable())worker.join();}
    ~Impl(){halt();CloseHandle(wake);}
};
FdstqInterface::FdstqInterface(std::wstring path):impl_(std::make_unique<Impl>(*this,std::move(path))){}
FdstqInterface::~FdstqInterface(){impl_.reset();CloseHandle(available_event);}
void FdstqInterface::configure(FdstqConfiguration c,bool provider_enabled){const bool enabled=c.enabled && provider_enabled;if(c==impl_->configuration && enabled==impl_->enabled)return;impl_->halt();std::string error;const bool valid=fdstq_valid(c,error);{std::lock_guard lock(impl_->mutex);impl_->configuration=c;impl_->enabled=enabled;impl_->test=false;impl_->desired.fill(false);impl_->intent={};impl_->intent_at=0;impl_->moving_mask=0;impl_->published=fdstq_device(c.serial,++impl_->generation);impl_->message=valid?(enabled?"Waiting for exact FDSTQ serial":"FDSTQ support disabled"):error;impl_->read=impl_->write=0;for(auto& fault:impl_->faults)fault.clear();impl_->lost=false;++metadata_revision;}
    if(enabled && valid){impl_->stop=false;impl_->worker=std::thread([this]{impl_->run();});}SetEvent(available_event);
}
std::vector<DeviceInfo> FdstqInterface::devices(){std::lock_guard lock(impl_->mutex);return impl_->configuration.serial>0?std::vector<DeviceInfo>{impl_->published}:std::vector<DeviceInfo>{};}
bool FdstqInterface::latest(std::size_t d,std::size_t c,Sample& value)const noexcept{if(d!=first_phidget_device || c>=12)return false;value.relative=value.report_end=false;value.device=static_cast<std::uint16_t>(d);value.control=static_cast<std::uint16_t>(c);return impl_->latest[c].read(value);}
void FdstqInterface::outputs(const std::array<bool,5>& values){std::lock_guard lock(impl_->mutex);impl_->desired=values;SetEvent(impl_->wake);}
void FdstqInterface::motion(const FdstqMotion& m){auto checked=m;for(unsigned n=0;n<3;++n)if(m.enabled[n] && (!std::isfinite(m.target[n]) || !std::isfinite(m.zero[n]) || !std::isfinite(m.full[n]) || m.zero[n]==m.full[n] || m.target[n]<std::min(m.zero[n],m.full[n]) || m.target[n]>std::max(m.zero[n],m.full[n]) || std::min(m.zero[n],m.full[n])<0 || std::max(m.zero[n],m.full[n])>16383))checked={};if((m.trim || m.servo) && (!std::isfinite(m.trim_degrees) || m.trim_degrees<0 || m.trim_degrees>15))checked={};std::lock_guard lock(impl_->mutex);impl_->intent=checked;impl_->intent_at=GetTickCount64();SetEvent(impl_->wake);}
unsigned FdstqInterface::moving() const noexcept{return impl_->moving_mask.load();}
unsigned FdstqInterface::motor_available() const noexcept{return impl_->motor_mask.load();}
std::array<FdstqDriveStatus,2> FdstqInterface::thrust_status(){std::lock_guard lock(impl_->mutex);return impl_->published_thrust;}
void FdstqInterface::identify(bool enabled){std::lock_guard lock(impl_->mutex);impl_->test=enabled && impl_->test_ready;if(impl_->test)impl_->test_started=GetTickCount64();SetEvent(impl_->wake);}
std::string FdstqInterface::status(){std::lock_guard lock(impl_->mutex);return impl_->message;}
bool FdstqInterface::identifying(){std::lock_guard lock(impl_->mutex);return impl_->test;}
bool FdstqInterface::identification_available(){std::lock_guard lock(impl_->mutex);return impl_->test_ready;}
void FdstqInterface::trace(fds::Trace* trace){impl_->trace=trace;}
}

