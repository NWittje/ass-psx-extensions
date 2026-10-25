#include "hwio/phidget_motor.hpp"
#include <phidget22.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <cstring>

namespace hwio {
namespace {
struct SystemMotorClock final:MotorClock {std::uint64_t now_ms() const noexcept override{return GetTickCount64();}};
enum class Operation { open,inspect,update,tuning,limits,reference,engage,absolute,relative,jog,wave,renew,stop,disengage,close };
bool movement(Operation op){return op==Operation::reference || op==Operation::engage || op==Operation::absolute || op==Operation::relative || op==Operation::jog || op==Operation::wave || op==Operation::renew;}
bool finite(double value){return std::isfinite(value);}
bool parameters_valid(const MotorParameters& p) {
    for(const auto value:{p.kp,p.ki,p.kd,p.rescale,p.deadband,p.velocity_limit,p.acceleration,p.minimum,p.maximum,p.jog_lead})if(!finite(value))return false;
    return p.rescale!=0 && p.deadband>=0 && p.velocity_limit>0 && p.acceleration>0 && (!p.current_limit || (finite(*p.current_limit) && *p.current_limit>=0)) && (p.numeric_range_only || p.minimum<p.maximum) && p.jog_lead>0 &&
        !p.units.empty() && p.units.size()<=64 && p.feedback_interval_ms && p.failsafe_ms && p.failsafe_refresh_ms && p.failsafe_refresh_ms<=p.failsafe_ms/3 && p.jog_interval_ms && p.jog_lease_ms>p.jog_interval_ms &&
        (!p.current_regulator_gain || (finite(*p.current_regulator_gain) && *p.current_regulator_gain>0)) && (!p.feed_forward || finite(*p.feed_forward));
}
template<class T>void load(const PhidgetSession& session,T& value,const char* name,bool required=true){const auto proc=session.symbol(name,required);static_assert(sizeof(value)==sizeof(proc));memcpy(&value,&proc,sizeof(value));}
struct Api {
    #define F(name) decltype(&name) name##_{};
    F(PhidgetMotorPositionController_create) F(Phidget_close) F(Phidget_delete) F(Phidget_open)
    F(Phidget_setDeviceSerialNumber) F(Phidget_setChannel) F(Phidget_setHubPort) F(Phidget_setIsHubPortDevice) F(Phidget_setIsLocal) F(Phidget_setIsRemote) F(Phidget_setServerName)
    F(Phidget_getDeviceSerialNumber) F(Phidget_getChannel) F(Phidget_getHubPort) F(Phidget_getIsRemote) F(Phidget_getChannelClass) F(Phidget_getDeviceID) F(Phidget_getDeviceVersion)
    F(Phidget_getParent) F(Phidget_release)
    F(Phidget_setOnAttachHandler) F(Phidget_setOnDetachHandler) F(Phidget_setOnErrorHandler)
    F(PhidgetMotorPositionController_setOnPositionChangeHandler)
    F(PhidgetMotorPositionController_setEngaged) F(PhidgetMotorPositionController_getEngaged)
    F(PhidgetMotorPositionController_setTargetPosition) F(PhidgetMotorPositionController_getTargetPosition) F(PhidgetMotorPositionController_getPosition) F(PhidgetMotorPositionController_addPositionOffset)
    #define PAIR(name) F(PhidgetMotorPositionController_set##name) F(PhidgetMotorPositionController_get##name)
    PAIR(Kp) PAIR(Ki) PAIR(Kd) PAIR(RescaleFactor) PAIR(DeadBand) PAIR(VelocityLimit) PAIR(Acceleration) PAIR(CurrentLimit) PAIR(DataInterval)
    PAIR(NormalizePID) PAIR(IOMode) PAIR(PositionType) PAIR(CurrentRegulatorGain)
    #undef PAIR
    #define BOUNDS(name) F(PhidgetMotorPositionController_getMin##name) F(PhidgetMotorPositionController_getMax##name)
    BOUNDS(Acceleration) BOUNDS(VelocityLimit) BOUNDS(CurrentLimit) BOUNDS(CurrentRegulatorGain) BOUNDS(Position) BOUNDS(DataInterval) BOUNDS(FailsafeTime)
    #undef BOUNDS
    F(PhidgetMotorPositionController_enableFailsafe) F(PhidgetMotorPositionController_resetFailsafe)
    F(PhidgetMotorPositionController_getExpectedPosition) F(PhidgetMotorPositionController_getDutyCycle)
    F(PhidgetMotorPositionController_setEnableExpectedPosition) F(PhidgetMotorPositionController_getEnableExpectedPosition)
    F(PhidgetMotorPositionController_setOnExpectedPositionChangeHandler)
    #undef F
    explicit Api(const PhidgetSession& session) {
        #define L(name) load(session,name##_,#name);
        L(PhidgetMotorPositionController_create) L(Phidget_close) L(Phidget_delete) L(Phidget_open)
        L(Phidget_setDeviceSerialNumber) L(Phidget_setChannel) L(Phidget_setHubPort) L(Phidget_setIsHubPortDevice) L(Phidget_setIsLocal) L(Phidget_setIsRemote) L(Phidget_setServerName)
        L(Phidget_getDeviceSerialNumber) L(Phidget_getChannel) L(Phidget_getHubPort) L(Phidget_getIsRemote) L(Phidget_getChannelClass) L(Phidget_getDeviceID) L(Phidget_getDeviceVersion)
        L(Phidget_getParent) L(Phidget_release)
        L(Phidget_setOnAttachHandler) L(Phidget_setOnDetachHandler) L(Phidget_setOnErrorHandler) L(PhidgetMotorPositionController_setOnPositionChangeHandler)
        L(PhidgetMotorPositionController_setEngaged) L(PhidgetMotorPositionController_getEngaged) L(PhidgetMotorPositionController_setTargetPosition) L(PhidgetMotorPositionController_getTargetPosition)
        L(PhidgetMotorPositionController_getPosition) L(PhidgetMotorPositionController_addPositionOffset)
        #define LP(name) L(PhidgetMotorPositionController_set##name) L(PhidgetMotorPositionController_get##name)
        LP(Kp) LP(Ki) LP(Kd) LP(RescaleFactor) LP(DeadBand) LP(VelocityLimit) LP(Acceleration) LP(DataInterval)
        #undef LP
        #define LB(name) L(PhidgetMotorPositionController_getMin##name) L(PhidgetMotorPositionController_getMax##name)
        LB(Acceleration) LB(VelocityLimit) LB(Position) LB(DataInterval) LB(FailsafeTime)
        #undef LB
        L(PhidgetMotorPositionController_enableFailsafe) L(PhidgetMotorPositionController_resetFailsafe)
        #undef L
        #define O(name) load(session,name##_,#name,false);
        #define OP(name) O(PhidgetMotorPositionController_set##name) O(PhidgetMotorPositionController_get##name)
        OP(CurrentLimit) OP(NormalizePID) OP(IOMode) OP(PositionType) OP(CurrentRegulatorGain)
        #undef OP
        O(PhidgetMotorPositionController_getMinCurrentLimit) O(PhidgetMotorPositionController_getMaxCurrentLimit)
        O(PhidgetMotorPositionController_getMinCurrentRegulatorGain) O(PhidgetMotorPositionController_getMaxCurrentRegulatorGain)
        O(PhidgetMotorPositionController_getExpectedPosition) O(PhidgetMotorPositionController_getDutyCycle)
        O(PhidgetMotorPositionController_setEnableExpectedPosition) O(PhidgetMotorPositionController_getEnableExpectedPosition)
        O(PhidgetMotorPositionController_setOnExpectedPositionChangeHandler)
        #undef O
    }
};
}
struct PhidgetMotor::Impl {
    std::shared_ptr<PhidgetSession> session;
    MotorAddress address;
    std::shared_ptr<MotorClock> clock;
    std::unique_ptr<Api> api;
    PhidgetLease server,claim;
    PhidgetMotorPositionControllerHandle handle{};
    std::thread worker;
    HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    std::atomic<bool> shutdown{},enabled{true},terminated{};
    std::atomic<std::uint64_t> motion_epoch{1},channel_epoch{1};
    std::mutex queue_mutex;
    mutable std::mutex view_mutex;
    struct Command {Operation op{};std::uint64_t id{},epoch{},channel_epoch{},generation{},stamp{};double value{},minimum{},maximum{};MotorParameters parameters;unsigned retries{};};
    struct Event {unsigned kind{};double position{};int code{};std::uint64_t stamp{};};
    std::array<Command,64> commands;
    std::optional<Command> priority;
    std::optional<Command> retry;std::uint64_t retry_at{},retry_start{},executing_id{};unsigned retry_count{};int deferred_code{};bool retry_context{};
    MotorGuardSnapshot pending_guard,travel_guard;bool guard_pending{};
    unsigned release_attempts{};int release_error{};MotorError target_error{MotorError::sdk_error};
    std::uint64_t opening_deadline{},reopen_after{};bool recovery_attempted{};
    std::array<std::optional<Command>,4> coalesced; // Limits, absolute target, renewal, full update.
    std::array<Event,128> events;
    std::size_t command_read{},command_write{},event_read{},event_write{};
    unsigned lost{}; // Ordered command/lifecycle loss is distinct from coalesced position observations.
    bool position_pending{},callback_position_known{},position_invalid{};double callback_position{};std::uint64_t position_activity{};
    bool expected_pending{};double callback_expected{};std::uint64_t expected_activity{},expected_sequence{};int original_expected_enable{-1};
    std::atomic<std::uint64_t> submissions{},coalesces{},overflows{};
    std::uint64_t next_id{},opening_id{},active_id{},active_epoch{},last_feedback{},still_since{},last_failsafe{},jog_at{},jog_renewed{};
    double previous_position{},jog_speed{},jog_accumulator{};
    bool position_known{},ever_engaged{},attachment_seen{},watchdog{},inspection{},recover_failsafe{};
    MotorWaveform waveform{};double wave_amplitude{};std::uint64_t wave_start{};
    MotorError configuration_error{MotorError::sdk_error};
    MotorSnapshot view,published;
    MotorCommandResult latest;
    std::array<MotorCommandResult,256> results;
    Impl(std::shared_ptr<PhidgetSession> s,MotorAddress a,MotorParameters p,std::shared_ptr<MotorClock> c):session(std::move(s)),address(std::move(a)),clock(c?std::move(c):std::make_shared<SystemMotorClock>()) {
        if(!session || !event)throw std::runtime_error("Cannot create Phidget motor service");
        view.requested=std::move(p);published=view;worker=std::thread([this]{run();});
    }
    ~Impl(){shutdown=true;SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    void report(MotorCommandResult result){std::lock_guard lock(view_mutex);results[result.id%results.size()]=result;latest=std::move(result);}
    void done(std::uint64_t id,MotorResultStage stage=MotorResultStage::sdk_succeeded,MotorError error=MotorError::none,int code=0,std::string reason={}){if(deferred_code && retry_context && id==executing_id)return;if(id)report({id,stage,error,code,std::move(reason)});}
    void publish(){std::lock_guard lock(view_mutex);view.last_result=latest;view.command_submissions=submissions;view.command_coalesced=coalesces;view.command_overflows=overflows;view.watchdog_active=watchdog;view.failsafe_renewed_ms=last_failsafe;published=view;}
    MotorCommandResult submit(Operation operation,double value=0,double minimum=0,double maximum=0,MotorParameters parameters={}) {
        Command command;command.op=operation;command.value=value;command.minimum=minimum;command.maximum=maximum;command.parameters=std::move(parameters);command.stamp=clock->now_ms();
        const bool safety=operation==Operation::stop || operation==Operation::disengage || operation==Operation::close;
        std::lock_guard lock(queue_mutex);
        command.id=++next_id;{std::lock_guard guard(view_mutex);command.generation=published.generation;}
        if(terminated){MotorCommandResult result{command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Motor worker has stopped"};report(result);return result;}
        command.channel_epoch=operation==Operation::close?++channel_epoch:channel_epoch.load();
        command.epoch=(safety || operation==Operation::update)?++motion_epoch:motion_epoch.load();
        int mailbox=operation==Operation::limits?0:operation==Operation::absolute?1:operation==Operation::renew?2:operation==Operation::update?3:-1;
        const auto result=MotorCommandResult{command.id,MotorResultStage::queued};report(result);
        auto replace=[&](std::optional<Command>& pending){if(pending){done(pending->id,MotorResultStage::cancelled,MotorError::cancelled,0,"Superseded by latest motor command");++coalesces;}pending=std::move(command);};
        ++submissions;
        if(safety){
            if(priority && (priority->op==Operation::close || (priority->op==Operation::disengage && operation==Operation::stop)))done(command.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Stronger safety operation pending");
            else replace(priority);
        }else if(mailbox>=0)replace(coalesced[unsigned(mailbox)]);
        else if(command_write-command_read==commands.size()){lost|=1;++overflows;SetEvent(event);MotorCommandResult rejected{command.id,MotorResultStage::rejected,MotorError::queue_full,0,"Motor command queue overflow; motor will disengage"};report(rejected);return rejected;}
        else commands[command_write++%commands.size()]=std::move(command);
        SetEvent(event);return result;
    }
    void enqueue(unsigned kind,double value=0,int code=0) noexcept {
        std::lock_guard lock(queue_mutex);
        if(kind==4){
            // Position is latest-state feedback, not an ordered input edge. Preserve
            // activity and invalid samples even if a slow SDK call spans a burst.
            position_pending=true;
            if(!finite(value))position_invalid=true;
            else {if(!callback_position_known || value!=callback_position)position_activity=clock->now_ms();callback_position=value;callback_position_known=true;}
        }else if(kind==5){expected_pending=true;callback_expected=value;expected_activity=clock->now_ms();++expected_sequence;}
        else if(event_write-event_read==events.size())lost|=2;
        else events[event_write++%events.size()]={kind,value,code,clock->now_ms()};
        SetEvent(event);
    }
    static void CCONV attached(PhidgetHandle,void* ctx){static_cast<Impl*>(ctx)->enqueue(1);}
    static void CCONV detached(PhidgetHandle,void* ctx){static_cast<Impl*>(ctx)->enqueue(2);}
    static void CCONV error(PhidgetHandle,void* ctx,Phidget_ErrorEventCode code,const char*){static_cast<Impl*>(ctx)->enqueue(3,0,static_cast<int>(code));}
    static void CCONV position(PhidgetMotorPositionControllerHandle,void* ctx,double value){static_cast<Impl*>(ctx)->enqueue(4,value);}
    static void CCONV expected(PhidgetMotorPositionControllerHandle,void* ctx,double value){static_cast<Impl*>(ctx)->enqueue(5,value);}
    PhidgetHandle base() const{return reinterpret_cast<PhidgetHandle>(handle);}
    void cancel_active(const std::string& reason){done(active_id,MotorResultStage::cancelled,MotorError::cancelled,0,reason);active_id=0;jog_speed=0;view.wave_active=false;view.target_attained=false;}
    int release_drive(){
        cancel_active("Motor disengaged");int code{};
        if((!handle || !view.attached) && view.drive!=MotorDriveState::off){view.drive=MotorDriveState::unknown;view.engaged=false;release_error=-1;return release_error;} // No attached SDK session can confirm release.

        if(handle && view.attached && (view.engaged || view.drive!=MotorDriveState::off)){
            if(release_attempts>=2 && view.drive==MotorDriveState::unknown)return release_error?release_error:EPHIDGET_UNEXPECTED;
            ++release_attempts;code=api->PhidgetMotorPositionController_setEngaged_(handle,0);
            if(!code){int engaged{};code=api->PhidgetMotorPositionController_getEngaged_(handle,&engaged);if(!code && engaged)code=EPHIDGET_UNEXPECTED;}
        }
        release_error=code;view.drive=code?MotorDriveState::unknown:MotorDriveState::off;view.engaged=code!=0;
        if(!code){release_attempts=0;if(view.state!=MotorState::fault)view.state=view.referenced?MotorState::ready:MotorState::unreferenced;}
        return code;
    }
    void fault(std::string reason,int code=0,MotorError error_kind=MotorError::sdk_error){
        if(deferred_code && retry_context)return;
        // A software channel that never attached cannot have accepted a drive
        // command. Close that failed open before releasing its physical claim;
        // an absent DCC must not reserve the VINT port of a replacement Stepper.
        const bool never_attached=handle && !attachment_seen;
        ++motion_epoch;if(never_attached)cancel_active("Motor channel never attached");
        const int release_code=never_attached?0:release_drive();
        recover_failsafe=!never_attached && !recovery_attempted && (code==EPHIDGET_FAILSAFE || code==EEPHIDGET_FAILSAFE || release_code==EPHIDGET_FAILSAFE || view.drive==MotorDriveState::unknown);
        if(recover_failsafe)reopen_after=clock->now_ms()+(view.drive==MotorDriveState::unknown?view.applied.failsafe_ms:0);
        view.fault_kind=!never_attached && view.drive==MotorDriveState::unknown?MotorFaultKind::unknown_drive:error_kind==MotorError::queue_full?MotorFaultKind::continuity:MotorFaultKind::sdk;
        reason+=" [session "+std::to_string(view.generation)+", config "+std::to_string(view.configuration_revision)+", queue "+std::to_string(submissions.load())+", coalesced "+std::to_string(coalesces.load())+", overflow "+std::to_string(overflows.load())+", watchdog "+std::to_string(last_failsafe)+", native "+std::to_string(code)+", release "+std::to_string(release_code)+"]";
        view.configured=false;view.referenced=false;view.feedback_valid=false;view.expected_position.reset();view.duty_cycle.reset();view.state=MotorState::fault;view.fault=std::move(reason);view.fault_error=error_kind;view.fault_code=code;
        view.last_fault=view.fault;view.last_fault_code=code;view.last_fault_ms=clock->now_ms();
        const auto failed_opening=opening_id;opening_id=0;
        if(never_attached){close_handle();view.state=MotorState::fault;view.drive=MotorDriveState::off;}
        // A completed failed-open receipt must also mean its native reservation
        // is gone. Callers may immediately open another channel class.
        if(failed_opening)done(failed_opening,MotorResultStage::rejected,error_kind,code,view.fault);
    }
    bool call(PhidgetReturnCode code,const char* operation){if(!code){if(watchdog && clock->now_ms()-last_failsafe>=view.requested.failsafe_refresh_ms){const auto renewal=api->PhidgetMotorPositionController_resetFailsafe_(handle);if(renewal){fault("Renew failsafe during SDK operation",int(renewal));return false;}last_failsafe=clock->now_ms();}return true;}if(retry_context && (code==EPHIDGET_BUSY || code==EPHIDGET_AGAIN || code==EPHIDGET_INTERRUPTED || code==EPHIDGET_TIMEOUT)){deferred_code=int(code);view.fault_code=deferred_code;view.fault=std::string(operation)+" temporarily unavailable; retrying";return false;}fault(std::string(operation)+" failed ("+std::to_string(code)+")",static_cast<int>(code));return false;}
    void close_handle(){
        recover_failsafe=false;
        done(opening_id,MotorResultStage::cancelled,MotorError::cancelled,0,"Motor session closed");opening_id=0;
        if(handle){release_drive();api->Phidget_setOnAttachHandler_(base(),nullptr,nullptr);api->Phidget_setOnDetachHandler_(base(),nullptr,nullptr);api->Phidget_setOnErrorHandler_(base(),nullptr,nullptr);api->PhidgetMotorPositionController_setOnPositionChangeHandler_(handle,nullptr,nullptr);
            if(api->PhidgetMotorPositionController_setOnExpectedPositionChangeHandler_)api->PhidgetMotorPositionController_setOnExpectedPositionChangeHandler_(handle,nullptr,nullptr);
            if(original_expected_enable>=0 && view.attached && api->PhidgetMotorPositionController_setEnableExpectedPosition_)api->PhidgetMotorPositionController_setEnableExpectedPosition_(handle,original_expected_enable);
            api->Phidget_close_(base());auto generic=base();api->Phidget_delete_(&generic);handle=nullptr;}
        // SDK close/delete quiesces callbacks before their context or DLL lease dies.
        claim={};server={};view.attached=false;view.engaged=false;view.target_revision_valid=false;view.configured=false;view.referenced=false;view.feedback_valid=false;view.expected_position.reset();view.duty_cycle.reset();watchdog=false;position_known=false;ever_engaged=false;attachment_seen=false;
        ++view.generation;if(view.drive!=MotorDriveState::unknown)view.drive=MotorDriveState::off;release_attempts=0;view.state=MotorState::closed;
        view.expected_event_valid=view.expected_feedback_enabled=false;original_expected_enable=-1;
        std::lock_guard lock(queue_mutex);event_read=event_write;position_pending=position_invalid=callback_position_known=expected_pending=false;position_activity=0;
    }
    void begin_open(std::uint64_t id,bool inspect_only=false){
        inspection=inspect_only;if(id)recovery_attempted=false;
        if(!enabled){done(id,MotorResultStage::rejected,MotorError::not_ready,0,"Phidget provider is disabled");return;}
        if(handle){done(id,MotorResultStage::rejected,MotorError::busy,0,"Motor channel is already open");return;}
        attachment_seen=false;
        try{
            if(address.resource.serial<=0 || address.resource.hub_port<-1 || address.resource.hub_port>5 || address.resource.channel<0 || address.model<0 || address.minimum_firmware<0)throw std::runtime_error("Invalid exact motor address");
            if(!inspect_only && !parameters_valid(view.requested))throw std::runtime_error("Motor parameters require finite explicit tuning, travel and timing limits");
            if(view.requested.feed_forward){done(id,MotorResultStage::rejected,MotorError::unsupported,0,"Phidget22 position controllers expose no feed-forward gain");return;}
            if(!api)api=std::make_unique<Api>(*session);
            claim=session->claim(address.resource,"PhidgetMotor");
            if(address.remote)server=session->server(address.server);
            if(!call(api->PhidgetMotorPositionController_create_(&handle),"Create position controller"))throw std::runtime_error(view.fault);
            opening_id=id;opening_deadline=clock->now_ms()+5000;view.state=MotorState::opening;view.drive=MotorDriveState::unknown;view.fault.clear();
            if(!call(api->Phidget_setDeviceSerialNumber_(base(),address.resource.serial),"Set serial") || !call(api->Phidget_setChannel_(base(),address.resource.channel),"Set motor channel"))return;
            if(address.resource.hub_port>=0 && (!call(api->Phidget_setHubPort_(base(),address.resource.hub_port),"Set hub port") || !call(api->Phidget_setIsHubPortDevice_(base(),0),"Select VINT module")))return;
            if(!call(address.remote?api->Phidget_setIsRemote_(base(),1):api->Phidget_setIsLocal_(base(),1),"Select transport"))return;
            if(address.remote && !call(api->Phidget_setServerName_(base(),server.server_name.c_str()),"Select server"))return;
            if(!call(api->Phidget_setOnAttachHandler_(base(),attached,this),"Attach handler") || !call(api->Phidget_setOnDetachHandler_(base(),detached,this),"Detach handler") || !call(api->Phidget_setOnErrorHandler_(base(),error,this),"Error handler") || !call(api->PhidgetMotorPositionController_setOnPositionChangeHandler_(handle,position,this),"Position handler") || !call(api->Phidget_open_(base()),"Open motor"))return;
        }catch(const std::exception& ex){if(handle)close_handle();claim={};server={};view.state=MotorState::fault;view.fault=ex.what();opening_id=0;done(id,MotorResultStage::rejected,MotorError::sdk_error,0,view.fault);}
    }
    bool discover_capabilities(){
        auto& cap=view.capabilities;cap={};int serial{},channel{},hub{},remote{},firmware{};Phidget_DeviceID model{};Phidget_ChannelClass cls{};
        if(!call(api->Phidget_getDeviceSerialNumber_(base(),&serial),"Read serial") || !call(api->Phidget_getChannel_(base(),&channel),"Read channel") || !call(api->Phidget_getIsRemote_(base(),&remote),"Read transport") || !call(api->Phidget_getDeviceID_(base(),&model),"Read model") || !call(api->Phidget_getDeviceVersion_(base(),&firmware),"Read firmware") || !call(api->Phidget_getChannelClass_(base(),&cls),"Read channel class"))return false;
        if(serial!=address.resource.serial || channel!=address.resource.channel || bool(remote)!=address.remote || cls!=PHIDCHCLASS_MOTORPOSITIONCONTROLLER || (address.model && address.model!=static_cast<int>(model)) || firmware<address.minimum_firmware){fault("Attached hardware does not match the exact motor address/model/firmware");return false;}
        if(address.resource.hub_port>=0 && (!call(api->Phidget_getHubPort_(base(),&hub),"Read hub port") || hub!=address.resource.hub_port)){fault("Attached motor is on a different hub port");return false;}
        if(address.resource.hub_port<0){
            PhidgetHandle device{},parent{};const auto device_result=api->Phidget_getParent_(base(),&device);
            const auto parent_result=device_result==EPHIDGET_OK && device?api->Phidget_getParent_(device,&parent):EPHIDGET_UNEXPECTED;
            const bool vint=parent!=nullptr;if(parent)api->Phidget_release_(&parent);if(device)api->Phidget_release_(&device);
            if(device_result || parent_result || vint){fault("Direct motor address requires a direct device; VINT motors require an exact hub port");return false;}
        }
        cap.position_controller=true;cap.model=static_cast<int>(model);cap.firmware=firmware;
        int value{};double numeric{};Phidget_EncoderIOMode io{};Phidget_PositionType type{};
        auto supported=[&](PhidgetReturnCode result){if(result==EPHIDGET_UNSUPPORTED)return false;if(result!=EPHIDGET_OK && result!=EPHIDGET_UNKNOWNVAL)fault("Cannot establish motor capability",static_cast<int>(result));return result==EPHIDGET_OK || result==EPHIDGET_UNKNOWNVAL;};
        cap.normalize_pid=api->PhidgetMotorPositionController_setNormalizePID_ && api->PhidgetMotorPositionController_getNormalizePID_ && supported(api->PhidgetMotorPositionController_getNormalizePID_(handle,&value));
        cap.encoder_mode=api->PhidgetMotorPositionController_setIOMode_ && api->PhidgetMotorPositionController_getIOMode_ && supported(api->PhidgetMotorPositionController_getIOMode_(handle,&io));
        cap.feedback_source=api->PhidgetMotorPositionController_setPositionType_ && api->PhidgetMotorPositionController_getPositionType_ && supported(api->PhidgetMotorPositionController_getPositionType_(handle,&type));
        cap.current_regulator_gain=api->PhidgetMotorPositionController_setCurrentRegulatorGain_ && api->PhidgetMotorPositionController_getCurrentRegulatorGain_ && api->PhidgetMotorPositionController_getMinCurrentRegulatorGain_ && api->PhidgetMotorPositionController_getMaxCurrentRegulatorGain_ && supported(api->PhidgetMotorPositionController_getCurrentRegulatorGain_(handle,&numeric));
        cap.expected_position=api->PhidgetMotorPositionController_getExpectedPosition_ && supported(api->PhidgetMotorPositionController_getExpectedPosition_(handle,&numeric));
        cap.duty_cycle=api->PhidgetMotorPositionController_getDutyCycle_ && supported(api->PhidgetMotorPositionController_getDutyCycle_(handle,&numeric));
        cap.current_limit=api->PhidgetMotorPositionController_setCurrentLimit_ && api->PhidgetMotorPositionController_getCurrentLimit_ && api->PhidgetMotorPositionController_getMinCurrentLimit_ && api->PhidgetMotorPositionController_getMaxCurrentLimit_ && supported(api->PhidgetMotorPositionController_getMinCurrentLimit_(handle,&cap.minimum_current));
        if(cap.current_limit && !call(api->PhidgetMotorPositionController_getMaxCurrentLimit_(handle,&cap.maximum_current),"Current bounds"))return false;
        if(cap.current_regulator_gain && (!call(api->PhidgetMotorPositionController_getMinCurrentRegulatorGain_(handle,&cap.minimum_regulator_gain),"Minimum regulator gain") || !call(api->PhidgetMotorPositionController_getMaxCurrentRegulatorGain_(handle,&cap.maximum_regulator_gain),"Maximum regulator gain")))return false;
        return view.state!=MotorState::fault;
    }
    bool inspect_parameters(){
        auto& p=view.applied;auto& c=view.capabilities;
        #define GET(name,member) if(!call(api->PhidgetMotorPositionController_get##name##_(handle,&p.member),"Inspect " #name))return false;
        GET(Kp,kp) GET(Ki,ki) GET(Kd,kd) GET(RescaleFactor,rescale) GET(DeadBand,deadband) GET(VelocityLimit,velocity_limit) GET(Acceleration,acceleration)
        #undef GET
        #define BOUND(name,member) if(!call(api->PhidgetMotorPositionController_getMin##name##_(handle,&c.minimum_##member),"Inspect minimum " #name) || !call(api->PhidgetMotorPositionController_getMax##name##_(handle,&c.maximum_##member),"Inspect maximum " #name))return false;
        BOUND(Acceleration,acceleration) BOUND(VelocityLimit,velocity) BOUND(Position,position) BOUND(DataInterval,interval_ms) BOUND(FailsafeTime,failsafe_ms)
        #undef BOUND
        if(c.minimum_position>c.maximum_position)std::swap(c.minimum_position,c.maximum_position);
        if(c.current_limit){double current{};if(!call(api->PhidgetMotorPositionController_getCurrentLimit_(handle,&current),"Inspect current limit"))return false;p.current_limit=current;}
        // Gains depend on this mode. Inspection must expose the controller's
        // actual interpretation rather than the caller's parameter defaults.
        p.normalize_pid.reset();
        if(c.normalize_pid){int normalized{};if(!call(api->PhidgetMotorPositionController_getNormalizePID_(handle,&normalized),"Inspect PID normalization"))return false;p.normalize_pid=normalized!=0;}
        view.state=MotorState::unreferenced;return true;
    }
    void apply_limits(const Command& command){
        const auto& c=view.capabilities;
        if(!view.configured || !view.feedback_valid || !finite(command.value) || !finite(command.minimum) || command.value<=0 || command.value<c.minimum_velocity || command.value>c.maximum_velocity || command.minimum<c.minimum_acceleration || command.minimum>c.maximum_acceleration){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Live velocity/acceleration outside device bounds");return;}
        const auto set=[&](auto setter,auto getter,double desired,double old,const char* label){if(desired==old)return true;double actual{};return call(setter(handle,desired),label) && call(getter(handle,&actual),label) && finite(actual) && std::abs(actual-desired)<=1e-9*std::max(1.,std::abs(desired));};
        const auto previous_period=view.wave_period;const auto previous_start=wave_start;
        if(!set(api->PhidgetMotorPositionController_setVelocityLimit_,api->PhidgetMotorPositionController_getVelocityLimit_,command.value,view.applied.velocity_limit,"Live velocity") || !set(api->PhidgetMotorPositionController_setAcceleration_,api->PhidgetMotorPositionController_getAcceleration_,command.minimum,view.applied.acceleration,"Live acceleration")){
            if(view.state!=MotorState::fault)fault("Live motion-limit readback mismatch");done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;
        }
        view.applied.velocity_limit=view.requested.velocity_limit=command.value;view.applied.acceleration=view.requested.acceleration=command.minimum;
        jog_speed=std::copysign(std::min(std::abs(jog_speed),command.value),jog_speed);
        if(view.wave_active){const auto now=clock->now_ms();const double phase=std::fmod(double(now-previous_start)/1000.,previous_period)/previous_period;view.wave_period=motor_wave_period(waveform,wave_amplitude,command.value,command.minimum,view.applied.feedback_interval_ms/1000.);wave_start=now-static_cast<std::uint64_t>(phase*view.wave_period*1000.);}
        ++view.configuration_revision;done(command.id);
    }
    void apply_tuning(const Command& command){
        if(!handle || !view.attached || !view.configured || !finite(command.value) || !finite(command.minimum) || !finite(command.maximum)){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Configured motor and finite PID gains required");return;}
        const auto set=[&](auto setter,auto getter,double desired,const char* label){double actual{};return call(setter(handle,desired),label) && call(getter(handle,&actual),label) && finite(actual) && std::abs(actual-desired)<=1e-9*std::max(1.,std::abs(desired));};
        if(!set(api->PhidgetMotorPositionController_setKp_,api->PhidgetMotorPositionController_getKp_,command.value,"Live Kp") || !set(api->PhidgetMotorPositionController_setKi_,api->PhidgetMotorPositionController_getKi_,command.minimum,"Live Ki") || !set(api->PhidgetMotorPositionController_setKd_,api->PhidgetMotorPositionController_getKd_,command.maximum,"Live Kd")){
            if(view.state!=MotorState::fault)fault("Live PID readback mismatch; configuration incomplete");done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;
        }
        view.applied.kp=view.requested.kp=command.value;view.applied.ki=view.requested.ki=command.minimum;view.applied.kd=view.requested.kd=command.maximum;++view.configuration_revision;done(command.id);
    }
    bool apply_parameters(MotorParameters p){
        view.configured=false;
        configuration_error=MotorError::sdk_error;
        const auto& cap=view.capabilities;
        if(!parameters_valid(p) || (p.encoder_mode && (static_cast<int>(*p.encoder_mode)<1 || static_cast<int>(*p.encoder_mode)>5)) || (p.feedback_source && *p.feedback_source!=MotorFeedbackSource::encoder && *p.feedback_source!=MotorFeedbackSource::hall)){
            configuration_error=MotorError::invalid_argument;view.fault="Invalid motor parameters";return false;
        }
        if(p.feed_forward || (p.normalize_pid && !cap.normalize_pid) || (p.encoder_mode && !cap.encoder_mode) || (p.feedback_source && !cap.feedback_source) || (p.current_regulator_gain && !cap.current_regulator_gain) || (p.current_limit && !cap.current_limit)){
            configuration_error=MotorError::unsupported;view.fault="Requested property unavailable on this motor";return false;
        }
        if(cap.current_limit && !p.current_limit){configuration_error=MotorError::invalid_argument;view.fault="Current-limiting motor requires an explicit current limit";return false;}
        const bool same_coordinate=p.rescale==view.applied.rescale && p.feedback_source==view.applied.feedback_source && p.encoder_mode==view.applied.encoder_mode;
        const bool keep_reference=view.referenced && same_coordinate;
        if(!call(api->PhidgetMotorPositionController_setRescaleFactor_(handle,p.rescale),"Rescale"))return false;
        auto& limits=view.capabilities;
        #define BOUNDS(name,member) if(!call(api->PhidgetMotorPositionController_getMin##name##_(handle,&limits.minimum_##member),"Minimum " #name) || !call(api->PhidgetMotorPositionController_getMax##name##_(handle,&limits.maximum_##member),"Maximum " #name))return false;
        BOUNDS(Acceleration,acceleration) BOUNDS(VelocityLimit,velocity) BOUNDS(Position,position)
        BOUNDS(DataInterval,interval_ms) BOUNDS(FailsafeTime,failsafe_ms)
        #undef BOUNDS
        if(limits.minimum_position>limits.maximum_position)std::swap(limits.minimum_position,limits.maximum_position);
        if(p.numeric_range_only){p.minimum=limits.minimum_position;p.maximum=limits.maximum_position;}
        const double step=std::abs(p.rescale);
        auto within=[](auto value,auto lo,auto hi){return value>=lo && value<=hi;};
        if(!within(p.acceleration,limits.minimum_acceleration,limits.maximum_acceleration) || !within(p.velocity_limit,limits.minimum_velocity,limits.maximum_velocity) ||
           (p.current_limit && !within(*p.current_limit,limits.minimum_current,limits.maximum_current)) || !within(p.feedback_interval_ms,limits.minimum_interval_ms,limits.maximum_interval_ms) ||
           !within(p.failsafe_ms,limits.minimum_failsafe_ms,limits.maximum_failsafe_ms) || (p.current_regulator_gain && !within(*p.current_regulator_gain,limits.minimum_regulator_gain,limits.maximum_regulator_gain)) || p.minimum<limits.minimum_position || p.maximum>limits.maximum_position || p.jog_lead<step || p.deadband/step>4294967295.){configuration_error=MotorError::invalid_argument;view.fault="Requested motor parameter exceeds device bounds";return false;}
        const double deadband=std::round(p.deadband/step)*step;
        #define SET(name,value) if(!call(api->PhidgetMotorPositionController_set##name##_(handle,value),"Set " #name))return false;
        if(p.normalize_pid){SET(NormalizePID,*p.normalize_pid?1:0)}
        if(p.encoder_mode){SET(IOMode,static_cast<Phidget_EncoderIOMode>(*p.encoder_mode))}
        if(p.feedback_source){Phidget_PositionType current{};if(!call(api->PhidgetMotorPositionController_getPositionType_(handle,&current),"Read feedback source"))return false;if(current!=static_cast<Phidget_PositionType>(*p.feedback_source)){SET(PositionType,static_cast<Phidget_PositionType>(*p.feedback_source))}}
        if(p.current_regulator_gain){SET(CurrentRegulatorGain,*p.current_regulator_gain)}
        SET(Kp,p.kp) SET(Ki,p.ki) SET(Kd,p.kd) SET(DeadBand,deadband) SET(VelocityLimit,p.velocity_limit) SET(Acceleration,p.acceleration)
        if(p.current_limit){SET(CurrentLimit,*p.current_limit)}
        SET(DataInterval,p.feedback_interval_ms)
        #undef SET
        auto equal=[](double a,double b){return finite(a) && std::abs(a-b)<=1e-9*std::max({1.,std::abs(a),std::abs(b)});};double actual{};
        #define VERIFY(name,value) if(!call(api->PhidgetMotorPositionController_get##name##_(handle,&actual),"Read back " #name) || !equal(actual,value)){fault("Motor configuration readback mismatch: " #name);return false;}
        VERIFY(Kp,p.kp) VERIFY(Ki,p.ki) VERIFY(Kd,p.kd) VERIFY(RescaleFactor,p.rescale) VERIFY(DeadBand,deadband) VERIFY(VelocityLimit,p.velocity_limit) VERIFY(Acceleration,p.acceleration)
        if(p.current_limit){VERIFY(CurrentLimit,*p.current_limit)}
        if(p.current_regulator_gain){VERIFY(CurrentRegulatorGain,*p.current_regulator_gain)}
        #undef VERIFY
        uint32_t interval{};int normalized{};Phidget_EncoderIOMode mode{};Phidget_PositionType source{};
        if(!call(api->PhidgetMotorPositionController_getDataInterval_(handle,&interval),"Read back interval") || interval!=p.feedback_interval_ms){fault("Motor interval readback mismatch");return false;}
        if(p.normalize_pid && (!call(api->PhidgetMotorPositionController_getNormalizePID_(handle,&normalized),"Read back normalization") || bool(normalized)!=*p.normalize_pid)){fault("Motor PID normalization readback mismatch");return false;}
        if(p.encoder_mode && (!call(api->PhidgetMotorPositionController_getIOMode_(handle,&mode),"Read back encoder mode") || static_cast<int>(mode)!=static_cast<int>(*p.encoder_mode))){fault("Encoder mode readback mismatch");return false;}
        if(p.feedback_source && (!call(api->PhidgetMotorPositionController_getPositionType_(handle,&source),"Read back feedback source") || static_cast<int>(source)!=static_cast<int>(*p.feedback_source))){fault("Feedback source readback mismatch");return false;}
        if(!call(api->PhidgetMotorPositionController_enableFailsafe_(handle,p.failsafe_ms),"Enable motor failsafe"))return false;
        watchdog=true;last_failsafe=clock->now_ms();view.applied=p;view.applied.deadband=deadband;view.configured=true;view.referenced=keep_reference;view.fault.clear();view.fault_error=MotorError::none;view.fault_code=0;++view.configuration_revision;
        if(!same_coordinate){position_known=false;++view.coordinate_revision;}
        view.state=view.referenced?MotorState::ready:MotorState::unreferenced;return true;
    }
    void observe(double value,std::uint64_t now){
        if(!finite(value)){view.feedback_valid=false;fault("Non-finite motor position");return;}
        if(!position_known || std::abs(value-previous_position)>std::abs(view.requested.rescale)*.5)still_since=now;
        position_known=true;previous_position=value;view.position=value;view.feedback_valid=true;view.observed_ms=now;
    }
    bool read_feedback(std::uint64_t now){
        double value{};const auto result=api->PhidgetMotorPositionController_getPosition_(handle,&value);view.checked_ms=now;last_feedback=now;
        if(result){view.feedback_valid=false;view.referenced=false;cancel_active("Motor feedback invalid");if(view.engaged || (result!=EPHIDGET_UNKNOWNVAL && result!=EPHIDGET_NOTATTACHED))fault("Motor position feedback unavailable",static_cast<int>(result));else view.state=MotorState::unreferenced;return false;}
        // Getter time is separate from the arrival time of change-only events.
        const bool unchanged=view.feedback_valid && position_known && value==previous_position;const auto stamp=view.observed_ms;observe(value,now);if(unchanged)view.observed_ms=stamp;
        if(view.capabilities.expected_position && !view.expected_event_valid){const auto code=api->PhidgetMotorPositionController_getExpectedPosition_(handle,&value);if(!code && finite(value))view.expected_position=value;else view.expected_position.reset();}
        if(view.capabilities.duty_cycle){const auto code=api->PhidgetMotorPositionController_getDutyCycle_(handle,&value);if(!code && finite(value))view.duty_cycle=value;else view.duty_cycle.reset();}
        return view.feedback_valid;
    }
    void attach(){
        attachment_seen=true;
        ++view.generation;++motion_epoch;cancel_active("New motor attachment");view.attached=true;view.configured=false;view.referenced=false;view.feedback_valid=false;view.expected_position.reset();view.duty_cycle.reset();view.engaged=false;view.drive=MotorDriveState::unknown;release_attempts=0;position_known=false;ever_engaged=false;watchdog=false;view.state=MotorState::opening;
        if(!discover_capabilities() || !call(static_cast<PhidgetReturnCode>(release_drive()),"Initial disengagement") || !(inspection?inspect_parameters():apply_parameters(view.requested))){if(view.state!=MotorState::fault)fault(view.fault,0,configuration_error);return;}
        view.expected_event_valid=view.expected_feedback_enabled=false;
        if(api->PhidgetMotorPositionController_setEnableExpectedPosition_ && api->PhidgetMotorPositionController_getEnableExpectedPosition_ && api->PhidgetMotorPositionController_setOnExpectedPositionChangeHandler_){
            int enabled_value{};
            if(!api->PhidgetMotorPositionController_getEnableExpectedPosition_(handle,&enabled_value)){
                original_expected_enable=enabled_value;
                if(!api->PhidgetMotorPositionController_setOnExpectedPositionChangeHandler_(handle,expected,this) && !api->PhidgetMotorPositionController_setEnableExpectedPosition_(handle,1) && !api->PhidgetMotorPositionController_getEnableExpectedPosition_(handle,&enabled_value))view.expected_feedback_enabled=enabled_value!=0;
            }
        }
        recover_failsafe=false;view.drive=MotorDriveState::off;view.fault_kind=MotorFaultKind::none;read_feedback(clock->now_ms());
        if(view.feedback_valid && opening_id){done(opening_id);opening_id=0;}
    }
    bool target(double value,bool jogging=false,bool holding=false){
        const auto& p=view.applied;if(holding || view.state==MotorState::waving || view.state==MotorState::jogging)view.target_revision_valid=false;const double requested=value;target_error=MotorError::sdk_error;
        if(!holding){
            if(!travel_guard.manual_page && travel_guard.required && travel_guard.motor_generation!=view.generation){target_error=MotorError::not_ready;fault("Physical guard belongs to another motor session",0,MotorError::not_ready);return false;}
            const auto guarded=motor_guard_target(travel_guard,view.position,value);
            if(guarded.decision==MotorMotionDecision::unavailable){target_error=MotorError::not_ready;fault("Calibrated physical feedback unavailable",0,MotorError::not_ready);return false;}
            if(guarded.decision==MotorMotionDecision::limit){const auto id=active_id;target_error=MotorError::soft_limit;++motion_epoch;const auto code=release_drive();if(code)fault("Release at calibrated limit failed",code);else {view.fault_kind=MotorFaultKind::soft_limit;view.fault="Calibrated limit; outward movement blocked";}done(id,MotorResultStage::limited,MotorError::soft_limit,code,view.fault);return false;}
            value=(travel_guard.manual_page || travel_guard.owner==MotorOwner::homing)?guarded.target:motor_operating_target(view.position,guarded.target,p.minimum,p.maximum);
        }
        if(!finite(value) || value<view.capabilities.minimum_position || value>view.capabilities.maximum_position)return false;
        const bool recovery=holding || travel_guard.manual_page || travel_guard.owner==MotorOwner::homing || view.position<p.minimum || view.position>p.maximum;
        const double lo=recovery?view.capabilities.minimum_position:p.minimum,hi=recovery?view.capabilities.maximum_position:p.maximum;
        const double low=jogging?std::max(lo,view.position-p.jog_lead):lo;
        const double high=jogging?std::min(hi,view.position+p.jog_lead):hi;
        const double step=std::abs(p.rescale);
        // The SDK rounds raw targets to encoder pulses. Choose a point on that
        // lattice inside travel/lead bounds before sending it, and expose the
        // requested and effective positions separately.
        const double minimum_count=std::ceil((low-view.position)/step);
        const double maximum_count=std::floor((high-view.position)/step);
        if(minimum_count>maximum_count){fault("No encoder-representable target inside operating bounds");return false;}
        const double count=std::clamp(std::round((value-view.position)/step),minimum_count,maximum_count);
        const double effective=view.position+count*step;
        if(effective<low || effective>high){fault("Rounded target outside operating bounds");return false;}
        if(std::abs(effective-requested)>.51*step)view.target_revision_valid=false;view.desired_target=requested;
        if(!holding && effective==view.target)return true;
        if(!call(api->PhidgetMotorPositionController_setTargetPosition_(handle,effective),"Position target"))return false;
        double actual{};if(!call(api->PhidgetMotorPositionController_getTargetPosition_(handle,&actual),"Read back target") || !finite(actual) || actual<low || actual>high || std::abs(actual-effective)>1e-9*std::max(1.,std::abs(effective))){fault("Motor target readback mismatch");return false;}
        view.target=actual;return true;
    }
    void expire_jog(std::uint64_t now){
        if((view.state==MotorState::jogging || view.state==MotorState::waving) && now-jog_renewed>=view.applied.jog_lease_ms){
            ++motion_epoch;const auto code=release_drive();
            if(code)fault("Disengagement after jog lease expiry failed",code);
            else view.fault="Jog motion lease expired";
        }
    }
    void execute(Command& command){
        target_error=MotorError::sdk_error;
        // A queued renewal cannot revive a lease which expired during a slow
        // SDK operation. Check the clock before accepting further movement.
        expire_jog(clock->now_ms());
        const bool control_priority=command.op==Operation::stop || command.op==Operation::disengage || command.op==Operation::close;
        if((!control_priority && command.channel_epoch!=channel_epoch) || (movement(command.op) && !control_priority && command.epoch!=motion_epoch) || ((movement(command.op) || command.op==Operation::tuning || command.op==Operation::limits || command.op==Operation::update) && command.generation!=view.generation)){done(command.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Command superseded or from an old attachment");return;}
        if(command.op==Operation::open || command.op==Operation::inspect){begin_open(command.id,command.op==Operation::inspect);return;}
        if(command.op==Operation::tuning){apply_tuning(command);return;}
        if(command.op==Operation::limits){apply_limits(command);return;}
        if(command.op==Operation::close){done(opening_id,MotorResultStage::cancelled,MotorError::cancelled);opening_id=0;close_handle();done(command.id);return;}
        if(command.op==Operation::disengage){const auto code=release_drive();if(code)fault("Disengagement failed",code);done(command.id,code?MotorResultStage::rejected:MotorResultStage::sdk_succeeded,code?MotorError::sdk_error:MotorError::none,code,code?view.fault:std::string{});return;}
        if(command.op==Operation::update){
            const auto code=release_drive();view.requested=std::move(command.parameters);
            if(code){fault("Cannot disengage for parameter update",code);done(command.id,MotorResultStage::rejected,MotorError::sdk_error,code,view.fault);return;}
            if(!handle || !view.attached){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"No attached motor");return;}
            if((!view.configured && !inspection) || (ever_engaged && view.requested.feedback_source!=view.applied.feedback_source)){close_handle();begin_open(command.id);return;}
            inspection=false;
            if(!apply_parameters(view.requested)){if(view.state!=MotorState::fault)fault(view.fault,0,configuration_error);done(command.id,MotorResultStage::rejected,configuration_error,view.fault_code,view.fault);return;}
            if(!read_feedback(clock->now_ms())){if(view.state==MotorState::fault)done(command.id,MotorResultStage::rejected,view.fault_error,view.fault_code,view.fault);else {opening_id=command.id;opening_deadline=clock->now_ms()+5000;}return;}done(command.id);return;
        }
        if(!enabled || !handle || !view.attached || !view.configured || !view.feedback_valid){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Attached, configured motor feedback required");return;}
        const auto now=clock->now_ms();
        if(command.op==Operation::reference){
            if(view.engaged || !finite(command.value) || !finite(command.minimum) || !finite(command.maximum) || command.minimum>=command.maximum || command.value<command.minimum || command.value>command.maximum || command.minimum<view.capabilities.minimum_position || command.maximum>view.capabilities.maximum_position){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Reference requires disengagement and matching valid travel bounds");return;}
            if(!read_feedback(now) || now-still_since<view.applied.feedback_interval_ms){done(command.id,MotorResultStage::rejected,MotorError::busy,0,"Stationary valid position feedback required for reference");return;}
            // Rejecting a reference while engaged must leave its current move
            // monitored. Only a valid reference invalidates queued coordinates.
            ++motion_epoch;
            if(!call(api->PhidgetMotorPositionController_addPositionOffset_(handle,command.value-view.position),"Set motor reference")){done(command.id,target_error==MotorError::soft_limit?MotorResultStage::limited:MotorResultStage::rejected,target_error,view.fault_code,view.fault);return;}
            view.requested.minimum=view.applied.minimum=command.minimum;view.requested.maximum=view.applied.maximum=command.maximum;
            position_known=false;if(!read_feedback(now)){done(command.id,MotorResultStage::rejected,MotorError::sdk_error,0,"Reference position unavailable");return;}
            if(!target(view.position,false,true)){done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;}
            view.referenced=true;++view.coordinate_revision;view.state=MotorState::ready;done(command.id);return;
        }
        if(command.op==Operation::stop){cancel_active("Stop");if(view.engaged && (!read_feedback(now) || !target(view.position,false,true))){done(command.id,target_error==MotorError::soft_limit?MotorResultStage::limited:MotorResultStage::rejected,target_error,view.fault_code,view.fault);return;}view.state=view.engaged?MotorState::holding:view.referenced?MotorState::ready:MotorState::unreferenced;done(command.id);return;}
        if(command.op==Operation::engage){
            if(!travel_guard.manual_page && travel_guard.required && (!travel_guard.valid || travel_guard.motor_generation!=view.generation || travel_guard.coordinate_revision!=view.coordinate_revision)){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Current calibrated physical guard required before engagement");return;}
            if(!read_feedback(now)){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Current motor feedback required");return;}
            if(view.position<view.capabilities.minimum_position || view.position>view.capabilities.maximum_position){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Actual position outside SDK numeric range");return;}
            auto cancelled=[&]{return command.epoch!=motion_epoch || !enabled || shutdown;};
            if(cancelled()){done(command.id,MotorResultStage::cancelled,MotorError::cancelled);return;}
            if(!target(view.position,false,true)){done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;}
            // A remote acknowledgement can take time. Check again before
            // applying drive: a pending release must inhibit activation.
            if(cancelled()){done(command.id,MotorResultStage::cancelled,MotorError::cancelled);return;}
            if(!guard_velocity(now)){done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;}view.drive=MotorDriveState::unknown;view.engaged=true;
            if(!call(api->PhidgetMotorPositionController_setEngaged_(handle,1),"Engage motor")){done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;}
            int engaged{};if(!call(api->PhidgetMotorPositionController_getEngaged_(handle,&engaged),"Read back engagement") || !engaged){if(view.state!=MotorState::fault)fault("Motor engagement readback mismatch");done(command.id,MotorResultStage::rejected,MotorError::sdk_error,view.fault_code,view.fault);return;}view.drive=MotorDriveState::on;recovery_attempted=false;view.engaged=true;ever_engaged=true;view.state=MotorState::holding;done(command.id);return;
        }
        if(!view.engaged){
            // The endpoint guard can release between the mapping snapshot and
            // this queued target. Preserve that directional limit outcome; it
            // is not a new SDK failure or a reason to discard session homing.
            if(command.op==Operation::absolute && view.drive==MotorDriveState::off && view.fault_kind==MotorFaultKind::soft_limit)
                done(command.id,MotorResultStage::limited,MotorError::soft_limit,0,"Calibrated limit; drive released; inward movement may re-engage");
            else done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Explicit motor engagement required");
            return;
        }
        if(command.op==Operation::renew){if(view.state!=MotorState::jogging && view.state!=MotorState::waving){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"No active jog lease");return;}jog_renewed=std::max(jog_renewed,command.stamp);done(command.id);return;}
        if(command.op==Operation::wave){
            const auto type=static_cast<MotorWaveform>(static_cast<int>(command.minimum));
            const double period=motor_wave_period(type,command.value,view.applied.velocity_limit,view.applied.acceleration,view.applied.feedback_interval_ms/1000.);
            if(!finite(period) || period<=0 || !read_feedback(now) || !finite(view.position-command.value) || !finite(view.position+command.value) || (!travel_guard.manual_page && (view.position-command.value<view.applied.minimum || view.position+command.value>view.applied.maximum))){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Wave amplitude or bounds invalid");return;}
            cancel_active("New tuning wave");waveform=type;wave_amplitude=command.value;wave_start=now;
            view.wave_centre=view.position;view.wave_period=period;view.wave_active=true;
            jog_at=jog_renewed=now;view.state=MotorState::waving;active_id=command.id;active_epoch=command.epoch;
            if(!target(type==MotorWaveform::sine?view.position:view.position+wave_amplitude)){done(command.id,target_error==MotorError::soft_limit?MotorResultStage::limited:MotorResultStage::rejected,target_error,view.fault_code,view.fault);return;}
            done(command.id);return;
        }
        if(command.op==Operation::jog){
            if(!finite(command.value) || command.value==0 || std::abs(command.value)>view.applied.velocity_limit){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Jog speed must be nonzero and within the configured velocity limit");return;}
            if(!read_feedback(now)){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Current motor feedback required");return;}
            if(command.epoch!=motion_epoch){done(command.id,MotorResultStage::cancelled,MotorError::cancelled);return;}
            cancel_active("New jog");if(!target(view.position,false,true)){done(command.id,target_error==MotorError::soft_limit?MotorResultStage::limited:MotorResultStage::rejected,target_error,view.fault_code,view.fault);return;}
            jog_speed=command.value;jog_accumulator=view.position;jog_at=jog_renewed=now;view.state=MotorState::jogging;active_id=command.id;active_epoch=command.epoch;done(command.id);return;
        }
        if(!read_feedback(now)){done(command.id,MotorResultStage::rejected,MotorError::not_ready,0,"Current motor feedback required");return;}
        if(command.epoch!=motion_epoch){done(command.id,MotorResultStage::cancelled,MotorError::cancelled);return;}
        const double desired=command.op==Operation::relative?view.position+command.value:command.value;
        if(!finite(desired) || (!travel_guard.manual_page && travel_guard.owner!=MotorOwner::homing && (view.position>=view.applied.minimum && view.position<=view.applied.maximum) && (desired<view.applied.minimum || desired>view.applied.maximum))){done(command.id,MotorResultStage::rejected,MotorError::invalid_argument,0,"Target outside calibrated motor travel");return;}
        view.target_start_position=view.position;cancel_active("Superseded position target");if(!target(desired)){done(command.id,target_error==MotorError::soft_limit?MotorResultStage::limited:MotorResultStage::rejected,target_error,view.fault_code,view.fault);return;}
        active_id=command.id;active_epoch=command.epoch;view.target_command_id=command.id;view.target_accepted_ms=clock->now_ms();view.target_velocity=view.applied.velocity_limit;view.target_acceleration=view.applied.acceleration;view.expected_event_valid=false;view.target_revision_valid=std::abs(view.target-desired)<=.51*std::abs(view.applied.rescale);view.target_attained=false;view.state=MotorState::moving;done(command.id);
    }
    bool guard_velocity(std::uint64_t now){
        const double effective=view.requested.velocity_limit;
        if(effective!=view.applied.velocity_limit){
            if(!call(api->PhidgetMotorPositionController_setVelocityLimit_(handle,effective),"Guarded velocity"))return false;
            double readback{};if(!call(api->PhidgetMotorPositionController_getVelocityLimit_(handle,&readback),"Guarded velocity readback") || !finite(readback) || readback<=0 || std::abs(readback-effective)>1e-9*std::max(1.,effective)){if(view.state!=MotorState::fault)fault("Guarded velocity readback mismatch");return false;}
            view.applied.velocity_limit=effective;jog_speed=std::copysign(std::min(std::abs(jog_speed),effective),jog_speed);
            if(view.wave_active){const double phase=std::fmod(double(now-wave_start)/1000.,view.wave_period)/view.wave_period;view.wave_period=motor_wave_period(waveform,wave_amplitude,effective,view.applied.acceleration,view.applied.feedback_interval_ms/1000.);wave_start=now-static_cast<std::uint64_t>(phase*view.wave_period*1000.);}
        }
        return true;
    }
    void service(){
        const auto now=clock->now_ms();
        expire_jog(now);
        if(!enabled && handle){close_handle();return;}
        if(recover_failsafe && handle){if(now<reopen_after)return;recover_failsafe=false;recovery_attempted=true;close_handle();begin_open(0,inspection);return;}
        if((opening_id || view.state==MotorState::opening) && now>=opening_deadline){
            const auto stage=!view.attached?"controller not attached; check device power, VINT connection and other applications":!view.feedback_valid?"controller attached, but no valid encoder position was received":"controller attached, but configuration did not complete";
            fault(std::string("Motor initialization: ")+stage+" (serial "+std::to_string(address.resource.serial)+", hub port "+std::to_string(address.resource.hub_port)+", channel "+std::to_string(address.resource.channel)+")",0,MotorError::not_ready);return;
        }
        if(!handle || !view.attached || view.state==MotorState::fault)return;
        if(now-last_feedback>=view.requested.feedback_interval_ms)read_feedback(now);
        if(view.feedback_valid && opening_id && (view.configured || inspection)){done(opening_id);opening_id=0;}
        if(!view.configured || view.state==MotorState::fault)return;
        if(watchdog && now-last_failsafe>=view.applied.failsafe_refresh_ms){if(!call(api->PhidgetMotorPositionController_resetFailsafe_(handle),"Renew hardware failsafe"))return;last_failsafe=now;}
        if(!view.engaged)return;
        if(active_id && active_epoch!=motion_epoch)return; // A priority control will run next.
        if(!view.feedback_valid){fault("Engaged motor lost position feedback");return;}
        const auto& p=view.applied;
        if(view.position<view.capabilities.minimum_position || view.position>view.capabilities.maximum_position){fault("Actual motor position outside SDK numeric range");return;}
        if(!travel_guard.manual_page && travel_guard.required && !travel_guard.valid){fault("Calibrated physical feedback unavailable",0,MotorError::not_ready);return;}
        if(!travel_guard.manual_page && travel_guard.required){const auto guarded=motor_guard_target(travel_guard,view.position,view.target);if(guarded.decision!=MotorMotionDecision::allow){target(view.target);return;}if(std::abs(guarded.target-view.target)>=std::abs(p.rescale)){const auto goal=view.desired_target;if(!target(guarded.target))return;view.desired_target=goal;}}
        if(!guard_velocity(now))return;
        if(view.state==MotorState::waving){
            if(now-jog_at>=p.feedback_interval_ms){
                jog_at=now;const double phase=std::fmod((now-wave_start)/1000.,view.wave_period)/view.wave_period;
                const double desired=view.wave_centre+wave_amplitude*(waveform==MotorWaveform::sine?std::sin(6.2831853071795864769*phase):phase<.5?1.:-1.);
                if(!target(desired))return;
            }
        }else if(view.state==MotorState::jogging){
            if(now-jog_at>=p.jog_interval_ms){
                const auto elapsed=std::min(now-jog_at,static_cast<std::uint64_t>(p.jog_interval_ms));jog_at=now;
                const double proposed=jog_accumulator+jog_speed*static_cast<double>(elapsed)/1000.;
                jog_accumulator=std::clamp(proposed,view.position-p.jog_lead,view.position+p.jog_lead);
                if(!target(jog_accumulator,true))return;
                if(!travel_guard.manual_page && ((jog_speed>0 && (view.position>=p.maximum-p.deadband || p.maximum-view.position<std::abs(p.rescale))) || (jog_speed<0 && (view.position<=p.minimum+p.deadband || view.position-p.minimum<std::abs(p.rescale))))){jog_speed=0;done(active_id,MotorResultStage::attained);active_id=0;view.state=MotorState::holding;}
            }
        }else if(view.state==MotorState::moving){if(!retry && view.desired_target!=view.target && now-jog_at>=p.feedback_interval_ms){jog_at=now;if(!target(view.desired_target))return;}if(std::abs(view.desired_target-view.position)<=std::max(p.deadband,std::abs(p.rescale)*.5)){done(active_id,MotorResultStage::attained);active_id=0;view.target_attained=true;view.state=MotorState::holding;}}
    }
    void run(){
        try{while(!shutdown){
            Event incoming;Command command;bool have_event{},have_command{},have_position{},invalid_position{};unsigned overflow{};std::uint64_t activity{};
            {std::lock_guard lock(queue_mutex);if(expected_pending){expected_pending=false;view.expected_event_valid=finite(callback_expected) && view.expected_feedback_enabled && view.attached;view.expected_position=finite(callback_expected)?std::optional<double>{callback_expected}:std::nullopt;view.expected_observed_ms=expected_activity;view.expected_sequence=expected_sequence;view.expected_generation=view.generation;view.expected_coordinate=view.coordinate_revision;}}
            {std::lock_guard lock(queue_mutex);if(guard_pending){travel_guard=pending_guard;guard_pending=false;}overflow=lost;lost=0;have_position=position_pending;invalid_position=position_invalid;activity=position_activity;position_pending=position_invalid=false;if(priority){command=std::move(*priority);priority.reset();have_command=true;}else if(retry && clock->now_ms()>=retry_at){command=std::move(*retry);retry.reset();have_command=true;}else if(event_read!=event_write){incoming=events[event_read++%events.size()];have_event=true;}else if(coalesced[3]){command=std::move(*coalesced[3]);coalesced[3].reset();have_command=true;}
            else if(command_read!=command_write){command=std::move(commands[command_read++%commands.size()]);have_command=true;}
            else for(auto& pending:coalesced)if(pending){command=std::move(*pending);pending.reset();have_command=true;break;}}
            if(overflow){
                fault(overflow==1?"Motor command queue overflow; continuity lost":overflow==2?"Motor lifecycle event queue overflow; continuity lost":"Motor event/command queue overflow; continuity lost",0,MotorError::queue_full);
                if(have_command)done(command.id,MotorResultStage::cancelled,MotorError::cancelled,0,"Queue continuity lost");
                have_command=have_event=have_position=false;
                std::array<std::uint64_t,72> ids{};std::size_t count{};
                {std::lock_guard lock(queue_mutex);while(command_read!=command_write)ids[count++]=commands[command_read++%commands.size()].id;if(priority){ids[count++]=priority->id;priority.reset();}for(auto& pending:coalesced)if(pending){ids[count++]=pending->id;pending.reset();}event_read=event_write;}
                for(std::size_t n=0;n<count;++n)done(ids[n],MotorResultStage::cancelled,MotorError::cancelled,0,"Queue continuity lost");
            }
            if(have_event && handle){if(incoming.kind==1)attach();else if(incoming.kind==2){view.attached=false;++view.generation;view.feedback_valid=false;watchdog=false;fault("Motor detached");}else if(incoming.kind==3)fault("Phidget motor error event",incoming.code);}
            if(have_position && handle && view.attached && view.state!=MotorState::fault){if(invalid_position)fault("Non-finite motor position");else {still_since=std::max(still_since,activity);read_feedback(clock->now_ms());}}
            if(retry && retry->epoch!=motion_epoch.load()){done(retry->id,MotorResultStage::cancelled,MotorError::cancelled,0,"Priority cancellation superseded SDK retry");retry.reset();}
            if(have_command){
                if(retry && retry->op==command.op){done(retry->id,MotorResultStage::cancelled,MotorError::cancelled,0,"Latest command superseded SDK retry");retry.reset();}
                executing_id=command.id;deferred_code=0;retry_context=command.op==Operation::absolute || command.op==Operation::limits;
                execute(command);retry_context=false;
                if(deferred_code){
                    if(clock->now_ms()-command.stamp>=4000 || !view.feedback_valid || !view.attached){const auto code=deferred_code;deferred_code=0;fault("Repeated transient SDK failures prevented motor control",code);done(command.id,MotorResultStage::rejected,MotorError::sdk_error,code,view.fault);}
                    else {++command.retries;retry_at=clock->now_ms()+std::min(1000u,250u<<(std::min(command.retries,3u)-1));if(retry)done(retry->id,MotorResultStage::cancelled,MotorError::cancelled,0,"New retry superseded deferred operation");retry=command;}
                }else if(view.state!=MotorState::fault && view.fault.find("temporarily unavailable")!=std::string::npos){view.fault.clear();view.fault_code=0;}
            }
            service();publish();if(!have_event && !have_command && !have_position)WaitForSingleObject(event,10);
        }}catch(const std::exception& ex){fault(ex.what());publish();}
        close_handle();publish();terminated=true;
    }
};
PhidgetMotor::PhidgetMotor(std::shared_ptr<PhidgetSession> session,MotorAddress address,MotorParameters parameters,std::shared_ptr<MotorClock> clock):impl_(std::make_unique<Impl>(std::move(session),std::move(address),std::move(parameters),std::move(clock))){}
PhidgetMotor::~PhidgetMotor()=default;
MotorCommandResult PhidgetMotor::open(){return impl_->submit(Operation::open);}
MotorCommandResult PhidgetMotor::inspect(){return impl_->submit(Operation::inspect);}
MotorCommandResult PhidgetMotor::update_limits(double velocity,double acceleration){return impl_->submit(Operation::limits,velocity,acceleration);}
MotorCommandResult PhidgetMotor::update_tuning(double kp,double ki,double kd){return impl_->submit(Operation::tuning,kp,ki,kd);}
MotorCommandResult PhidgetMotor::start_wave(MotorWaveform type,double amplitude){return impl_->submit(Operation::wave,amplitude,static_cast<double>(type));}
MotorCommandResult PhidgetMotor::update(MotorParameters value){return impl_->submit(Operation::update,0,0,0,std::move(value));}
MotorCommandResult PhidgetMotor::set_reference(double value,double minimum,double maximum){return impl_->submit(Operation::reference,value,minimum,maximum);}
MotorCommandResult PhidgetMotor::engage(){return impl_->submit(Operation::engage);}
MotorCommandResult PhidgetMotor::move_to(double value){return impl_->submit(Operation::absolute,value);}
MotorCommandResult PhidgetMotor::move_by(double value){return impl_->submit(Operation::relative,value);}
MotorCommandResult PhidgetMotor::jog(double value){return impl_->submit(Operation::jog,value);}
MotorCommandResult PhidgetMotor::renew_jog(){return impl_->submit(Operation::renew);}
MotorCommandResult PhidgetMotor::stop(){return impl_->submit(Operation::stop);}
MotorCommandResult PhidgetMotor::disengage(){return impl_->submit(Operation::disengage);}
MotorCommandResult PhidgetMotor::close(){return impl_->submit(Operation::close);}
void PhidgetMotor::provider_enabled(bool value){impl_->enabled=value;if(!value)++impl_->motion_epoch;SetEvent(impl_->event);}
void PhidgetMotor::guard(MotorGuardSnapshot g){{std::lock_guard lock(impl_->queue_mutex);impl_->pending_guard=g;impl_->guard_pending=true;}SetEvent(impl_->event);}
void PhidgetMotor::wake(){SetEvent(impl_->event);}
MotorSnapshot PhidgetMotor::snapshot() const{std::lock_guard lock(impl_->view_mutex);return impl_->published;}
std::optional<MotorCommandResult> PhidgetMotor::result(std::uint64_t id) const{if(!id)return {};std::lock_guard lock(impl_->view_mutex);const auto& result=impl_->results[id%impl_->results.size()];if(result.id!=id)return {};return result;}
}
