#include "hwio/motor_mapping.hpp"
#include "hwio/catalogue.hpp"
#include "hwio/storage.hpp"
#include "hwio/actuator_repository.hpp"
#include "hwio/motor_backlash.hpp"
#include <Windows.h>
#include <psx/diagnostics/journal.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace hwio {
const wchar_t* motor_axis_name(MotorAxis a) noexcept {
    constexpr const wchar_t* names[]{L"Throttle 1",L"Throttle 2",L"Throttle 3",L"Throttle 4",L"Speedbrake lever",L"TE flap â€” left outboard",L"TE flap â€” left inboard",L"TE flap â€” right inboard",L"TE flap â€” right outboard",L"Trim indication â€” left",L"Trim indication â€” right"};
    return unsigned(a)<motor_axes?names[unsigned(a)]:L"Unknown axis";
}
bool validate_motor_mappings(const MotorMappingDefinitions& d,std::string& error){
    std::array<bool,10> numbered{};for(const auto& x:d)if(x.analogue_output!=UINT_MAX){if(x.analogue_output>=10 || numbered[x.analogue_output]){error="Duplicate or invalid analogue output number";return false;}numbered[x.analogue_output]=true;}
    std::array<int,motor_slots> owners;owners.fill(-1);
    for(unsigned i=0;i<motor_axes;++i){const auto& x=d[i];if(x.motor>=motor_slots || !std::isfinite(x.following_error) || x.captured>3 || (x.captured==3 && x.low==x.high) || x.feedback.bindings.size()>1 || x.learned_direction<-1 || x.learned_direction>1 || x.direction_key.size()>2048 || bool(x.learned_direction)!=!x.direction_key.empty() || x.direction_key.find_first_of("\r\n")!=x.direction_key.npos){error="Invalid motor mapping, calibration or direction evidence";return false;}
        if(!x.enabled)continue;const int category=i<4?0:int(i);if(owners[x.motor]>=0 && owners[x.motor]!=category){error="A motor may share throttles only; mixed functions compete for ownership";return false;}owners[x.motor]=category;
    }error.clear();return true;
}
bool load_motor_mappings(const std::wstring& directory,MotorMappingDefinitions& definitions,std::string& error,MotorAlignments* alignments,std::string* alignment_warning){try {
    if(alignment_warning)alignment_warning->clear();
    std::optional<std::string> data;if(!actuator_section(directory,"motors",L"motor-mappings.cfg",data,error))return false;if(!data)return true;if(data->size()>262144)throw std::runtime_error("Motor mappings exceed 256 KiB");
    std::istringstream in(*data);std::string header;std::getline(in,header);if(!header.empty() && header.back()=='\r')header.pop_back();const bool numbered=header=="[hwio-motor-mappings-v6]",learning=numbered || header=="[hwio-motor-mappings-v5]",recovery=learning || header=="[hwio-motor-mappings-v4]",response=recovery || header=="[hwio-motor-mappings-v3]",direction=response || header=="[hwio-motor-mappings-v2]";if(!direction && header!="[hwio-motor-mappings-v1]")throw std::runtime_error("Invalid motor-mappings.cfg header");
    MotorMappingDefinitions parsed;for(unsigned i=0;i<motor_axes;++i){unsigned index{};std::string feedback;auto& d=parsed[i];if(!(in>>index>>d.enabled>>d.motor>>d.auto_home>>d.low>>d.high>>d.captured>>d.following_error>>d.following_timeout_ms>>std::quoted(feedback)) || index!=i || (!feedback.empty() && !profile_decode(feedback,d.feedback,error)))throw std::runtime_error("Invalid motor mapping row");if(direction && !(in>>d.learned_direction>>std::quoted(d.direction_key)))throw std::runtime_error("Invalid learned motor direction");if(response){double counts{};if(!(in>>counts) || !std::isfinite(counts) || counts<0 || counts>std::numeric_limits<unsigned>::max() || std::floor(counts)!=counts)throw std::runtime_error("Invalid sensor response count limit");d.sensor_response_counts=static_cast<unsigned>(counts);}if(recovery && !(in>>d.response_delay_ms>>d.lag_allowance_ms))throw std::runtime_error("Invalid motor recovery timing");if(numbered && (!(in>>d.analogue_output) || (d.analogue_output!=UINT_MAX && d.analogue_output>=10)))throw std::runtime_error("Invalid analogue output number");}
    if(!validate_motor_mappings(parsed,error))return false;
    MotorAlignments learned;
    if(learning){try {
        std::string marker;unsigned count{};
        if(!(in>>marker>>count) || marker!="alignments" || count!=motor_slots)throw std::runtime_error("Invalid motor alignment header");
        for(unsigned n=0;n<count;++n){unsigned index{};auto& map=learned[n];
            if(!(in>>marker>>index>>std::quoted(map.identity)>>map.directions[0].count>>map.directions[1].count) || marker!="alignment" || index!=n || map.identity.size()>8192)throw std::runtime_error("Invalid motor alignment identity");
            for(auto& table:map.directions){if(table.count>MotorAlignmentTable::capacity)throw std::runtime_error("Motor alignment exceeds 32 points per direction");for(unsigned i=0;i<table.count;++i){auto& p=table.points[i];if(!(in>>p.raw>>p.delta>>p.observations))throw std::runtime_error("Invalid motor alignment point");}}
            if(!map.valid())throw std::runtime_error("Unusable motor alignment values");
        }
        in>>std::ws;if(!in.eof())throw std::runtime_error("Unexpected motor alignment data");
    }catch(const std::exception& e){learned={};if(alignment_warning)*alignment_warning=std::string(e.what())+"; learned compensation unavailable";}}
    else {in>>std::ws;if(!in.eof())throw std::runtime_error("Unexpected motor mapping data");}
    definitions=std::move(parsed);if(alignments)*alignments=std::move(learned);error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
std::string motor_mappings_encode(const MotorMappingDefinitions& d,const MotorAlignments& alignments){
    std::string error;
    if(!validate_motor_mappings(d,error))throw std::runtime_error(error);std::ostringstream text;text<<"[hwio-motor-mappings-v6]\n"<<std::setprecision(17);for(unsigned i=0;i<motor_axes;++i){const auto& x=d[i];text<<i<<' '<<x.enabled<<' '<<x.motor<<' '<<x.auto_home<<' '<<x.low<<' '<<x.high<<' '<<x.captured<<' '<<x.following_error<<' '<<x.following_timeout_ms<<' '<<std::quoted(x.feedback.id.empty()?std::string{}:profile_encode(x.feedback))<<' '<<x.learned_direction<<' '<<std::quoted(x.direction_key)<<' '<<x.sensor_response_counts<<' '<<x.response_delay_ms<<' '<<x.lag_allowance_ms<<' '<<x.analogue_output<<'\n';}
    text<<"alignments "<<motor_slots<<'\n';
    const auto& learned=alignments;
    for(unsigned n=0;n<motor_slots;++n){const auto& map=learned[n];if(!map.valid())throw std::runtime_error("Invalid motor alignment; previous configuration retained");text<<"alignment "<<n<<' '<<std::quoted(map.identity)<<' '<<map.directions[0].count<<' '<<map.directions[1].count<<'\n';for(const auto& table:map.directions)for(unsigned i=0;i<table.count;++i){const auto& p=table.points[i];text<<p.raw<<' '<<p.delta<<' '<<p.observations<<'\n';}}
    if(text.str().size()>262144)throw std::runtime_error("Motor mappings exceed 256 KiB");
    return text.str();
}
bool save_motor_mappings(const std::wstring& directory,const MotorMappingDefinitions& d,std::string& error,const MotorAlignments* alignments){try {
    const MotorAlignments empty{};const auto text=motor_mappings_encode(d,alignments?*alignments:empty);
    return actuator_section_save(directory,"motors",text,error);
}catch(const std::exception& e){error=e.what();return false;}}

namespace {
std::uint64_t fingerprint(std::string_view text){std::uint64_t h=14695981039346656037ull;for(const unsigned char c:text){h^=c;h*=1099511628211ull;}return h;}
bool fields(std::string_view text,unsigned count,std::array<int,10>& values){unsigned index{};while(true){if(index>=count)return false;const auto pos=text.find(';');if(!integer(text.substr(0,pos),values[index++]))return false;if(pos==text.npos)break;text.remove_prefix(pos+1);}return index==count;}
bool same_source(const MotorMappingDefinition& a,const MotorMappingDefinition& b){return a.enabled==b.enabled && a.motor==b.motor && a.low==b.low && a.high==b.high && a.captured==b.captured && profile_encode(a.feedback)==profile_encode(b.feedback);}
std::string direction_key(const MotorDeviceAddress& a,std::uint64_t calibration,std::uint64_t source,const MotorCapabilities& c){std::ostringstream out;out<<std::quoted(a.provider)<<' '<<std::quoted(a.device)<<' '<<a.serial<<' '<<a.hub_port<<' '<<a.channel<<' '<<a.model<<' '<<a.remote<<' '<<std::quoted(a.server)<<' '<<a.port<<' '<<calibration<<' '<<source<<' '<<c.model<<' '<<c.firmware;if(a.mode!=MotorDriveMode::position)out<<' '<<unsigned(a.mode);return out.str();}
std::string alignment_key(const MotorDefinition& d,const MotorAxisFeedback& f,const MotorSnapshot& m){
    if(!f.source_exact)return {};
    std::ostringstream out;out<<direction_key(d.address,0,f.source_identity,m.capabilities)<<std::setprecision(17)
        <<' '<<f.raw_low<<' '<<f.raw_high<<' '<<f.code_count<<' '<<std::quoted(m.applied.units)<<' '<<m.applied.rescale
        <<' '<<unsigned(d.feedback.kind)<<' '<<d.feedback.inverted<<' '<<std::quoted(d.feedback.input_profile)
        <<' '<<std::quoted(direction_key(d.feedback.encoder,0,0,{}));
    return out.str();
}
}
void MotorMappingInputs::configure(const MotorMappingDefinitions& defs,const AxisConfiguration& axes,const std::vector<DeviceInfo>& devices){
    const auto previous=sensors_;sensors_={};std::vector<DeviceIdentity> ids;std::vector<const DeviceInfo*> connected;for(const auto& dev:devices)if(dev.connected){ids.push_back(dev.identity);connected.push_back(&dev);}
    for(unsigned i=0;i<motor_axes;++i)if(defs[i].enabled){const Profile* profile{};double low{},high{};bool calibrated{};std::uint64_t revision{};
        if(i<=4){const unsigned slot=i<4?9+i:8;const auto& row=axes.rows[slot];const auto& setup=row.applied;if(!row.managed || !row.active || setup.source.bindings.empty())continue;const auto& b=setup.source.bindings[0];const auto& c=b.calibration;
            // Forward idle is the centre of a combined axis. Reverse is never
            // included in the motor traversal or its position conversion.
            low=i<4 && b.throttle_layout==ThrottleLayout::combined?c.center:c.minimum;high=c.maximum;if(c.inverted){if(i<4 && b.throttle_layout==ThrottleLayout::combined)high=c.minimum;else std::swap(low,high);}
            const auto required=i<4 && b.throttle_layout==ThrottleLayout::combined?7u:5u;profile=&setup.source;calibrated=(setup.captured&required)==required && calibration_valid(c);revision=fingerprint(profile_encode(setup.source)+std::to_string(setup.captured));
        }else {const auto& d=defs[i];profile=&d.feedback;low=d.low;high=d.high;calibrated=d.captured==3;revision=fingerprint(profile_encode(d.feedback)+std::to_string(d.low)+":"+std::to_string(d.high)+":"+std::to_string(d.captured));}
        auto& s=sensors_[i];s.calibrated=calibrated;s.combined=i<4 && profile && !profile->bindings.empty() && profile->bindings[0].throttle_layout==ThrottleLayout::combined;s.revision=revision;if(!profile || profile->bindings.empty() || profile->bindings[0].sources.empty() || low==high)continue;const auto match=match_profile(*profile,ids);if(match<0)continue;const auto& dev=*connected[unsigned(match)];if(dev.slot>=max_devices)continue;
        auto resolved=*profile;resolved.identity=dev.identity;
        s.exact=(!profile->identity.serial.empty() && profile->identity.serial==dev.identity.serial) || (!profile->identity.hardware_key.empty() && profile->identity.hardware_key==dev.identity.hardware_key) || (!profile->identity.container_id.empty() && profile->identity.container_id==dev.identity.container_id);
        if(!resolved.identity.serial.empty() || !resolved.identity.hardware_key.empty()){resolved.identity.instance_path.clear();resolved.identity.container_id.clear();resolved.identity.name.clear();}
        // Physical alignment is independent of logical binding/axis names and
        // PSX destinations. Retain exact resolved sensor geometry/calibration.
        Profile physical;physical.identity=resolved.identity;Binding geometry;
        geometry.sources=profile->bindings[0].sources;geometry.calibration=profile->bindings[0].calibration;geometry.throttle_layout=profile->bindings[0].throttle_layout;physical.bindings.push_back(std::move(geometry));
        s.identity=fingerprint(profile_encode(physical));
        for(unsigned c=0;c<dev.controls.size() && c<max_controls;++c)if(same(dev.controls[c].key,profile->bindings[0].sources[0].key) && !dev.controls[c].relative){s.slot=dev.slot;s.control=c;s.resolved=true;s.codes=double(dev.controls[c].maximum)-dev.controls[c].minimum+1;s.generation=dev.generation;s.low=low;s.inverse_span=1/(high-low);break;}
        const auto& old=previous[i];if(s.resolved && old.resolved && s.slot==old.slot && s.control==old.control && s.generation==old.generation && s.revision==old.revision && s.identity==old.identity)s.physical=old.physical;
    }
}
void MotorMappingInputs::baseline(const SourceBank& bank){for(auto& s:sensors_)if(s.resolved && bank[s.slot][s.control].available)s.physical=bank[s.slot][s.control];}
void MotorMappingInputs::observe(const Sample& sample)noexcept{if(sample.relative || sample.report_end)return;for(auto& s:sensors_)if(s.resolved && s.slot==sample.device && s.control==sample.control && s.generation==sample.generation)s.physical={sample.value,!sample.invalid,sample.qpc,sample.sequence};}
void MotorMappingInputs::invalidate()noexcept{for(auto& s:sensors_)s.physical={};}
MotorMappingFrame MotorMappingInputs::frame(const Mapper& mapper,std::uint64_t now)const{return make_frame(nullptr,mapper,now);}
MotorMappingFrame MotorMappingInputs::frame(const SourceBank& bank,const Mapper& mapper,std::uint64_t now)const{return make_frame(&bank,mapper,now);}
MotorMappingFrame MotorMappingInputs::make_frame(const SourceBank* bank,const Mapper& mapper,std::uint64_t now)const{
    MotorMappingFrame f;f.stamp_ms=now;f.psx_generation=mapper.epoch;f.psx_ready=mapper.ready;f.authority=mapper.at;f.at_inactive_sequence=mapper.at_inactive_sequence;const auto qpc=psx::diagnostics::qpc_now();const auto frequency=psx::diagnostics::qpc_frequency();
    for(unsigned i=0;i<motor_axes;++i){const auto& s=sensors_[i];if(i<4)f.combined_throttles[i]=s.combined;auto& fdb=f.sensors[i];fdb.calibrated=s.calibrated;fdb.calibration_revision=s.revision;fdb.generation=s.generation;fdb.source_identity=s.identity;fdb.source_exact=s.exact;fdb.code_count=s.codes;if(s.resolved){const auto& raw=bank?(*bank)[s.slot][s.control]:s.physical;fdb.valid=raw.available;if(raw.qpc>0 && raw.qpc<=qpc && frequency){const auto age=std::uint64_t(std::ceil(1000.*double(qpc-raw.qpc)/double(frequency)));fdb.sample_ms=age<now?now-age:0;}fdb.fraction=(raw.value-s.low)*s.inverse_span;fdb.resolution=std::abs(s.inverse_span);fdb.raw=raw.value;fdb.raw_low=s.low;fdb.raw_high=s.low+1/s.inverse_span;fdb.raw_valid=raw.available;fdb.sample_sequence=raw.sequence;}}
    for(const auto& m:mapper.mappings)if(m.externally_calculated && m.binding.target.record==RecordKey{RecordKind::qs,436} && m.binding.target.field<4 && m.state.available && m.state.baseline){f.pilot_values[m.binding.target.field]=m.state.desired;f.pilot_valid[m.binding.target.field]=true;}
    std::array<int,10> decoded{};auto record=[&](const char* key){const auto it=mapper.records.find(key);return it==mapper.records.end()?static_cast<const Record*>(nullptr):&it->second;};
    if(const auto* r=record("Qh58")){int arm{};if(integer(r->observed,arm) && (arm==0 || arm==1)){f.at_arm_valid=true;f.at_armed=arm==1;}}
    if(const auto* r=record("Qs436");r && fields(r->observed,4,decoded) && std::all_of(decoded.begin(),decoded.begin()+4,[](int n){return n>=-8925 && n<=5000;}))for(unsigned i=0;i<4;++i){f.targets[i]=decoded[i]/5000.;f.target_valid[i]=decoded[i]>=0;f.target_sequence[i]=r->sequence;}
    if(const auto* r=record("Qh388")){int n{};if(integer(r->observed,n) && n>=0 && n<=800){f.targets[4]=n/800.;f.target_valid[4]=true;f.target_sequence[4]=r->sequence;}}
    if(const auto* r=record("Qs425");r && fields(r->observed,10,decoded) && std::all_of(decoded.begin(),decoded.begin()+6,[](int n){return n>=0 && n<=500;}) && std::all_of(decoded.begin()+6,decoded.end(),[](int n){return n>=0 && n<=2250;}))for(unsigned i=5;i<9;++i){f.targets[i]=decoded[i+1]/2250.;f.target_valid[i]=true;f.target_sequence[i]=r->sequence;}
    const auto* trim=record("Qi119");const auto* power=record("Qi226");int t{},p{};if(trim && power && integer(trim->observed,t) && t>=0 && t<=15000 && integer(power->observed,p) && p>=0)for(unsigned i=9;i<11;++i){f.targets[i]=t/15000.;f.target_valid[i]=(p&(1<<(i-9)))!=0;f.target_sequence[i]=std::max(trim->sequence,power->sequence);}return f;
}
MotorMappingFrame motor_mapping_frame(const MotorMappingDefinitions& defs,const AxisConfiguration& axes,const std::vector<DeviceInfo>& devices,const SourceBank& bank,const Mapper& mapper,std::uint64_t now){MotorMappingInputs inputs;inputs.configure(defs,axes,devices);return inputs.frame(bank,mapper,now);}
std::optional<Command> motor_throttle_feedback(Mapper& mapper,const MotorMappingRouting& routing,std::int64_t qpc){
    if(!routing.feedback_mask || (routing.feedback_mask&~15u) || !mapper.ready)return {};
    const auto record=mapper.records.find("Qs436");if(record==mapper.records.end())return {};
    std::string payload=record->second.effective;std::optional<std::string> line;
    for(unsigned i=0;i<4;++i)if(routing.feedback_mask&(1u<<i)){const auto* target=flight_target(9+i);if(!target || !target->descriptor.writable)return {};line=compose(target->descriptor,payload,routing.throttle_values[i]);if(!line)return {};payload=line->substr(line->find('=')+1);}
    record->second.effective=payload;return Command{0xF000000001ull,mapper.epoch,0,qpc,*line+"\n",false,static_cast<std::uint8_t>(routing.feedback_mask)};
}
MotorMappings::MotorMappings(MotorBench& b,std::wstring path):bench_(b),directory_(std::move(path)){
    load_motor_mappings(directory_,definitions_,storage_error_,&alignments_,&alignment_warning_);const auto logs=(directory_==executable_directory()?profile_directory():directory_)+L"\\logs";
    // Upgrade only a terminal successful HWIO record, never an arbitrary quiet
    // zero or a failed/interrupted probe. It remains a candidate until the
    // current calibrated source and exact controller identity match at Home.
    try{const auto file=std::filesystem::path(logs)/L"last-motor-homing.csv";if(std::filesystem::exists(file) && std::filesystem::file_size(file)<=2097152){std::ifstream in(file);std::string line,last,header;bool complete{};while(std::getline(in,line)){if(header.empty())header=line;if(line.starts_with("# "))complete=line.find("stopped: Session homing and parking are complete and motor disengagement is requested.")!=line.npos;if(!line.empty() && line[0]!='#' && line.starts_with("elapsed_ms")==false)last=line;}
        const auto axis_at=header.find(" (");const auto motor_at=header.find(" / motor ");const auto device_at=header.find(" / ",motor_at==header.npos?header.size():motor_at+9);
        if(complete && axis_at!=header.npos && motor_at!=header.npos && device_at!=header.npos){const auto axis=unsigned(std::stoul(header.substr(axis_at+2))),slot=unsigned(std::stoul(header.substr(motor_at+9)));std::vector<std::string> cells;std::istringstream row(last);while(std::getline(row,line,','))cells.push_back(line);
            if(slot<motor_slots && axis<motor_axes && cells.size()>=27 && cells[1]=="stopped" && cells[14]=="1" && cells[15]=="1"){const int polarity=std::stoi(cells[20]);const auto calibration=std::stoull(cells[19]);if((polarity==1 || polarity==-1) && calibration)legacy_directions_[slot]={header.substr(device_at+3),axis,calibration,polarity};}
        }
    }}catch(...){} // Absent/unqualified diagnostics never disable generic I/O.
    following_log_=std::make_unique<MotorFollowingLog>(logs);homing_log_=std::make_unique<MotorHomingLog>(logs);persistence_thread_=std::thread([this]{persist_directions();});
}
MotorMappings::~MotorMappings(){shutdown();{std::lock_guard lock(persistence_mutex_);persistence_stop_=true;}persistence_wake_.notify_one();if(persistence_thread_.joinable())persistence_thread_.join();}
void MotorMappings::queue_direction_save(){{std::lock_guard lock(persistence_mutex_);persistence_pending_=true;}persistence_wake_.notify_one();}
void MotorMappings::persist_directions(){for(;;){
    {std::unique_lock lock(persistence_mutex_);persistence_wake_.wait(lock,[this]{return persistence_pending_ || persistence_stop_;});if(!persistence_pending_ && persistence_stop_)return;persistence_pending_=false;}
    std::lock_guard save_lock(save_mutex_);MotorMappingDefinitions copy;MotorAlignments learned;{std::lock_guard lock(mutex_);copy=definitions_;learned=alignments_;}
    std::string error;const bool saved=save_motor_mappings(directory_,copy,error,&learned);{std::lock_guard lock(mutex_);direction_storage_error_=saved?std::string{}:error;if(!saved)alignment_save_.changed(sessions_[0].tick_ms);}
}}
MotorMappingDefinitions MotorMappings::definitions()const{std::lock_guard lock(mutex_);return definitions_;}
std::uint64_t MotorMappings::definition_revision()const{std::lock_guard lock(mutex_);return definition_revision_;}

bool MotorMappings::assign(MotorMappingDefinitions d,std::string& error){
    std::lock_guard save_lock(save_mutex_);std::lock_guard lock(mutex_);
    if(!storage_error_.empty()){error=storage_error_;return false;}
    for(unsigned i=0;i<motor_axes;++i)if(same_source(d[i],definitions_[i])){d[i].direction_key=definitions_[i].direction_key;d[i].learned_direction=definitions_[i].learned_direction;}else {d[i].direction_key.clear();d[i].learned_direction=0;}
    if(!save_motor_mappings(directory_,d,error,&alignments_))return false;
    alignment_save_.submitted();
    std::array<bool,motor_slots> changed{};
    for(unsigned i=0;i<motor_axes;++i)if(!same_source(d[i],definitions_[i])){changed[d[i].motor]=changed[definitions_[i].motor]=true;}
    for(unsigned n=0;n<motor_slots;++n){unsigned old_master=motor_axes,new_master=motor_axes;for(unsigned i=0;i<motor_axes;++i){if(definitions_[i].enabled && definitions_[i].motor==n)old_master=std::min(old_master,i);if(d[i].enabled && d[i].motor==n)new_master=std::min(new_master,i);}changed[n]=old_master!=new_master || (old_master<motor_axes && !same_source(d[old_master],definitions_[old_master]));}
    for(unsigned n=0;n<motor_slots;++n)if(changed[n]){sessions_[n].map.valid=sessions_[n].view.homed=false;end(n,sessions_[n],"Assignment changed; Home now required");sessions_[n].auto_attempted=true;}
    definitions_=std::move(d);++definition_revision_;++assignment_revision_;return true;
}
void MotorMappings::manual_page(int selected){
    std::lock_guard lock(mutex_);
    for(unsigned n=0;n<motor_slots;++n){auto& s=sessions_[n];const bool wanted=int(n)==selected;if(s.manual_page==wanted)continue;
        if(wanted){
            if(s.owner==MotorOwner::homing){homing_log_->finish(n,"Homing cancelled by manual motor page",false);s.map.valid=s.view.homed=false;s.latched=true;s.home_requested=s.opening=false;}
            s.manual_page=true;s.resume_page=false;s.claimed=false;s.opening=false;s.owner=MotorOwner::bench;
            s.view.status="Manual motor page; assignment paused";
        }else {
            s.manual_page=false;s.resume_page=s.map.valid && s.view.homed && !s.latched;s.owner=MotorOwner::stopped;
            s.view.status=s.resume_page?"Returning motor to assignment control":"Manual page closed; assignment stopped or homing required";
        }
        s.command=s.limits=s.release={};s.command_target=s.command_hold=s.initial_requested=s.reversal_hold=false;s.movement_direction=0;s.response.reset();s.accepted_ms=0;s.limit_velocity=s.limit_acceleration=0;s.rejection={};s.rejection_kind=0;
        s.correction.reset();s.correction_hold=s.goal_known=s.force_correction=s.limit_hold=false;
        s.physical_hold=s.correction_moved=false;s.arrival_after_ms=s.arrival_after_sequence=0;s.disconnect_window.reset();s.tracking={};
        s.view.phase=MotorFollowingPhase::stopped;
    }
    bench_.manual_page(selected);
}
void MotorMappings::home(unsigned n){if(n>=motor_slots)return;std::lock_guard lock(mutex_);auto& s=sessions_[n];s.home_requested=true;s.opening=s.log_started=false;s.failure_revision=bench_.snapshot(n).initialization_failure_revision;s.automatic=false;hidden_=false;}
void MotorMappings::stop(unsigned n,std::uint64_t operation){if(n>=motor_slots)return;std::lock_guard lock(mutex_);auto& s=sessions_[n];s.stop_requested=true;s.reset_requested=false;bench_.disengage(n);if(operation)s.view.operation_id=operation;}
bool MotorMappings::reset_home(unsigned n,std::uint64_t revision,std::uint64_t operation,std::string& error){
    std::lock_guard lock(mutex_);if(n>=motor_slots){error="Invalid motor index";return false;}auto& s=sessions_[n];
    if(operation && s.view.operation_id==operation)return true;
    if(s.view.fault_revision!=revision){error="Motor fault changed; read status before reset";return false;}
    if(s.view.reset_pending){error="Motor reset already underway";return false;}
    if(!bench_.snapshot(n).draft.address.assigned()){error="Motor has no assigned controller";return false;}
    s.view.reset_pending=true;s.view.reset_failed=false;s.view.operation_id=operation;s.reset_requested=true;s.stop_requested=false;s.rejection={};s.rejection_kind=0;error.clear();return true;
}
psx::hwio::motors::v2::Packet MotorMappings::motor_status_v2(std::uint64_t session)const{
    namespace m=psx::hwio::motors;std::lock_guard lock(mutex_);m::v2::Packet packet;packet.session=session;packet.revision=assignment_revision_;
    for(unsigned n=0;n<motor_slots;++n){const auto& s=sessions_[n];const auto b=bench_.snapshot(n);auto& status=packet.motors[n];status.generation=b.live.generation;status.fault_revision=s.view.fault_revision;status.operation_id=s.view.operation_id;
        const bool mapped=std::any_of(definitions_.begin(),definitions_.end(),[&](const auto& d){return d.enabled && d.motor==n;});
        status.flags=(b.draft.address.assigned()?m::assigned:0)|((s.view.major_fault || b.live.drive==MotorDriveState::unknown)?m::major_fault:0)|(mapped && !s.view.homed?m::homing_required:0)|(b.live.drive==MotorDriveState::off?m::release_confirmed:0)|(s.view.reset_pending?m::reset_pending:0)|(s.view.reset_failed?m::reset_failed:0);
        status.phase=s.owner==MotorOwner::homing?m::Phase::homing:s.view.reset_pending?m::Phase::restoring:s.view.phase==MotorFollowingPhase::correcting?m::Phase::recovering:static_cast<m::Phase>(s.view.phase);
        if(s.view.phase==MotorFollowingPhase::correcting)status.flags|=m::recovering;
        const auto& cause=s.view.major_fault?s.view.cause:s.view.status;std::copy_n(cause.begin(),std::min(cause.size(),status.cause.size()-1),status.cause.begin());
    }return packet;
}
psx::hwio::motors::Packet MotorMappings::motor_status(std::uint64_t session)const{
    const auto current=motor_status_v2(session);psx::hwio::motors::Packet legacy;legacy.session=current.session;legacy.revision=current.revision;std::copy_n(current.motors.begin(),legacy.motors.size(),legacy.motors.begin());return legacy;
}
std::array<psx::hwio::thrust_control::Lever,4> MotorMappings::thrust_status(const MotorMappingFrame& frame)const{
    namespace t=psx::hwio::thrust_control;std::lock_guard lock(mutex_);std::array<t::Lever,4> result;
    for(unsigned i=0;i<4;++i){auto& l=result[i];l.master=i;if(!definitions_[i].enabled)continue;
        const auto n=definitions_[i].motor;const auto& s=sessions_[n];const auto b=bench_.snapshot(n);const auto& m=b.live;const auto& d=b.draft;
        if(!d.address.assigned())continue;for(unsigned j=0;j<i;++j)if(definitions_[j].enabled && definitions_[j].motor==n){l.master=j;break;}
        const auto& f=frame.sensors[l.master];l.flags=t::covered|(m.drive==MotorDriveState::off?t::drive_released:0);l.group=n+1;
        // Backend configuration_revision also advances for ordinary trajectory
        // limits. Those do not replace the motor, sensor or session Home.
        l.generation=(m.generation+1)*1099511628211ull^(f.generation+1);l.calibration=(f.calibration_revision+1)*1099511628211ull^(m.coordinate_revision+1)^f.source_identity;
        if(s.command.id || s.release.id || s.ios_handover)l.flags&=~t::drive_released;
        if(f.valid && f.calibrated && std::isfinite(f.fraction)){l.flags|=t::physical_valid;l.physical_percent=f.fraction*100;}
        std::string reason;
        if(b.manual_page || s.manual_page || s.resume_page)reason="Manual motor page owns this motor";
        else if(m.state==MotorState::fault || s.view.major_fault || m.drive==MotorDriveState::unknown)reason=!m.fault.empty()?m.fault:!s.view.cause.empty()?s.view.cause:"Motor fault or unconfirmed drive release";
        else if(s.latched || b.operator_stopped)reason="Motor stopped; Home/reset required before IOS positioning";
        else if(s.home_requested || s.owner==MotorOwner::homing || s.view.reset_pending)reason="Motor homing/reset in progress";
        else if(!b.mapping_owned)reason="Motor control ownership unavailable";
        else if(!s.map.valid || !s.view.homed)reason="Session homing required";
        else if(!frame.provider_enabled)reason="Motor provider disabled";
        else if(!f.valid || !f.calibrated || !std::isfinite(f.fraction))reason="Calibrated physical feedback unavailable";
        else if(!m.attached || !m.configured || !m.feedback_valid || b.pending)reason="Motor initialization/configuration unavailable";
        else if(!d.maximum_velocity || !d.maximum_acceleration || (m.capabilities.current_limit && !d.current_limit))reason="Motor motion/current limits required";
        else if(m.generation!=s.motor_generation || m.coordinate_revision!=s.coordinate || f.generation!=s.sensor_generation || f.calibration_revision!=s.calibration || f.source_identity!=s.source_identity)reason="Motor/sensor continuity changed; Home required";
        else l.flags|=t::ready;
        l.phase=!(l.flags&t::ready)?t::Phase::unavailable:s.view.phase==MotorFollowingPhase::limit?t::Phase::limit:s.physical_hold?t::Phase::held:s.correction.active()?t::Phase::correcting:s.ios_owned?t::Phase::positioning:t::Phase::ready;
        l.operation_id=s.ios_operation;std::copy_n(reason.begin(),std::min(reason.size(),l.reason.size()-1),l.reason.begin());
    }return result;
}
std::uint32_t MotorMappings::thrust_released_groups(std::uint64_t operation,bool releasing)const{
    std::lock_guard lock(mutex_);std::uint32_t result{};
    for(unsigned n=0;n<motor_slots;++n){const auto& s=sessions_[n];const auto b=bench_.snapshot(n);if(b.live.drive!=MotorDriveState::off || b.live.engaged)continue;
        if(releasing && s.ios_owned && (s.ios_drive || s.ios_operation!=operation))continue;
        if(s.release.id && b.live.state!=MotorState::closed){const auto r=bench_.mapping_result(n,s.release.id);if(!r || !*r || r->stage==MotorResultStage::queued)continue;}
        if(s.command.id){const auto r=bench_.mapping_result(n,s.command.id);if(!r || r->stage==MotorResultStage::queued)continue;}
        if(s.ios_handover)continue;result|=1u<<n;
    }return result;
}
void MotorMappings::end(unsigned n,Session& s,std::string status,bool fault){
    s.ios_owned=s.ios_drive=s.ios_handover=false;s.ios_operation=0;
    s.command_hold=s.initial_requested=s.reversal_hold=false;s.movement_direction=0;
    s.correction.reset();s.correction_hold=s.goal_known=s.force_correction=s.limit_hold=false;
    s.physical_hold=s.correction_moved=false;s.arrival_after_ms=s.arrival_after_sequence=0;s.disconnect_window.reset();s.tracking={};homing_log_->finish(n,status,fault);if(s.claimed)s.release=bench_.release_mapping(n);s.claimed=false;s.owner=MotorOwner::stopped;s.latched=true;s.home_requested=s.opening=s.finishing=false;s.command=s.limits={};s.response.reset();s.await_rearm=s.inactive_seen=false;s.disconnect_delivery.clear();
    auto& v=s.view;v.engaged=false;v.state=fault?HomingState::fault:HomingState::stopped;v.phase=fault?MotorFollowingPhase::fault:MotorFollowingPhase::stopped;v.status=std::move(status);
    if(fault){v.major_fault=true;v.cause=v.status;++v.fault_revision;}v.reset_failed=v.reset_pending && fault;v.reset_pending=false;++v.revision;
}
void MotorMappings::inhibit(unsigned n,std::string reason){if(n>=motor_slots)return;std::lock_guard lock(mutex_);auto& s=sessions_[n];if(s.claimed || s.home_requested || s.view.status!=reason){s.map.valid=s.view.homed=false;end(n,s,std::move(reason),true);}}
void MotorMappings::at_disconnect_delivery(unsigned motors,std::string_view status){
    std::lock_guard lock(mutex_);for(unsigned n=0;n<motor_slots;++n)if(motors&(1u<<n)){auto& s=sessions_[n];if(s.disconnect_delivery==status)continue;s.disconnect_delivery=status;
        if(s.await_rearm && !s.latched && !s.manual_page && !s.resume_page){s.view.status=s.disconnect_delivery+"; "+s.view.cause;++s.view.revision;}
    }
}
void MotorMappings::hide_manual(){std::lock_guard lock(mutex_);hidden_=true;}
void MotorMappings::shutdown(){std::lock_guard lock(mutex_);for(unsigned n=0;n<motor_slots;++n)end(n,sessions_[n],"Operator stopped; drive released");if(alignment_save_.dirty){alignment_save_.submitted();queue_direction_save();}}
std::array<MotorMappingView,motor_axes> MotorMappings::views()const{std::lock_guard lock(mutex_);std::array<MotorMappingView,motor_axes> result;
    for(unsigned i=0;i<motor_axes;++i)if(definitions_[i].enabled){result[i]=sessions_[definitions_[i].motor].view;const auto error=homing_log_->error();if(!error.empty())result[i].status+="; Homing log: "+error;if(!direction_storage_error_.empty())result[i].status+="; Motor alignment/direction save: "+direction_storage_error_;if(!alignment_warning_.empty())result[i].status+="; "+alignment_warning_;}return result;
}
MotorMappingRouting MotorMappings::tick(const MotorMappingFrame& frame){
    std::lock_guard lock(mutex_);MotorMappingRouting routing;
    if(alignment_save_.due(frame.stamp_ms)){alignment_save_.submitted();queue_direction_save();}
    for(unsigned n=0;n<motor_slots;++n){
        auto& s=sessions_[n];auto& v=s.view;const auto now=frame.stamp_ms;s.tick_ms=now;v.target_change=v.retarget_kind=0;v.request_direction=0;
        unsigned master=motor_axes,mask{};bool automatic{};
        for(unsigned i=0;i<motor_axes;++i)if(definitions_[i].enabled && definitions_[i].motor==n){master=std::min(master,i);if(i<4)mask|=1u<<i;automatic|=definitions_[i].auto_home;}
        const auto b=bench_.snapshot(n);const auto& m=b.live;const auto& d=b.draft;
        v.engaged=m.engaged;v.release_confirmed=m.drive==MotorDriveState::off;
        if(s.stop_requested){s.stop_requested=false;s.reset_requested=false;end(n,s,"Operator stopped; drive release requested");continue;}
        if(s.reset_requested && !b.manual_page){
            const auto& sensor=master<motor_axes?frame.sensors[master]:MotorAxisFeedback{};
            const bool retained=master<motor_axes && v.homed && s.map.valid && m.configured && m.feedback_valid && m.generation==s.motor_generation && m.coordinate_revision==s.coordinate && sensor.valid && sensor.generation==s.sensor_generation && sensor.calibration_revision==s.calibration && sensor.source_identity==s.source_identity;
            if(retained){
                if(m.engaged || m.drive!=MotorDriveState::off){bench_.disengage(n);v.status="Reset waiting for confirmed drive release";continue;}
                std::string error;if(!b.mapping_owned && !bench_.claim_mapping(n,error)){v.status=error;continue;}
                s.reset_requested=false;s.claimed=true;s.owner=MotorOwner::following;s.latched=s.await_rearm=s.inactive_seen=false;s.response.reset();s.accepted_ms=0;s.command=s.limits={};s.release={};s.limit_velocity=s.limit_acceleration=0;v.major_fault=v.reset_pending=v.reset_failed=false;v.cause.clear();v.status="Motor reset complete; session homing retained";continue;
            }
            s.reset_requested=false;s.opening=false;s.home_requested=master<motor_axes;s.log_started=false;s.failure_revision=b.initialization_failure_revision;s.automatic=true;s.latched=true;s.owner=MotorOwner::stopped;s.map.valid=v.homed=false;bench_.activate(n);
        }
        if(master==motor_axes){bench_.guard(n,{});if(v.reset_pending && !b.pending && m.configured && m.feedback_valid && m.drive==MotorDriveState::off){v.major_fault=v.reset_pending=v.reset_failed=false;v.status="Motor reset complete; disengaged";}continue;}
        auto feedback=frame.sensors[master];
        if(!feedback.raw_valid){feedback.raw_low=0;feedback.raw_high=feedback.resolution>0?1/feedback.resolution:0;feedback.raw=feedback.fraction*feedback.raw_high;}
        const double raw=feedback.raw;v.sensor=feedback.fraction;
        if(now>s.last_ms){s.measured_velocity=(m.position-s.last_count)*1000/double(now-s.last_ms);s.last_ms=now;s.last_count=m.position;}
        const bool ready=frame.provider_enabled && feedback.valid && feedback.calibrated && std::isfinite(raw) && feedback.raw_low!=feedback.raw_high && m.attached && m.configured && m.feedback_valid && d.maximum_velocity && d.maximum_acceleration && (!m.capabilities.current_limit || d.current_limit);
        const bool ios=master<4 && frame.ios_owned;
        const double demand_fraction=ios?frame.ios_percent[master]/100:frame.targets[master];
        const auto demand_generation=ios?frame.ios_token:frame.psx_generation;
        const bool active=ios?frame.ios_drive:frame.psx_ready && frame.target_valid[master] && (master>=4 || frame.authority==AtAuthority::servo);
        // Observe native A/T inactivity before any pending SDK receipt, manual
        // page or settings update can defer motion servicing. The receive-side
        // sequence also retains an inactive-active pair drained in one batch.
        if(!ios && s.await_rearm){
            if(s.rearm_psx_generation!=frame.psx_generation){s.rearm_psx_generation=frame.psx_generation;s.rearm_inactive_sequence=frame.at_inactive_sequence;}
            if(frame.psx_ready && (master<4?
                frame.authority==AtAuthority::physical || frame.at_inactive_sequence!=s.rearm_inactive_sequence:
                !frame.target_valid[master]))s.inactive_seen=true;
        }
        auto disconnect=[&]{if(!ios && master<4 && frame.psx_ready && frame.authority==AtAuthority::servo){routing.disconnect_at=true;routing.disconnect_faults|=1u<<n;}};
        auto hard_fault=[&](std::string reason,bool invalidate=true){if(invalidate)s.map.valid=v.homed=false;routing.fault_mask|=1u<<n;routing.fault_reasons[n]=reason;end(n,s,reason,true);MotorFollowingTrace t;t.ms=now;t.motor=n;t.raw=raw;t.raw_low=feedback.raw_low;t.raw_high=feedback.raw_high;t.encoder=m.position;t.accepted=m.target;t.velocity=m.applied.velocity_limit;t.acceleration=m.applied.acceleration;t.generation=m.generation;t.sensor_generation=feedback.generation;t.coordinate=m.coordinate_revision;t.calibration=feedback.calibration_revision;t.phase=unsigned(v.phase);t.drive=int(m.drive);t.valid=ready;t.sdk_code=m.fault_code;t.codes=feedback.code_count;following_log_->sample(t,reason,true);};
        // Physical pilot feedback remains independent of motor engagement.
        if(ios)routing.suppress_throttles|=mask;
        if(!ios && master<4 && frame.psx_ready && feedback.valid && (b.manual_page || (s.owner!=MotorOwner::homing && !s.home_requested && !s.reset_requested))){
            if(s.psx_generation!=frame.psx_generation){s.psx_generation=frame.psx_generation;s.feedback_ms=0;s.last_feedback=-100000;s.response.reset();}
            if(frame.authority==AtAuthority::servo){routing.suppress_throttles|=mask;if(now-s.feedback_ms>=1000){s.feedback_ms=now;routing.feedback_mask|=mask;const int measured=frame.pilot_valid[master] && frame.pilot_values[master]<0?frame.pilot_values[master]:int(std::lround(std::clamp(feedback.fraction,0.,1.)*5000));for(unsigned i=0;i<4;++i)if(mask&(1u<<i))routing.throttle_values[i]=measured;}}
            else if(frame.authority==AtAuthority::physical && frame.pilot_valid[master]){const int value=frame.pilot_values[master];routing.suppress_throttles|=mask;if(value!=s.last_feedback){s.last_feedback=value;routing.feedback_mask|=mask;for(unsigned i=0;i<4;++i)if(mask&(1u<<i))routing.throttle_values[i]=value;}}
        }
        if(b.manual_page){s.disconnect_window.reset();s.tracking={};
            v.phase=MotorFollowingPhase::stopped;v.status="Manual motor page; assignment paused";continue;
        }
        if(s.resume_page){
            if(b.pending || m.engaged || m.drive!=MotorDriveState::off){v.status="Waiting for manual drive release before assignment control";continue;}
            if(!ready || m.generation!=s.motor_generation || m.coordinate_revision!=s.coordinate || feedback.generation!=s.sensor_generation || feedback.calibration_revision!=s.calibration || feedback.source_identity!=s.source_identity || direction_key(d.address,feedback.calibration_revision,feedback.source_identity,m.capabilities)!=s.identity){s.resume_page=false;hard_fault("Motor/sensor assignment or continuity changed during manual control; homing required");continue;}
            std::string error;if(!bench_.claim_mapping(n,error)){v.status="Returning to assignment: "+error;continue;}
            s.resume_page=false;s.claimed=true;s.owner=MotorOwner::following;s.command=s.limits=s.release={};s.response.reset();s.accepted_ms=0;s.limit_velocity=s.limit_acceleration=0;v.status="Assignment control restored; session homing retained";continue;
        }
        if(s.claimed)bench_.mapping_heartbeat(n);
        if((s.home_requested || s.owner==MotorOwner::homing) && hidden_ && !s.automatic){end(n,s,"Homing stopped; page hidden");continue;}
        if(!b.manual_page && !s.resume_page && s.claimed && !b.mapping_owned && (b.operator_stopped || b.running || b.jogging)){
            homing_log_->finish(n,"Homing/mapped following stopped by operator motor control",false);s.claimed=false;s.latched=true;s.auto_attempted=true;s.home_requested=false;s.owner=b.running || b.jogging?MotorOwner::bench:MotorOwner::stopped;s.command=s.limits={};s.response.reset();v.state=HomingState::stopped;v.phase=MotorFollowingPhase::stopped;v.status="Operator motor control; mapped following stopped";
        }
        if((s.owner==MotorOwner::homing || v.homed) && !s.latched && !b.manual_page && !s.resume_page){
            if(b.pending && !m.engaged && m.generation==s.motor_generation && m.coordinate_revision==s.coordinate && feedback.valid){s.disconnect_window.reset();s.tracking={};v.status="Applying motor settings; motion paused";continue;}
            if(!ready || m.generation!=s.motor_generation || m.coordinate_revision!=s.coordinate || feedback.generation!=s.sensor_generation || feedback.calibration_revision!=s.calibration || feedback.source_identity!=s.source_identity || m.state==MotorState::fault || !b.mapping_owned){
                const char* reason=!feedback.valid?"Physical sensor feedback unavailable":!frame.provider_enabled?"Motor provider disabled":m.generation!=s.motor_generation?"Motor generation changed":m.coordinate_revision!=s.coordinate?"Encoder coordinates changed":feedback.generation!=s.sensor_generation?"Sensor generation changed":feedback.calibration_revision!=s.calibration?"Sensor calibration changed":!b.mapping_owned?"Motor control ownership lost":m.state==MotorState::fault?"Motor SDK fault":"Motor/sensor configuration unavailable";
                hard_fault(std::string(reason)+(m.fault.empty()?"":"; "+m.fault));continue;
            }
        }
        if(!b.manual_page && automatic && !s.auto_attempted && !s.latched && !b.dirty && b.saved.address.assigned()){
            if(!m.attached && !b.pending && (!s.opening || now-s.opening_ms>=1000)){bench_.activate(n);s.opening=true;s.opening_ms=now;s.failure_revision=b.initialization_failure_revision;}
            if(ready && !b.pending){s.home_requested=true;s.automatic=true;}
        }
        if(s.home_requested && !b.manual_page){
            if(const auto error=bench_.initialization_failure_since(n,s.failure_revision)){hard_fault("Motor initialization failed: "+error->reason);continue;}
            if(!ready || b.pending){
                if(!s.opening){bench_.activate(n);s.opening=true;s.opening_ms=now;}
                if(now-s.opening_ms>=5000){hard_fault("Motor initialization deadline exceeded: "+b.status);continue;}
                v.state=HomingState::waiting;v.status="Waiting for calibrated sensor and configured motor";continue;
            }
            std::string error;if(!s.claimed && !bench_.claim_mapping(n,error)){v.status=error;continue;}
            s.claimed=true;s.latched=false;s.auto_attempted=true;s.home_requested=s.opening=false;s.await_rearm=s.inactive_seen=false;s.disconnect_delivery.clear();s.owner=MotorOwner::homing;s.finishing=s.home_hold=false;s.command=s.limits={};s.limit_velocity=s.limit_acceleration=0;s.map={};v.homed=false;s.response.reset();s.edge=0;s.correction.reset();s.correction_hold=s.goal_known=s.force_correction=s.physical_hold=s.correction_moved=s.limit_hold=false;s.arrival_after_ms=s.arrival_after_sequence=0;s.disconnect_window.reset();s.tracking={};
            s.motor_generation=m.generation;s.coordinate=m.coordinate_revision;s.sensor_generation=feedback.generation;s.calibration=feedback.calibration_revision;s.source_identity=feedback.source_identity;
            s.identity=direction_key(d.address,feedback.calibration_revision,feedback.source_identity,m.capabilities);
            s.command_hold=s.initial_requested=s.reversal_hold=false;s.movement_direction=0;
            s.rejection={};s.rejection_kind=0;
            s.home.begin({feedback.raw_low,feedback.raw_high,feedback.code_count,0,d.sensor_buffer.value_or(0),d.backlash},{now,raw,m.position,*d.maximum_velocity,*d.maximum_acceleration,m.drive==MotorDriveState::off,false});
            s.release=bench_.mapping_stop(n,true);v.state=HomingState::probe;v.phase=MotorFollowingPhase::waiting;
        }
        if(s.map.valid)s.edge=motor_sensor_edge(s.map.sensor,raw,s.edge);
        MotorGuardSnapshot guard;guard.required=feedback.calibrated;guard.valid=feedback.valid;guard.mapped=v.homed;guard.owner=s.owner;guard.fraction=feedback.fraction;guard.resolution=feedback.resolution;guard.motor_generation=m.generation;guard.sensor_generation=feedback.generation;guard.calibration_revision=feedback.calibration_revision;guard.coordinate_revision=m.coordinate_revision;guard.identity=fingerprint(s.identity);guard.polarity=s.map.valid?s.map.polarity():0;guard.edge=s.map.valid?s.edge:2;guard.jitter=s.map.valid?s.map.sensor.jitter/s.map.sensor.span():0;if(s.map.valid)guard.fraction=s.map.sensor.units(raw)/s.map.sensor.span();bench_.guard(n,guard);
        auto trace=[&]{v.pending_command=s.command.id;v.pending_limits=s.limits.id;v.pending_release=s.release.id;v.correction_service_ms=s.correction.active() && now>=s.correction.serviced_ms()?now-s.correction.serviced_ms():0;v.target_anchor=s.episode_anchor;v.arrival_band=s.map.valid?s.map.sensor.jitter+s.map.sensor.buffer():0;v.endpoint_edge=s.edge;MotorFollowingTrace t;t.retarget_kind=v.retarget_kind;t.stop_reason=v.stop_reason;t.drive_event=v.drive_event;t.pending_command=v.pending_command;t.pending_limits=v.pending_limits;t.pending_release=v.pending_release;t.correction_service_ms=v.correction_service_ms;t.episode_anchor=s.episode_anchor;t.target_change=v.target_change;t.edge=s.edge;t.request_direction=v.request_direction;t.ms=now;t.motor=n;t.phase=unsigned(v.phase);t.generation=m.generation;t.sensor_generation=feedback.generation;t.coordinate=m.coordinate_revision;t.calibration=feedback.calibration_revision;t.encoder=m.position;t.raw=raw;t.raw_low=feedback.raw_low;t.raw_high=feedback.raw_high;t.sensor=feedback.fraction;t.desired=v.demand;t.correction=v.correction;t.accepted=s.accepted;t.accepted_id=s.accepted_id;t.accepted_ms=s.accepted_ms;t.accepted_raw=s.accepted_raw;t.accepted_velocity=s.accepted_velocity;t.accepted_acceleration=s.accepted_acceleration;t.nominal_target=s.nominal_target;t.compensated_target=s.episode_target;t.submitted_target=s.target;t.correcting=s.correction.active();t.encoder_attained=m.target_attained && m.target_command_id==s.accepted_id;t.expected_sequence=m.expected_sequence;t.expected_ms=m.expected_observed_ms;t.expected=m.expected_position.value_or(m.position);t.velocity=m.applied.velocity_limit;t.acceleration=m.applied.acceleration;t.measured_velocity=s.measured_velocity;t.response_ms=static_cast<unsigned>(std::min<std::uint64_t>(s.response.response_age(now),std::numeric_limits<unsigned>::max()));t.sensor_ms=feedback.sample_ms;t.sensor_sequence=feedback.sample_sequence;t.arrival_after_ms=s.arrival_after_ms;t.arrival_after_sequence=s.arrival_after_sequence;t.tracking_started=s.tracking.started;t.tracking_elapsed=s.tracking.elapsed;t.tracking_range=s.tracking.range;t.tracking_error=s.tracking.error;t.tracking_origin=s.tracking.origin;t.tracking_progress=s.tracking.progress;t.tracking_direction=s.tracking.direction;t.tracking_active=s.tracking.active;t.physical_hold=s.physical_hold;t.encoder_ms=m.observed_ms;t.drive=int(m.drive);t.valid=ready;t.sdk_code=m.fault_code;t.submissions=m.command_submissions;t.coalesced=m.command_coalesced;t.overflows=m.command_overflows;t.expected_event=m.expected_event_valid;t.codes=feedback.code_count;t.buffer=s.map.valid?s.map.sensor.buffer():s.home.sensor().buffer();t.jitter=s.map.valid?s.map.sensor.jitter:s.home.sensor().jitter;t.demand_fraction=demand_fraction;t.demand_sequence=ios?frame.ios_operation:frame.target_sequence[master];t.demand_valid=ios?frame.ios_drive:frame.target_valid[master];t.first_count=s.map.first_count;t.first_raw=s.map.first_raw;t.second_count=s.map.second_count;t.second_raw=s.map.second_raw;t.map_valid=s.map.valid;t.software_position=m.capabilities.software_position;t.reference=m.reference_position;t.reference_velocity=m.reference_velocity;t.reference_acceleration=m.reference_acceleration;t.requested_drive=m.requested_drive;t.accepted_drive=m.accepted_drive;t.reported_drive=m.reported_drive.value_or(0);t.reported_drive_valid=m.reported_drive.has_value();following_log_->sample(t,v.status,v.major_fault || s.await_rearm);};
        bool target_accepted{},target_limited{};
        auto cancel_motion=[&]{if((m.engaged || m.drive!=MotorDriveState::off) && !s.release.id){s.release=bench_.mapping_stop(n,true);v.drive_event=2;}s.command=s.limits={};s.command_target=s.command_hold=s.initial_requested=s.reversal_hold=s.home_hold=false;s.movement_direction=0;s.limit_velocity=s.limit_acceleration=0;s.accepted_ms=0;s.accepted_movement=false;s.response.reset();s.arrival_after_ms=s.arrival_after_sequence=0;};
        auto holding_stop=[&](unsigned reason){s.command_raw=s.goal_raw;s.command_episode=s.episode;s.command=bench_.mapping_stop(n,false);s.command_target=false;s.command_hold=s.correction_hold=true;v.stop_reason=reason;v.drive_event=1;};
        auto release=[&]{cancel_motion();s.correction.reset();s.correction_hold=s.goal_known=s.force_correction=s.physical_hold=s.correction_moved=s.limit_hold=false;s.disconnect_window.reset();};
        if(master<4 && (s.ios_owned!=ios || (ios && s.ios_drive!=frame.ios_drive))){
            // Cancel the prior authority even when its queued engage is not yet
            // reflected in the snapshot. Never let it overtake this handover.
            release();if(!s.release.id)s.release=bench_.mapping_stop(n,true);
            s.ios_owned=ios;s.ios_drive=ios && frame.ios_drive;s.ios_handover=true;s.ios_operation=0;s.last_feedback=-100000;s.feedback_ms=0;
        }
        // An endpoint suspends drive, not the Home or movement episode. Discard
        // cancelled receipt/learning state; resume through bounded correction.
        auto limit_pause=[&]{cancel_motion();s.correction.reset();s.correction_hold=s.physical_hold=s.correction_moved=false;s.force_correction=s.limit_hold=true;s.disconnect_window.reset();s.tracking={};};
        auto rejected=[&](MotorCommandResult& pending,const MotorCommandResult& result,bool is_target){
            const bool continuity=ready && m.generation==s.motor_generation && m.coordinate_revision==s.coordinate && feedback.generation==s.sensor_generation && feedback.calibration_revision==s.calibration && feedback.source_identity==s.source_identity && m.state!=MotorState::fault && m.drive!=MotorDriveState::unknown && b.mapping_owned;
            const bool invalid=result.error==MotorError::invalid_argument || result.error==MotorError::unsupported;
            const bool temporary=result.error==MotorError::busy || result.error==MotorError::not_ready;
            if(s.owner==MotorOwner::following && s.map.valid && continuity && (invalid || temporary)){
                s.rejection=result;s.rejection_kind=temporary?4:&pending==&s.limits?2:is_target?1:3;
                s.rejected_target=s.target;s.rejected_velocity=*d.maximum_velocity;s.rejected_acceleration=*d.maximum_acceleration;s.rejected_configuration=m.configuration_revision;s.retry_ms_=now+250;
                pending={};return -2;
            }
            hard_fault("Motor operation failed: "+result.reason,!continuity);return -1;
        };
        auto acknowledge=[&](MotorCommandResult& pending,bool is_target){
            if(!pending.id){if(pending.stage==MotorResultStage::rejected && !pending.reason.empty())return rejected(pending,pending,is_target);return 1;}
            const auto result=bench_.mapping_result(n,pending.id);if(!result || result->stage==MotorResultStage::queued)return 0;
            if(result->stage==MotorResultStage::cancelled){if(&pending==&s.limits)s.limit_velocity=s.limit_acceleration=0;pending={};return 1;}
            if(result->stage==MotorResultStage::limited && s.owner!=MotorOwner::homing){pending={};target_limited=true;return 0;}
            if(!*result)return rejected(pending,*result,is_target);
            // Result publication and the retained UI/bench snapshot are separate.
            // Consume engagement/Stop only when the snapshot confirms its state;
            // otherwise a target can overtake the release which just completed.
            if(&pending==&s.command && !is_target &&
                (s.command_hold ? (m.engaged && m.state!=MotorState::holding) : (!m.engaged || m.drive!=MotorDriveState::on)))return 0;
            if(&pending==&s.limits){if(m.applied.velocity_limit!=s.limit_velocity || m.applied.acceleration!=s.limit_acceleration)return 0;s.accepted_velocity=m.applied.velocity_limit;s.accepted_acceleration=m.applied.acceleration;}
            if(&pending==&s.command && !is_target && s.command_hold){
                s.accepted_movement=false;
                s.accepted=m.target;s.accepted_id=pending.id;s.accepted_ms=now;s.accepted_raw=s.command_raw;s.accepted_episode=s.command_episode;
                s.accepted_velocity=m.applied.velocity_limit;s.accepted_acceleration=m.applied.acceleration;
                v.drive_event=6;
            }else if(&pending==&s.command && !is_target)v.drive_event=4;
            if(is_target){if(m.target_command_id!=pending.id)return 0;s.accepted_movement=s.owner==MotorOwner::following;s.accepted=m.target;s.accepted_id=pending.id;s.accepted_ms=m.target_accepted_ms?m.target_accepted_ms:now;s.accepted_raw=s.command_raw;s.accepted_episode=s.command_episode;s.accepted_velocity=m.target_velocity>0?m.target_velocity:m.applied.velocity_limit;s.accepted_acceleration=m.target_acceleration>0?m.target_acceleration:m.applied.acceleration;target_accepted=true;if(s.owner==MotorOwner::following){s.arrival_after_ms=s.accepted_ms;s.arrival_after_sequence=s.command_sensor_sequence;if(!s.correction.active()){s.response.reset();s.response.prepare(raw,m.position,s.measured_velocity);}}}pending={};return 1;
        };
        const int command_ready=acknowledge(s.command,s.command_target),limits_ready=acknowledge(s.limits,false);s.command_target=s.command.id?s.command_target:false;
        if(!s.command.id)s.command_hold=false;
        if(command_ready==-2 || limits_ready==-2){release();v.phase=MotorFollowingPhase::waiting;v.status="Motor request rejected; homing retained: "+s.rejection.reason;trace();continue;}
        if(command_ready<0 || limits_ready<0){trace();continue;}
        auto limits=[&](double velocity,double acceleration){velocity=std::max(m.capabilities.minimum_velocity,velocity);if(s.limits.id)return false;if(s.limit_velocity==velocity && s.limit_acceleration==acceleration)return limits_ready>0;s.limit_velocity=velocity;s.limit_acceleration=acceleration;s.limits=bench_.mapping_limits(n,velocity,acceleration);return false;};
        auto home_observation=[&](bool drive_off,bool accepted,bool stopped){
            HomeObservation observation{now,raw,m.position,*d.maximum_velocity,*d.maximum_acceleration,drive_off,accepted,stopped};
            observation.motion=HomeAcceptedMotion{m.target,m.applied.velocity_limit,m.applied.acceleration,s.measured_velocity,m.target_revision_valid?m.target_accepted_ms:0};
            if(m.expected_event_valid && m.expected_position && m.expected_generation==m.generation && m.expected_coordinate==m.coordinate_revision && m.expected_observed_ms>m.target_accepted_ms && now>=m.expected_observed_ms && now-m.expected_observed_ms<=2*m.applied.feedback_interval_ms){observation.motion->expected=m.expected_position;observation.motion->expected_ms=m.expected_observed_ms;}
            return observation;
        };
        // Suppression covers the complete Home ownership interval, including
        // the initial pending release and final release acknowledgement.
        if(s.owner==MotorOwner::homing){if(master<4)routing.suppress_throttles|=mask;else if(master==4)routing.suppress_speedbrake=true;}
        if(s.release.id){const auto result=bench_.mapping_result(n,s.release.id);if(result && result->stage!=MotorResultStage::queued && !*result){hard_fault("Drive release failed: "+result->reason,false);continue;}
            if(result && result->stage!=MotorResultStage::queued && m.drive==MotorDriveState::off && !m.engaged){s.release={};v.drive_event=3;}
            if(s.owner==MotorOwner::homing && s.home.phase()==HomePhase::jitter)
                (void)s.home.step(home_observation(false,false,false),master<=4?0:.5,master<4);
            // A successful receipt does not make the earlier snapshot current.
            // No engage/target may overlap an outstanding drive release.
            trace();continue;
        }
        if(s.ios_handover){if(m.engaged || m.drive!=MotorDriveState::off){v.status="Waiting for thrust authority drive release";trace();continue;}s.ios_handover=false;}
        if(ios)s.ios_operation=frame.ios_operation;
        if(target_limited){limit_pause();v.phase=MotorFollowingPhase::limit;v.status="Limit hold; outward movement blocked; homing retained";trace();continue;}
        if(s.owner==MotorOwner::homing){
            const auto action=s.home.step(home_observation(m.drive==MotorDriveState::off,target_accepted,s.home_hold && !s.command.id),master<=4?0:.5,master<4);
            const auto& contract=s.home.sensor();MotorHomingSample sample;sample.stamp_ms=now;sample.raw=raw;sample.raw_low=feedback.raw_low;sample.raw_high=feedback.raw_high;sample.raw_valid=sample.sensor_valid=feedback.valid;sample.motor_valid=m.feedback_valid;sample.position=m.position;sample.target=m.target;sample.sensor=feedback.fraction;sample.resolution=feedback.resolution;sample.speed=action.direction*m.applied.velocity_limit;sample.requested_velocity=action.direction*action.speed_fraction**d.maximum_velocity;sample.acceleration=m.applied.acceleration;sample.polarity=s.home.polarity();sample.endpoint_band=(contract.jitter+contract.buffer())/contract.span();sample.measured_velocity=s.measured_velocity;sample.command_status=int(s.command.id?s.command.stage:m.last_result.stage);sample.response_limit=0;sample.engaged=m.engaged;sample.motor_generation=m.generation;sample.sensor_generation=feedback.generation;sample.calibration_revision=feedback.calibration_revision;sample.coordinate_revision=m.coordinate_revision;sample.configuration_revision=m.configuration_revision;sample.drive_state=int(m.drive);sample.codes=contract.codes;sample.buffer=contract.buffer();sample.jitter=contract.jitter;sample.probe_excursion=s.home.probe_excursion(raw);
            static constexpr const char* phases[]{"idle","jitter","probe","fwd","correct","capture","back","park","stopped","error"};
            static constexpr const char* descriptions[]{"Waiting to home","Sampling stationary sensor jitter for two seconds","Probing positive motor direction and reversing if necessary","Traversing to the first sensor endpoint","Correcting an endpoint crossing inward at five percent speed","Capturing the stationary motor-coordinate and sensor pair","Traversing to the opposite sensor endpoint","Parking at the function resting position","Session homing and parking are complete and motor disengagement is requested","Homing failed"};
            if(!s.log_started){homing_log_->begin(n,master,d.address.device,sample);s.log_started=true;}
            sample.motor_age_ms=now>=m.observed_ms?now-m.observed_ms:0;sample.sensor_age_ms=now>=feedback.sample_ms?now-feedback.sample_ms:0;
            homing_log_->update(n,sample,phases[unsigned(action.phase)],action.error.empty()?descriptions[unsigned(action.phase)]:action.error,!action.error.empty());
            v.status=descriptions[unsigned(action.phase)];v.state=action.phase==HomePhase::first?HomingState::seeking_first:action.phase==HomePhase::second?HomingState::seeking_second:action.phase==HomePhase::park || action.phase==HomePhase::complete?HomingState::parking:action.phase==HomePhase::capture?HomingState::settling_first:HomingState::probe;
            if(!action.error.empty()){hard_fault(action.error);continue;}
            if(action.complete){
                release();s.map=s.home.map();s.finishing=true;
                if(m.drive==MotorDriveState::off && !m.engaged && !s.release.id){
                    v.homed=s.map.valid;s.owner=MotorOwner::following;s.finishing=false;
                    v.encoder_low=s.map.target(feedback.raw_low);v.encoder_high=s.map.target(feedback.raw_high);
                    s.alignment_identity=alignment_key(d,feedback,m);
                    if(!s.alignment_identity.empty() && alignments_[n].identity!=s.alignment_identity){
                        if(!alignments_[n].identity.empty())alignment_warning_="Motor "+std::to_string(n)+" alignment identity changed; previous compensation ignored";
                        alignments_[n]={};alignments_[n].identity=s.alignment_identity;
                    }
                    v.major_fault=v.reset_pending=v.reset_failed=false;v.cause.clear();v.state=HomingState::following;
                    v.status=frame.psx_ready?"Homing complete; waiting for PSX authority":"Homing complete, waiting for PSX";
                    homing_log_->finish(n,"Session homing and parking are complete; drive release confirmed",false);
                    auto& definition=definitions_[master];definition.learned_direction=s.map.polarity();definition.direction_key=s.identity;++definition_revision_;queue_direction_save();
                }
                trace();continue;
            }
            if(action.release){release();continue;}
            if(action.hold){if(!s.home_hold){s.home_hold=true;s.command=bench_.mapping_stop(n,false);s.command_target=false;s.command_hold=true;}continue;}
            if(!action.move || !command_ready || !limits_ready)continue;
            if(!limits(*d.maximum_velocity*action.speed_fraction,*d.maximum_acceleration))continue;
            if(!m.engaged){s.home_hold=false;s.command=bench_.mapping_engage(n);s.command_target=false;continue;}
            if(now-s.sent_ms>=100){s.home_hold=false;s.target=action.target;s.command=bench_.mapping_target(n,s.target);s.command_target=true;s.sent_ms=now;}continue;
        }
        if(!v.homed || s.latched){s.disconnect_window.reset();s.tracking={};trace();continue;}
        if(!ios && s.await_rearm){if(active && s.inactive_seen){s.await_rearm=s.inactive_seen=false;s.disconnect_delivery.clear();s.response.reset();s.accepted=m.position;s.command={};s.disconnect_window.reset();s.tracking={};v.cause.clear();}else {release();v.phase=MotorFollowingPhase::waiting;v.status=(master<4?(s.inactive_seen?std::string("Waiting for A/T re-engagement"):s.disconnect_delivery.empty()?std::string("A/T disconnect pending"):s.disconnect_delivery):std::string("Waiting for PSX output reactivation or IOS reset"))+"; "+v.cause;trace();continue;}}
        if(!active){release();s.tracking={};v.phase=MotorFollowingPhase::waiting;v.status=ios?"IOS thrust hold paused; drive released":!frame.psx_ready?"Homing complete, waiting for PSX":master<4 && frame.authority==AtAuthority::physical?"Manual / HOLD; drive released":master<4?"Waiting for valid PSX A/T mode and forward target; drive released":"PSX target or power unavailable; drive released";trace();continue;}
        s.map.sensor.configured_buffer=d.sensor_buffer.value_or(0);s.map.sensor.backlash=d.backlash;
        if(ios)s.ios_operation=frame.ios_operation;
        const auto& contract=s.map.sensor;const double wanted=contract.low+std::clamp(demand_fraction,0.,1.)*(contract.high-contract.low);v.demand=wanted;
        const double counts_per_raw=(s.map.second_count-s.map.first_count)/(s.map.second_raw-s.map.first_raw),scale=1/counts_per_raw;
        const double band=contract.jitter+contract.buffer();const bool inside=std::abs(wanted-raw)<=band;
        const bool changed_goal=!s.goal_known || wanted!=s.goal_raw || s.goal_psx_generation!=demand_generation;
        const bool new_goal=changed_goal && (!s.goal_known || s.goal_psx_generation!=demand_generation || std::abs(wanted-s.episode_anchor)>band);
        v.target_change=new_goal?1:changed_goal?2:0;
        const double required=(wanted-raw)*counts_per_raw;
        const int direction=inside?0:required>0?1:-1;
        const bool reversal=direction && s.movement_direction && direction!=s.movement_direction && (m.engaged || s.command.id);
        if(new_goal){
            ++s.episode;
            s.correction_note.clear();s.goal_known=true;s.goal_raw=s.episode_anchor=wanted;s.goal_psx_generation=demand_generation;
            s.physical_hold=s.physical_hold && inside;s.nominal_target=s.map.target(wanted);s.force_correction=false;
            if(inside){s.episode_target=m.target;s.episode_offset=m.target-s.nominal_target;}
            else {
                const auto& alignment=alignments_[n];
                s.episode_offset=!s.alignment_identity.empty() && alignment.identity==s.alignment_identity?
                    (m.capabilities.software_position?alignment.lookup(wanted,direction):motor_alignment_backlash(alignment,s.map,wanted,direction,d.backlash)):
                    (m.capabilities.software_position?0:motor_backlash_offset(s.map,wanted,direction,d.backlash));
                s.episode_target=s.nominal_target+s.episode_offset;
                if((s.episode_target-m.position)*direction<=0){s.episode_offset=0;s.episode_target=s.nominal_target;s.force_correction=true;}
            }
            s.initial_requested=!inside && !s.force_correction;
            if(inside || s.force_correction){
                if(s.correction.active())s.correction.retarget(wanted,band);
                v.retarget_kind=inside?5:3;
            }else {
                // Same-direction demand ramps replace their destination, not
                // the drive with a new holding Stop. Keep any SDK receipt and
                // its accepted physical destination until it is consumed.
                s.correction.reset();s.correction_hold=false;v.retarget_kind=2;
            }
        }else if(changed_goal){
            // Keep the exact new demand, but neither alternate compensation
            // flanks nor restart the current correction/arrival handoff.
            s.goal_raw=wanted;s.nominal_target=s.map.target(wanted);
            if(s.correction.active())s.correction.retarget(wanted,band);
            else if(!s.physical_hold && !s.force_correction && !s.limit_hold)s.episode_target=s.nominal_target+s.episode_offset;
            v.retarget_kind=1;
        }
        if(reversal && !s.reversal_hold){
            // A physical direction reversal is the one target-change case
            // requiring cancellation. Further coalesced updates cannot replace
            // this holding receipt or restart its wait.
            s.correction.reset();s.correction_note.clear();holding_stop(2);s.reversal_hold=true;v.retarget_kind=4;
            if(!new_goal){s.force_correction=true;s.initial_requested=false;}
        }
        if(direction)s.movement_direction=direction;
        // Re-check even a within-band updated initial estimate against the
        // current encoder. Physical error, not a stale absolute conversion,
        // determines direction after shaft/load disagreement or overshoot.
        if(!inside && !s.correction.active() && (s.episode_target-m.position)*direction<=0){s.force_correction=true;s.initial_requested=false;}
        if(s.rejection_kind){
            const bool same_settings=s.rejected_configuration==m.configuration_revision && s.rejected_velocity==*d.maximum_velocity && s.rejected_acceleration==*d.maximum_acceleration;
            const bool same_request=s.rejection_kind==4?now<s.retry_ms_:same_settings && (s.rejection_kind!=1 || s.episode_target==s.rejected_target);
            if(same_request){release();v.phase=MotorFollowingPhase::waiting;v.status="Motor request rejected; homing retained: "+s.rejection.reason;trace();continue;}
            s.rejection={};s.rejection_kind=0;
        }
        if(!s.limit_hold && !m.engaged && m.fault_kind==MotorFaultKind::soft_limit && (s.correction.active() || s.physical_hold || s.accepted_ms)){limit_pause();v.phase=MotorFollowingPhase::limit;v.status="Limit hold; controller blocked outward movement; homing retained";trace();continue;}
        auto checked_target=[&](double candidate){const double direction=(candidate-m.position)*guard.polarity;v.request_direction=direction>0?1:direction<0?-1:0;return motor_guard_target(guard,m.position,candidate);};
        // An outward absolute estimate falls back to actual encoder-relative
        // correction. The same endpoint cutout still checks each corrective step.
        if(!inside && !s.correction.active() && checked_target(s.episode_target).decision==MotorMotionDecision::limit)s.force_correction=true;
        const bool slow=s.correction.active() || s.force_correction || s.physical_hold;
        const double candidate=slow?m.position+std::clamp((wanted-raw)*counts_per_raw,-.02**d.maximum_velocity,.02**d.maximum_velocity):s.episode_target;
        if((s.limit_hold && inside) || (!inside && checked_target(candidate).decision==MotorMotionDecision::limit)){
            if(!s.limit_hold)limit_pause();
            s.disconnect_window.reset();s.tracking={};v.phase=MotorFollowingPhase::limit;v.status="Limit hold; waiting for permitted inward movement";trace();continue;
        }
        // Valid physical feedback and the current candidate, not an A/T-on
        // level alone, authorize resumption after the confirmed release above.
        if(s.limit_hold)s.limit_hold=false;
        if(master<4 && !ios){
            s.tracking=s.disconnect_window.observe(now,raw,wanted,contract.span());
            if(s.tracking.disconnect){
                const auto& t=s.tracking;
                const std::string reason="Physical tracking failure: error="+std::to_string(t.error)+" ("+std::to_string(100*t.error/t.range)+"%), progress="+std::to_string(t.progress)+" ("+std::to_string(100*t.progress/t.range)+"%), elapsed="+std::to_string(t.elapsed)+" ms, range="+std::to_string(t.range)+", raw="+std::to_string(raw)+", target="+std::to_string(wanted)+", origin="+std::to_string(t.origin);
                disconnect();routing.fault_mask|=1u<<n;routing.fault_reasons[n]=reason;s.await_rearm=true;s.inactive_seen=false;
                s.rearm_psx_generation=frame.psx_generation;s.rearm_inactive_sequence=frame.at_inactive_sequence;
                v.cause=reason;++v.fault_revision;v.major_fault=false;release();v.phase=MotorFollowingPhase::waiting;v.status="A/T disconnect pending; "+reason;trace();continue;
            }
        }else {s.disconnect_window.reset();s.tracking={};}
        if(s.reversal_hold){if(s.command.id){v.phase=MotorFollowingPhase::waiting;v.status="Direction reversal; holding Stop acknowledgement pending";trace();continue;}s.reversal_hold=false;}
        if(!m.engaged){
            s.physical_hold=false;
            if(command_ready && limits_ready && !s.command.id && !s.limits.id && limits((slow?.2:1.)**d.maximum_velocity,*d.maximum_acceleration)){s.command=bench_.mapping_engage(n);s.command_target=s.command_hold=false;}
            v.phase=MotorFollowingPhase::waiting;v.status="Engaging at measured position";trace();continue;
        }
        auto begin_correction=[&]{
            const double movement=(s.goal_raw-raw)*counts_per_raw;const int direction=movement>0?1:movement<0?-1:0;
            s.correction.begin(s.goal_raw,now,direction);s.correction_moved=false;s.force_correction=s.physical_hold=false;
            s.response.reset();s.correction_note.clear();s.initial_requested=false;
            if(!s.correction_hold)holding_stop(inside?3:1);
        };
        if(!s.correction.active()){
            if(inside && !s.physical_hold)begin_correction();
            else if(!inside && (s.physical_hold || s.force_correction))begin_correction();
            else if(!inside && s.accepted_ms && s.accepted_movement && !s.correction_hold){
                std::optional<double> expected;
                if(m.expected_event_valid && m.expected_position && m.target_command_id==s.accepted_id && m.expected_generation==m.generation && m.expected_coordinate==m.coordinate_revision && m.expected_observed_ms>s.accepted_ms && now>=m.expected_observed_ms && now-m.expected_observed_ms<=2*m.applied.feedback_interval_ms)expected=m.expected_position;
                const auto result=s.response.observe(now,contract,raw,m.position,s.accepted,s.goal_raw,scale,s.accepted_velocity,s.accepted_acceleration,expected,expected?m.expected_observed_ms:0,m.target_attained && m.target_command_id==s.accepted_id);
                if(!s.initial_requested && s.accepted_episode==s.episode &&
                    (result.kind==MotorResponseKind::missed_arrival || result.kind==MotorResponseKind::nonresponse)){begin_correction();s.correction_note=result.reason;}
            }
        }
        if(s.correction.active()){
            v.phase=MotorFollowingPhase::correcting;v.status=inside?"Settling physical position":s.correction_note.empty()?"Correcting physical position at 20% maximum velocity":"Correcting: "+s.correction_note;v.correction=m.position-s.nominal_target;
            const double velocity=.2**d.maximum_velocity;const bool valid_velocity=velocity>=m.capabilities.minimum_velocity;
            if(!valid_velocity && !inside)v.status="Correction velocity below controller minimum; waiting for valid limits";
            const bool movement_ready=!inside && valid_velocity && command_ready && limits_ready && limits(velocity,*d.maximum_acceleration);
            const bool attained=m.target_attained && m.target_command_id==s.accepted_id;
            const auto action=s.correction.step({now,raw,m.position,counts_per_raw,velocity,std::abs(m.applied.rescale),band,
                movement_ready && !s.command.id && !s.limits.id,attained,s.correction_hold && !s.command.id && m.state==MotorState::holding,
                feedback.sample_ms,feedback.sample_sequence,s.arrival_after_ms,s.arrival_after_sequence});
            if(movement_ready && !action.move && std::abs((s.goal_raw-raw)*counts_per_raw)<std::abs(m.applied.rescale))v.status="Correcting; physical error is below the motor position resolution";
            if(action.hold){
                if(!s.correction_hold){s.home_hold=false;holding_stop(3);}
            }else if(action.move){
                if(checked_target(action.target).decision==MotorMotionDecision::limit){limit_pause();v.phase=MotorFollowingPhase::limit;v.status="Limit hold; outward correction blocked; homing retained";trace();continue;}
                s.correction_hold=false;s.correction_moved=true;s.target=action.target;s.command_raw=s.goal_raw;s.command_sensor_sequence=feedback.sample_sequence;
                s.command_episode=s.episode;s.command=bench_.mapping_target(n,s.target);s.command_target=true;s.command_hold=false;
            }else if(action.complete){
                auto& alignment=alignments_[n];
                if(action.learn && s.correction_moved && !s.alignment_identity.empty() && alignment.identity==s.alignment_identity && action.direction){
                    alignment.learn(action.learn_raw,action.learn_encoder-s.map.target(action.learn_raw),action.direction,contract.buffer());alignment_save_.changed(now);alignment_warning_.clear();
                }
                s.episode_target=s.accepted=m.target;s.episode_offset=m.target-s.nominal_target;
                s.correction_hold=false;s.physical_hold=true;s.response.reset();v.correction=s.episode_offset;
                v.phase=MotorFollowingPhase::following;v.status=ios?"Following IOS thrust; physical target held":"Following PSX; physical target held";s.correction_note.clear();
                v.drive_event=5;s.movement_direction=0;
            }
            if(master==4)routing.suppress_speedbrake=true;trace();continue;
        }
        if(inside && s.physical_hold){v.phase=MotorFollowingPhase::following;v.status=ios?"Following IOS thrust; physical target held":"Following PSX; physical target held";if(master==4)routing.suppress_speedbrake=true;trace();continue;}
        if(!command_ready || !limits_ready){trace();continue;}
        s.correction_hold=false;
        if(!limits(*d.maximum_velocity,*d.maximum_acceleration)){trace();continue;}
        if(!s.command.id && (!s.accepted_ms || s.accepted_raw!=s.goal_raw || std::abs(s.episode_target-s.accepted)>=.5*std::abs(m.applied.rescale))){
            s.response.prepare(raw,m.position);s.target=s.episode_target;s.command_raw=s.goal_raw;s.command_sensor_sequence=feedback.sample_sequence;
            s.command_episode=s.episode;s.command=bench_.mapping_target(n,s.target);s.command_target=true;s.command_hold=false;
        }
        s.initial_requested=false;
        v.phase=MotorFollowingPhase::following;v.status=ios?"Following IOS thrust; initial mapped position move":"Following PSX; initial mapped position move";v.correction=s.episode_offset;if(master==4)routing.suppress_speedbrake=true;trace();
    }return routing;
}
}
