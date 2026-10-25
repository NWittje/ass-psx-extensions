#include "hwio/software_motor.hpp"
#include <Windows.h>
#include <atomic>
#include <array>
#include <thread>
#include <stdexcept>

namespace hwio {
namespace {
struct LoopClock final:MotorClock {std::uint64_t now_ms()const noexcept override{return GetTickCount64();}};
std::atomic<std::uint64_t> next_software_session{1};
std::atomic<void*> tuning_owner{};
std::uint64_t new_session()noexcept{return next_software_session.fetch_add(1,std::memory_order_relaxed);}
struct Invalid {std::string reason;MotorError error{MotorError::invalid_argument};};
enum class Op {inspect,configure,tuning,ff,gains,auto_tune,diagnostic,limits,target,reference,engage,jog,wave,renew,hold,release,close};
bool moves(Op op){return op==Op::engage || op==Op::target || op==Op::jog || op==Op::wave || op==Op::diagnostic;}
}
struct SoftwarePositionMotor::Impl {
    struct Command {Op op{};MotorCommandResult result;MotorParameters parameters;double a{},b{},c{};MotorWaveform waveform{};std::uint64_t epoch{};MotorTuningRequest tune;};
    std::unique_ptr<MotorDrive> drive;std::function<MotorPositionSample(const MotorFeedbackDefinition&)> source;std::shared_ptr<MotorClock> clock;
    mutable std::mutex mutex;MotorSnapshot published;std::array<MotorCommandResult,256> results{};
    std::array<Command,64> queue{};unsigned read{},write{};std::optional<Command> priority;
    MotorFeedbackDefinition selection;MotorGuardSnapshot guard_value;
    std::uint64_t selection_revision{},sequence{},epoch{};
    std::atomic<bool> enabled{true},shutdown{};HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};std::thread worker;
    MotorSnapshot live;MotorPositionLoop loop;MotorFeedbackDefinition selected;std::uint64_t selected_revision{},feedback_generation{},last_tick{},last_renew{},lease{},wave_started{};
    double offset{},jog_speed{},amplitude{},stationary_position{},wave_phase{};std::uint64_t stationary_since{};MotorWaveform waveform{};MotorCommandResult pending_release;
    MotorGuardSnapshot guard;
    bool direct_measurement{};double direct_effort{};
    MotorAutoTuner tuner;MotorTuningStatus published_tuning;MotorParameters before_tuning;bool owns_tuning{},validation_started{},finishing_tuning{};std::uint64_t release_started{};
    std::vector<MotorTuningSample> published_trace;
    explicit Impl(std::unique_ptr<MotorDrive> d,std::function<MotorPositionSample(const MotorFeedbackDefinition&)> s,std::shared_ptr<MotorClock> t):drive(std::move(d)),source(std::move(s)),clock(t?std::move(t):std::make_shared<LoopClock>()){
        if(!event || !drive)throw std::runtime_error("Cannot create software motor");published_trace.reserve(2048);worker=std::thread([this]{run();});
    }
    ~Impl(){shutdown=true;SetEvent(event);if(worker.joinable())worker.join();tuner.wait_analysis();if(owns_tuning){void* owner=this;tuning_owner.compare_exchange_strong(owner,nullptr);}CloseHandle(event);}
    void finish(MotorCommandResult result){live.last_result=result;std::lock_guard lock(mutex);results[result.id%results.size()]=std::move(result);}
    MotorCommandResult submit(Op op,double a=0,double b=0,double c=0,MotorParameters p={},MotorWaveform w=MotorWaveform::sine,MotorTuningRequest tune={}){
        std::lock_guard lock(mutex);MotorCommandResult r{++sequence,MotorResultStage::queued};Command cmd{op,r,std::move(p),a,b,c,w,epoch,tune};
        if(shutdown){r.stage=MotorResultStage::rejected;r.error=MotorError::not_ready;r.reason="Motor worker closed";return r;}
        if(op==Op::hold || op==Op::release || op==Op::close){++epoch;cmd.epoch=epoch;if(priority){auto cancelled=priority->result;cancelled.stage=MotorResultStage::cancelled;cancelled.error=MotorError::cancelled;results[cancelled.id%results.size()]=std::move(cancelled);}priority=cmd;}
        else {
            // The latest demand/limits/lease replaces an unprocessed one. Its
            // old command gets a cancellation result rather than disappearing.
            bool replaced{};if(op==Op::target || op==Op::limits || op==Op::renew || op==Op::tuning || op==Op::ff || op==Op::gains)for(unsigned i=read;i<write;++i)if(queue[i%queue.size()].op==op){auto old=queue[i%queue.size()].result;old.stage=MotorResultStage::cancelled;old.error=MotorError::cancelled;results[old.id%results.size()]=std::move(old);queue[i%queue.size()]=cmd;replaced=true;++published.command_coalesced;break;}
            if(!replaced){if(write-read==queue.size()){r.stage=MotorResultStage::rejected;r.error=MotorError::queue_full;r.reason="Software motor command queue full; drive release requested";++epoch;priority=Command{Op::release,{++sequence,MotorResultStage::queued}};++published.command_overflows;}else queue[write++%queue.size()]=cmd;}
        }
        ++published.command_submissions;results[r.id%results.size()]=r;SetEvent(event);return r;
    }
    void release(bool close=false){
        direct_measurement=false;direct_effort=0;jog_speed=0;live.wave_active=false;live.engaged=false;live.target_attained=false;loop.reset(live.position);live.backlash_takeup=false;live.requested_drive=0;
        if(live.attached){drive->release();live.accepted_drive=0;live.drive=drive->observation().release_confirmed?MotorDriveState::off:MotorDriveState::unknown;}
        else if(live.drive!=MotorDriveState::unknown)live.drive=MotorDriveState::off;
        live.state=close?MotorState::closed:live.configured?MotorState::ready:MotorState::unreferenced;
        if(close){drive->close();live.attached=live.configured=live.feedback_valid=live.referenced=false;
            if(pending_release.id){pending_release.stage=MotorResultStage::cancelled;pending_release.error=MotorError::cancelled;pending_release.reason="Channel closed before release confirmation";finish(pending_release);pending_release={};}}
    }
    void fault(const std::string& cause,int native_code=0){
        if(tuner.status().active || finishing_tuning){tuner.cancel(cause);tuner.restoration(false,"Motor channel fault; pre-run configuration retained but hardware unavailable");finishing_tuning=false;tuner.finalizing(false);}
        live.fault=cause;live.fault_error=MotorError::sdk_error;live.fault_kind=MotorFaultKind::sdk;
        live.fault_code=native_code;live.last_fault=cause;live.last_fault_code=native_code;live.last_fault_ms=clock->now_ms();
        try{release();}catch(const std::exception& e){live.drive=MotorDriveState::unknown;live.fault+="; release: "+std::string(e.what());}
        drive->close();live.attached=live.configured=live.feedback_valid=live.referenced=false;live.generation=new_session();live.state=MotorState::fault;
        if(pending_release.id){pending_release.stage=MotorResultStage::rejected;pending_release.error=MotorError::sdk_error;pending_release.sdk_code=native_code;pending_release.reason=live.fault;finish(pending_release);pending_release={};}
    }
    void sample(std::uint64_t now){
        const auto s=selected.kind==MotorPositionSource::encoder?drive->encoder(selected):selected.kind==MotorPositionSource::analogue && source?source(selected):MotorPositionSample{};
        // Acquisition/SDK work may have advanced the clock since command
        // dispatch. Compare the returned check time after acquiring it.
        now=clock->now_ms();
        const bool valid=s.valid && std::isfinite(s.position) && s.checked_ms<=now && now-s.checked_ms<=500;
        if(!valid){if(live.engaged)fault("Selected motor position feedback unavailable");live.feedback_valid=false;return;}
        if(feedback_generation && s.generation!=feedback_generation){release();live.referenced=false;live.generation=new_session();++live.coordinate_revision;offset=0;}
        feedback_generation=s.generation;live.feedback_valid=true;live.position=s.position+offset;live.observed_ms=s.observed_ms;live.feedback_sequence=s.sequence;live.feedback_generation=s.generation;live.feedback_quantum=s.quantum;live.feedback_code_count=s.code_count;live.feedback_raw_position=selected.inverted?-s.position:s.position;
        if(!stationary_since || std::abs(live.position-stationary_position)>=.5){stationary_position=live.position;stationary_since=now;}
    }
    void execute(Command& cmd,std::uint64_t now){
        auto result=cmd.result;result.stage=cmd.op==Op::inspect || cmd.op==Op::configure || cmd.op==Op::release?MotorResultStage::sdk_succeeded:MotorResultStage::host_applied;
        try{
            {std::lock_guard lock(mutex);if((moves(cmd.op) || cmd.op==Op::auto_tune || cmd.op==Op::reference || cmd.op==Op::inspect) && cmd.epoch!=epoch){result.stage=MotorResultStage::cancelled;result.error=MotorError::cancelled;result.reason="Movement cancelled by Stop/Disengage";results[result.id%results.size()]=result;return;}}
            const auto p=cmd.parameters;
            if((tuner.status().active || finishing_tuning) && cmd.op!=Op::renew && cmd.op!=Op::auto_tune){
                if(cmd.op==Op::hold || cmd.op==Op::release || cmd.op==Op::close || cmd.op==Op::configure || cmd.op==Op::inspect){cancel_tuning("Auto tune cancelled by Stop/settings/channel operation");
                    if(finishing_tuning && (cmd.op==Op::configure || cmd.op==Op::inspect || cmd.op==Op::close)){finishing_tuning=false;tuner.finalizing(false);tuner.restoration(false,"Release unconfirmed; explicit settings/channel operation superseded tuning restoration");}}
                else throw Invalid{"Auto tune exclusively owns this motor; stop tuning before edits or movement",MotorError::busy};
            }
            switch(cmd.op){
            case Op::inspect:{
                if(!enabled)throw std::runtime_error("Phidgets support disabled");release(true);live={};live.generation=new_session();live.capabilities=drive->open();live.attached=true;live.drive=drive->observation().release_confirmed?MotorDriveState::off:MotorDriveState::unknown;live.state=MotorState::unreferenced;feedback_generation=0;sample(now);break;}
            case Op::configure:
                release();live.configured=false;if(!live.attached || !enabled)throw Invalid{"Motor not attached/enabled"};
                for(double x:{p.kp,p.ki,p.kd,p.vff,p.aff,p.backlash,p.velocity_limit,p.acceleration,p.minimum,p.maximum})if(!std::isfinite(x))throw Invalid{"Nonfinite position-loop parameter"};
                if(p.velocity_limit<=0 || p.acceleration<=0 || p.minimum>=p.maximum || p.kp<0 || p.ki<0 || p.kd<0 || p.backlash<0)throw Invalid{"Invalid position-loop gains or limits"};
                live.requested=p;drive->configure(p);live.applied=p;live.configured=true;live.fault.clear();live.fault_kind=MotorFaultKind::none;live.state=MotorState::ready;++live.configuration_revision;last_renew=now;live.watchdog_active=drive->hardware_watchdog_active();break;
            case Op::tuning:case Op::ff:
                if(!std::isfinite(cmd.a) || !std::isfinite(cmd.b) || !std::isfinite(cmd.c) || (cmd.op==Op::tuning && (cmd.a<0 || cmd.b<0 || cmd.c<0)))throw Invalid{"Invalid position-loop gains"};
                if(cmd.op==Op::tuning){live.applied.kp=cmd.a;live.applied.ki=cmd.b;live.applied.kd=cmd.c;}else {live.applied.vff=cmd.a;live.applied.aff=cmd.b;}++live.configuration_revision;break;
            case Op::gains:{const auto g=MotorLoopGains::from(p);for(double x:{g.kp,g.ki,g.kd,g.vff,g.aff})if(!std::isfinite(x))throw Invalid{"Nonfinite five-gain update"};if(g.kp<0 || g.ki<0 || g.kd<0)throw Invalid{"Negative PID gain"};g.apply(live.applied);g.apply(live.requested);++live.configuration_revision;break;}
            case Op::auto_tune:{
                if(tuner.status().active){cancel_tuning("Auto tune cancelled by operator");break;}
                if(!guard.manual_page || guard.owner==MotorOwner::homing)throw Invalid{"Auto tune requires exclusive visible Bench ownership",MotorError::busy};
                void* free=nullptr;if(!tuning_owner.compare_exchange_strong(free,this))throw Invalid{"Another automatic tuning run is active",MotorError::busy};owns_tuning=true;
                release();sample(now);before_tuning=live.applied;validation_started=false;std::string reason;
                if(!tuner.start(cmd.tune,live,now,reason)){void* owner=this;tuning_owner.compare_exchange_strong(owner,nullptr);owns_tuning=false;throw Invalid{reason};}
                lease=now;break;}
            case Op::diagnostic:
                if(!guard.manual_page || guard.owner==MotorOwner::homing || !enabled || !live.attached || !live.configured || !live.feedback_valid || pending_release.id || live.drive==MotorDriveState::unknown)throw Invalid{"Backlash measurement requires valid feedback and exclusive visible Bench ownership",MotorError::busy};
                if(!std::isfinite(cmd.a) || std::abs(cmd.a)>1 || cmd.a==0)throw Invalid{"Invalid measurement drive fraction"};
                direct_measurement=true;direct_effort=cmd.a;jog_speed=0;live.wave_active=false;live.engaged=true;live.state=MotorState::jogging;lease=now;
                live.requested_drive=cmd.a;live.drive_requested_ms=clock->now_ms();drive->command(cmd.a);live.accepted_drive=cmd.a;live.drive_accepted_ms=clock->now_ms();live.drive=MotorDriveState::on;result.stage=MotorResultStage::sdk_succeeded;break;
            case Op::limits:
                if(!std::isfinite(cmd.a) || !std::isfinite(cmd.b) || cmd.a<=0 || cmd.b<=0)throw Invalid{"Invalid reference velocity/acceleration"};
                if(live.wave_active){const double period=motor_wave_period(waveform,amplitude,cmd.a,cmd.b,.05);if(period<=0)throw Invalid{"Generator period outside numeric range"};wave_phase=std::fmod(wave_phase+double(now-wave_started)/1000./live.wave_period,1.);wave_started=now;live.wave_period=period;}
                live.applied.velocity_limit=cmd.a;live.applied.acceleration=cmd.b;++live.configuration_revision;break;
            case Op::reference:
                if(live.engaged || !live.feedback_valid || now-stationary_since<200){result.stage=MotorResultStage::rejected;result.error=MotorError::busy;result.reason="Zero needs stationary disengaged feedback";finish(result);return;}
                offset+=cmd.a-live.position;live.position=cmd.a;live.applied.minimum=cmd.b;live.applied.maximum=cmd.c;live.referenced=true;++live.coordinate_revision;loop.reset(live.position);live.target=live.position;break;
            case Op::engage:
                if(pending_release.id || live.drive==MotorDriveState::unknown)throw Invalid{"Waiting for native drive-release confirmation",MotorError::busy};
                sample(now);if(!enabled || !live.configured || !live.feedback_valid || live.applied.kp<=0)throw Invalid{"Configure position feedback, positive Kp and physical limits before movement"};
                live.engaged=true;live.target=live.desired_target=live.position;live.target_attained=live.target_revision_valid=false;loop.reset(live.position);live.state=MotorState::holding;break;
            case Op::target:
                if(!live.engaged && live.drive==MotorDriveState::off && live.fault_kind==MotorFaultKind::soft_limit){result.stage=MotorResultStage::limited;result.error=MotorError::soft_limit;result.reason="Calibrated limit; drive released; inward movement may re-engage";break;}
                if(!live.engaged || !std::isfinite(cmd.a))throw Invalid{"Motor not engaged or invalid target"};
                {const auto permitted=motor_guard_target(guard,live.position,cmd.a);if(permitted.decision!=MotorMotionDecision::allow){release();live.fault_kind=MotorFaultKind::soft_limit;result.stage=MotorResultStage::limited;result.error=MotorError::soft_limit;result.reason="Calibrated limit; outward movement blocked";break;}}
                if(!guard.manual_page && guard.owner!=MotorOwner::homing && (cmd.a<live.applied.minimum || cmd.a>live.applied.maximum))throw Invalid{"Position target outside operating range"};
                live.target=live.desired_target=cmd.a;live.target_start_position=live.position;live.target_command_id=result.id;live.target_accepted_ms=now;live.target_velocity=live.applied.velocity_limit;live.target_acceleration=live.applied.acceleration;live.target_revision_valid=true;live.target_attained=false;live.state=MotorState::moving;jog_speed=0;live.wave_active=false;break;
            case Op::jog:
                if(!live.engaged || !std::isfinite(cmd.a))throw Invalid{"Engage before manual drive"};jog_speed=std::clamp(cmd.a,-live.applied.velocity_limit,live.applied.velocity_limit);lease=now;live.wave_active=false;live.state=MotorState::jogging;break;
            case Op::wave:
                if(!live.engaged || !std::isfinite(cmd.a) || cmd.a<=0)throw Invalid{"Invalid generator amplitude/engagement"};
                if(!guard.manual_page && (live.position-cmd.a<live.applied.minimum || live.position+cmd.a>live.applied.maximum))throw Invalid{"Generator exceeds operating range"};
                {const auto period=motor_wave_period(cmd.waveform,cmd.a,live.applied.velocity_limit,live.applied.acceleration,.05);if(period<=0)throw Invalid{"Generator period outside numeric range"};waveform=cmd.waveform;amplitude=cmd.a;live.wave_centre=live.position;live.wave_period=period;wave_phase=0;wave_started=lease=now;live.wave_active=true;jog_speed=0;live.state=MotorState::waving;break;}
            case Op::renew:lease=now;break;
            case Op::hold:
                if(direct_measurement){direct_measurement=false;direct_effort=0;drive->command(0);live.requested_drive=live.accepted_drive=0;live.engaged=false;live.state=MotorState::ready;live.drive=MotorDriveState::on;loop.reset(live.position);break;}
                // Stop holds the current selected sensor, just as the native
                // position backend refreshes encoder feedback before holding.
                // A prior polling sample can otherwise pull an arrived motor
                // back outside its endpoint band.
                if(live.engaged){sample(now);if(!live.feedback_valid || !live.attached)throw Invalid{"Stop position feedback unavailable",MotorError::not_ready};}
                jog_speed=0;live.wave_active=false;live.target=live.desired_target=live.position;live.target_attained=live.target_revision_valid=false;loop.reset(live.position);live.state=live.engaged?MotorState::holding:MotorState::ready;break;
            case Op::release:
                release();if(live.drive==MotorDriveState::off){live.accepted_drive=0;}
                else if(!live.attached)throw Invalid{"Drive release unconfirmed; restore/inspect the physical channel",MotorError::not_ready};
                else {pending_release=result;pending_release.stage=MotorResultStage::queued;result=pending_release;}break;
            case Op::close:release(true);break;
            }
        }catch(const Invalid& e){result.stage=MotorResultStage::rejected;result.error=e.error;result.reason=e.reason;}catch(const MotorDriveFailure& e){result.stage=MotorResultStage::rejected;result.error=MotorError::sdk_error;result.sdk_code=e.code;result.reason=e.what();fault(result.reason,e.code);}catch(const std::exception& e){result.stage=MotorResultStage::rejected;result.error=MotorError::sdk_error;result.reason=e.what();if(cmd.op!=Op::inspect || live.attached)fault(result.reason);}
        finish(std::move(result));
    }
    void tick(std::uint64_t now){
        const double dt=last_tick && now>=last_tick?double(now-last_tick)/1000.:.05;last_tick=now;
        if(!live.attached)return;
        if(!enabled){cancel_tuning("Phidget provider disabled");release(true);return;}
        sample(now);if(!live.attached)return;const auto observation=drive->observation();
        if(!observation.attached){release(true);live.fault="Motor detached; position continuity lost";live.generation=new_session();return;}
        live.reported_drive=observation.reported;
        if(live.configured && now-last_renew>=100){drive->renew();last_renew=live.failsafe_renewed_ms=now;}
        if(finishing_tuning){
            tuner.completed_at(now);
            if(tuner.status().result.success && (!live.feedback_valid || live.generation!=tuner.status().session || live.coordinate_revision!=tuner.status().coordinate)){cancel_tuning("Feedback/session changed before tuning release confirmation");return;}
            if(observation.release_confirmed){live.drive=MotorDriveState::off;tuner.restoration(true);tuner.finalizing(false);finishing_tuning=false;}
            else if(now-release_started>=2000 || now-tuner.status().started_ms>=90000){MotorLoopGains::from(before_tuning).apply(live.applied);MotorLoopGains::from(before_tuning).apply(live.requested);tuner.restoration(false,"Native drive release not confirmed within tuning deadline");tuner.finalizing(false);finishing_tuning=false;live.drive=MotorDriveState::unknown;}
            return;
        }
        if(!live.engaged && observation.release_confirmed){live.drive=MotorDriveState::off;live.accepted_drive=0;if(pending_release.id){pending_release.stage=MotorResultStage::sdk_succeeded;finish(pending_release);pending_release={};}}
        if(tuner.status().active){
            if(!guard.manual_page || guard.owner==MotorOwner::homing || now-lease>=live.applied.jog_lease_ms){cancel_tuning("Auto tune lost visible Bench ownership/lease");return;}
            if(dt<=0)return;if(dt>=1){cancel_tuning("Auto tune worker missed watchdog interval");return;}
            const auto action=tuner.tick(live,now,dt);
            if(!action.active){finish_tuning();return;}
            const auto phase=tuner.status().phase;
            if(phase==MotorTunePhase::noise || phase==MotorTunePhase::analysis){if(live.engaged || live.drive==MotorDriveState::on)release();return;}
            double demand{};
            if(action.gains){action.gains->apply(live.applied);action.gains->apply(live.requested);loop.reset(live.position);validation_started=true;++live.configuration_revision;}
            if(action.direct){demand=action.value;live.engaged=true;live.state=MotorState::moving;}
            else {if(!validation_started){loop.reset(live.position);validation_started=true;}live.engaged=true;live.state=MotorState::moving;live.target=action.value;demand=loop.step(action.value,live.position,dt,live.applied);live.reference_position=loop.position;live.reference_velocity=loop.velocity;live.reference_acceleration=loop.acceleration;live.reference_shaft_position=loop.shaft_position();live.backlash_takeup=loop.taking_up();}
            if(!std::isfinite(demand))throw std::runtime_error("Nonfinite tuning drive");live.requested_drive=demand;live.drive_requested_ms=clock->now_ms();drive->command(demand);live.accepted_drive=demand;live.drive_accepted_ms=clock->now_ms();tuner.command_accepted(now,live.drive_requested_ms,live.drive_accepted_ms,demand,demand);live.drive=MotorDriveState::on;return;
        }
        if(direct_measurement){
            if(!guard.manual_page || guard.owner==MotorOwner::homing || now-lease>=live.applied.jog_lease_ms){release();live.fault="Backlash measurement cancelled; Bench ownership/lease ended";return;}
            if(!live.feedback_valid)return;
            live.requested_drive=direct_effort;return;
        }
        if(!live.engaged || !live.feedback_valid)return;
        if((jog_speed!=0 || live.wave_active) && now-lease>=live.applied.jog_lease_ms){release();live.fault="Manual/generator lease expired; drive released";return;}
        if(dt<=0)return;if(dt>=1){release();live.fault="Software motor worker missed watchdog interval; drive released";return;}
        double target=live.target;
        if(jog_speed!=0){
            const double lead=std::max(live.applied.jog_lead,live.applied.velocity_limit*.1);
            target=std::clamp(live.desired_target+jog_speed*dt,live.position-lead,live.position+lead);
            if(!guard.manual_page && guard.owner!=MotorOwner::homing)target=motor_operating_target(live.position,target,live.applied.minimum,live.applied.maximum);
        }
        else if(live.wave_active){const double phase=wave_phase+double(now-wave_started)/1000./live.wave_period;target=live.wave_centre+amplitude*(waveform==MotorWaveform::sine?std::sin(6.283185307179586*phase):(std::fmod(phase,1.)<.5?1.:-1.));}
        const auto permitted=motor_guard_target(guard,live.position,target);
        if(permitted.decision!=MotorMotionDecision::allow){release();live.fault_kind=MotorFaultKind::soft_limit;live.fault="Calibrated limit; outward movement blocked";return;}
        const double demand=loop.step(target,live.position,dt,live.applied);if(!std::isfinite(demand))throw std::runtime_error("Nonfinite position-loop drive; no hardware command submitted");live.requested_drive=demand;live.drive_requested_ms=clock->now_ms();drive->command(demand);live.accepted_drive=demand;live.drive_accepted_ms=clock->now_ms();live.drive=MotorDriveState::on;
        live.reference_position=loop.position;live.reference_velocity=loop.velocity;live.reference_acceleration=loop.acceleration;
        live.reference_shaft_position=loop.shaft_position();live.backlash_takeup=loop.taking_up();
        live.expected_position=loop.position;live.expected_feedback_enabled=false;live.expected_event_valid=false; // Host reference, never a claimed controller event.
        if(live.state==MotorState::moving && std::abs(loop.position-live.target)<.5*std::abs(live.applied.rescale) && loop.velocity==0 && std::abs(live.position-live.target)<=std::max(live.applied.deadband,.5*std::abs(live.applied.rescale))){live.target_attained=true;live.state=MotorState::holding;finish({live.target_command_id,MotorResultStage::attained});}
        if(jog_speed!=0 || live.wave_active){live.target=live.desired_target=target;live.target_accepted_ms=now;}
    }
    void run()noexcept{
        while(!shutdown){const auto now=clock->now_ms();try{
            std::optional<Command> next;bool changed{};MotorFeedbackDefinition new_selection;
            {std::lock_guard lock(mutex);guard=guard_value;if(selected_revision!=selection_revision){new_selection=selection;selected_revision=selection_revision;changed=true;}
                if(priority){next=std::move(priority);priority.reset();}else if(read!=write)next=std::move(queue[read++%queue.size()]);}
            if(changed){cancel_tuning("Selected tuning feedback changed");release();selected=std::move(new_selection);feedback_generation=0;live.feedback_valid=live.referenced=false;++live.coordinate_revision;offset=0;}
            if(next)execute(*next,now);
            const auto tick_now=clock->now_ms();if(!last_tick || tick_now-last_tick>=50)tick(tick_now);
            if(owns_tuning && !tuner.status().active && !finishing_tuning && !tuner.analysis_pending()){void* owner=this;tuning_owner.compare_exchange_strong(owner,nullptr);owns_tuning=false;}
        }catch(const MotorDriveFailure& e){fault(e.what(),e.code);}catch(const std::exception& e){fault(e.what());}
            {std::lock_guard lock(mutex);const auto submitted=published.command_submissions,coalesced=published.command_coalesced,overflows=published.command_overflows;live.checked_ms=clock->now_ms();published=live;if(published_tuning.run!=tuner.status().run)published_trace.clear();published_tuning=tuner.status();const auto trace=tuner.trace();if(trace.size()>published_trace.size())published_trace.insert(published_trace.end(),trace.begin()+published_trace.size(),trace.end());published.command_submissions=submitted;published.command_coalesced=coalesced;published.command_overflows=overflows;}
            WaitForSingleObject(event,10);
        }
        try{cancel_tuning("HWIO closing");release(true);}catch(...){drive->close();}std::lock_guard lock(mutex);published=live;published_tuning=tuner.status();
    }
    void finish_tuning(){
        const bool success=tuner.status().result.success;
        finishing_tuning=false;tuner.finalizing(false);
        try {release();if(!live.attached || !live.configured)throw std::runtime_error("Channel unavailable");
            if(!success){MotorLoopGains::from(before_tuning).apply(live.applied);MotorLoopGains::from(before_tuning).apply(live.requested);}
            ++live.configuration_revision;tuner.completed_at(clock->now_ms());if(live.drive==MotorDriveState::off)tuner.restoration(true);else {finishing_tuning=true;release_started=clock->now_ms();tuner.finalizing(true);}
        }catch(const std::exception& e){tuner.restoration(false,e.what());if(success)tuner.cancel("Tuning drive release failed");}
    }
    void cancel_tuning(const std::string& reason){if(!tuner.status().active && !finishing_tuning)return;tuner.cancel(reason);finish_tuning();}
};
SoftwarePositionMotor::SoftwarePositionMotor(std::unique_ptr<MotorDrive> d,std::function<MotorPositionSample(const MotorFeedbackDefinition&)> s,std::shared_ptr<MotorClock> c):impl_(std::make_unique<Impl>(std::move(d),std::move(s),std::move(c))){}
SoftwarePositionMotor::~SoftwarePositionMotor()=default;
void SoftwarePositionMotor::provider_enabled(bool on){impl_->enabled=on;if(!on)disengage();SetEvent(impl_->event);}
void SoftwarePositionMotor::feedback(const MotorFeedbackDefinition& d){std::lock_guard lock(impl_->mutex);if(impl_->selection!=d){impl_->selection=d;++impl_->selection_revision;}SetEvent(impl_->event);}
void SoftwarePositionMotor::guard(MotorGuardSnapshot g){std::lock_guard lock(impl_->mutex);impl_->guard_value=g;}
MotorSnapshot SoftwarePositionMotor::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->published;}
MotorTuningStatus SoftwarePositionMotor::tuning_status()const{std::lock_guard lock(impl_->mutex);return impl_->published_tuning;}
std::vector<MotorTuningSample> SoftwarePositionMotor::tuning_samples()const{std::lock_guard lock(impl_->mutex);return impl_->published_trace;}
bool SoftwarePositionMotor::quiescent()const{const auto s=snapshot();return !s.attached && (s.state==MotorState::closed || s.state==MotorState::fault);}
std::optional<MotorCommandResult> SoftwarePositionMotor::result(std::uint64_t n)const{std::lock_guard lock(impl_->mutex);const auto& r=impl_->results[n%impl_->results.size()];return r.id==n?std::optional{r}:std::nullopt;}
MotorCommandResult SoftwarePositionMotor::inspect(){return impl_->submit(Op::inspect);}
MotorCommandResult SoftwarePositionMotor::configure(MotorParameters p){return impl_->submit(Op::configure,0,0,0,std::move(p));}
MotorCommandResult SoftwarePositionMotor::tuning(double p,double i,double d){return impl_->submit(Op::tuning,p,i,d);}
MotorCommandResult SoftwarePositionMotor::feedforward(double v,double a){return impl_->submit(Op::ff,v,a);}
MotorCommandResult SoftwarePositionMotor::gains(MotorLoopGains g){MotorParameters p;g.apply(p);return impl_->submit(Op::gains,0,0,0,p);}
MotorCommandResult SoftwarePositionMotor::auto_tune(MotorTuningRequest r){return impl_->submit(Op::auto_tune,0,0,0,{},MotorWaveform::sine,r);}
MotorCommandResult SoftwarePositionMotor::diagnostic_drive(double value){return impl_->submit(Op::diagnostic,value);}
MotorCommandResult SoftwarePositionMotor::limits(double v,double a){return impl_->submit(Op::limits,v,a);}
MotorCommandResult SoftwarePositionMotor::target(double p){return impl_->submit(Op::target,p);}
MotorCommandResult SoftwarePositionMotor::reference(double p,double low,double high){return impl_->submit(Op::reference,p,low,high);}
MotorCommandResult SoftwarePositionMotor::engage(){return impl_->submit(Op::engage);}
MotorCommandResult SoftwarePositionMotor::jog(double v){return impl_->submit(Op::jog,v);}
MotorCommandResult SoftwarePositionMotor::wave(MotorWaveform w,double a){return impl_->submit(Op::wave,a,0,0,{},w);}
MotorCommandResult SoftwarePositionMotor::renew(){return impl_->submit(Op::renew);}
MotorCommandResult SoftwarePositionMotor::stop(){return impl_->submit(Op::hold);}
MotorCommandResult SoftwarePositionMotor::disengage(){return impl_->submit(Op::release);}
MotorCommandResult SoftwarePositionMotor::close(){return impl_->submit(Op::close);}
}
