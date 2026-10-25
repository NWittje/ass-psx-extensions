#include "hwio/servo.hpp"
#include "hwio/actuator_repository.hpp"
#include "hwio/catalogue.hpp"
#include "hwio/phidget_io.hpp"
#include <phidget22.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace hwio {
std::optional<double> servo_numeric(std::string_view text,int field,unsigned fields)noexcept{return actuator_numeric(text,field,fields);}
bool servo_definition_valid(const ServoDefinition& d,std::string& error){
    auto finite=[](double v){return std::isfinite(v);};const auto& a=d.address;const auto& s=d.source;const auto& p=d.parameters;
    if(d.name.size()>128 || a.device.size()>1024 || a.server.size()>253 || (a.assigned() && (a.provider!="phidget22" || a.serial<=0 || a.channel>65535 || a.hub_port<-1 || a.hub_port>255 || !a.port || a.port>65535 || (a.remote && a.server.empty()))) || a.server.find_first_of("\r\n\t /\\")!=a.server.npos){error="Invalid exact servo address or name";return false;}
    const bool record=s.record.size()>=3 && s.record.size()<=10 && s.record[0]=='Q' && (s.record[1]=='i' || s.record[1]=='h' || s.record[1]=='s') && std::all_of(s.record.begin()+2,s.record.end(),[](char c){return c>='0' && c<='9';});
    if(!record || s.id.size()>128 || s.name.size()>256 || s.unit.size()>64 || s.power_record.size()>10 || s.field<-1 || s.fields<1 || s.fields>128 || (s.field>=0 && unsigned(s.field)>=s.fields) || s.shift>31 || !finite(s.minimum) || !finite(s.maximum) || s.minimum>=s.maximum || !finite(s.maximum-s.minimum) || !finite(s.scale) || s.scale==0 || !finite(s.offset) || !finite(p.pulse_min) || !finite(p.pulse_max) || p.pulse_min<0 || p.pulse_max<0 || (p.pulse_max && p.pulse_min>=p.pulse_max) || d.update_ms<20 || d.update_ms>10000 || !finite(d.deadband_us) || d.deadband_us<0){error="Enter a numeric PSX source, ordered source range, valid pulse range and update settings";return false;}
    for(const auto& value:{p.velocity,p.acceleration,p.torque,d.at_min,d.at_max,d.at_centre})if(value && !finite(*value)){error="Servo settings must be finite";return false;}
    for(const auto& point:d.manual_points)if(point && (!finite(point->value) || !finite(point->pulse))){error="Calibration values must be finite";return false;}
    if((p.velocity && *p.velocity<=0) || (p.acceleration && *p.acceleration<=0) || (p.torque && *p.torque<0) || p.interval_ms>60000 || p.failsafe_ms>60000 || (p.voltage && (*p.voltage<1 || *p.voltage>3)) || (d.at_min && d.at_max && *d.at_min==*d.at_max)){error="Invalid servo limits or identical captured positions";return false;}
    if((d.at_min || d.at_max || d.at_centre || d.manual_points[0] || d.manual_points[1]) && p.pulse_max<=p.pulse_min){error="Captured servo positions require a complete native pulse mapping";return false;}
    if(p.pulse_max){
        for(const auto& v:{d.at_min,d.at_max,d.at_centre})if(v && (*v<p.pulse_min || *v>p.pulse_max)){error="Captured position is outside the configured native pulse mapping";return false;}
        for(const auto& point:d.manual_points)if(point && (point->pulse<p.pulse_min || point->pulse>p.pulse_max)){error="Manual calibration is outside the native pulse mapping";return false;}
    }
    if(!s.power_record.empty()){auto check=d;check.source.record=s.power_record;check.source.power_record.clear();if(!servo_definition_valid(check,error))return false;}
    error.clear();return true;
}
bool ServoCurve::compile(const ServoDefinition& d)noexcept{
    count=0;pulse_min=d.parameters.pulse_min;pulse_max=d.parameters.pulse_max;
    if(!std::isfinite(d.source.maximum-d.source.minimum) || d.source.minimum>=d.source.maximum || pulse_min>=pulse_max)return false;
    auto add=[&](double value,double pulse){
        if(!std::isfinite(value) || !std::isfinite(pulse))return;
        for(unsigned i=0;i<count;++i)if(points[i].value==value){points[i].pulse=pulse;return;}
        points[count++]={value,pulse};
    };
    if(d.at_min)add(d.source.minimum,*d.at_min);
    if(d.at_centre)add(d.source.minimum+(d.source.maximum-d.source.minimum)/2,*d.at_centre);
    if(d.at_max)add(d.source.maximum,*d.at_max);
    if(d.manual_values)for(const auto& p:d.manual_points)if(p)add(p->value,p->pulse);
    std::sort(points.begin(),points.begin()+count,[](const auto& a,const auto& b){return a.value<b.value;});
    if(count<2)return false;
    for(unsigned i=1;i<count;++i)if(!std::isfinite((points[i].pulse-points[i-1].pulse)/(points[i].value-points[i-1].value))){count=0;return false;}
    return true;
}
std::optional<double> ServoCurve::map(double value)const noexcept{
    if(count<2 || !std::isfinite(value))return {};
    unsigned i=1;while(i+1<count && value>points[i].value)++i;
    const auto& a=points[i-1];const auto& b=points[i];
    const double pulse=a.pulse+(value-a.value)*(b.pulse-a.pulse)/(b.value-a.value);
    return std::isfinite(pulse)?std::optional(std::clamp(pulse,pulse_min,pulse_max)):std::nullopt;
}
std::optional<double> servo_pulse(const ServoDefinition& d,double value)noexcept{ServoCurve curve;if(!curve.compile(d))return {};return curve.map(std::clamp(value,d.source.minimum,d.source.maximum));}
std::vector<ServoSource> servo_sources(){return numeric_sources();}
namespace {
void optional_write(std::ostream& out,const std::optional<double>& v){if(v)out<<*v;else out<<'_';out<<' ';}
bool optional_read(std::istream& in,std::optional<double>& v){std::string t;if(!(in>>t))return false;if(t=="_"){v.reset();return true;}auto n=servo_numeric(t,-1,1);if(!n)return false;v=*n;return true;}
ServoDefinition initial(unsigned n){ServoDefinition d;d.name="Servo "+std::to_string(n);const auto sources=servo_sources();if(!sources.empty())d.source=sources.front();else{d.source.id="custom";d.source.name="Custom numeric source";d.source.record="Qi119";d.source.minimum=0;d.source.maximum=15000;}return d;}
}
bool load_servo_settings(const std::wstring& directory,std::array<ServoDefinition,servo_slots>& definitions,std::string& error){
    try{auto parsed=definitions;for(unsigned n=0;n<servo_slots;++n)parsed[n]=initial(n);std::optional<std::string> data;if(!actuator_section(directory,"servos",L"servo-settings.cfg",data,error))return false;
        if(!data){definitions=std::move(parsed);error.clear();return true;}if(data->size()>262144)throw std::runtime_error("Servo settings exceed 256 KiB");
        std::istringstream in(*data);std::string header;std::getline(in,header);if(!header.empty() && header.back()=='\r')header.pop_back();const bool version3=header=="[hwio-servo-settings-v3]",version2=version3 || header=="[hwio-servo-settings-v2]";if(!version2 && header!="[hwio-servo-settings-v1]")throw std::runtime_error("Unsupported servo settings version");
        for(unsigned n=0;n<servo_slots;++n){auto& d=parsed[n];auto& a=d.address;auto& s=d.source;auto& p=d.parameters;unsigned slot{};int voltage{};
            if(!(in>>slot>>std::quoted(d.name)>>std::quoted(a.provider)>>std::quoted(a.device)>>a.serial>>a.hub_port>>a.channel>>a.model>>a.remote>>std::quoted(a.server)>>a.port>>std::quoted(s.id)>>std::quoted(s.name)>>std::quoted(s.record)>>std::quoted(s.unit)>>s.field>>s.fields>>s.mask>>s.shift>>s.minimum>>s.maximum>>s.scale>>s.offset>>std::quoted(s.power_record)>>s.power_mask>>p.pulse_min>>p.pulse_max) || slot!=n)throw std::runtime_error("Invalid servo address/source");
            for(auto* v:{&p.velocity,&p.acceleration,&p.torque,&d.at_min,&d.at_max})if(!optional_read(in,*v))throw std::runtime_error("Invalid servo numeric setting");
            if(!(in>>voltage>>p.ramping>>p.interval_ms>>p.failsafe_ms>>d.update_ms>>d.deadband_us))throw std::runtime_error("Incomplete servo settings");if(voltage)p.voltage=voltage;if(version2 && !(in>>s.demand))throw std::runtime_error("Invalid numeric source demand flag");
            if(version3){
                if(!optional_read(in,d.at_centre) || !(in>>d.manual_values))throw std::runtime_error("Invalid servo centre calibration");
                for(auto& point:d.manual_points){std::optional<double> value,pulse;if(!optional_read(in,value) || !optional_read(in,pulse) || value.has_value()!=pulse.has_value())throw std::runtime_error("Invalid manual servo calibration");if(value)point=ServoCalibrationPoint{*value,*pulse};}
            }
            if(!servo_definition_valid(d,error))return false;
            for(unsigned k=0;k<n;++k)if(d.address.same_physical(parsed[k].address))throw std::runtime_error("Duplicate physical servo ownership");
        }
        in>>std::ws;if(!in.eof())throw std::runtime_error("Unexpected servo settings data");definitions=std::move(parsed);error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool save_servo_settings(const std::wstring& directory,const std::array<ServoDefinition,servo_slots>& definitions,std::string& error){
    try{std::ostringstream out;out.imbue(std::locale::classic());out<<"[hwio-servo-settings-v3]\n"<<std::setprecision(17);
        for(unsigned n=0;n<servo_slots;++n){const auto& d=definitions[n];if(!servo_definition_valid(d,error))return false;const auto& a=d.address;const auto& s=d.source;const auto& p=d.parameters;
            for(unsigned k=0;k<n;++k)if(a.same_physical(definitions[k].address))throw std::runtime_error("Duplicate physical servo ownership");
            out<<n<<' '<<std::quoted(d.name)<<' '<<std::quoted(a.provider)<<' '<<std::quoted(a.device)<<' '<<a.serial<<' '<<a.hub_port<<' '<<a.channel<<' '<<a.model<<' '<<a.remote<<' '<<std::quoted(a.server)<<' '<<a.port<<' '<<std::quoted(s.id)<<' '<<std::quoted(s.name)<<' '<<std::quoted(s.record)<<' '<<std::quoted(s.unit)<<' '<<s.field<<' '<<s.fields<<' '<<s.mask<<' '<<s.shift<<' '<<s.minimum<<' '<<s.maximum<<' '<<s.scale<<' '<<s.offset<<' '<<std::quoted(s.power_record)<<' '<<s.power_mask<<' '<<p.pulse_min<<' '<<p.pulse_max<<' ';
            for(const auto& v:{p.velocity,p.acceleration,p.torque,d.at_min,d.at_max})optional_write(out,v);
            out<<p.voltage.value_or(0)<<' '<<p.ramping<<' '<<p.interval_ms<<' '<<p.failsafe_ms<<' '<<d.update_ms<<' '<<d.deadband_us<<' '<<s.demand<<' ';optional_write(out,d.at_centre);out<<d.manual_values<<' ';for(const auto& point:d.manual_points){optional_write(out,point?std::optional(point->value):std::nullopt);optional_write(out,point?std::optional(point->pulse):std::nullopt);}out<<'\n';
        }
        const auto text=out.str();if(text.size()>262144)throw std::runtime_error("Servo settings exceed 256 KiB");
        return actuator_section_save(directory,"servos",text,error);
    }catch(const std::exception& e){error=e.what();return false;}
}
struct ServoIndicators::Impl {
    struct Slot {ServoView view;std::shared_ptr<ServoBackend> backend;MotorDeviceAddress address;std::optional<double> requested;MotorCommandResult pending;enum class Pending {none,inspect,configure,target,engage,release,closing} operation{};bool calibrating{},following{},open_requested{},configure_requested{},external{},page{},automatic{};std::uint64_t generation{},last_command{};ServoCurve curve;bool mapping_valid{};
        void compile(){mapping_valid=curve.compile(view.draft);}
        std::optional<double> map(double value)const{return mapping_valid?curve.map(std::clamp(value,view.draft.source.minimum,view.draft.source.maximum)):std::nullopt;}
    };
    std::wstring directory;Factory factory;mutable std::mutex mutex;std::condition_variable wake;std::thread worker;bool quit{},enabled{};std::atomic<std::uint64_t> source_revision{1};
    std::array<Slot,servo_slots> slots;
    enum class Op {inspect,calibrate,manual,follow,hold,save};
    struct Event {Op op;unsigned slot;double value{};std::uint64_t generation{};bool attached{},faulted{};std::shared_ptr<const ServoDefinition> definition;bool keep_manual{};};std::deque<Event> queue;std::array<bool,servo_slots> release{};
    struct SourceCache {std::uint64_t epoch{UINT64_MAX},sequence{UINT64_MAX},power_sequence{UINT64_MAX};ServoSource source;std::optional<double> number,power;};std::array<SourceCache,servo_slots> source_cache;std::array<std::optional<double>,servo_slots> sources;std::array<std::optional<double>,servo_slots> currents;std::array<std::uint64_t,servo_slots> current_times{};
    void stop(Slot& s){s.automatic=false;s.calibrating=false;s.following=false;s.requested.reset();s.open_requested=false;s.configure_requested=false;if(s.backend){s.pending=s.backend->disengage();s.operation=Slot::Pending::release;}s.view.phase=s.view.draft.address.assigned()?ServoPhase::ready:ServoPhase::unassigned;}
    void fail(Slot& s,std::string reason){stop(s);s.view.phase=ServoPhase::fault;s.view.status=std::move(reason);}
    void backend(Slot& s){if(!s.view.draft.address.assigned())throw std::runtime_error("Choose a servo device and channel");s.open_requested=true;if(s.backend && s.address!=s.view.draft.address){if(s.operation!=Slot::Pending::closing){s.pending=s.backend->close();s.operation=Slot::Pending::closing;}return;}if(!s.backend){s.backend=factory(s.view.draft.address);s.backend->provider_enabled(enabled);s.address=s.view.draft.address;s.pending=s.backend->inspect();s.operation=Slot::Pending::inspect;}s.open_requested=true;}
    void event(const Event& e){auto& s=slots[e.slot];auto& v=s.view;
        const bool motion=e.op==Op::calibrate || e.op==Op::manual || e.op==Op::follow || e.op==Op::hold;
        if(motion && e.generation && s.backend){const auto live=s.backend->snapshot();if(live.generation!=e.generation || live.attached!=e.attached || (!live.fault.empty())!=e.faulted){v.status="Cancelled request: servo continuity changed before dispatch";return;}}
        switch(e.op){
        case Op::hold:s.requested.reset();if(s.backend && s.operation==Slot::Pending::none && v.live.configured && v.live.accepted){s.pending=s.backend->hold();s.operation=Slot::Pending::release;}break;
        case Op::save:break; // The worker persists its captured definition outside the state lock.
        case Op::inspect:backend(s);v.status="Opening disengaged for inspection";break;
        case Op::calibrate:if(s.external)throw std::runtime_error("Shared actuator mapping owns this servo");stop(s);backend(s);s.calibrating=true;s.configure_requested=true;v.phase=ServoPhase::calibrating;v.status="Calibrating; saved mechanical range bypassed; native pulse bounds apply";break;
        case Op::manual:if(!s.calibrating)throw std::runtime_error("Open the servo page before manual motion");if(!std::isfinite(e.value))throw std::runtime_error("Manual servo position must be finite");s.requested=e.value;v.manual=e.value;break;
        case Op::follow:if(s.external)throw std::runtime_error("Shared actuator mapping owns this servo");if(!s.mapping_valid)throw std::runtime_error("Assign at least two distinct calibration values first");if(!sources[e.slot])throw std::runtime_error("PSX source or indicator power unavailable");stop(s);backend(s);s.following=true;s.configure_requested=true;v.phase=ServoPhase::following;v.status="Following PSX";break;
        }
    }
    void persist(unsigned n,const ServoDefinition& definition,std::unique_lock<std::mutex>& lock,bool keep_manual){
        std::array<ServoDefinition,servo_slots> definitions;for(unsigned k=0;k<servo_slots;++k)definitions[k]=slots[k].view.saved;definitions[n]=definition;
        if(!keep_manual)stop(slots[n]);slots[n].view.status=keep_manual?"Saving calibration/settings":"Saving settings; disengagement requested";
        std::string error;bool saved{};lock.unlock();
        try{saved=save_servo_settings(directory,definitions,error);}catch(...){lock.lock();throw;}lock.lock();
        auto& slot=slots[n];auto& v=slot.view;if(!saved){slot.automatic=false;v.status="Unsaved: "+error;return;}v.saved=std::move(definitions[n]);v.dirty=v.draft!=v.saved;v.status=keep_manual?(slot.page?"Calibration saved; manual control":"Calibration saved"):"Settings/calibration saved; select Follow PSX to resume";
    }
    void tick(unsigned n){auto& s=slots[n];auto& v=s.view;v.source_value=sources[n];v.live.current=currents[n];v.live.current_ms=current_times[n];
        if(s.automatic && enabled && !s.page && !s.external && !s.following && !v.dirty && s.mapping_valid && v.draft.address.assigned()){
            if(sources[n]){backend(s);s.following=true;s.configure_requested=true;v.phase=ServoPhase::following;v.status="Following PSX";}
            else{v.phase=ServoPhase::source_unavailable;v.status="Waiting for PSX source/power";}
        }
        if(s.backend && s.address!=v.draft.address){if(s.operation!=Slot::Pending::closing){s.pending=s.backend->close();s.operation=Slot::Pending::closing;}const auto done=s.pending?s.backend->result(s.pending.id):std::optional<MotorCommandResult>(s.pending);if(!done)return;v.live=s.backend->snapshot();if(!*done){s.following=s.calibrating=false;s.requested.reset();v.phase=ServoPhase::fault;v.status=done->reason;return;}s.backend.reset();s.operation=Slot::Pending::none;if(s.open_requested && v.draft.address.assigned())backend(s);}if(!s.backend)return;
        v.live=s.backend->snapshot();v.live.current=currents[n];v.live.current_ms=current_times[n];const bool expected_generation_change=s.configure_requested || s.operation==Slot::Pending::inspect || s.operation==Slot::Pending::configure;
        if(s.operation!=Slot::Pending::none){if(!s.pending){fail(s,s.pending.reason);s.operation=Slot::Pending::none;return;}const auto done=s.backend->result(s.pending.id);if(done){const auto op=s.operation;s.operation=Slot::Pending::none;if(!*done){fail(s,done->reason);return;}v.live=s.backend->snapshot();if(op==Slot::Pending::inspect && v.draft.parameters.pulse_max==0){const auto& c=v.live.capabilities;v.draft.parameters.pulse_min=std::max(500.,c.pulse_min);v.draft.parameters.pulse_max=std::min(2500.,c.pulse_max);v.dirty=true;}
            if(op==Slot::Pending::configure && s.calibrating)v.status="Manual control; position the slider and assign calibration points";
        }}
        if(s.generation && v.live.generation!=s.generation && !expected_generation_change){stop(s);v.phase=ServoPhase::detached;v.status="Attachment changed; fresh configuration and explicit Follow PSX required";}s.generation=v.live.generation;
        if(!v.live.fault.empty() && !s.configure_requested && s.operation!=Slot::Pending::inspect && s.operation!=Slot::Pending::configure){s.following=false;s.calibrating=false;s.requested.reset();v.phase=v.live.attached?ServoPhase::fault:ServoPhase::detached;v.status=v.live.fault;return;}
        if(s.operation!=Slot::Pending::none)return;
        if(s.configure_requested){s.configure_requested=false;s.pending=s.backend->configure(v.draft.parameters);s.operation=Slot::Pending::configure;return;}
        if(s.following){if(!sources[n]){const bool resume=s.automatic;stop(s);s.automatic=resume;v.phase=ServoPhase::source_unavailable;v.status=resume?"Waiting for PSX source/power":"Source/power unavailable; disengagement requested; select Follow PSX after recovery";return;}s.requested=s.map(*sources[n]);}
        if(s.requested && v.live.configured && GetTickCount64()-s.last_command>=v.draft.update_ms){const auto target=*s.requested;if(!v.live.accepted || !v.live.desired || std::abs(target-*v.live.desired)>v.draft.deadband_us){s.pending=s.backend->target(target);s.operation=Slot::Pending::target;s.last_command=GetTickCount64();return;}s.requested.reset();}
        if((s.calibrating || s.following) && v.live.configured && v.live.accepted && !v.live.released_at_target && v.live.drive!=MotorDriveState::on){s.pending=s.backend->engage();s.operation=Slot::Pending::engage;return;}
        if(!s.calibrating && !s.following && v.phase!=ServoPhase::fault && v.phase!=ServoPhase::source_unavailable && v.phase!=ServoPhase::detached)v.phase=s.mapping_valid?ServoPhase::ready:ServoPhase::needs_calibration;
    }
    void run(){std::unique_lock lock(mutex);while(!quit || !queue.empty()){wake.wait_for(lock,std::chrono::milliseconds(25));for(unsigned n=0;n<servo_slots;++n)if(release[n]){release[n]=false;stop(slots[n]);slots[n].view.status="Disengagement requested";}
            while(!queue.empty()){auto e=queue.front();queue.pop_front();try{if(e.op==Op::save && e.definition)persist(e.slot,*e.definition,lock,e.keep_manual);else event(e);}catch(const std::exception& error){slots[e.slot].view.status=error.what();}}
            for(unsigned n=0;n<servo_slots;++n)try{tick(n);++slots[n].view.revision;}catch(const std::exception& error){fail(slots[n],error.what());}
        }for(auto& s:slots)if(s.backend){s.backend->close();s.backend.reset();}}
    void push(Op op,unsigned n,double value=0){if(n>=servo_slots)return;std::lock_guard lock(mutex);if(quit)return;if(queue.size()>=64){queue.erase(std::remove_if(queue.begin(),queue.end(),[n](const auto& e){return e.slot==n && e.op!=Op::save;}),queue.end());stop(slots[n]);slots[n].view.status="Servo UI queue full; queued motion cancelled, disengagement requested";return;}Event event{op,n,value};if(slots[n].backend){const auto live=slots[n].backend->snapshot();event.generation=live.generation;event.attached=live.attached;event.faulted=!live.fault.empty();}queue.push_back(event);wake.notify_one();}
};
ServoIndicators::ServoIndicators(std::wstring d,Factory f):impl_(std::make_unique<Impl>()){
    impl_->directory=std::move(d);impl_->factory=std::move(f);std::array<ServoDefinition,servo_slots> settings;std::string error;if(!load_servo_settings(impl_->directory,settings,error))for(unsigned n=0;n<servo_slots;++n)settings[n]=initial(n);
    for(unsigned n=0;n<servo_slots;++n){auto& s=impl_->slots[n];s.view.draft=s.view.saved=settings[n];s.view.status=error.empty()?(settings[n].address.assigned()?"Waiting for PSX source/power":"Select a servo device/channel"):error;s.compile();s.automatic=error.empty();s.view.phase=settings[n].address.assigned()?(s.mapping_valid?ServoPhase::ready:ServoPhase::needs_calibration):ServoPhase::unassigned;}
    impl_->worker=std::thread([this]{impl_->run();});
}
ServoIndicators::~ServoIndicators(){close();}
ServoView ServoIndicators::snapshot(unsigned n)const{std::lock_guard lock(impl_->mutex);return n<servo_slots?impl_->slots[n].view:ServoView{};}
std::array<ServoDefinition,servo_slots> ServoIndicators::saved()const{std::lock_guard lock(impl_->mutex);std::array<ServoDefinition,servo_slots> out;for(unsigned n=0;n<servo_slots;++n)out[n]=impl_->slots[n].view.saved;return out;}
bool ServoIndicators::edit(unsigned n,ServoDefinition d,std::string& error){if(n>=servo_slots || !servo_definition_valid(d,error))return false;std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];
    for(unsigned k=0;k<servo_slots;++k)if(k!=n && d.address.same_physical(impl_->slots[k].view.draft.address)){error="Physical servo already selected by Servo "+std::to_string(k);return false;}
    const auto& old=s.view.draft;if(old==d)return true;
    if(old.address!=d.address || old.source.record!=d.source.record || old.source.field!=d.source.field || old.source.fields!=d.source.fields || old.source.scale!=d.source.scale || old.source.offset!=d.source.offset || old.source.unit!=d.source.unit || old.source.minimum!=d.source.minimum || old.source.maximum!=d.source.maximum || old.parameters.pulse_min!=d.parameters.pulse_min || old.parameters.pulse_max!=d.parameters.pulse_max){d.at_min.reset();d.at_max.reset();d.at_centre.reset();d.manual_points={};}
    const bool reconfigure=old.address!=d.address || old.source!=d.source || old.parameters!=d.parameters || (!s.page && (old.at_min!=d.at_min || old.at_max!=d.at_max || old.at_centre!=d.at_centre || old.manual_points!=d.manual_points || old.manual_values!=d.manual_values));
    if(old.source!=d.source || old.address!=d.address)++impl_->source_revision;
    if(reconfigure){impl_->queue.erase(std::remove_if(impl_->queue.begin(),impl_->queue.end(),[n](const auto& event){return event.slot==n && event.op!=Impl::Op::save;}),impl_->queue.end());impl_->stop(s);}
    s.view.draft=std::move(d);s.compile();s.view.dirty=s.view.draft!=s.view.saved;s.view.status="Settings changed";error.clear();return true;
}
bool ServoIndicators::save(unsigned n,std::string& error){try{if(n>=servo_slots){error="Invalid servo slot";return false;}std::lock_guard lock(impl_->mutex);if(impl_->quit || impl_->queue.size()>=64){error="Servo persistence queue unavailable";return false;}Impl::Event event{Impl::Op::save,n};event.definition=std::make_shared<ServoDefinition>(impl_->slots[n].view.draft);impl_->queue.push_back(std::move(event));impl_->wake.notify_one();error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool ServoIndicators::save_automatic(unsigned n,std::string& error){try{
    if(n>=servo_slots){error="Invalid servo slot";return false;}
    std::lock_guard lock(impl_->mutex);if(impl_->quit || impl_->queue.size()>=64){error="Servo persistence queue unavailable";return false;}
    Impl::Event event{Impl::Op::save,n};event.definition=std::make_shared<ServoDefinition>(impl_->slots[n].view.draft);event.keep_manual=true;impl_->queue.push_back(std::move(event));impl_->wake.notify_one();error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
void ServoIndicators::manual_page(unsigned n,bool active){
    if(n>=servo_slots)return;std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];
    if(!active && !s.page)return;
    if(active && s.external){s.view.status="Shared actuator mapping owns this servo";return;}
    impl_->queue.erase(std::remove_if(impl_->queue.begin(),impl_->queue.end(),[n](const auto& e){return e.slot==n && e.op!=Impl::Op::save;}),impl_->queue.end());
    impl_->stop(s);s.page=active;
    if(!active){s.automatic=true;s.view.status=s.view.dirty?"Unsaved calibration; PSX following paused":s.mapping_valid?"Waiting for PSX source/power":"Assign at least two calibration values";}
    else if(s.view.draft.address.assigned()){
        try{impl_->backend(s);s.calibrating=true;s.configure_requested=true;s.view.manual.reset();s.view.phase=ServoPhase::calibrating;s.view.status="Preparing manual control";}catch(const std::exception& e){impl_->fail(s,e.what());}
    }else s.view.status="Choose a servo device/channel";
    impl_->wake.notify_one();
}
bool ServoIndicators::assign_point(unsigned n,unsigned point,std::optional<double> value,std::string& error){
    if(n>=servo_slots || point>=5 || (point>=3 && (!value || !std::isfinite(*value)))){error="Enter a finite real scale value";return false;}
    {std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];auto& v=s.view;
        if(!s.backend || !s.page || !s.calibrating || s.address!=v.draft.address){error="Open the selected servo for manual positioning first";return false;}
        const auto actual=s.backend->snapshot();
        if(!actual.attached || !actual.configured || !actual.accepted || !actual.desired || !v.manual || std::abs(*actual.desired-*v.manual)>1e-9){error="Wait for the current manual position to be accepted";return false;}
        auto d=v.draft;if(point==0)d.at_min=actual.accepted;else if(point==1)d.at_centre=actual.accepted;else if(point==2)d.at_max=actual.accepted;else d.manual_points[point-3]=ServoCalibrationPoint{*value,*actual.accepted};
        if(!servo_definition_valid(d,error))return false;v.draft=std::move(d);s.compile();v.dirty=v.draft!=v.saved;
    }
    return save_automatic(n,error);
}
void ServoIndicators::revert(unsigned n){if(n>=servo_slots)return;stop(n);std::lock_guard lock(impl_->mutex);auto& v=impl_->slots[n].view;v.draft=v.saved;impl_->slots[n].compile();v.dirty=false;v.status="Saved settings restored; disengagement requested";++impl_->source_revision;}
void ServoIndicators::clear(unsigned n){if(n>=servo_slots)return;stop(n);std::lock_guard lock(impl_->mutex);auto& v=impl_->slots[n].view;v.draft.address={};v.draft.at_min.reset();v.draft.at_max.reset();impl_->slots[n].compile();v.dirty=v.draft!=v.saved;v.phase=ServoPhase::unassigned;v.status="Cleared draft; save explicitly";++impl_->source_revision;}
void ServoIndicators::inspect(unsigned n){impl_->push(Impl::Op::inspect,n);}void ServoIndicators::calibrate(unsigned n){impl_->push(Impl::Op::calibrate,n);}
void ServoIndicators::manual(unsigned n,double p){impl_->push(Impl::Op::manual,n,p);}
void ServoIndicators::capture(unsigned n,bool maximum){if(n>=servo_slots)return;std::lock_guard lock(impl_->mutex);auto& s=impl_->slots[n];auto& v=s.view;if(!s.backend || !s.calibrating || s.address!=v.draft.address){v.status="Select Calibrate and an SDK-accepted manual pulse first";return;}const auto actual=s.backend->snapshot();if(!actual.attached || !actual.configured || !actual.accepted || !actual.desired || !v.manual || std::abs(*actual.desired-*v.manual)>1e-9){v.status="Wait for an SDK-accepted manual pulse before capturing";return;}if(maximum)v.draft.at_max=actual.accepted;else v.draft.at_min=actual.accepted;s.compile();v.dirty=v.draft!=v.saved;v.status="Accepted command captured; shaft position is not measured";}
void ServoIndicators::follow(unsigned n){impl_->push(Impl::Op::follow,n);}void ServoIndicators::cancel(unsigned n){revert(n);if(n<servo_slots){std::lock_guard lock(impl_->mutex);impl_->slots[n].view.status="Calibration cancelled; saved mapping retained; disengagement requested";}}void ServoIndicators::hold(unsigned n){impl_->push(Impl::Op::hold,n);}
void ServoIndicators::stop(unsigned n){if(n>=servo_slots)return;std::lock_guard lock(impl_->mutex);impl_->release[n]=true;impl_->slots[n].external=false;impl_->queue.erase(std::remove_if(impl_->queue.begin(),impl_->queue.end(),[n](const auto& e){return e.slot==n && e.op!=Impl::Op::save;}),impl_->queue.end());if(impl_->slots[n].backend)impl_->slots[n].backend->disengage();impl_->wake.notify_one();}
std::shared_ptr<ServoBackend> ServoIndicators::external(unsigned n,bool acquire,std::string& error){
    std::lock_guard lock(impl_->mutex);if(n>=servo_slots){error="Invalid servo";return {};}auto& s=impl_->slots[n];if(!acquire){s.external=false;impl_->stop(s);return {};}
    if(s.calibrating){error="Servo calibration owns this output";return {};}if(!s.view.draft.address.assigned()){error="Choose a servo device first";return {};}
    if(!s.external){impl_->stop(s);s.external=true;impl_->backend(s);s.configure_requested=true;}
    if(!s.backend || !s.backend->snapshot().configured){error="Opening servo disengaged";return {};}error.clear();return s.backend;
}
bool ServoIndicators::external_active(unsigned n)const{std::lock_guard lock(impl_->mutex);return n<servo_slots && impl_->slots[n].external;}
void ServoIndicators::provider_enabled(bool enabled){std::lock_guard lock(impl_->mutex);if(impl_->enabled==enabled)return;impl_->enabled=enabled;if(!enabled)impl_->queue.erase(std::remove_if(impl_->queue.begin(),impl_->queue.end(),[](const auto& e){return e.op!=Impl::Op::save;}),impl_->queue.end());for(auto& s:impl_->slots){if(s.backend)s.backend->provider_enabled(enabled);if(!enabled){impl_->stop(s);s.view.status="Phidgets support disabled; disengagement requested";}else if(!s.page && !s.external)s.automatic=true;}impl_->wake.notify_one();}
void ServoIndicators::observe(Mapper& mapper,const std::vector<PhidgetReading>& readings){std::lock_guard lock(impl_->mutex);
    for(unsigned n=0;n<servo_slots;++n){const auto& d=impl_->slots[n].view.draft;const auto& source=d.source;if(!d.address.assigned()){impl_->sources[n].reset();impl_->currents[n].reset();continue;}auto& cache=impl_->source_cache[n];
        if(cache.epoch!=mapper.epoch || cache.source!=source){cache={};cache.epoch=mapper.epoch;cache.source=source;mapper.watched_records.insert(source.record);if(!source.power_record.empty())mapper.watched_records.insert(source.power_record);}
        auto& value=impl_->sources[n];value.reset();
        if(mapper.ready){const auto found=mapper.records.find(source.record);if(found!=mapper.records.end()){
                if(cache.sequence!=found->second.sequence){cache.sequence=found->second.sequence;cache.number=servo_numeric(found->second.observed,source.field,source.fields);if(cache.number && source.record[1]!='s' && (*cache.number<INT32_MIN || *cache.number>INT32_MAX || std::floor(*cache.number)!=*cache.number || source.fields!=1))cache.number.reset();if(cache.number){auto& number=*cache.number;if(source.mask){if(number>=0 && number<=UINT32_MAX && std::floor(number)==number)number=double((std::uint32_t(number)&source.mask)>>source.shift);else cache.number.reset();}if(cache.number){number=number*source.scale+source.offset;if(!std::isfinite(number))cache.number.reset();}}}value=cache.number;
            }else{cache.sequence=UINT64_MAX;cache.number.reset();}
            if(!source.power_record.empty()){const auto power=mapper.records.find(source.power_record);if(power==mapper.records.end()){value.reset();cache.power_sequence=UINT64_MAX;cache.power.reset();}else{if(cache.power_sequence!=power->second.sequence){cache.power_sequence=power->second.sequence;cache.power=servo_numeric(power->second.observed,-1,1);}const auto& p=cache.power;if(!p || *p<0 || *p>UINT32_MAX || std::floor(*p)!=*p || !(std::uint32_t(*p)&source.power_mask))value.reset();}}
        }
        impl_->currents[n].reset();for(const auto& reading:readings)if(reading.channel.channel_class==PHIDCHCLASS_CURRENTINPUT && reading.channel.address.device==d.address.device && reading.channel.address.channel==d.address.channel && reading.valid){impl_->currents[n]=reading.value;impl_->current_times[n]=reading.checked_ms;break;}
    }
}std::uint64_t ServoIndicators::source_revision()const noexcept{return impl_->source_revision.load();}
std::vector<std::string> ServoIndicators::demand_records()const{std::lock_guard lock(impl_->mutex);std::set<std::string> result;for(const auto& slot:impl_->slots)if(slot.view.draft.address.assigned() && slot.view.draft.source.demand)result.insert(slot.view.draft.source.record);return {result.begin(),result.end()};}
std::vector<std::string> ServoIndicators::source_records()const{std::lock_guard lock(impl_->mutex);std::set<std::string> records;for(const auto& s:impl_->slots)if(s.view.draft.address.assigned()){records.insert(s.view.draft.source.record);if(!s.view.draft.source.power_record.empty())records.insert(s.view.draft.source.power_record);}return {records.begin(),records.end()};}void ServoIndicators::close(){if(!impl_ || !impl_->worker.joinable())return;{std::lock_guard lock(impl_->mutex);impl_->quit=true;impl_->queue.erase(std::remove_if(impl_->queue.begin(),impl_->queue.end(),[](const auto& e){return e.op!=Impl::Op::save;}),impl_->queue.end());for(auto& s:impl_->slots)impl_->stop(s);}impl_->wake.notify_one();impl_->worker.join();}
}
