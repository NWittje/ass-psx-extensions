#include "hwio/motor_bench.hpp"
#include "hwio/core.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace hwio {
namespace {
struct BenchClock final:MotorClock {std::uint64_t now_ms()const noexcept override{return GetTickCount64();}};
bool number(double n){return std::isfinite(n);}
bool optional_number(const std::optional<double>& n){return !n || number(*n);}
void write_optional(std::ostream& out,const std::optional<double>& v){if(v)out<<*v;else out<<'_';out<<' ';}
bool read_optional(std::istream& in,std::optional<double>& v){std::string token;if(!(in>>token))return false;if(token=="_"){v.reset();return true;}char* end{};const double n=strtod(token.c_str(),&end);if(end==token.c_str() || *end || !number(n))return false;v=n;return true;}
std::string tuning_identity(const MotorDeviceAddress& a){std::ostringstream s;s<<std::quoted(a.provider)<<' '<<std::quoted(a.device)<<' '<<a.serial<<' '<<a.hub_port<<' '<<a.channel<<' '<<a.model<<' '<<unsigned(a.mode)<<' '<<a.remote<<' '<<std::quoted(a.server)<<' '<<a.port;return s.str();}
}
bool motor_definition_valid(const MotorDefinition& d,std::string& error){
    const auto& a=d.address;
    if(a.provider.size()>64 || a.device.size()>1024 || a.server.size()>253 || a.provider.find_first_of("\r\n")!=a.provider.npos || a.device.find_first_of("\r\n")!=a.device.npos || a.server.find_first_of("\r\n\t /\\")!=a.server.npos || a.serial<0 || (a.assigned() && a.serial<=0) || a.hub_port<-1 || a.hub_port>255 || a.channel<-1 || a.channel>65535 || a.model<0 || !a.port || a.port>65535){error="Invalid motor device/address";return false;}
    if(d.sensor_buffer && (!number(*d.sensor_buffer) || *d.sensor_buffer<=0)){error="Sensor buffer must be positive raw sensor units, or blank for Auto";return false;}
    if(!number(d.backlash) || d.backlash<0){error="Backlash must be zero or positive raw potentiometer counts";return false;}
    if(!optional_number(d.minimum) || !optional_number(d.maximum) || !optional_number(d.maximum_velocity) || !optional_number(d.maximum_acceleration) || !optional_number(d.current_limit) || !optional_number(d.vff) || !optional_number(d.aff) || !number(d.velocity_fraction) || !number(d.acceleration_percent) || !number(d.kp) || !number(d.ki) || !number(d.kd) || !number(d.amplitude) || d.velocity_fraction<0 || d.velocity_fraction>1 || d.acceleration_percent<.1 || d.acceleration_percent>100 || d.amplitude<=0 || (d.maximum_velocity && *d.maximum_velocity<=0) || (d.maximum_acceleration && *d.maximum_acceleration<=0) || (d.current_limit && *d.current_limit<0) || bool(d.minimum)!=bool(d.maximum) || (d.minimum && *d.minimum>=*d.maximum) || (d.waveform!=MotorWaveform::sine && d.waveform!=MotorWaveform::square)){error="Enter finite motor settings; both travel bounds or neither, positive limits/amplitude and valid slider ranges";return false;}
    if(unsigned(a.mode)>2 || unsigned(d.feedback.kind)>2 || d.feedback.input_profile.size()>32768 || !number(d.drive.maximum_duty) || d.drive.maximum_duty<=0 || d.drive.maximum_duty>1 || !number(d.drive.pwm_ramp) || d.drive.pwm_ramp<=0){error="Invalid motor mode, feedback or PWM settings";return false;}
    for(const auto* value:{&d.drive.native_velocity,&d.drive.native_acceleration,&d.drive.speed_kp,&d.drive.speed_ki,&d.drive.speed_kd,&d.drive.speed_deadband})if(!optional_number(*value) || (*value && **value<0)){error="Invalid native drive setting";return false;}
    const auto& encoder=d.feedback.encoder;if(d.feedback.kind==MotorPositionSource::encoder && encoder.assigned() && (encoder.provider!="phidget22" || encoder.serial<=0 || encoder.hub_port<-1 || encoder.hub_port>255 || encoder.channel>65535 || !encoder.port || encoder.port>65535 || encoder.server.size()>253 || encoder.server.find_first_of("\r\n\t /\\")!=std::string::npos)){error="Invalid exact encoder feedback address";return false;}
    if(d.drive.speed_feedback && (*d.drive.speed_feedback<1 || *d.drive.speed_feedback>2)){error="Invalid native speed feedback source";return false;}
    error.clear();return true;
}
bool load_motor_settings(const std::wstring& directory,std::array<MotorDefinition,motor_slots>& definitions,std::string& error){
    try{
        const auto file=std::filesystem::path(directory)/L"motor-settings.cfg";if(!std::filesystem::exists(file)){error.clear();return true;}
        if(std::filesystem::file_size(file)>524288)throw std::runtime_error("Motor settings exceed 512 KiB");
        std::ifstream in(file,std::ios::binary);std::string header;std::getline(in,header);if(!header.empty() && header.back()=='\r')header.pop_back();
        const bool backlash=header=="[hwio-motor-settings-v4]",buffered=backlash || header=="[hwio-motor-settings-v3]",extended=buffered || header=="[hwio-motor-settings-v2]";
        if(!extended && header!="[hwio-motor-settings-v1]")throw std::runtime_error("Invalid motor-settings.cfg header");
        std::array<MotorDefinition,motor_slots> parsed;
        for(unsigned n=0;n<(extended?motor_slots:position_motor_slots);++n){auto& d=parsed[n];auto& a=d.address;unsigned slot{},wave{};
            if(!(in>>slot>>std::quoted(a.provider)>>std::quoted(a.device)>>a.serial>>a.hub_port>>a.channel>>a.model>>a.remote>>std::quoted(a.server)>>a.port) || slot!=n)throw std::runtime_error("Invalid motor address/slot");
            for(auto* v:{&d.minimum,&d.maximum,&d.maximum_velocity,&d.maximum_acceleration,&d.current_limit,&d.vff,&d.aff})if(!read_optional(in,*v))throw std::runtime_error("Invalid optional motor setting");
            if(!(in>>d.velocity_fraction>>d.acceleration_percent>>d.kp>>d.ki>>d.kd>>d.amplitude>>wave) || wave>1)throw std::runtime_error("Invalid motor tuning");
            d.waveform=static_cast<MotorWaveform>(wave);
            if(extended){unsigned mode{},kind{};auto& e=d.feedback.encoder;
                if(!(in>>mode>>kind>>d.feedback.inverted>>std::quoted(d.feedback.input_profile)>>std::quoted(e.provider)>>std::quoted(e.device)>>e.serial>>e.hub_port>>e.channel>>e.model>>e.remote>>std::quoted(e.server)>>e.port>>d.drive.maximum_duty>>d.drive.pwm_ramp) || mode>2 || kind>2)throw std::runtime_error("Invalid motor drive/feedback definition");
                a.mode=static_cast<MotorDriveMode>(mode);d.feedback.kind=static_cast<MotorPositionSource>(kind);
                for(auto* value:{&d.drive.native_velocity,&d.drive.native_acceleration,&d.drive.speed_kp,&d.drive.speed_ki,&d.drive.speed_kd,&d.drive.speed_deadband})if(!read_optional(in,*value))throw std::runtime_error("Invalid native drive settings");
                int source{};if(!(in>>source) || source<0 || source>2)throw std::runtime_error("Invalid native feedback selection");if(source)d.drive.speed_feedback=source;
            }
            if(buffered && !read_optional(in,d.sensor_buffer))throw std::runtime_error("Invalid sensor buffer");
            if(backlash && !(in>>d.backlash))throw std::runtime_error("Invalid backlash setting");
            if(!motor_definition_valid(d,error))return false;
        }
        in>>std::ws;if(!in.eof())throw std::runtime_error("Unexpected motor settings data");definitions=std::move(parsed);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool save_motor_settings(const std::wstring& directory,const std::array<MotorDefinition,motor_slots>& definitions,std::string& error){
    try{
        std::ostringstream text;text.imbue(std::locale::classic());text<<"[hwio-motor-settings-v4]\n"<<std::setprecision(17);
        for(unsigned n=0;n<motor_slots;++n){const auto& d=definitions[n];if(!motor_definition_valid(d,error))return false;const auto& a=d.address;
            text<<n<<' '<<std::quoted(a.provider)<<' '<<std::quoted(a.device)<<' '<<a.serial<<' '<<a.hub_port<<' '<<a.channel<<' '<<a.model<<' '<<a.remote<<' '<<std::quoted(a.server)<<' '<<a.port<<' ';
            for(const auto* v:{&d.minimum,&d.maximum,&d.maximum_velocity,&d.maximum_acceleration,&d.current_limit,&d.vff,&d.aff})write_optional(text,*v);
            text<<d.velocity_fraction<<' '<<d.acceleration_percent<<' '<<d.kp<<' '<<d.ki<<' '<<d.kd<<' '<<d.amplitude<<' '<<static_cast<unsigned>(d.waveform)<<' ';
            const auto& e=d.feedback.encoder;text<<unsigned(a.mode)<<' '<<unsigned(d.feedback.kind)<<' '<<d.feedback.inverted<<' '<<std::quoted(d.feedback.input_profile)<<' '<<std::quoted(e.provider)<<' '<<std::quoted(e.device)<<' '<<e.serial<<' '<<e.hub_port<<' '<<e.channel<<' '<<e.model<<' '<<e.remote<<' '<<std::quoted(e.server)<<' '<<e.port<<' '<<d.drive.maximum_duty<<' '<<d.drive.pwm_ramp<<' ';
            for(const auto* value:{&d.drive.native_velocity,&d.drive.native_acceleration,&d.drive.speed_kp,&d.drive.speed_ki,&d.drive.speed_kd,&d.drive.speed_deadband})write_optional(text,*value);
            text<<d.drive.speed_feedback.value_or(0)<<' ';write_optional(text,d.sensor_buffer);text<<d.backlash<<'\n';
        }
        if(text.str().size()>524288)throw std::runtime_error("Motor settings exceed 512 KiB");
        const auto file=std::filesystem::path(directory)/L"motor-settings.cfg";const auto temporary=file.wstring()+L".tmp";std::ofstream out(std::filesystem::path(temporary),std::ios::binary|std::ios::trunc);out<<text.str();out.flush();if(!out)throw std::runtime_error("Cannot write motor settings staging file");out.close();
        const bool exists=std::filesystem::exists(file);const auto backup=file.wstring()+L".bak";
        if(!(exists?ReplaceFileW(file.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr):MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH)))throw std::runtime_error("Cannot commit motor settings: Windows error "+std::to_string(GetLastError()));
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
struct MotorBench::Impl {
    enum class Op {activate,assign,edit,tuning,clear,save,revert,zero,drive,play,auto_tune,find_backlash,hold,disengage,hide};
    struct Command {Op op{};unsigned slot{};int direction{};MotorDefinition definition;std::uint64_t epoch{},draft_revision{};bool initialization_change{};};
    struct Slot {
        MotorBenchView view;MotorDefinition requested;MotorGuardSnapshot guard;std::uint64_t draft_revision{};std::shared_ptr<PositionMotorBackend> backend,candidate;MotorDeviceAddress candidate_address;
        std::shared_ptr<PositionMotorBackend> release_backend;MotorCommandResult opening,configuration,referencing,motion,tuning;std::optional<Command> deferred;std::string initialization_error;
        std::uint64_t heartbeat{},mapping_heartbeat{},renewed{},zero_deadline{},sample_ms{},motion_generation{},motion_coordinate{};
        std::uint64_t candidate_revision{},operation_epoch{},mapping_epoch{},mapping_failure_revision{},configuration_generation{},acknowledged_generation{};unsigned queued_initializations{};bool configuration_ready{},full_configuration{},initialization_failed{},want_zero{},defaults{true},pending_motion{},want_wave{};int direction{};
        MotorCommandResult auto_command;std::uint64_t auto_revision{},auto_run{},auto_completed{};
        MotorBacklashFinder finder;MotorFeedbackDefinition find_feedback;ShaftReader shaft_reader;MotorCommandResult find_command;
        unsigned find_revision{};int find_engage{};bool find_accepted{};std::uint64_t find_generation{},find_coordinate{},find_draft{};
        std::vector<MotorTuningSample> last_tuning_trace;
        static constexpr unsigned failure_capacity=8;
        std::array<MotorInitializationFailure,failure_capacity> failures{};unsigned failure_begin{},failure_count{};
        static constexpr unsigned history_capacity=8192;
        std::array<MotorTraceSample,history_capacity> samples{};unsigned begin{},count{};
        std::array<MotorTraceSample,6> bucket{};bool bucket_valid{};std::uint64_t bucket_start{};
    };
    std::wstring directory;MotorBackendFactory factory;std::shared_ptr<MotorClock> clock;
    PositionSampler potentiometer;std::function<ShaftReader(const MotorDeviceAddress&)> shaft_factory;
    mutable std::mutex mutex;std::mutex queue_mutex;std::array<Slot,motor_slots> slots;
    std::array<Command,64> queue;std::array<std::optional<Command>,motor_slots> edits;unsigned read{},write{};
    std::thread worker;HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};std::atomic<bool> shutdown{},overflow{},hide_requested{};
    std::string storage_problem;std::uint64_t feedback_revision{1},next_tuning_run{};
    std::array<std::atomic<std::uint64_t>,motor_slots> epochs{};
    std::array<std::atomic<unsigned>,motor_slots> cancellations{}; // 1 holds, 2 removes drive; serviced ahead of queued movement.
    std::vector<std::shared_ptr<PositionMotorBackend>> retired;
    struct SaveResult {unsigned slot{};MotorDefinition definition;bool success{};std::string error;};
    std::future<SaveResult> saving;
    void retire(std::shared_ptr<PositionMotorBackend>& b){if(b){b->close();retired.push_back(std::move(b));}}

    Impl(std::wstring path,MotorBackendFactory make,std::shared_ptr<MotorClock> time):directory(std::move(path)),factory(std::move(make)),clock(time?std::move(time):std::make_shared<BenchClock>()){
        if(!event || !factory)throw std::runtime_error("Cannot create motor bench");std::array<MotorDefinition,motor_slots> saved;
        load_motor_settings(directory,saved,storage_problem);for(unsigned n=0;n<motor_slots;++n){slots[n].requested=slots[n].view.saved=slots[n].view.draft=saved[n];slots[n].defaults=saved[n]==MotorDefinition{};slots[n].view.status=storage_problem.empty()?"Unassigned":storage_problem;}
        worker=std::thread([this]{run();});
    }
    ~Impl(){shutdown=true;SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    enum EditImpact : unsigned { bench_change=1, gain_change=2, limit_change=4, configuration_change=8 };
    static unsigned edit_impact(const MotorDefinition& before,const MotorDefinition& after){
        unsigned impact{};auto live=before;
        if(before.waveform!=after.waveform || before.amplitude!=after.amplitude)impact|=bench_change;
        if(before.kp!=after.kp || before.ki!=after.ki || before.kd!=after.kd || before.vff!=after.vff || before.aff!=after.aff)impact|=gain_change;
        if(before.maximum_velocity!=after.maximum_velocity || before.maximum_acceleration!=after.maximum_acceleration || before.velocity_fraction!=after.velocity_fraction || before.acceleration_percent!=after.acceleration_percent)impact|=limit_change;
        live.sensor_buffer=after.sensor_buffer;live.waveform=after.waveform;live.amplitude=after.amplitude;live.kp=after.kp;live.ki=after.ki;live.kd=after.kd;live.vff=after.vff;live.aff=after.aff;live.maximum_velocity=after.maximum_velocity;live.maximum_acceleration=after.maximum_acceleration;live.velocity_fraction=after.velocity_fraction;live.acceleration_percent=after.acceleration_percent;
        return live==after?impact:impact|configuration_change;
    }
    void submit(Command command){if(command.slot>=motor_slots || shutdown)return;
        const bool edit=command.op==Op::edit || command.op==Op::tuning;
        const bool cancel=command.op==Op::hold || command.op==Op::disengage || command.op==Op::clear || command.op==Op::revert || command.op==Op::assign || command.op==Op::zero;
        // Admission and ownership use the same mutex as claim/engage. A queued
        // replacement must already exclude the old ready channel, before the
        // worker creates its candidate or calls the SDK configuration operation.
        std::unique_lock ownership(mutex);
        if(shutdown)return;auto& s=slots[command.slot];
        if((s.view.automatic.active || s.auto_command.id) && (edit || command.op==Op::save || command.op==Op::drive || command.op==Op::play || command.op==Op::zero)){s.view.status="Stop automatic tuning before changing settings or movement";return;}
        if(s.finder.status().active && (edit || command.op==Op::save || command.op==Op::drive || command.op==Op::play || command.op==Op::auto_tune || command.op==Op::zero)){s.view.status="Stop backlash measurement before changing settings or movement";return;}
        if(command.op==Op::assign && command.definition.address.assigned() && ((command.slot<position_motor_slots)!=(command.definition.address.mode==MotorDriveMode::position))){s.view.status="Select an onboard position controller for Motor0–9, or a PWM/velocity drive for Motor10–19";return;}
        if(edit){command.definition.address=s.view.draft.address;std::string reason;if(!motor_definition_valid(command.definition,reason)){s.view.status=reason;return;}}
        const bool full_edit=edit && ((edit_impact(s.requested,command.definition)&configuration_change)!=0 || s.queued_initializations || s.candidate || s.full_configuration);
        if(edit){s.requested=command.definition;command.draft_revision=++s.draft_revision;}
        if(command.op==Op::save){command.definition=s.requested;command.definition.address=s.view.draft.address;command.draft_revision=s.draft_revision;}
        if(edit || cancel)++feedback_revision;
        if(command.op==Op::revert){command.definition=s.view.saved;s.requested=s.view.saved;command.draft_revision=++s.draft_revision;}
        const bool initialize=command.op==Op::activate && (!s.backend || !s.configuration_ready || s.initialization_failed || !s.view.live.attached);
        command.initialization_change=initialize || full_edit || command.op==Op::assign || command.op==Op::clear || command.op==Op::revert || command.op==Op::zero;
        const bool cancels=cancel || full_edit || initialize;const bool was_owned=cancels && s.view.mapping_owned;
        if(cancel)s.view.operator_stopped=true;
        // Coordinate-preserving configuration pauses a mapping; only explicit
        // ownership/coordinate operations revoke its physical session map.
        if(cancel || initialize)s.view.mapping_owned=false;
        command.epoch=cancels?++epochs[command.slot]:epochs[command.slot].load();
        if(full_edit && was_owned)s.mapping_epoch=command.epoch;
        if(cancels){const unsigned mode=command.op==Op::hold && !was_owned?1u:2u;auto prior=cancellations[command.slot].load();while(prior<mode && !cancellations[command.slot].compare_exchange_weak(prior,mode)){} }
        if(command.op==Op::hold || command.op==Op::disengage){SetEvent(event);return;}
        std::lock_guard lock(queue_mutex);
        if(edit){if(edits[command.slot] && edits[command.slot]->initialization_change)--s.queued_initializations;if(command.initialization_change)++s.queued_initializations;edits[command.slot]=std::move(command);}
        else if(write-read==queue.size())overflow=true;
        else {if(command.initialization_change)++s.queued_initializations;queue[write++%queue.size()]=std::move(command);}
        SetEvent(event);
    }
    static bool initializing(const Slot& s){return s.queued_initializations || s.candidate || s.opening.id || s.configuration.id || s.want_zero || s.referencing.id;}
    static void failed_initialization(Slot& s,std::string error,bool distinct=false){
        if(!s.initialization_failed || distinct){
            if(s.view.initialization_failure_revision==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Motor initialization failure revision exhausted");
            MotorInitializationFailure failure{++s.view.initialization_failure_revision,s.acknowledged_generation!=0,false,std::move(error)};
            if(!s.initialization_failed)s.initialization_error=failure.reason;
            if(s.failure_count==Slot::failure_capacity){s.failure_begin=(s.failure_begin+1)%Slot::failure_capacity;--s.failure_count;}
            s.failures[(s.failure_begin+s.failure_count++)%Slot::failure_capacity]=std::move(failure);
        }
        s.initialization_failed=true;s.configuration_ready=false;s.view.status=s.initialization_error;
    }
    bool mapping_current(unsigned n,const Slot& s)const{return !s.view.manual_page && s.view.mapping_owned && s.mapping_epoch==epochs[n] && s.mapping_failure_revision==s.view.initialization_failure_revision && !shutdown && !overflow && !cancellations[n];}
    static MotorGuardSnapshot effective_guard(const Slot& s){auto g=s.guard;g.manual_page=s.view.manual_page;return g;}
    void forget_measurement(Slot& s){s.shaft_reader={};s.find_feedback={};s.find_command={};s.find_engage=0;++feedback_revision;}
    void release(Slot& slot,bool hold=false){if(slot.finder.status().active){slot.finder.cancel("Backlash measurement cancelled");slot.view.backlash_find=slot.finder.status();forget_measurement(slot);}if(slot.backend)slot.backend->guard(effective_guard(slot));slot.deferred.reset();slot.pending_motion=false;slot.want_zero=false;slot.referencing={};slot.motion={};slot.zero_deadline=0;slot.direction=0;slot.view.running=slot.view.jogging=false;slot.view.end_ms=clock->now_ms();if(slot.backend){if(hold)slot.backend->stop();else {slot.release_backend=slot.backend;slot.view.release=slot.backend->disengage();}}flush(slot);}
    void push(Slot& s,MotorTraceSample value){if(s.count==Slot::history_capacity){s.begin=(s.begin+1)%Slot::history_capacity;--s.count;}s.samples[(s.begin+s.count++)%Slot::history_capacity]=value;++s.view.history_revision;}
    void flush(Slot& s){if(!s.bucket_valid)return;std::sort(s.bucket.begin(),s.bucket.end(),[](const auto& a,const auto& b){return a.ms<b.ms;});std::uint64_t previous=~std::uint64_t{};for(const auto& v:s.bucket)if(v.ms!=previous){push(s,v);previous=v.ms;}s.bucket_valid=false;}
    void record(Slot& s,const MotorSnapshot& v){
        if(!s.view.running && !s.view.jogging && !s.view.automatic.active)return;if(v.checked_ms==s.sample_ms)return;s.sample_ms=v.checked_ms;
        MotorTraceSample value{v.checked_ms,s.view.automatic.active?s.view.automatic.target:v.target,v.position,v.feedback_valid,!s.view.automatic.active || s.view.automatic.command_valid};
        // Six extrema/endpoints per bucket must fit a complete displayed cycle,
        // including long, slowly driven waves, inside the fixed history ring.
        constexpr unsigned buckets=Slot::history_capacity/8;
        const double interval=std::clamp(std::ceil(s.view.period/buckets*1050.),1.,double(std::numeric_limits<std::uint64_t>::max()/2));
        const auto bucket_ms=static_cast<std::uint64_t>(interval);
        if(!s.bucket_valid || value.ms-s.bucket_start>=bucket_ms || !value.valid){flush(s);s.bucket.fill(value);s.bucket_start=value.ms;s.bucket_valid=true;}
        else {s.bucket[5]=value;if(value.actual<s.bucket[1].actual)s.bucket[1]=value;if(value.actual>s.bucket[2].actual)s.bucket[2]=value;if(value.command<s.bucket[3].command)s.bucket[3]=value;if(value.command>s.bucket[4].command)s.bucket[4]=value;}
    }
    void break_trace(Slot& s){flush(s);push(s,{clock->now_ms(),s.view.live.target,s.view.live.position,false});}
    bool parameters(Slot& s,MotorParameters& p,bool bench_motion=false){
        const auto& d=s.view.draft;const auto& c=s.view.live.capabilities;std::string problem;
        if(!motor_definition_valid(d,problem)){s.view.status=problem;return false;}
        if(!d.maximum_velocity || !d.maximum_acceleration || (c.current_limit && !d.current_limit)){s.view.status="Enter maximum velocity/acceleration and required current limit";return false;}
        if(*d.maximum_velocity<c.minimum_velocity || *d.maximum_velocity>c.maximum_velocity || *d.maximum_acceleration<c.minimum_acceleration || *d.maximum_acceleration>c.maximum_acceleration){s.view.status="Configured maxima exceed this controller's supported range";return false;}
        if(!c.feed_forward && (d.vff || d.aff)){s.view.status="Feed-forward unavailable on this backend";return false;}
        if(bench_motion && d.velocity_fraction<=0){s.view.status="Velocity is zero";return false;}
        p.drive=d.drive;p.vff=d.vff.value_or(0);p.aff=d.aff.value_or(0);p.units=c.software_position?"sensor counts":"encoder pulses";p.kp=d.kp;p.ki=d.ki;p.kd=d.kd;p.velocity_limit=*d.maximum_velocity*(bench_motion?d.velocity_fraction:1.);p.acceleration=*d.maximum_acceleration*(bench_motion?d.acceleration_percent/100.:1.);p.current_limit=c.current_limit?d.current_limit:std::optional<double>{};
        p.backlash=c.software_position && d.feedback.kind==MotorPositionSource::analogue?d.backlash:0;
        p.normalize_pid=c.normalize_pid?std::optional<bool>{true}:std::optional<bool>{};p.numeric_range_only=!d.minimum;p.minimum=d.minimum.value_or(c.minimum_position);p.maximum=d.maximum.value_or(c.maximum_position);p.jog_lead=std::max(1.,p.velocity_limit*.2);
        p.feedback_interval_ms=c.software_position?50u:std::max(20u,c.minimum_interval_ms);p.failsafe_ms=std::max(1000u,c.minimum_failsafe_ms);p.failsafe_refresh_ms=100;
        return true;
    }
    void open(Slot& s,const MotorDeviceAddress& address){
        if(!address.assigned())return;
        if(!s.candidate && s.backend && s.view.draft.address==address && s.view.live.attached && s.configuration_ready && !s.initialization_failed)return;
        release(s);retire(s.candidate);s.opening={};s.view.pending=s.configuration.id!=0;
        if(s.backend && s.view.draft.address==address && s.view.live.attached){
            // Explicit activation/reselection can acknowledge the retained
            // channel after a rejected replacement. Reuse its owned handle;
            // opening another backend for that address would conflict with it.
            if(s.configuration.id && !s.initialization_failed)return;
            if(s.configuration_ready && !s.initialization_failed)return;
            MotorParameters p;if(parameters(s,p)){s.initialization_failed=false;s.initialization_error.clear();s.configuration_ready=false;s.full_configuration=true;s.configuration=(s.backend->feedback(s.view.draft.feedback),s.backend->configure(p));s.view.pending=true;s.view.status="Reconfiguring retained assigned motor; disengaged";}
            return;
        }
        s.initialization_failed=false;s.initialization_error.clear();
        for(const auto& other:slots)if(&other!=&s && (other.view.draft.address.same_physical(address) || (other.candidate && other.candidate_address.same_physical(address)))){failed_initialization(s,"Motor resource occupied by Motor "+std::to_string(&other-slots.data())+"; Clear its assignment before changing control mode/owner");return;}
        if(s.backend && s.view.draft.address.same_physical(address) && s.view.draft.address.mode!=address.mode){failed_initialization(s,"Clear this motor before selecting another physical control class");return;}
        try {if(retired.size()>=32)throw std::runtime_error("Wait for previous motor sessions to close");s.candidate_revision=s.draft_revision;s.candidate=factory(address);if(!s.candidate)throw std::runtime_error("Motor backend unavailable");s.candidate_address=address;s.candidate->feedback(s.view.draft.feedback);s.opening=s.candidate->inspect();s.view.pending=true;s.view.status="Opening disengaged motor channel";}catch(const std::exception& e){failed_initialization(s,e.what());retire(s.candidate);s.view.pending=false;}
    }
    void execute(Command& command){auto& s=slots[command.slot];
        if((command.op==Op::drive || command.op==Op::play || command.op==Op::auto_tune || command.op==Op::find_backlash || command.op==Op::zero) && command.epoch!=epochs[command.slot])return;
        if((command.op==Op::edit || command.op==Op::tuning) && command.draft_revision!=s.draft_revision)return;

        switch(command.op){
        case Op::activate:if(!s.candidate)open(s,s.view.draft.address);break;
        case Op::assign:s.view.mapping_owned=false;open(s,command.definition.address);break;
        case Op::clear:s.view.mapping_owned=false;if(s.backend && s.view.automatic.run)s.last_tuning_trace=s.backend->tuning_samples();release(s);retire(s.candidate);retire(s.backend);s.view.draft.address=s.requested.address={};s.view.live={};s.view.automatic.active=s.view.automatic.finalizing=false;s.view.automatic.phase=MotorTunePhase::cancelled;s.view.automatic.message="Tuning cancelled by Clear; channel closing";s.auto_command={};s.auto_run=0;s.configuration_ready=s.full_configuration=s.initialization_failed=false;s.initialization_error.clear();s.configuration_generation=s.acknowledged_generation=0;s.configuration=s.opening=s.referencing={};s.view.pending=false;s.view.status="Unassigned; draft retained";break;
        case Op::edit:case Op::tuning:{command.definition.address=s.view.draft.address;std::string problem;if(!motor_definition_valid(command.definition,problem)){s.view.status=problem;break;}const auto impact=edit_impact(s.view.draft,command.definition);
            const bool mapped_bench_sliders_only=s.view.mapping_owned && s.view.draft.maximum_velocity==command.definition.maximum_velocity && s.view.draft.maximum_acceleration==command.definition.maximum_acceleration;
            s.view.draft=std::move(command.definition);s.defaults=false;
            if(!command.initialization_change && !s.full_configuration && !(impact&configuration_change)){
                if((impact&bench_change) && s.view.running)release(s,true);
                if(s.view.draft.velocity_fraction==0 && !s.view.mapping_owned){release(s);s.view.status="Velocity zero; bench motor paused";}
                if(s.backend && s.view.live.configured){
                    if(impact&gain_change){s.tuning=s.view.live.capabilities.software_position?s.backend->gains({s.view.draft.kp,s.view.draft.ki,s.view.draft.kd,s.view.draft.vff.value_or(0),s.view.draft.aff.value_or(0)}):s.backend->tuning(s.view.draft.kp,s.view.draft.ki,s.view.draft.kd);}
                    if((impact&limit_change) && !mapped_bench_sliders_only){MotorParameters p;if(parameters(s,p,s.view.running || s.view.jogging)){s.configuration=s.backend->limits(p.velocity_limit,p.acceleration);s.view.pending=true;}else release(s);}
                }
            }else {release(s);if(s.backend && s.view.live.attached){MotorParameters p;if(parameters(s,p)){s.initialization_failed=false;s.initialization_error.clear();s.configuration_ready=false;s.full_configuration=true;s.configuration=(s.backend->feedback(s.view.draft.feedback),s.backend->configure(p));s.view.pending=true;}}}break;}
        case Op::save:{if(!storage_problem.empty()){s.view.status=storage_problem;break;}if(s.candidate && s.candidate_address!=s.view.draft.address){s.view.status="Wait for channel inspection before saving settings";break;}if(saving.valid()){s.view.status="Save already pending";break;}std::array<MotorDefinition,motor_slots> values;for(unsigned n=0;n<motor_slots;++n)values[n]=slots[n].view.saved;const auto path=directory;const auto slot=command.slot;
            values[command.slot]=command.definition;const auto captured=command.definition;
            saving=std::async(std::launch::async,[path,values,slot,captured]{SaveResult result{slot,captured};result.success=save_motor_settings(path,values,result.error);return result;});s.view.status="Saving settings";break;}
        case Op::revert:{s.view.mapping_owned=false;release(s);retire(s.candidate);s.opening={};s.view.pending=false;const auto saved=command.definition;if(saved.address==s.view.draft.address){s.view.draft=saved;MotorParameters p;if(s.backend && parameters(s,p)){s.initialization_failed=false;s.initialization_error.clear();s.configuration_ready=false;s.full_configuration=true;s.configuration=(s.backend->feedback(s.view.draft.feedback),s.backend->configure(p));s.view.pending=true;}}else {retire(s.backend);s.view.draft=saved;open(s,saved.address);}s.defaults=false;s.view.status="Reverted to saved settings; motor disengaged";break;}
        case Op::zero:{s.view.mapping_owned=false;release(s);MotorParameters p;if(!s.backend || !parameters(s,p))break;s.initialization_failed=false;s.initialization_error.clear();s.configuration_ready=false;s.full_configuration=true;s.configuration=(s.backend->feedback(s.view.draft.feedback),s.backend->configure(p));s.view.pending=true;s.want_zero=true;s.operation_epoch=command.epoch;s.zero_deadline=clock->now_ms()+2000;s.view.status="Waiting for stationary feedback to zero";break;}
        case Op::find_backlash:{
            if(s.finder.status().active){release(s);s.view.status="Backlash measurement cancelled";break;}
            if(!s.view.manual_page || s.view.mapping_owned){s.view.status="Open this motor's page before measuring backlash";break;}
            if(std::any_of(slots.begin(),slots.end(),[](const Slot& x){return x.finder.status().active || x.view.automatic.active || x.auto_command.id;})){s.view.status="Another motor measurement is active";break;}
            if(initializing(s)){s.deferred=command;s.view.pending=true;break;}
            MotorParameters p;if(!s.backend || !s.configuration_ready || !s.view.live.feedback_valid || !parameters(s,p,true))break;
            if(command.slot<position_motor_slots && (s.view.live.applied.velocity_limit!=p.velocity_limit || s.view.live.applied.acceleration!=p.acceleration)){
                s.configuration=s.backend->limits(p.velocity_limit,p.acceleration);s.deferred=command;s.view.pending=true;break;
            }
            auto feedback=command.slot<position_motor_slots?command.definition.feedback:s.view.draft.feedback;
            if(feedback.kind!=MotorPositionSource::analogue || feedback.input_profile.empty() || !potentiometer){s.view.status="Select a potentiometer for backlash measurement";break;}
            release(s);s.find_feedback=std::move(feedback);
            try{if(command.slot>=position_motor_slots){if(!shaft_factory)throw std::runtime_error("Find backlash requires motor-shaft feedback; potentiometer-only movement and Auto tune remain available");s.shaft_reader=shaft_factory(s.view.draft.address);if(!s.shaft_reader)throw std::runtime_error("Find backlash requires motor-shaft feedback; no measurement started");}}
            catch(const std::exception& e){s.view.status=e.what();forget_measurement(s);break;}
            s.find_generation=s.view.live.generation;s.find_coordinate=s.view.live.coordinate_revision;s.find_draft=s.draft_revision;s.operation_epoch=command.epoch;s.heartbeat=clock->now_ms();
            s.finder.start(clock->now_ms());s.find_revision=0;s.find_accepted=false;s.find_command={};s.view.backlash_find=s.finder.status();s.view.status=s.view.backlash_find.message;++feedback_revision;break;}
        case Op::drive:case Op::play:case Op::auto_tune:{
            if(command.op==Op::auto_tune && (s.view.automatic.active || s.auto_command.id)){release(s);s.view.status="Automatic tuning cancellation requested";break;}
            if(command.op==Op::auto_tune && (s.view.draft.kp<0 || s.view.draft.ki<0 || s.view.draft.kd<0)){s.view.status="Enter valid nonnegative host PID gains (zero is allowed before Auto tune)";break;}
            if(s.view.mapping_owned){s.view.status="Motor is owned by an axis mapping; Stop that mapping first";break;}if(initializing(s) || (command.op==Op::auto_tune && s.tuning.id)){s.deferred=command;s.heartbeat=clock->now_ms();s.view.pending=true;s.view.status="Waiting for committed settings before movement";break;}if(!s.backend || !s.view.live.attached || !s.view.live.configured || !s.view.live.feedback_valid){s.view.status="Configure valid motor feedback before motion";break;}MotorParameters requested;if(!parameters(s,requested,true))break;
            if(!s.view.manual_page && s.guard.required && !s.guard.valid){s.view.status=s.guard.valid?"Unknown motor direction near an endpoint; use the bounded Home probe":"Calibrated physical feedback unavailable; motor remains disengaged";release(s);break;}
            if(command.op==Op::play){const auto a=s.view.draft.amplitude,c=s.view.live.position;if(!number(c-a) || !number(c+a) || (!s.view.manual_page && (c-a<s.view.live.applied.minimum || c+a>s.view.live.applied.maximum))){s.view.status="Wave amplitude exceeds motor travel; motor remains disengaged";release(s);break;}if(motor_wave_period(s.view.draft.waveform,a,requested.velocity_limit,requested.acceleration,requested.feedback_interval_ms/1000.)<=0){s.view.status="Wave period is outside the numeric range; motor remains disengaged";release(s);break;}}
            // A mapping uses physical maxima. Before manual/wave ownership,
            // acknowledge the bench target limits rather than inheriting them.
            if(s.view.live.applied.velocity_limit!=requested.velocity_limit || s.view.live.applied.acceleration!=requested.acceleration){s.configuration=s.backend->limits(requested.velocity_limit,requested.acceleration);if(!s.configuration.id){const auto reason=s.configuration.reason.empty()?"Bench motion limits unavailable":s.configuration.reason;release(s);s.view.pending=false;s.view.status=reason;break;}s.deferred=command;s.heartbeat=clock->now_ms();s.view.pending=true;s.view.status="Applying bench motion limits";break;}
            if(command.op==Op::auto_tune){
                if(command.slot<position_motor_slots || !s.view.manual_page){s.view.status="Auto tune requires the visible Motor10–19 page";break;}
                release(s);MotorTuningRequest r;r.mode=s.view.draft.address.mode;r.run=++next_tuning_run;r.session=s.view.live.generation;r.coordinate=s.view.live.coordinate_revision;r.draft_revision=s.draft_revision;r.travel=s.view.draft.amplitude;r.quantum=s.view.live.feedback_quantum;r.code_count=s.view.live.feedback_code_count;r.buffer=s.view.draft.sensor_buffer.value_or(5+5*std::clamp((r.code_count-512.)/(4096.-512.),0.,1.));
                r.motor_identity=tuning_identity(s.view.draft.address);const auto& feedback=s.view.draft.feedback;r.feedback_identity=std::to_string(unsigned(feedback.kind))+" "+std::to_string(feedback.inverted)+" "+(feedback.kind==MotorPositionSource::encoder?tuning_identity(feedback.encoder):feedback.input_profile);s.last_tuning_trace.clear();
                s.backend->guard(effective_guard(s));s.auto_revision=s.draft_revision;s.auto_run=r.run;s.auto_command=s.backend->auto_tune(r);s.view.automatic={};
                if(!s.auto_command.id || !s.auto_command){s.view.pending=false;s.view.status=s.auto_command.reason.empty()?"Automatic tuning unavailable on this backend":s.auto_command.reason;s.auto_command={};break;}
                s.view.automatic.active=true;s.view.automatic.run=r.run;s.view.pending=true;s.heartbeat=clock->now_ms();s.operation_epoch=command.epoch;s.view.centre=s.view.live.position;s.view.period=90;s.begin=s.count=0;s.bucket_valid=false;++s.view.history_revision;s.view.status="Starting automatic tuning; total travel ±"+std::to_string(r.travel/2);break;
            }
            release(s,true);s.backend->guard(effective_guard(s));s.motion=s.backend->engage();s.pending_motion=true;s.want_wave=command.op==Op::play;s.direction=command.direction;s.heartbeat=clock->now_ms();s.motion_generation=s.view.live.generation;s.motion_coordinate=s.view.live.coordinate_revision;s.operation_epoch=command.epoch;s.view.pending=true;break;}
        case Op::hold:release(s,true);break;
        case Op::disengage:s.view.operator_stopped=true;release(s);break;
        case Op::hide:for(auto& slot:slots)if(!slot.view.mapping_owned)release(slot);break;
        }
        s.view.dirty=s.view.draft!=s.view.saved;
    }
    static bool complete(const std::shared_ptr<PositionMotorBackend>& b,MotorCommandResult& command){if(!command.id)return false;const auto result=b->result(command.id);if(!result || result->stage==MotorResultStage::queued)return false;command=*result;return true;}
    void measure(Slot& s,std::uint64_t now){
        if(!s.finder.status().active)return;
        const auto index=unsigned(&s-slots.data());const bool native=index<position_motor_slots;
        if(!s.view.manual_page || now-s.heartbeat>=250 || s.operation_epoch!=epochs[index] || s.find_generation!=s.view.live.generation || s.find_coordinate!=s.view.live.coordinate_revision || s.find_draft!=s.draft_revision || !s.view.live.attached || !s.view.live.configured){s.finder.cancel("Backlash measurement cancelled by ownership, feedback or configuration change");}
        if(s.finder.status().active){
            if(s.find_revision!=s.finder.action_revision()){
                s.find_revision=s.finder.action_revision();s.find_accepted=false;s.find_engage=0;
                const int direction=s.finder.direction();
                if(direction==2)s.find_command=s.backend->disengage();
                else if(direction==0)s.find_command=s.backend->stop();
                else if(native){s.find_engage=direction;s.find_command=s.backend->engage();}
                else s.find_command=s.backend->diagnostic_drive(direction*s.view.draft.velocity_fraction);
                if(!s.find_command.id)s.finder.cancel(s.find_command.reason.empty()?"Backlash measurement operation unavailable":s.find_command.reason);
            }
            if(s.find_command.id && complete(s.backend,s.find_command)){
                if(!s.find_command)s.finder.cancel("Backlash measurement: "+s.find_command.reason);
                else if(s.find_engage){const int direction=s.find_engage;s.find_engage=0;s.find_command=s.backend->jog(direction**s.view.draft.maximum_velocity*s.view.draft.velocity_fraction);}
                else {s.find_accepted=true;s.find_command={};}
            }
            if(now-s.renewed>=100){s.backend->renew();s.renewed=now;}
            MotorPositionSample shaft;
            if(native){const auto& v=s.view.live;shaft.position=v.position;shaft.generation=v.generation;shaft.sequence=v.feedback_sequence;shaft.observed_ms=v.observed_ms;shaft.checked_ms=v.checked_ms;shaft.valid=v.feedback_valid;}
            else if(s.shaft_reader)shaft=s.shaft_reader();
            const auto pot=potentiometer(s.find_feedback);
            // Sampling and native acknowledgements can advance the clock. Judge
            // validity after acquiring the pair, not against service entry time.
            s.finder.tick(pot,shaft,clock->now_ms(),s.find_accepted,s.view.live.drive==MotorDriveState::off);
        }
        s.view.backlash_find=s.finder.status();s.view.status=s.view.backlash_find.message;
        if(!s.finder.status().active){
            if(s.finder.status().success && s.finder.status().phase==BacklashPhase::complete){
                s.view.draft.backlash=s.finder.status().raw_counts;s.requested=s.view.draft;++s.draft_revision;s.view.dirty=s.view.draft!=s.view.saved;
                if(!native){MotorParameters p;if(parameters(s,p)){s.configuration_ready=false;s.full_configuration=true;s.configuration=s.backend->configure(p);}}
            }else {s.release_backend=s.backend;s.view.release=s.backend->disengage();}
            forget_measurement(s);
        }
    }
    void service(Slot& s){if(s.release_backend && s.view.release.id){if(const auto r=s.release_backend->result(s.view.release.id);r && r->stage!=MotorResultStage::queued){s.view.release=*r;if(s.release_backend!=s.backend)retired.push_back(std::move(s.release_backend));else s.release_backend.reset();}}
const auto now=clock->now_ms();
        if(s.deferred && !initializing(s)){auto command=std::move(*s.deferred);s.deferred.reset();s.view.pending=false;execute(command);}
        if(s.candidate && complete(s.candidate,s.opening)){
            if(s.opening){retire(s.backend);s.backend=std::move(s.candidate);s.configuration=s.referencing=s.tuning=s.motion={};s.configuration_ready=s.full_configuration=s.initialization_failed=false;s.initialization_error.clear();s.configuration_generation=s.acknowledged_generation=0;s.view.draft.address=s.candidate_address;s.view.live=s.backend->snapshot();s.view.status="Motor disengaged; configure motion limits";
                if(s.defaults && s.draft_revision==s.candidate_revision){s.view.draft.kp=s.view.live.applied.kp;s.view.draft.ki=s.view.live.applied.ki;s.view.draft.kd=s.view.live.applied.kd;}if(s.draft_revision==s.candidate_revision)s.requested=s.view.draft;else s.requested.address=s.view.draft.address;s.backend->guard(effective_guard(s));
                MotorParameters p;if(parameters(s,p)){s.initialization_failed=false;s.initialization_error.clear();s.configuration_ready=false;s.full_configuration=true;s.configuration=(s.backend->feedback(s.view.draft.feedback),s.backend->configure(p));}
            }else {failed_initialization(s,s.opening.reason);retire(s.candidate);}s.opening={};s.view.pending=s.configuration.id!=0;s.view.dirty=s.view.draft!=s.view.saved;
        }
        if(!s.backend)return;s.view.live=s.backend->snapshot();
        const auto automatic=s.backend->tuning_status();
        if(s.auto_command.id && complete(s.backend,s.auto_command)){if(!s.auto_command){s.view.status=s.auto_command.reason;s.view.automatic.active=false;}s.auto_command={};s.view.pending=false;}
        if(automatic.run==s.auto_run && automatic.run){
            s.view.automatic=automatic;
            if(automatic.active){
                if(s.operation_epoch!=epochs[unsigned(&s-slots.data())] || now-s.heartbeat>=250 || !s.view.manual_page){release(s);}
                else {if(now-s.renewed>=100){s.backend->renew();s.renewed=now;}record(s,s.view.live);s.view.end_ms=now;s.view.status=automatic.message;}
            }else if(!automatic.finalizing && s.auto_completed!=automatic.run){
                s.auto_completed=automatic.run;s.last_tuning_trace=s.backend->tuning_samples();flush(s);s.view.end_ms=now;s.view.status=automatic.message;
                if(automatic.result.success && automatic.result.restored && s.draft_revision==s.auto_revision){const auto g=automatic.result.gains;s.view.draft.kp=g.kp;s.view.draft.ki=g.ki;s.view.draft.kd=g.kd;s.view.draft.vff=g.vff;s.view.draft.aff=g.aff;s.requested=s.view.draft;++s.draft_revision;s.view.dirty=s.view.draft!=s.view.saved;}
                else if(automatic.result.success){s.view.status="Stale tuning result discarded; draft/session changed";}
            }
        }
        if(s.configuration_ready && s.view.live.generation==s.configuration_generation && s.view.live.attached && s.view.live.configured && s.view.live.feedback_valid)s.acknowledged_generation=s.configuration_generation;
        s.view.live.configured=s.view.live.configured && s.configuration_ready;
        if(s.view.mapping_owned && now-s.mapping_heartbeat>=250){s.view.mapping_owned=false;release(s);s.view.status="Mapping worker lease expired; motor disengaged";}
        if(s.configuration.id && complete(s.backend,s.configuration)){s.view.pending=false;if(!s.configuration){failed_initialization(s,s.configuration.reason);s.want_zero=false;}else {s.configuration_ready=!s.initialization_failed;s.full_configuration=false;s.view.status=s.want_zero?"Waiting for stationary feedback to zero":s.view.live.engaged?"Live motion limits applied":"Configured; motor disengaged";}const auto acknowledged=s.backend->snapshot();if(s.configuration && !s.initialization_failed){s.configuration_generation=acknowledged.generation;if(acknowledged.attached && acknowledged.configured && acknowledged.feedback_valid)s.acknowledged_generation=acknowledged.generation;}s.configuration={};s.view.live.configured=acknowledged.configured && s.configuration_ready;}
        if(s.tuning.id && complete(s.backend,s.tuning)){s.view.status=s.tuning?"PID gains applied":s.tuning.reason;if(!s.tuning)release(s);s.tuning={};}
        if(s.want_zero && !s.configuration.id){
            if(now>=s.zero_deadline){s.want_zero=false;s.view.status="Position is not stationary; Zero position again after stopping";}
            else if(s.referencing.id){if(complete(s.backend,s.referencing)){if(s.referencing){s.want_zero=false;s.view.status="Zero position established";}else if(s.referencing.error!=MotorError::busy){s.want_zero=false;s.view.status=s.referencing.reason;}s.referencing={};}}
            else if(s.view.live.configured && s.view.live.feedback_valid && !s.view.live.engaged)s.referencing=s.backend->reference(0,s.view.live.applied.minimum,s.view.live.applied.maximum);
        }
        const auto index=static_cast<unsigned>(&s-slots.data());
        if((s.pending_motion || s.view.running || s.view.jogging || s.want_zero) && s.operation_epoch!=epochs[index])release(s);
        if(s.pending_motion && complete(s.backend,s.motion)){
            if(s.motion && now-s.heartbeat<250 && s.view.live.generation==s.motion_generation && s.view.live.coordinate_revision==s.motion_coordinate){s.view.centre=s.view.live.position;s.view.period=s.want_wave?motor_wave_period(s.view.draft.waveform,s.view.draft.amplitude,s.view.live.applied.velocity_limit,s.view.live.applied.acceleration,s.view.live.applied.feedback_interval_ms/1000.):5;
                s.motion=s.want_wave?s.backend->wave(s.view.draft.waveform,s.view.draft.amplitude):s.backend->jog(s.direction*s.view.live.applied.velocity_limit);s.pending_motion=false;s.view.running=s.want_wave;s.view.jogging=!s.want_wave;s.view.pending=false;s.begin=s.count=0;s.bucket_valid=false;++s.view.history_revision;s.sample_ms=0;s.renewed=now;s.view.status=s.want_wave?"Tuning wave running":"Manual drive";
            }else {s.view.status=s.motion.reason;release(s);s.view.pending=false;}
        }
        if(s.view.running || s.view.jogging){
            if(s.view.running && s.view.live.wave_active){s.view.centre=s.view.live.wave_centre;s.view.period=s.view.live.wave_period;}
            if(now-s.heartbeat>=250 || s.view.live.generation!=s.motion_generation || s.view.live.coordinate_revision!=s.motion_coordinate || !s.view.live.feedback_valid || s.view.live.state==MotorState::fault){break_trace(s);release(s);s.view.status="Motor control lost; disengaged";}
            else if(s.motion.id && complete(s.backend,s.motion) && !s.motion){s.view.status=s.motion.reason;release(s);}
            else {if(now-s.renewed>=100){s.backend->renew();s.renewed=now;}record(s,s.view.live);s.view.end_ms=now;}
        }
        measure(s,now);
        if(s.view.live.state==MotorState::fault)s.view.status=s.view.live.fault;
    }
    void run(){try {while(!shutdown){
        {std::lock_guard lock(mutex);bool lost{};std::optional<Command> next;
            {std::lock_guard queued(queue_mutex);lost=overflow.exchange(false);if(lost){read=write;for(unsigned n=0;n<motor_slots;++n){slots[n].queued_initializations=0;edits[n].reset();}}else if(read!=write)next=std::move(queue[read++%queue.size()]);else for(auto& edit:edits)if(edit){next=std::move(*edit);edit.reset();break;}}
            if(lost){for(unsigned n=0;n<motor_slots;++n){++epochs[n];auto& s=slots[n];failed_initialization(s,"Motor command queue overflow; continuity lost",true);s.view.mapping_owned=false;release(s);retire(s.candidate);retire(s.backend);s.configuration=s.opening=s.referencing=s.tuning=s.motion={};s.view.live={};s.configuration_ready=s.full_configuration=s.initialization_failed=false;s.initialization_error.clear();s.configuration_generation=s.acknowledged_generation=0;s.view.pending=false;++s.view.command_overflows;s.view.status="Motor command queue overflow; reopen channel and restore valid feedback";}}
            if(hide_requested.exchange(false))for(auto& s:slots)if(!s.view.mapping_owned){release(s);s.view.status="Motor controls cancelled";}
            for(unsigned n=0;n<motor_slots;++n)if(const auto mode=cancellations[n].exchange(0))release(slots[n],mode==1);
            if(next){if(next->op==Op::drive || next->op==Op::play || next->op==Op::auto_tune || next->op==Op::find_backlash || next->op==Op::zero){std::optional<Command> prerequisite;{std::lock_guard queued(queue_mutex);if(edits[next->slot]){prerequisite=std::move(edits[next->slot]);edits[next->slot].reset();}}if(prerequisite){execute(*prerequisite);if(prerequisite->initialization_change)--slots[next->slot].queued_initializations;}}
                execute(*next);if(next->initialization_change)--slots[next->slot].queued_initializations;}for(auto& s:slots)service(s);
            if(saving.valid() && saving.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto result=saving.get();auto& s=slots[result.slot];if(result.success){s.view.saved=std::move(result.definition);s.defaults=false;s.view.dirty=s.view.draft!=s.view.saved;s.view.status="Settings saved";}else s.view.status=result.error;}
        }
        std::erase_if(retired,[](const auto& backend){return backend->quiescent();});
        WaitForSingleObject(event,10);
    }}catch(const std::exception& e){std::lock_guard lock(mutex);for(auto& s:slots){release(s);s.view.status=std::string("Motor service stopped: ")+e.what();}}
        shutdown=true;
        {std::lock_guard lock(mutex);for(auto& s:slots){s.view.mapping_owned=false;release(s);retire(s.candidate);retire(s.backend);s.queued_initializations=0;s.configuration_ready=s.full_configuration=s.initialization_failed=false;s.initialization_error.clear();s.configuration_generation=s.acknowledged_generation=0;s.opening=s.configuration=s.referencing={};s.view.live={};s.view.pending=false;}}
        retired.clear(); // Slow channel close/join never owns the UI snapshot mutex.
        if(saving.valid())saving.wait();
    }
};
MotorBench::MotorBench(std::wstring path,MotorBackendFactory factory,std::shared_ptr<MotorClock> clock):impl_(std::make_unique<Impl>(std::move(path),std::move(factory),std::move(clock))){}
MotorBench::~MotorBench()=default;
std::uint64_t MotorBench::feedback_revision()const{std::lock_guard lock(impl_->mutex);return impl_->feedback_revision;}
std::array<MotorFeedbackDefinition,motor_slots> MotorBench::feedback_definitions()const{std::lock_guard lock(impl_->mutex);std::array<MotorFeedbackDefinition,motor_slots> out;for(unsigned n=0;n<motor_slots;++n){const auto& s=impl_->slots[n];out[n]=s.finder.status().active?s.find_feedback:s.view.draft.feedback;}return out;}
void MotorBench::measurement_sources(PositionSampler sample,std::function<ShaftReader(const MotorDeviceAddress&)> shaft){std::lock_guard lock(impl_->mutex);impl_->potentiometer=std::move(sample);impl_->shaft_factory=std::move(shaft);}
void MotorBench::find_backlash(unsigned n,MotorFeedbackDefinition feedback){Impl::Command c{Impl::Op::find_backlash,n};c.definition.feedback=std::move(feedback);impl_->submit(std::move(c));}
void MotorBench::activate(unsigned n){impl_->submit({Impl::Op::activate,n});}
void MotorBench::assign(unsigned n,MotorDeviceAddress a){Impl::Command c{Impl::Op::assign,n};c.definition.address=std::move(a);impl_->submit(std::move(c));}
void MotorBench::edit(unsigned n,MotorDefinition d,bool tuning){Impl::Command c{tuning?Impl::Op::tuning:Impl::Op::edit,n};c.definition=std::move(d);impl_->submit(std::move(c));}
void MotorBench::clear(unsigned n){impl_->submit({Impl::Op::clear,n});}
void MotorBench::save(unsigned n){impl_->submit({Impl::Op::save,n});}
void MotorBench::revert(unsigned n){impl_->submit({Impl::Op::revert,n});}
void MotorBench::zero(unsigned n){impl_->submit({Impl::Op::zero,n});}
void MotorBench::drive(unsigned n,int direction){if(direction!=1 && direction!=-1){disengage(n);return;}Impl::Command c{Impl::Op::drive,n};c.direction=direction;impl_->submit(std::move(c));}
void MotorBench::play(unsigned n){impl_->submit({Impl::Op::play,n});}
void MotorBench::auto_tune(unsigned n){impl_->submit({Impl::Op::auto_tune,n});}
void MotorBench::hold(unsigned n){impl_->submit({Impl::Op::hold,n});}
void MotorBench::disengage(unsigned n){impl_->submit({Impl::Op::disengage,n});}
void MotorBench::heartbeat(unsigned n){if(n>=motor_slots)return;std::lock_guard lock(impl_->mutex);impl_->slots[n].heartbeat=impl_->clock->now_ms();SetEvent(impl_->event);}
void MotorBench::manual_page(int selected){
    std::lock_guard lock(impl_->mutex);
    for(unsigned n=0;n<motor_slots;++n){auto& s=impl_->slots[n];const bool wanted=int(n)==selected;if(s.view.manual_page==wanted)continue;
        ++impl_->epochs[n];s.view.manual_page=wanted;s.view.mapping_owned=false;impl_->release(s);
        s.view.status=wanted?"Manual motor page; assignment paused":"Manual page closed; drive release requested";
    }
    SetEvent(impl_->event);
}
void MotorBench::hide(){std::lock_guard lock(impl_->mutex);for(unsigned n=0;n<motor_slots;++n)if(!impl_->slots[n].view.mapping_owned)++impl_->epochs[n];impl_->hide_requested=true;SetEvent(impl_->event);}
void MotorBench::stop(){impl_->shutdown=true;SetEvent(impl_->event);if(impl_->worker.joinable())impl_->worker.join();}
void MotorBench::wake(){SetEvent(impl_->event);}
MotorBenchView MotorBench::snapshot(unsigned n)const{std::lock_guard lock(impl_->mutex);if(n>=motor_slots)return {};const auto& s=impl_->slots[n];auto view=s.view;if(s.release_backend && view.release.id)if(const auto r=s.release_backend->result(view.release.id))view.release=*r;view.pending=view.pending || Impl::initializing(s);if(s.initialization_failed)view.status=s.initialization_error;view.mapping_owned=impl_->mapping_current(n,s);return view;}
void MotorBench::guard(unsigned n,MotorGuardSnapshot g){if(n>=motor_slots)return;std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];
    const bool changed=g.identity!=s.guard.identity || g.motor_generation!=s.guard.motor_generation || g.sensor_generation!=s.guard.sensor_generation || g.calibration_revision!=s.guard.calibration_revision || g.coordinate_revision!=s.guard.coordinate_revision;
    if(!s.view.manual_page && changed && g.required && (s.view.running || s.view.jogging || s.pending_motion)){++impl_->epochs[n];impl_->release(s);s.view.status="Physical sensor/motor identity changed; movement cancelled";}
    s.guard=g;if(s.backend)s.backend->guard(Impl::effective_guard(s));
}
std::optional<MotorInitializationFailure> MotorBench::initialization_failure_since(unsigned n,std::uint64_t revision,bool known_session_only)const{
    std::lock_guard lock(impl_->mutex);if(n>=motor_slots)return {};const auto& s=impl_->slots[n];const auto latest=s.view.initialization_failure_revision;
    if(revision==latest)return {};
    if(revision>latest || !s.failure_count || revision+1<s.failures[s.failure_begin].revision)return MotorInitializationFailure{latest,true,true,"Motor initialization failure history is incomplete; Home now required"};
    for(unsigned i=0;i<s.failure_count;++i){const auto& failure=s.failures[(s.failure_begin+i)%Impl::Slot::failure_capacity];if(failure.revision>revision && (!known_session_only || failure.known_session))return failure;}
    return {};
}
bool MotorBench::claim_mapping(unsigned n,std::string& error){if(n>=motor_slots)return false;std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];
    if(s.view.manual_page){error="Manual motor page owns this motor";return false;}
    if(Impl::initializing(s) || impl_->cancellations[n] || impl_->overflow || impl_->shutdown){error="Wait for motor initialization/configuration to complete";return false;}
    if(s.view.running || s.view.jogging || s.pending_motion || s.want_zero || s.view.mapping_owned || s.view.automatic.active || s.auto_command.id || s.finder.status().active){error="Motor is already controlled";return false;}
    if(s.initialization_failed){error=s.initialization_error;return false;}
    MotorParameters requested;if(!s.backend || !s.configuration_ready || !s.view.live.configured || !s.view.live.feedback_valid || !impl_->parameters(s,requested)){error="Configure the assigned motor first";return false;}
    s.mapping_heartbeat=impl_->clock->now_ms();s.mapping_epoch=impl_->epochs[n];s.mapping_failure_revision=s.view.initialization_failure_revision;s.view.mapping_owned=true;s.view.operator_stopped=false;error.clear();return true;
}
void MotorBench::mapping_heartbeat(unsigned n){if(n>=motor_slots)return;std::lock_guard lock(impl_->mutex);if(impl_->slots[n].view.mapping_owned)impl_->slots[n].mapping_heartbeat=impl_->clock->now_ms();}
MotorCommandResult MotorBench::release_mapping(unsigned n){if(n>=motor_slots)return {};std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];if(s.view.mapping_owned){s.view.mapping_owned=false;if(s.backend){s.release_backend=s.backend;s.view.release=s.backend->disengage();return s.view.release;}}return {};}
MotorCommandResult MotorBench::mapping_engage(unsigned n){std::lock_guard lock(impl_->mutex);auto& s=impl_->slots.at(n);return impl_->mapping_current(n,s) && !Impl::initializing(s) && s.configuration_ready && s.view.live.configured && s.view.live.feedback_valid && s.backend?s.backend->engage():MotorCommandResult{};}
MotorCommandResult MotorBench::mapping_target(unsigned n,double p){std::lock_guard lock(impl_->mutex);auto& s=impl_->slots.at(n);return impl_->mapping_current(n,s) && s.backend?s.backend->target(p):MotorCommandResult{};}
MotorCommandResult MotorBench::mapping_limits(unsigned n,double v,double a){std::lock_guard lock(impl_->mutex);auto& s=impl_->slots.at(n);return impl_->mapping_current(n,s) && s.backend?s.backend->limits(v,a):MotorCommandResult{};}
MotorCommandResult MotorBench::mapping_stop(unsigned n,bool disengage){std::lock_guard lock(impl_->mutex);auto& s=impl_->slots.at(n);if(!s.view.mapping_owned || !s.backend)return {};if(!disengage)return s.backend->stop();s.release_backend=s.backend;s.view.release=s.backend->disengage();return s.view.release;}
std::optional<MotorCommandResult> MotorBench::mapping_result(unsigned n,std::uint64_t id)const{std::lock_guard lock(impl_->mutex);const auto& s=impl_->slots.at(n);return s.backend?s.backend->result(id):std::optional<MotorCommandResult>{};}
std::vector<MotorTraceSample> MotorBench::history(unsigned n)const{std::vector<MotorTraceSample> samples;if(n>=motor_slots)return samples;std::lock_guard lock(impl_->mutex);const auto& s=impl_->slots[n];samples.reserve(s.count+6);for(unsigned i=0;i<s.count;++i)samples.push_back(s.samples[(s.begin+i)%Impl::Slot::history_capacity]);if(s.bucket_valid)for(const auto& value:s.bucket)samples.push_back(value);std::sort(samples.begin(),samples.end(),[](const auto& a,const auto& b){return a.ms<b.ms;});return samples;}
std::string MotorBench::tuning_report()const{
    std::lock_guard lock(impl_->mutex);std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(12)<<"HWIO automatic host-position PID/FF tuning v1; no physical guarantee from model screening\n";
    for(unsigned n=0;n<motor_slots;++n){const auto& b=impl_->slots[n].view.backlash_find;if(b.phase!=BacklashPhase::idle)out<<"Backlash Motor"<<n<<" phase="<<unsigned(b.phase)<<" active="<<b.active<<" success="<<b.success<<" raw_counts="<<b.raw_counts<<" shaft_counts="<<b.shaft_counts<<" raw_per_count="<<b.raw_per_count<<" jitter="<<b.jitter<<" message="<<b.message<<'\n';}
    const Impl::Slot* latest{};for(const auto& s:impl_->slots)if(s.view.automatic.run && (!latest || s.view.automatic.started_ms>=latest->view.automatic.started_ms))latest=&s;
    for(unsigned n=position_motor_slots;n<motor_slots;++n){const auto& slot=impl_->slots[n];const auto& t=slot.view.automatic;if(!t.run)continue;const auto& r=t.result;const auto& g=r.gains;const auto& m=r.model;
        out<<"Motor "<<n<<" run="<<t.run<<" session="<<t.session<<" coordinate="<<t.coordinate<<" draft_revision="<<t.draft_revision<<" motor_identity="<<std::quoted(t.motor_identity)<<" feedback_identity="<<std::quoted(t.feedback_identity)<<" phase="<<unsigned(t.phase)<<" elapsed_ms="<<t.elapsed_ms<<" success="<<r.success<<" restored="<<r.restored<<" reason="<<std::quoted(t.message)<<'\n';
        out<<"window="<<t.low<<','<<t.high<<" J="<<t.jitter<<" B="<<t.buffer<<" q="<<t.quantum<<" E="<<t.tolerance<<" observed_travel="<<r.covered_travel<<" observed_speed="<<r.covered_speed<<" samples="<<r.observations<<" evaluations="<<r.evaluations<<'\n';
        const auto& p=t.parameters;out<<"mode="<<unsigned(t.mode)<<" raw_code_count="<<t.code_count<<" bench_velocity="<<p.velocity_limit<<" bench_acceleration="<<p.acceleration<<" current_limit="<<p.current_limit.value_or(0)<<" maximum_duty="<<p.drive.maximum_duty<<" duty_ramp="<<p.drive.pwm_ramp<<" native_velocity="<<p.drive.native_velocity.value_or(0)<<" native_acceleration="<<p.drive.native_acceleration.value_or(0)<<" original_Kp="<<p.kp<<" original_Ki="<<p.ki<<" original_Kd="<<p.kd<<" original_Vff="<<p.vff<<" original_Aff="<<p.aff<<'\n';
        out<<"Kpositive="<<m.gain[0]<<" Knegative="<<m.gain[1]<<" tau="<<m.tau<<" L="<<m.delay<<" fit_rms="<<m.rms<<" heldout_rms="<<m.validation_rms<<" interval="<<m.interval<<" Aff_identifiable="<<r.aff_identified<<" model_phase_margin="<<r.phase_margin<<" model_gain_margin_db="<<r.gain_margin_db<<'\n';
        out<<"Kp="<<g.kp<<" Ki="<<g.ki<<" Kd="<<g.kd<<" Vff="<<g.vff<<" Aff="<<g.aff<<" tracking_rms="<<r.tracking_rms<<" overshoot="<<r.overshoot<<" settling_seconds="<<r.settling_seconds<<" drive_variation="<<r.drive_variation<<" screened_retry_count="<<r.retries<<" normalized_native_ramp="<<m.pwm_ramp<<'\n';
    }
    if(latest){out<<"Latest trace: ms phase sequence generation observed_ms requested_ms accepted_ms raw_position selected_position requested_normalized accepted_normalized sdk_getter_value getter_available independent_observation\n";
        const auto trace=latest->backend && latest->view.automatic.active?latest->backend->tuning_samples():latest->last_tuning_trace;for(const auto& s:trace)out<<s.ms<<' '<<unsigned(s.phase)<<' '<<s.sequence<<' '<<s.generation<<' '<<s.observed_ms<<' '<<s.requested_ms<<' '<<s.accepted_ms<<' '<<s.raw_position<<' '<<s.position<<' '<<s.requested<<' '<<s.accepted<<' '<<s.applied<<' '<<s.telemetry_available<<' '<<s.fresh<<'\n';}
    return out.str();
}
}
