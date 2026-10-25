#include "hwio/stepper.hpp"
#include <phidget22.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
namespace hwio {
namespace {
std::atomic<std::uint64_t> stepper_generation{1};
class StepperTransport final:public MotorDrive {
    std::shared_ptr<PhidgetSession> sdk_;StepperDefinition definition_;std::optional<PhidgetServer> endpoint_;PhidgetLease claim_,server_;PhidgetStepperHandle h_{};
    std::atomic<bool> detached_{};std::atomic<int> error_{};bool configured_{},watchdog_{},qualified_{};std::uint64_t renew_{};MotorCapabilities caps_;
    #define FN(n) decltype(&n) n##_{};
    FN(PhidgetStepper_create) FN(Phidget_delete) FN(Phidget_close) FN(Phidget_openWaitForAttachment)
    FN(Phidget_setDeviceSerialNumber) FN(Phidget_setHubPort) FN(Phidget_setIsHubPortDevice) FN(Phidget_setChannel) FN(Phidget_setIsLocal) FN(Phidget_setIsRemote) FN(Phidget_setServerName)
    FN(Phidget_getDeviceID) FN(Phidget_getDeviceVersion) FN(Phidget_getChannelClass) FN(Phidget_getChannel) FN(Phidget_getHubPort) FN(Phidget_getIsRemote) FN(Phidget_getDeviceSerialNumber) FN(Phidget_getParent) FN(Phidget_release)
    FN(Phidget_setOnDetachHandler) FN(Phidget_setOnErrorHandler)
    FN(PhidgetStepper_setEngaged) FN(PhidgetStepper_getEngaged) FN(PhidgetStepper_getPosition) FN(PhidgetStepper_getVelocity) FN(PhidgetStepper_getIsMoving)
    FN(PhidgetStepper_setControlMode) FN(PhidgetStepper_getControlMode) FN(PhidgetStepper_setRescaleFactor) FN(PhidgetStepper_getRescaleFactor)
    FN(PhidgetStepper_setCurrentLimit) FN(PhidgetStepper_getCurrentLimit) FN(PhidgetStepper_getMinCurrentLimit) FN(PhidgetStepper_getMaxCurrentLimit)
    FN(PhidgetStepper_setHoldingCurrentLimit) FN(PhidgetStepper_getHoldingCurrentLimit)
    FN(PhidgetStepper_setAcceleration) FN(PhidgetStepper_getAcceleration) FN(PhidgetStepper_getMinAcceleration) FN(PhidgetStepper_getMaxAcceleration)
    FN(PhidgetStepper_setVelocityLimit) FN(PhidgetStepper_getVelocityLimit) FN(PhidgetStepper_getMinVelocityLimit) FN(PhidgetStepper_getMaxVelocityLimit)
    FN(PhidgetStepper_setTargetPosition) FN(PhidgetStepper_getTargetPosition) FN(PhidgetStepper_getMinPosition) FN(PhidgetStepper_getMaxPosition) FN(PhidgetStepper_addPositionOffset)
    FN(PhidgetStepper_setDataInterval) FN(PhidgetStepper_getMinDataInterval) FN(PhidgetStepper_getMaxDataInterval)
    FN(PhidgetStepper_enableFailsafe) FN(PhidgetStepper_resetFailsafe) FN(PhidgetStepper_getMinFailsafeTime) FN(PhidgetStepper_getMaxFailsafeTime)
    #undef FN
    static void CCONV detach(PhidgetHandle,void* p){static_cast<StepperTransport*>(p)->detached_=true;}
    static void CCONV error(PhidgetHandle,void* p,Phidget_ErrorEventCode e,const char*){static_cast<StepperTransport*>(p)->error_=int(e);}
    void check(PhidgetReturnCode r,const char* operation){if(r)throw MotorDriveFailure(int(r),operation);if(watchdog_ && GetTickCount64()-renew_>=100){const auto rc=PhidgetStepper_resetFailsafe_(h_);renew_=GetTickCount64();if(rc)throw MotorDriveFailure(int(rc),"Renew stepper watchdog");}}
    void healthy(){if(detached_ || error_)throw MotorDriveFailure(error_?error_.load():EPHIDGET_NOTATTACHED,"Stepper lifecycle");}
    template<class T>void load(T& f,const char* name){const auto p=sdk_->symbol(name);memcpy(&f,&p,sizeof(p));}
    void verified(decltype(&PhidgetStepper_setAcceleration) setter,decltype(&PhidgetStepper_getAcceleration) getter,double value,const char* label){check(setter(h_,value),label);double actual{};check(getter(h_,&actual),label);if(std::abs(actual-value)>1e-6*std::max(1.,std::abs(value)))throw MotorDriveFailure(EPHIDGET_UNEXPECTED,std::string(label)+" readback mismatch");}
public:
    StepperTransport(std::shared_ptr<PhidgetSession> sdk,StepperDefinition d,std::optional<PhidgetServer> server):sdk_(std::move(sdk)),definition_(std::move(d)),endpoint_(std::move(server)){
        #define L(n) load(n##_,#n);
        L(PhidgetStepper_create) L(Phidget_delete) L(Phidget_close) L(Phidget_openWaitForAttachment)
        L(Phidget_setDeviceSerialNumber) L(Phidget_setHubPort) L(Phidget_setIsHubPortDevice) L(Phidget_setChannel) L(Phidget_setIsLocal) L(Phidget_setIsRemote) L(Phidget_setServerName)
        L(Phidget_getDeviceID) L(Phidget_getDeviceVersion) L(Phidget_getChannelClass) L(Phidget_getChannel) L(Phidget_getHubPort) L(Phidget_getIsRemote) L(Phidget_getDeviceSerialNumber) L(Phidget_getParent) L(Phidget_release)
        L(Phidget_setOnDetachHandler) L(Phidget_setOnErrorHandler)
        L(PhidgetStepper_setEngaged) L(PhidgetStepper_getEngaged) L(PhidgetStepper_getPosition) L(PhidgetStepper_getVelocity) L(PhidgetStepper_getIsMoving)
        L(PhidgetStepper_setControlMode) L(PhidgetStepper_getControlMode) L(PhidgetStepper_setRescaleFactor) L(PhidgetStepper_getRescaleFactor)
        L(PhidgetStepper_setCurrentLimit) L(PhidgetStepper_getCurrentLimit) L(PhidgetStepper_getMinCurrentLimit) L(PhidgetStepper_getMaxCurrentLimit)
        L(PhidgetStepper_setHoldingCurrentLimit) L(PhidgetStepper_getHoldingCurrentLimit)
        L(PhidgetStepper_setAcceleration) L(PhidgetStepper_getAcceleration) L(PhidgetStepper_getMinAcceleration) L(PhidgetStepper_getMaxAcceleration)
        L(PhidgetStepper_setVelocityLimit) L(PhidgetStepper_getVelocityLimit) L(PhidgetStepper_getMinVelocityLimit) L(PhidgetStepper_getMaxVelocityLimit)
        L(PhidgetStepper_setTargetPosition) L(PhidgetStepper_getTargetPosition) L(PhidgetStepper_getMinPosition) L(PhidgetStepper_getMaxPosition) L(PhidgetStepper_addPositionOffset)
        L(PhidgetStepper_setDataInterval) L(PhidgetStepper_getMinDataInterval) L(PhidgetStepper_getMaxDataInterval)
        L(PhidgetStepper_enableFailsafe) L(PhidgetStepper_resetFailsafe) L(PhidgetStepper_getMinFailsafeTime) L(PhidgetStepper_getMaxFailsafeTime)
        #undef L
    }
    ~StepperTransport(){close();}
    MotorCapabilities open()override{
        close();const auto& a=definition_.address;claim_=sdk_->claim({a.serial,a.hub_port,a.channel},"HWIO stepper");if(a.remote){if(!endpoint_)throw std::runtime_error("Configure exact Phidget server");server_=sdk_->server(*endpoint_);}
        check(PhidgetStepper_create_(&h_),"Create stepper");auto raw=reinterpret_cast<PhidgetHandle>(h_);
        check(Phidget_setDeviceSerialNumber_(raw,a.serial),"Select stepper serial");check(Phidget_setChannel_(raw,a.channel),"Select stepper channel");check(Phidget_setIsRemote_(raw,a.remote),"Select remote");check(Phidget_setIsLocal_(raw,!a.remote),"Select local");
        if(a.hub_port>=0){check(Phidget_setHubPort_(raw,a.hub_port),"Select VINT port");check(Phidget_setIsHubPortDevice_(raw,0),"Select VINT child");}if(a.remote)check(Phidget_setServerName_(raw,server_.server_name.c_str()),"Select server");
        detached_=false;error_=0;check(Phidget_setOnDetachHandler_(raw,detach,this),"Install detach callback");check(Phidget_setOnErrorHandler_(raw,error,this),"Install error callback");check(Phidget_openWaitForAttachment_(raw,1000),"Attach exact stepper");
        Phidget_ChannelClass cls{};Phidget_DeviceID model{};int firmware{},number{},port{},remote{},index{};check(Phidget_getChannelClass_(raw,&cls),"Read stepper class");check(Phidget_getDeviceID_(raw,&model),"Read stepper model");check(Phidget_getDeviceVersion_(raw,&firmware),"Read firmware");check(Phidget_getChannel_(raw,&index),"Read channel");check(Phidget_getHubPort_(raw,&port),"Read port");check(Phidget_getIsRemote_(raw,&remote),"Read transport");
        PhidgetHandle parent{},hub{};try{check(Phidget_getParent_(raw,&parent),"Read controller parent");if(parent)check(Phidget_getParent_(parent,&hub),"Read VINT hub");check(Phidget_getDeviceSerialNumber_(hub?hub:raw,&number),"Read physical root serial");}catch(...){if(hub)Phidget_release_(&hub);if(parent)Phidget_release_(&parent);throw;}if(hub)Phidget_release_(&hub);if(parent)Phidget_release_(&parent);
        if(cls!=PHIDCHCLASS_STEPPER || model!=PHIDID_STC1005 || (a.model && a.model!=int(model)) || a.serial!=number || a.channel!=index || a.hub_port!=port || a.remote!=bool(remote))throw std::runtime_error("Stepper identity/topology is not the assigned STC1005");
        qualified_=true;
        release();verified(PhidgetStepper_setRescaleFactor_,PhidgetStepper_getRescaleFactor_,1,"Use native microsteps");
        caps_={};caps_.model=int(model);caps_.firmware=firmware;caps_.position_controller=true;caps_.software_position=definition_.run_mode;caps_.feed_forward=definition_.run_mode;caps_.current_limit=true;
        check(PhidgetStepper_getMinPosition_(h_,&caps_.minimum_position),"Read numeric minimum");check(PhidgetStepper_getMaxPosition_(h_,&caps_.maximum_position),"Read numeric maximum");
        check(PhidgetStepper_getMinCurrentLimit_(h_,&caps_.minimum_current),"Read current minimum");check(PhidgetStepper_getMaxCurrentLimit_(h_,&caps_.maximum_current),"Read current maximum");check(PhidgetStepper_getMinAcceleration_(h_,&caps_.minimum_acceleration),"Read acceleration minimum");check(PhidgetStepper_getMaxAcceleration_(h_,&caps_.maximum_acceleration),"Read acceleration maximum");check(PhidgetStepper_getMaxVelocityLimit_(h_,&caps_.maximum_velocity),"Read velocity maximum");
        check(PhidgetStepper_getMinVelocityLimit_(h_,&caps_.minimum_velocity),"Read velocity minimum");
        check(PhidgetStepper_getMinDataInterval_(h_,&caps_.minimum_interval_ms),"Read interval minimum");check(PhidgetStepper_getMaxDataInterval_(h_,&caps_.maximum_interval_ms),"Read interval maximum");check(PhidgetStepper_getMinFailsafeTime_(h_,&caps_.minimum_failsafe_ms),"Read watchdog minimum");check(PhidgetStepper_getMaxFailsafeTime_(h_,&caps_.maximum_failsafe_ms),"Read watchdog maximum");caps_.failsafe=true;auto result=caps_;if(definition_.run_mode){result.minimum_position=-9e12;result.maximum_position=9e12;result.minimum_velocity=result.minimum_acceleration=0;result.maximum_velocity=result.maximum_acceleration=1e9;}return result;
    }
    void configure(const MotorParameters& p)override{
        healthy();release();configured_=false;std::string reason;if(!stepper_definition_valid(definition_,true,reason))throw std::runtime_error(reason);
        const auto& d=definition_;if(d.current<caps_.minimum_current || d.current>caps_.maximum_current || d.holding_current<caps_.minimum_current || d.holding_current>d.current || d.velocity>caps_.maximum_velocity || d.acceleration<caps_.minimum_acceleration || d.acceleration>caps_.maximum_acceleration || d.interval_ms<caps_.minimum_interval_ms || d.interval_ms>caps_.maximum_interval_ms || d.failsafe_ms<caps_.minimum_failsafe_ms || d.failsafe_ms>caps_.maximum_failsafe_ms)throw std::runtime_error("Stepper setting outside attached controller capabilities");
        if(!d.run_mode && (!std::isfinite(p.velocity_limit) || !std::isfinite(p.acceleration) || p.velocity_limit<=0 || p.velocity_limit>caps_.maximum_velocity || p.acceleration<caps_.minimum_acceleration || p.acceleration>caps_.maximum_acceleration))throw std::runtime_error("STEP motion limits outside attached controller capabilities");
        const auto mode=d.run_mode?CONTROL_MODE_RUN:CONTROL_MODE_STEP;check(PhidgetStepper_setControlMode_(h_,mode),"Set stepper mode while disengaged");PhidgetStepper_ControlMode actual{};check(PhidgetStepper_getControlMode_(h_,&actual),"Read mode");if(actual!=mode)throw std::runtime_error("Stepper mode readback mismatch");
        verified(PhidgetStepper_setCurrentLimit_,PhidgetStepper_getCurrentLimit_,d.current,"Set current");verified(PhidgetStepper_setHoldingCurrentLimit_,PhidgetStepper_getHoldingCurrentLimit_,d.holding_current,"Set holding current");
        verified(PhidgetStepper_setAcceleration_,PhidgetStepper_getAcceleration_,d.run_mode?d.acceleration:p.acceleration,"Set acceleration");verified(PhidgetStepper_setVelocityLimit_,PhidgetStepper_getVelocityLimit_,d.run_mode?0:p.velocity_limit,"Set velocity");
        check(PhidgetStepper_setDataInterval_(h_,d.interval_ms),"Set report interval");check(PhidgetStepper_enableFailsafe_(h_,d.failsafe_ms),"Enable hardware watchdog");watchdog_=true;renew_=GetTickCount64();configured_=true;
    }
    void command(double u)override{healthy();if(!configured_ || !definition_.run_mode || !std::isfinite(u) || std::abs(u)>1)throw std::runtime_error("Invalid RUN-mode stepper velocity");check(PhidgetStepper_setVelocityLimit_(h_,u*definition_.velocity),"Set RUN velocity");check(PhidgetStepper_setEngaged_(h_,1),"Engage RUN mode");}
    double target(double value){healthy();if(!configured_ || definition_.run_mode || !std::isfinite(value) || value<caps_.minimum_position || value>caps_.maximum_position)throw std::runtime_error("Invalid STEP target");check(PhidgetStepper_setTargetPosition_(h_,value),"Set STEP target");double actual{};check(PhidgetStepper_getTargetPosition_(h_,&actual),"Read accepted STEP target");return actual;}
    void engage(){healthy();check(PhidgetStepper_setEngaged_(h_,1),"Engage stepper");}
    void limits(double v,double a){healthy();release();verified(PhidgetStepper_setAcceleration_,PhidgetStepper_getAcceleration_,a,"Set disengaged acceleration");verified(PhidgetStepper_setVelocityLimit_,PhidgetStepper_getVelocityLimit_,v,"Set velocity");}
    void velocity(double v){healthy();check(PhidgetStepper_setVelocityLimit_(h_,v),"Set STEP velocity");}
    double position(){healthy();double value{};check(PhidgetStepper_getPosition_(h_,&value),"Read generated step position");if(!std::isfinite(value))throw std::runtime_error("Invalid generated step position");return value;}
    double speed(){healthy();double value{};check(PhidgetStepper_getVelocity_(h_,&value),"Read generated step velocity");if(!std::isfinite(value))throw std::runtime_error("Invalid generated step velocity");return value;}
    bool moving(){healthy();int value{};check(PhidgetStepper_getIsMoving_(h_,&value),"Read generated motion");return value!=0;}
    void offset(double value){check(PhidgetStepper_addPositionOffset_(h_,value),"Set step coordinate");}
    void release()override{if(!h_)return;if(!qualified_)throw std::runtime_error("Unqualified controller; drive was not changed");check(PhidgetStepper_setEngaged_(h_,0),"Release stepper");int on{};check(PhidgetStepper_getEngaged_(h_,&on),"Verify release");if(on)throw std::runtime_error("Stepper drive release unconfirmed");}
    void renew()override{healthy();if(watchdog_){check(PhidgetStepper_resetFailsafe_(h_),"Renew stepper watchdog");renew_=GetTickCount64();}}
    MotorDriveObservation observation()override{healthy();if(!h_)return {};int engaged{};check(PhidgetStepper_getEngaged_(h_,&engaged),"Read drive");return {true,!engaged,speed()};}
    MotorPositionSample encoder(const MotorFeedbackDefinition&)override{return {};}// No measured shaft encoder exists on STC1005.
    void close()noexcept override{if(h_){try{release();}catch(...){}watchdog_=false;Phidget_setOnDetachHandler_(reinterpret_cast<PhidgetHandle>(h_),nullptr,nullptr);Phidget_setOnErrorHandler_(reinterpret_cast<PhidgetHandle>(h_),nullptr,nullptr);Phidget_close_(reinterpret_cast<PhidgetHandle>(h_));auto raw=reinterpret_cast<PhidgetHandle>(h_);Phidget_delete_(&raw);h_=nullptr;}configured_=watchdog_=qualified_=false;server_={};claim_={};}
};
class StepPosition final:public PositionMotorBackend {
    std::unique_ptr<StepperTransport> drive_;
    mutable std::mutex mutex_;std::condition_variable wake_;std::thread worker_;
    MotorSnapshot state_,published_;std::array<MotorCommandResult,64> results_{};
    enum class Op{inspect,configure,target,engage,hold,release,close,limits,reference,jog};
    struct Command {Op op;std::uint64_t id{},epoch{};MotorParameters parameters;double a{},b{},c{};};
    std::deque<Command> queue_;std::optional<Command> priority_;
    std::atomic<std::uint64_t> epoch_{1},jog_until_{};
    std::atomic<bool> enabled_{true};bool quit_{};
    std::uint64_t next_{},jog_next_{},submissions_{},coalesced_{},overflows_{};
    double jog_{};MotorParameters applied_;
    MotorCommandResult push(Op op,double a=0,double b=0,double c=0,MotorParameters p={}){
        std::lock_guard lock(mutex_);const auto id=++next_;++submissions_;
        const auto cancel=[&](const Command& old){results_[old.id%results_.size()]={old.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Superseded before execution"};};
        const bool priority=op==Op::release || op==Op::close || op==Op::hold;
        if(priority){++epoch_;for(const auto& old:queue_)cancel(old);queue_.clear();if(priority_)cancel(*priority_);priority_=Command{op,id,epoch_,std::move(p),a,b,c};}
        else {
            if(op==Op::target || op==Op::jog || op==Op::limits)std::erase_if(queue_,[&](const auto& old){if(old.op!=op)return false;cancel(old);++coalesced_;return true;});
            if(queue_.size()>=32){++overflows_;auto r=MotorCommandResult{id,MotorResultStage::rejected,MotorError::queue_full,0,"Stepper command queue full"};results_[id%results_.size()]=r;return r;}
            queue_.push_back({op,id,epoch_,std::move(p),a,b,c});
        }
        wake_.notify_one();return {id,MotorResultStage::queued};
    }
    void accepted(const Command& r,double desired,double actual){
        state_.desired_target=desired;state_.target=actual;state_.target_command_id=r.id;
        state_.target_accepted_ms=GetTickCount64();state_.target_revision_valid=true;
        state_.target_attained=false;state_.target_velocity=state_.applied.velocity_limit;state_.target_acceleration=state_.applied.acceleration;
    }
    void released(){jog_=0;drive_->release();state_.engaged=false;state_.drive=MotorDriveState::off;state_.target_attained=false;}
    void poll(std::uint64_t epoch){
        if(!state_.attached || state_.state==MotorState::fault)return;
        drive_->renew();state_.failsafe_renewed_ms=GetTickCount64();
        state_.position=drive_->position();const auto speed=drive_->speed();
        const auto observation=drive_->observation();state_.engaged=!observation.release_confirmed;
        state_.drive=state_.engaged?MotorDriveState::on:MotorDriveState::off;
        // Generated microsteps are not a measured shaft encoder or a fresh
        // expected-position event. Only the query time is advanced here.
        state_.expected_position.reset();state_.expected_event_valid=false;
        state_.feedback_valid=true;state_.checked_ms=GetTickCount64();state_.reference_velocity=speed;
        state_.target_attained=state_.engaged && state_.target_revision_valid && !drive_->moving() && std::abs(state_.position-state_.target)<.5;
        if(jog_ && enabled_ && epoch==epoch_){
            const auto now=GetTickCount64();
            if(now>=jog_until_){released();}
            else if(now>=jog_next_){
                state_.target_start_position=state_.position;state_.target=drive_->target(state_.position+jog_*.2);
                if(enabled_ && epoch==epoch_){drive_->engage();state_.engaged=true;state_.drive=MotorDriveState::on;}
                state_.target_attained=false;jog_next_=now+100;
            }
        }
        state_.state=!state_.configured?MotorState::unreferenced:!state_.engaged?MotorState::ready:jog_?MotorState::jogging:state_.target_attained?MotorState::holding:MotorState::moving;
    }
    MotorCommandResult execute(const Command& r){
        auto result=MotorCommandResult{r.id,MotorResultStage::sdk_succeeded};
        const auto reject=[&](MotorError error,const char* text){return MotorCommandResult{r.id,MotorResultStage::rejected,error,0,text};};
        if(r.epoch!=epoch_)return {r.id,MotorResultStage::cancelled,MotorError::cancelled};
        if(!enabled_ && r.op!=Op::close && r.op!=Op::release)return reject(MotorError::not_ready,"Phidgets support disabled");
        switch(r.op){
        case Op::inspect:
            jog_=0;state_={};state_.capabilities=drive_->open();state_.generation=stepper_generation.fetch_add(1);
            state_.attached=true;state_.state=MotorState::unreferenced;break;
        case Op::configure:
            if(!state_.attached || state_.state==MotorState::fault){
                drive_->close();state_={};state_.capabilities=drive_->open();state_.generation=stepper_generation.fetch_add(1);state_.attached=true;
            }
            drive_->configure(r.parameters);jog_=0;state_.requested=state_.applied=applied_=r.parameters;
            state_.configured=true;state_.target_revision_valid=state_.target_attained=false;
            state_.engaged=false;state_.drive=MotorDriveState::off;state_.fault.clear();state_.fault_code=0;state_.fault_kind=MotorFaultKind::none;
            state_.watchdog_active=true;state_.state=MotorState::ready;++state_.configuration_revision;break;
        case Op::target:
            if(!state_.configured)return reject(MotorError::not_ready,"Stepper configuration required");
            if(!std::isfinite(r.a) || r.a<state_.capabilities.minimum_position || r.a>state_.capabilities.maximum_position)return reject(MotorError::invalid_argument,"STEP target outside controller numeric range");
            jog_=0;drive_->velocity(applied_.velocity_limit);state_.applied.velocity_limit=applied_.velocity_limit;
            state_.target_start_position=drive_->position();accepted(r,r.a,drive_->target(r.a));break;
        case Op::engage:
            if(!state_.configured || !state_.target_revision_valid)return reject(MotorError::not_ready,"Configured STEP target required");
            drive_->engage();state_.engaged=true;state_.drive=MotorDriveState::on;break;
        case Op::hold:
            jog_=0;if(state_.attached && state_.configured){const auto position=drive_->position();state_.target_start_position=position;accepted(r,position,drive_->target(position));}break;
        case Op::release:
            jog_=0;if(state_.attached)released();break;
        case Op::close:{
            bool off=state_.drive==MotorDriveState::off;std::string warning;jog_=0;
            if(state_.attached)try{released();off=true;}catch(const MotorDriveFailure& e){off=false;warning="Channel closed; drive release unconfirmed: "+std::string(e.what());result.sdk_code=e.code;}catch(const std::exception& e){off=false;warning="Channel closed; drive release unconfirmed: "+std::string(e.what());}
            drive_->close();state_.attached=state_.configured=state_.feedback_valid=state_.engaged=state_.watchdog_active=false;
            state_.state=MotorState::closed;state_.drive=off?MotorDriveState::off:MotorDriveState::unknown;state_.target_attained=state_.target_revision_valid=false;
            if(!warning.empty()){state_.fault=warning;result.reason=warning;}break;}
        case Op::limits:
            if(!state_.configured)return reject(MotorError::not_ready,"Stepper configuration required");
            if(!std::isfinite(r.a) || !std::isfinite(r.b) || r.a<state_.capabilities.minimum_velocity || r.a>state_.capabilities.maximum_velocity || r.b<state_.capabilities.minimum_acceleration || r.b>state_.capabilities.maximum_acceleration)return reject(MotorError::invalid_argument,"STEP motion limits outside controller capabilities");
            if(r.b!=state_.applied.acceleration && state_.engaged){released();return reject(MotorError::busy,"Drive released; change stepper acceleration while disengaged");}
            if(r.b!=state_.applied.acceleration)drive_->limits(r.a,r.b);else drive_->velocity(r.a);
            state_.applied.velocity_limit=applied_.velocity_limit=r.a;state_.applied.acceleration=applied_.acceleration=r.b;break;
        case Op::reference:
            if(!state_.attached)return reject(MotorError::not_ready,"Stepper not attached");
            if(!std::isfinite(r.a) || r.a<state_.capabilities.minimum_position || r.a>state_.capabilities.maximum_position)return reject(MotorError::invalid_argument,"STEP zero outside numeric range");
            released();drive_->offset(r.a-drive_->position());state_.referenced=true;state_.target_revision_valid=false;state_.target=state_.desired_target=r.a;++state_.coordinate_revision;break;
        case Op::jog:
            if(!state_.configured)return reject(MotorError::not_ready,"Stepper configuration required");
            if(!std::isfinite(r.a) || std::abs(r.a)>applied_.velocity_limit)return reject(MotorError::invalid_argument,"STEP jog exceeds configured velocity");
            if(r.a==0){jog_=0;const auto position=drive_->position();accepted(r,position,drive_->target(position));break;}
            drive_->velocity(std::abs(r.a));state_.applied.velocity_limit=std::abs(r.a);
            jog_=r.a;jog_until_=GetTickCount64()+250;jog_next_=0;state_.target_revision_valid=false;break;
        }
        return result;
    }
    void run(){
        std::unique_lock lock(mutex_);
        while(!quit_){
            wake_.wait_for(lock,std::chrono::milliseconds(25),[&]{return quit_ || priority_.has_value() || !queue_.empty();});if(quit_)break;
            std::optional<Command> request;if(priority_){request=priority_;priority_.reset();}else if(!queue_.empty()){request=queue_.front();queue_.pop_front();}
            const auto epoch=epoch_.load();lock.unlock();MotorCommandResult result;
            try{
                if(!enabled_ && state_.attached && state_.engaged)released();
                if(request)result=execute(*request);
                poll(epoch);
            }catch(const std::exception& e){
                const auto* native=dynamic_cast<const MotorDriveFailure*>(&e);const int code=native?native->code:0;
                jog_=0;try{drive_->release();state_.drive=MotorDriveState::off;}catch(...){state_.drive=MotorDriveState::unknown;}
                state_.engaged=state_.configured=state_.feedback_valid=false;state_.target_attained=state_.target_revision_valid=false;
                state_.fault=e.what();state_.fault_code=code;state_.fault_error=MotorError::sdk_error;state_.fault_kind=MotorFaultKind::sdk;state_.state=MotorState::fault;
                if(request)result={request->id,MotorResultStage::rejected,MotorError::sdk_error,code,e.what()};
            }
            lock.lock();if(request){results_[request->id%results_.size()]=result;state_.last_result=result;}published_=state_;
        }
        lock.unlock();drive_->close();
    }
public:
    explicit StepPosition(std::unique_ptr<StepperTransport> d):drive_(std::move(d)){worker_=std::thread([this]{run();});}
    ~StepPosition(){{std::lock_guard lock(mutex_);quit_=true;wake_.notify_one();}worker_.join();}
    bool target_before_engage()const noexcept override{return true;}
    MotorCommandResult inspect()override{return push(Op::inspect);}
    MotorCommandResult configure(MotorParameters p)override{return push(Op::configure,0,0,0,std::move(p));}
    MotorCommandResult tuning(double,double,double)override{return {0,MotorResultStage::rejected,MotorError::unsupported,0,"STEP mode has no PID gains"};}
    MotorCommandResult limits(double v,double a)override{return push(Op::limits,v,a);}
    MotorCommandResult target(double p)override{return push(Op::target,p);}
    MotorCommandResult reference(double z,double a,double b)override{return push(Op::reference,z,a,b);}
    MotorCommandResult engage()override{return push(Op::engage);}
    MotorCommandResult jog(double v)override{return push(Op::jog,v);}
    MotorCommandResult wave(MotorWaveform,double)override{return {0,MotorResultStage::rejected,MotorError::unsupported};}
    MotorCommandResult renew()override{jog_until_=GetTickCount64()+250;return {0,MotorResultStage::host_applied};}
    MotorCommandResult stop()override{return push(Op::hold);}
    MotorCommandResult disengage()override{return push(Op::release);}
    MotorCommandResult close()override{return push(Op::close);}
    MotorSnapshot snapshot()const override{std::lock_guard lock(mutex_);auto s=published_;s.command_submissions=submissions_;s.command_coalesced=coalesced_;s.command_overflows=overflows_;return s;}
    std::optional<MotorCommandResult> result(std::uint64_t id)const override{std::lock_guard lock(mutex_);const auto& r=results_[id%results_.size()];return r.id==id?std::optional(r):std::nullopt;}
};
}
std::shared_ptr<PositionMotorBackend> phidget_stepper_backend(std::shared_ptr<PhidgetSession> sdk,const StepperDefinition& d,std::optional<PhidgetServer> server,std::function<MotorPositionSample(const MotorFeedbackDefinition&)> feedback){
    auto transport=std::make_unique<StepperTransport>(std::move(sdk),d,std::move(server));if(d.run_mode)return std::make_shared<SoftwarePositionMotor>(std::move(transport),std::move(feedback));return std::make_shared<StepPosition>(std::move(transport));
}
}
