#include "hwio/input_provider.hpp"
#include "hwio/fdstq_motion.hpp"
#include "hwio/catalogue.hpp"
#include "hwio/runtime.hpp"
#include "hwio/vendor_library.hpp"
#include "hwio/flightillusion_bridge.hpp"
#include "hwio/flightillusion_exporter.hpp"
#include "hwio/actuator_repository.hpp"
#include "hwio/fds_replay.hpp"
#include "hwio/failures.hpp"
#include "hwio/motor_pipe.hpp"
#include "hwio/actuator_control.hpp"
#include "hwio/thrust_control.hpp"
#include "hwio/situation_controls.hpp"
#include "hwio/motor_disconnect.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <set>
#include <cctype>
#include <tuple>

namespace hwio {
namespace {
template<class Packet>bool motor_wire_valid(const Packet& p){if constexpr(sizeof(Packet)==4096)return psx::hwio::motors::v2::valid(p);else return psx::hwio::motors::valid(p);}
template<class To,class From>To motor_packet(const From& from){To to;to.operation=from.operation;to.result=from.result;to.request_id=from.request_id;to.session=from.session;to.fault_revision=from.fault_revision;to.operation_id=from.operation_id;to.revision=from.revision;to.motor=from.motor;to.port=from.port;to.flags=from.flags;to.host=from.host;to.message=from.message;std::copy_n(from.motors.begin(),std::min(to.motors.size(),from.motors.size()),to.motors.begin());return to;}
std::vector<std::string> disabled_fds(const HardwareConfiguration& configuration){std::vector<std::string> result;for(const auto& card:configuration.devices)if(!card.enabled && (card.backend==BackendKind::fds_usb || card.backend==BackendKind::fds_ethernet))result.push_back(card.key);return result;}
bool fdstq_owns_axis(unsigned i,const FdstqConfiguration& tq){return tq.enabled && ((i<4 && (tq.paired?tq.steppers[i<2?0:1].enabled:i<2 && tq.steppers[i].enabled)) || (i==4 && tq.steppers[2].enabled) || (i>=9 && tq.trim_enabled));}
bool motor_mapping_conflicts(const MotorMappingDefinitions& mappings,const FdstqConfiguration& tq,std::string& error){
    if(!tq.enabled)return false;
    for(unsigned i=0;i<motor_axes;++i)if(mappings[i].enabled && fdstq_owns_axis(i,tq)){error="Motor axis is already owned by the FDSTQ assembly: "+utf8(motor_axis_name(MotorAxis(i)));return true;}return false;
}
}
Runtime::~Runtime() {
    stop();
    CloseHandle(change_);
}
MotorBench& Runtime::motor_bench(){
    if(!motor_bench_)motor_bench_=std::make_unique<MotorBench>(options_.data_dir,[this](const MotorDeviceAddress& a){return a.mode==MotorDriveMode::position?phidgets_.position_backend(a):phidgets_.software_backend(a,[this](const MotorFeedbackDefinition& d){return motor_feedback_.sample(d);});});
    motor_bench_->measurement_sources([this](const MotorFeedbackDefinition& d){return motor_feedback_.sample(d);},[this](const MotorDeviceAddress& a){return phidgets_.shaft_reader(a);});
    return *motor_bench_;
}
Steppers& Runtime::steppers(){if(!steppers_)steppers_=std::make_unique<Steppers>(options_.data_dir,[this](const StepperDefinition& d){return phidgets_.stepper_backend(d,[this](const MotorFeedbackDefinition& f){return motor_feedback_.sample(f);});});return *steppers_;}
Actuators& Runtime::actuators(){if(!actuators_)actuators_=std::make_unique<Actuators>(options_.data_dir,[this](ActuatorRef ref,std::string& error)->std::shared_ptr<PositionMotorBackend>{if(ref.kind==ActuatorKind::stepper){const auto address=steppers().snapshot(ref.slot).draft.address;const auto inventory=phidgets_.inventory();if(std::none_of(inventory.channels.begin(),inventory.channels.end(),[&](const auto& c){return c.channel_class==27 && c.address==address;})){error="Waiting for assigned stepper";return {};}return steppers().acquire(ref.slot,error);}if(ref.kind==ActuatorKind::servo)return actuator_servo_port(servos(),ref.slot,error);for(const auto& d:motor_mappings().definitions())if(d.enabled && d.motor==ref.slot){error="Flight-axis mapping owns this motor";return {};}return actuator_motor_port(motor_bench(),ref.slot,error);},[this](const MotorFeedbackDefinition& f){return motor_feedback_.sample(f);},[this](const MotorFeedbackDefinition& f,std::uint64_t sequence,MotorPositionSample* out,unsigned count,bool& lost){return motor_feedback_.events(f,sequence,out,count,lost);});return *actuators_;}
bool Runtime::assign_actuator(unsigned n,ActuatorMapping definition,std::string& error){std::lock_guard configuration(configuration_mutex_);if(definition.analogue_output!=UINT_MAX)for(const auto& d:motor_mappings().definitions())if(d.analogue_output==definition.analogue_output){error="Analogue output already contains a flight-axis assignment";return false;}if(definition.enabled && definition.output.kind==ActuatorKind::motor)for(const auto& d:motor_mappings().definitions())if(d.enabled && d.motor==definition.output.slot){error="Flight-axis mapping owns this motor; clear that assignment first";return false;}
    if(definition.enabled && definition.output.kind==ActuatorKind::stepper){const auto step=steppers().snapshot(definition.output.slot).draft;if(step.run_mode && (definition.calibration!=ActuatorCalibration::potentiometer || step.feedback!=definition.potentiometer)){error="RUN mode requires the same measured potentiometer as the stepper position loop";return false;}}
    if(definition.enabled && definition.output.kind==ActuatorKind::servo && definition.calibration==ActuatorCalibration::rotation){error="Endless RC rotation requires a qualified speed-servo and measured-rotation adapter";return false;}
    const bool ok=actuators().assign(n,std::move(definition),error);if(ok)SetEvent(change_);return ok;
}
bool Runtime::assign_motor_mappings(MotorMappingDefinitions definitions,std::string& error){std::lock_guard configuration_lock(configuration_mutex_);if(motor_mapping_conflicts(definitions,settings().fdstq,error))return false;for(const auto& gauge:actuators().definitions())for(const auto& axis:definitions){if(axis.analogue_output!=UINT_MAX && gauge.analogue_output==axis.analogue_output){error="Analogue output already contains an instrument assignment";return false;}if(gauge.enabled && gauge.output.kind==ActuatorKind::motor && axis.enabled && axis.motor==gauge.output.slot){error="Shared actuator mapping owns this motor";return false;}}const bool saved=motor_mappings().assign(std::move(definitions),error);if(saved)SetEvent(change_);return saved;}
void Runtime::configure_phidgets(const AppSettings& settings){
    std::vector<PhidgetServer> servers;
    auto add=[&](std::string host,unsigned port){if(host.empty())return;for(auto& c:host)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));PhidgetServer server{std::move(host),{},port};if(std::find(servers.begin(),servers.end(),server)==servers.end())servers.push_back(std::move(server));};
    if(settings.fdstq.remote)add(settings.fdstq.host,settings.fdstq.port);
    if(steppers_)for(const auto& d:steppers_->saved())if(d.address.assigned() && d.address.remote)add(d.address.server,d.address.port);
    if(servos_)for(const auto& d:servos_->saved())if(d.address.assigned() && d.address.remote)add(d.address.server,d.address.port);
    if(motor_bench_)for(unsigned n=0;n<motor_slots;++n){const auto definition=motor_bench_->snapshot(n).saved;if(definition.address.assigned() && definition.address.remote)add(definition.address.server,definition.address.port);if(definition.feedback.kind==MotorPositionSource::encoder && definition.feedback.encoder.assigned() && definition.feedback.encoder.remote)add(definition.feedback.encoder.server,definition.feedback.encoder.port);}
    std::sort(servers.begin(),servers.end(),[](const auto& a,const auto& b){return std::tie(a.host,a.port)<std::tie(b.host,b.port);});
    if(servers!=phidget_servers_){phidgets_.enable(false);phidgets_.servers(servers);phidget_servers_=std::move(servers);}
    phidgets_.enable(settings.phidgets_enabled);
}
void Runtime::read_profiles() {
    std::string error;
    const auto legacy = options_.data_dir == executable_directory() ? profile_directory() : std::wstring{};
    if (!prepare_configuration(options_.data_dir, legacy, error) ||
        !load_assignments(options_.data_dir + L"\\assignments.cfg", profiles_, display_assignments_, axes_, error, &buttons_, &hardware_,&cockpit_,&modules_))
        view_.problem = error;
}
void Runtime::start(RuntimeOptions options) {
    (void)active_catalogue();fdstq_.trace(&fds_.trace);
    fds_.trace.live(true);
    options_ = std::move(options);
    if (options_.data_dir.empty())
        options_.data_dir = executable_directory();
    module_library_=read_modules(std::filesystem::path(executable_directory())/L"fds-modules.xml");
    add_cpflight_profiles(module_library_);
    read_profiles();
    input_overrides_.load(options_.data_dir);
    settings_ = load_settings(options_.data_dir);
    PhidgetSession::default_library(settings_.phidget_api_path);
    trim_displays_.configure(settings_.trim_displays);
    trim_displays_.start();
    cpflight_.configure(settings_.cpflight);cpflight_.start();
    flightillusion_=std::make_unique<flightillusion::Provider>(flightillusion::file_store(std::filesystem::path(options_.data_dir)/L"flightillusion.cfg"));
    flightillusion_->refresh_ports();
    {std::string error;if(!prepare_actuator_repository(options_.data_dir,error))view_.problem="Actuator configuration: "+error;}
    (void)motor_bench(); // Definitions load without opening a physical channel.
    (void)servos(); // Saved indicators never open or engage merely on startup.
    motor_mappings_=std::make_unique<MotorMappings>(*motor_bench_,options_.data_dir);
    (void)steppers();(void)actuators();
    if(!settings_.provider_selection_initialized && (std::any_of(modules_.rows.begin(),modules_.rows.end(),[](const auto& row){return row.first.starts_with("fds-");}) || std::any_of(hardware_.devices.begin(),hardware_.devices.end(),[](const auto& d){return d.backend==BackendKind::fds_usb || d.backend==BackendKind::fds_ethernet;})))settings_.fds_enabled=true;
    if (!options_.endpoint_override) {
        const auto path = options_.data_dir + L"\\settings.cfg";
        wchar_t host[128]{};
        GetPrivateProfileStringW(L"main", L"host", L"127.0.0.1", host, 128, path.c_str());
        options_.host = utf8(host);
        const auto port = GetPrivateProfileIntW(L"main", L"port", default_main_port, path.c_str());
        if (port > 0 && port <= 65535)
            options_.port = static_cast<std::uint16_t>(port);
    }
    psx::diagnostics::Options diag;
    diag.directory =
        (options_.data_dir == executable_directory() ? profile_directory() : options_.data_dir) + L"\\logs";
    fds_.trace.fault_directory(std::filesystem::path(diag.directory)/L"fds-faults");
    diag.config_path = options_.data_dir + L"\\settings.cfg";
    diag.build = "psx-hwio " HWIO_VERSION;
    auto setting = [&](const wchar_t *section, const wchar_t *key, unsigned fallback, unsigned lo, unsigned hi) {
        wchar_t buffer[64]{};
        GetPrivateProfileStringW(section, key, L"", buffer, 64, diag.config_path.c_str());
        if (!buffer[0])
            return fallback;
        int v{};
        if (!integer(utf8(buffer), v) || v < 0 || static_cast<unsigned>(v) < lo || static_cast<unsigned>(v) > hi) {
            diag.config.valid = false;
            return fallback;
        }
        return static_cast<unsigned>(v);
    };
    diag.config.events_enabled = settings_.log_level != LogLevel::off;
    diag.config.minimum_severity =
        settings_.log_level == LogLevel::warn ? psx::diagnostics::Severity::warning : psx::diagnostics::Severity::info;
    diag.config.segment_bytes = setting(L"logging", L"segment_bytes", 8388608, 2048, 67108864);
    diag.config.retained_segments = setting(L"logging", L"retained_segments", 8, 2, 64);
    diag.config.flush_ms = setting(L"logging", L"flush_ms", 1000, 50, 5000);
    diag.config.repeat_ms = setting(L"logging", L"repeat_ms", 10000, 1000, 60000);
    diag.config.metrics_ms = setting(L"diagnostics", L"metrics_ms", 1000, 250, 10000);
    diag.config.status_ms = setting(L"diagnostics", L"status_ms", 250, 100, 1000);
    journal_.start(diag);
    journal_.producer(0, psx::diagnostics::Component::application).emit(psx::diagnostics::Code::process_start);
    if (!view_.problem.empty())
        journal_.producer(0, psx::diagnostics::Component::application)
            .emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::error, {},
                  psx::diagnostics::ErrorDomain::application, 0, view_.problem);
    ethernet_.configure(settings_.network_interface,hardware_,settings_.pokeys_enabled);
    ethernet_.start();
    fds_.enabled(settings_.fds_enabled);configure_phidgets(settings_);
    fds_.start(settings_.fds_api_path,settings_.network_interface,disabled_fds(hardware_));
    input_.start();
    transport_.start(options_.host, options_.port, options_.autoconnect,
                     journal_.producer(1, psx::diagnostics::Component::main));
    stop_ = false;
    motor_mappings(); // Construct retained services before UI/worker access.
    worker_ = std::thread(&Runtime::run, this);
}
void Runtime::stop() {
    fds_.trace.live(false);
    fdstq_.motion({});
    clear_output_tests();
    clear_module_tests();
    stop_ = true;
    SetEvent(change_);
    if (worker_.joinable())
        worker_.join();
    if(actuators_)actuators_->close();
    if(steppers_)steppers_->close();
    if(motor_mappings_)motor_mappings_->shutdown();
    if(motor_bench_)motor_bench_->stop();
    if(servos_)servos_->close();
    ethernet_.stop();
    trim_displays_.stop();
    cpflight_.stop();
    if(flightillusion_)flightillusion_->stop();
    fdstq_.configure(settings_.fdstq,false);phidgets_.enable(false);phidget_io_.configure({},hardware(),false);fds_.stop();
    input_.stop();
    transport_.stop();
    journal_.stop();
}
AppSettings Runtime::settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}
bool Runtime::assign_trim_display(unsigned role,std::uint64_t identity,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    if(role<1 || role>2){error="Unknown trim-indicator role";return false;}
    auto updated=settings();auto& binding=updated.trim_displays.roles[role-1];
    if(identity){const auto inventory=trim_displays_.snapshot();
        const auto device=std::find_if(inventory.devices.begin(),inventory.devices.end(),[&](const auto& d){return d.identity==identity && d.role==role && !d.duplicate;});
        if(device==inventory.devices.end()){error="Identify a display with the matching firmware role first";return false;}
        binding={identity,device->port};
    }else binding={};
    return save_settings(updated,error);
}
bool Runtime::assign_fdstq(std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);const auto settings_now=settings();const auto& tq=settings_now.fdstq;
    if(!tq.enabled || !settings_now.fds_enabled || !fdstq_valid(tq,error)){if(error.empty())error="Enable FDS and FDSTQ support and choose the exact InterfaceKit serial";return false;}
    const auto device=fdstq_device(tq.serial);auto a=std::make_unique<AxisConfiguration>(axes());auto b=buttons();const auto p=profiles();
    // Prepare every change before the single assignments.cfg atomic commit.
    const std::array<unsigned,6> slots{9,10,11,12,8,13};
    for(const auto slot:slots){if(!tq.paired && (slot==11 || slot==12))continue;auto& row=a->rows[slot];
        const auto control=slot==8?10u:slot==13?11u:8u+(tq.paired?(slot>=11):(slot==10));
        if((!row.applied.source.id.empty() && row.applied.source.identity.hardware_key!=device.identity.hardware_key) || (!row.draft.source.id.empty() && row.draft.source.identity.hardware_key!=device.identity.hardware_key)){error="Flight axis already assigned: "+utf8(flight_slot_name(slot))+"; clear it explicitly first";return false;}
        if(!row.draft.source.id.empty())continue;row.managed=true;axis_source(row.draft,slot,device,device.controls[control],false,false);
        if(slot>=9 && slot<=12){auto& binding=row.draft.source.bindings[0];binding.throttle_layout=ThrottleLayout::separate_forward_reverse;binding.calibration.centered=false;binding.reverse_calibration.centered=false;binding.throttle=ThrottlePolicy::at_servo_follower;}
        row.pending="Capture physical end stops / detents and Apply in Flight axes";
    }
    const auto* module_definition=module_profile(module_library_,tq.paired?"fds.phidget22.fdstq.paired":"fds.phidget22.fdstq.individual");
    if(!module_definition || !module_definition->unavailable.empty()){error="FDSTQ module profile unavailable; add module profiles and restart";return false;}
    std::vector<std::pair<std::string,unsigned>> actions;for(const auto& input:module_definition->inputs){if(input.channel>=8){error="FDSTQ profile input out of range";return false;}actions.emplace_back(input.action,input.channel);}
    for(const auto& [id,control]:actions){const auto* action=button_action(id);if(!action || !action->target.writable){error="Required catalogue action unavailable: "+id;return false;}
        for(const auto& [other_id,other]:b.rows){const auto* other_action=button_action(other_id);if(other_action && other_action->group==action->group && !other.applied.id.empty() && other.applied.identity.hardware_key!=device.identity.hardware_key){error="Button destination already assigned: "+action->name;return false;}}
        for(const auto& profile:p)for(const auto& binding:profile.bindings)if(binding.enabled && button_overlap(*action,binding.target)){error="Button destination already owned by profile "+profile.name;return false;}
        for(const auto& [cid,other]:cockpit().rows)if(other.applied.enabled){const auto definition=active_catalogue().cockpit.find(cid);if(definition!=active_catalogue().cockpit.end() && cockpit_destination_overlap(action->target,definition->second->target)){error="Button destination already owned by "+cid;return false;}}
        auto& row=b.rows[id];row.managed=true;row.draft=row.applied=button_source(device,device.controls[control]);row.pending.clear();b.toggles.erase(action->group);b.toggle_drafts.erase(action->group);
    }
    if(!button_configuration_valid(b,error) || !module_conflicts(module_library_,modules(),b,cockpit(),hardware(),p,error,a.get()))return false;
    const auto h=hardware();const auto c=cockpit();const auto m=modules();if(!save_assignments(options_.data_dir+L"\\assignments.cfg",p,display_assignments(),*a,error,&b,&h,&c,&m))return false;
    {std::lock_guard lock(mutex_);axes_=std::move(*a);buttons_=std::move(b);++axis_revision_;++button_revision_;}clear_module_tests();SetEvent(change_);return true;
}
bool Runtime::save_settings(const AppSettings &settings, std::string &error) {
    std::lock_guard configuration_lock(configuration_mutex_);
    if(motor_mappings_ && motor_mapping_conflicts(motor_mappings_->definitions(),settings.fdstq,error))return false;
    auto applied=settings;
    if (!hwio::save_settings(options_.data_dir, applied, error))
        return false;
    journal_.set_event_filter(settings.log_level != LogLevel::off, settings.log_level == LogLevel::warn
                                                                       ? psx::diagnostics::Severity::warning
                                                                       : psx::diagnostics::Severity::info);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = applied;
        ++settings_revision_;
    }
    journal_.producer(0, psx::diagnostics::Component::application)
        .emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::info, {},
              psx::diagnostics::ErrorDomain::none, 0, "Options applied: median-three + smoothing ms",
              {static_cast<std::uint64_t>(settings.smoothing_ms)});
    SetEvent(change_);
    return true;
}
HardwareConfiguration Runtime::hardware(){
    HardwareConfiguration out;{std::lock_guard lock(mutex_);out=hardware_;}
    for(const auto& h:ethernet_.health())if(h.serial && out.devices.size()<max_ethernet_devices && std::none_of(out.devices.begin(),out.devices.end(),[&](const auto& d){return d.key==h.key;})){
        NetworkDevice d;d.key=h.key;d.name=h.name;d.serial=h.serial;d.model=h.model;out.devices.push_back(std::move(d));
    }
    for(const auto& card:fds_.inventory())if(card.generic && std::none_of(out.devices.begin(),out.devices.end(),[&](const auto& d){return d.key==card.key;})) {
        NetworkDevice d;d.key=card.key;d.name=card.input.identity.name;d.backend=card.input.identity.backend;d.provider_model=card.model;d.provider_revision=static_cast<unsigned>(card.caps.update);out.devices.push_back(std::move(d));
    }
    for(const auto& card:phidget_io_.health())if(card.serial && std::none_of(out.devices.begin(),out.devices.end(),[&](const auto& d){return d.key==card.key;})){
        NetworkDevice d;d.key=card.key;d.name=card.name;d.serial=card.serial;d.backend=BackendKind::phidget22;out.devices.push_back(std::move(d));
    }
    // Retain the actual board model with its address. It is needed for card
    // transfers even when no I/O channel has yet been assigned/opened.
    for(const auto& card:phidgets_.inventory().devices)if(!card.key.empty()){
        auto found=std::find_if(out.devices.begin(),out.devices.end(),[&](const auto& d){return d.key==card.key;});
        if(found==out.devices.end()){NetworkDevice d;d.key=card.key;d.name=card.name;d.serial=static_cast<std::uint32_t>(card.serial);d.backend=BackendKind::phidget22;out.devices.push_back(std::move(d));found=std::prev(out.devices.end());}
        found->provider_model=std::to_string(card.model);found->provider_revision=static_cast<unsigned>(std::max(0,card.version));
    }
    return out;
}
bool Runtime::save_hardware(HardwareConfiguration configuration,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    {std::lock_guard lock(mutex_);if(!output_identification_card_.device.empty()){
        const auto only=[&](const HardwareConfiguration& h){HardwareConfiguration result;for(const auto& d:h.devices)if(d.key==output_identification_card_.device)result.devices.push_back(d);for(const auto& b:h.outputs)if(b.device==output_identification_card_.device)result.outputs.push_back(b);return hardware_encode(result);};
        if(only(hardware_)!=only(configuration)){error="Close output identification before editing this card";return false;}
    }}
    if(!hardware_valid(configuration,error))return false;
    auto p=profiles();auto d=display_assignments();auto a=std::make_unique<AxisConfiguration>(axes());auto b=buttons();
    if(!module_conflicts(module_library_,modules(),b,cockpit(),configuration,p,error,a.get()))return false;
    if(!save_assignments(options_.data_dir+L"\\assignments.cfg",p,d,*a,error,&b,&configuration))return false;
    {std::lock_guard lock(mutex_);hardware_=std::move(configuration);++hardware_revision_;}
    SetEvent(change_);return true;
}
CockpitConfiguration Runtime::cockpit(){std::lock_guard lock(mutex_);return cockpit_;}
ModuleConfiguration Runtime::modules(){std::lock_guard lock(mutex_);return modules_;}
bool Runtime::merge_output_catalogue(std::size_t& added,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    try{const auto directory=std::filesystem::path(executable_directory());auto current=read_catalogue(directory/L"catalogue.xml");const auto defaults=read_catalogue(directory/L"defaults/catalogue.xml");
        added=merge_digital_catalogue(*current,*defaults);if(added){current->revision+=" + FDS display definitions";write_catalogue(*current,directory/L"catalogue.xml");}return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool Runtime::merge_module_library(std::size_t& added,std::string& error) {
    std::lock_guard configuration_lock(configuration_mutex_);
    try {
        const auto directory=std::filesystem::path(executable_directory());const auto file=directory/L"fds-modules.xml";
        const auto defaults=read_modules(directory/L"defaults/fds-modules.xml");if(!defaults.problem.empty())throw std::runtime_error(defaults.problem);
        auto current=std::filesystem::exists(file)?read_modules(file):ModuleLibrary{};
        added=merge_modules(current,defaults);if(added)write_modules(current,file);return true;
    } catch(const std::exception& e){error=e.what();return false;}
}
bool Runtime::save_modules(ModuleConfiguration configuration,std::string& error,const std::set<std::string>* applied_rows) {
    std::lock_guard configuration_lock(configuration_mutex_);
    std::string identifying;{std::lock_guard lock(mutex_);identifying=output_identification_card_.device;}
    const auto previous=modules();
    const auto cards=module_inventory();
    for(auto& [id,row]:configuration.rows)if(row.pending=="apply" && (!applied_rows || applied_rows->count(id))) {
        (void)id;
        if(!row.draft.profile.empty()) {
            if(!identifying.empty() && (row.device==identifying || std::any_of(row.draft.associations.begin(),row.draft.associations.end(),[&](const auto& a){return a.second==identifying;}))){error="Output identification owns this card; finish or close it before applying a module";return false;}
            const auto* profile=module_profile(module_library_,row.draft.profile);
            if(!profile || !profile->unavailable.empty()){error=profile?profile->unavailable:"Module profile unavailable";return false;}
            const auto card=std::find_if(cards.begin(),cards.end(),[&](const auto& d){return d.key==row.device;});
            const auto model=card==cards.end()?row.model:card->model;
            const auto revision=card==cards.end()?row.update:card->update;
            if(profile->model!=model || revision<profile->minimum_update || revision>profile->maximum_update ||
               std::find(profile->roles.begin(),profile->roles.end(),row.draft.role)==profile->roles.end()){error="Profile does not qualify this model/revision/role";return false;}
            for(const auto& association:profile->associations)if(association.required && !row.draft.associations.count(association.id)){error="Select the required "+association.role+" board explicitly before Apply";return false;}
            for(const auto& [association,key]:row.draft.associations){const auto qualified=std::find_if(profile->associations.begin(),profile->associations.end(),[&](const auto& a){return a.id==association;});if(qualified==profile->associations.end()){error="Unknown child association; select this profile again";return false;}const auto child=std::find_if(cards.begin(),cards.end(),[&](const auto& d){return d.key==key;});const auto stored=configuration.rows.find(key);if(child!=cards.end()?(child->model!=qualified->model || child->input.identity.backend!=profile->backend):(stored==configuration.rows.end() || stored->second.model!=qualified->model)){error="Associated board model does not match "+qualified->role;return false;}}
            for(const auto& input:profile->inputs) {
                const auto action=module_reference(*profile,row.draft,input.action,input.association);
                if(!active_catalogue().action(action)){error="Missing catalogue action "+action+"; explicitly merge defaults and restart";return false;}
            }
            for(const auto& input:profile->analogue){const auto definition=module_reference(*profile,row.draft,input.definition,input.association);const auto found=active_catalogue().cockpit.find(definition);if(found==active_catalogue().cockpit.end() || found->second->encoder!=input.rotary || !found->second->target.writable){error="Missing/incompatible catalogue analogue input "+definition+"; explicitly merge defaults and restart";return false;}}
            for(const auto& lamp:profile->lamps){const auto output=module_reference(*profile,row.draft,lamp.output,lamp.association);if(!active_catalogue().digital_outputs.count(output)){error="Missing catalogue lamp "+output+"; explicitly merge defaults and restart";return false;}}
            for(const auto& display:profile->displays){const auto definition=module_reference(*profile,row.draft,display.display,display.association);if(!definition.empty() && !active_catalogue().displays.count(definition)){error="Missing catalogue display "+definition+"; explicitly merge defaults and restart";return false;}}
            row.draft.version=profile->version;
        }
        row.applied=row.draft;row.pending.clear();
    }
    const auto axes_configuration=std::make_unique<AxisConfiguration>(axes());const auto b=buttons();const auto c=cockpit();const auto h=hardware();
    if(!module_conflicts(module_library_,configuration,b,c,h,profiles(),error,axes_configuration.get()))return false;
    if(!save_assignments(options_.data_dir+L"\\assignments.cfg",profiles(),display_assignments(),*axes_configuration,error,&b,&h,&c,&configuration))return false;
    bool applied_changed=false;
    for(const auto& [id,row]:configuration.rows){const auto old=previous.rows.find(id);if(old==previous.rows.end()?!row.applied.profile.empty():row.applied!=old->second.applied)applied_changed=true;}
    for(const auto& [id,row]:previous.rows)if(!row.applied.profile.empty() && !configuration.rows.count(id))applied_changed=true;
    {std::lock_guard lock(mutex_);modules_=std::move(configuration);++module_revision_;if(applied_changed){identification_device_.clear();identification_profile_.clear();++identification_revision_;++module_applied_revision_;module_tests_.clear();++module_test_revision_;}}
    SetEvent(change_);return true;
}
bool Runtime::test_module(const std::string& device,bool on,std::string& error) {
    std::lock_guard lock(mutex_);
    const auto view=std::find_if(view_.modules.begin(),view_.modules.end(),[&](const auto& m){return m.device==device;});
    if(view==view_.modules.end() || !view->eligible || !view->connected){error="Applied connected output-bearing module required";return false;}
    const auto row=modules_.rows.find(device);const auto* profile=row==modules_.rows.end()?nullptr:module_profile(module_library_,row->second.applied.profile);
    if(!profile || profile->input_only || (profile->lamps.empty() && profile->displays.empty())){error="This module has no eligible lamp/readout test";return false;}
    module_tests_[device]=on;++module_test_revision_;SetEvent(change_);return true;
}
void Runtime::clear_module_tests(){fdstq_.identify(false);std::lock_guard lock(mutex_);if(!identification_device_.empty()){identification_device_.clear();identification_profile_.clear();++identification_revision_;SetEvent(change_);}if(module_tests_.empty())return;module_tests_.clear();++module_test_revision_;SetEvent(change_);}
bool Runtime::save_cockpit(CockpitConfiguration c,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);if(!cockpit_valid(c,error))return false;
    auto p=profiles();auto d=display_assignments();auto a=std::make_unique<AxisConfiguration>(axes());auto b=buttons();auto h=hardware();
    const auto previous=cockpit();
    for(auto& [id,row]:c.rows){
        if(row.pending!="apply")continue;
        row.pending.clear();const auto definition=active_catalogue().cockpit.find(id);
        if(definition==active_catalogue().cockpit.end()){row.pending="Catalogue definition unavailable";continue;}
        const auto& target=*definition->second;
        if(row.draft.enabled){
            if(!target.encoder && !row.draft.thresholds.empty() && row.draft.thresholds.size()+1!=target.detents.size())row.pending="Threshold count must be one less than selector detents";
            for(const auto& profile:p)for(const auto& binding:profile.bindings)if(binding.enabled && cockpit_destination_overlap(binding.target,target.target))row.pending="Destination owned by existing profile; clear that assignment first";
            auto source_conflict=[&](const Profile& profile,const Source& source){
                auto selected=source.profile.empty() || source.profile==profile.id?&profile:nullptr;if(!selected)for(const auto& candidate:p)if(candidate.id==source.profile)selected=&candidate;
                if(!selected || selected->identity.hardware_key!=row.draft.device)return false;
                return cockpit_source_overlap(source,target.encoder,row.draft);
            };
            for(const auto& profile:p)for(const auto& binding:profile.bindings)if(binding.enabled)for(const auto& source:binding.sources)if(source_conflict(profile,source))row.pending="Hardware source owned by existing profile; clear that assignment first";
            for(const auto& axis:a->rows)if(axis.managed && axis.active)for(const auto& binding:axis.applied.source.bindings)for(const auto& source:binding.sources)if(source_conflict(axis.applied.source,source))row.pending="Hardware source owned by flight controls; clear that assignment first";
            for(const auto& axis:a->rows)if(axis.managed && axis.active && !axis.applied.source.bindings.empty() && axis.applied.source.bindings.front().throttle_layout==ThrottleLayout::separate_forward_reverse && source_conflict(axis.applied.reverse,axis.applied.source.bindings.front().reverse_source))row.pending="Hardware source owned by flight controls; clear that assignment first";
            if(a->override_assigned)for(const auto& binding:a->override_source.bindings)for(const auto& source:binding.sources)if(source_conflict(a->override_source,source))row.pending="Hardware source owned by flight-control override; clear that assignment first";

            for(const auto& [action_id,button]:b.rows)if(!button.applied.id.empty()){
                if(const auto* action=button_action(action_id);action && cockpit_destination_overlap(action->target,target.target))row.pending="Destination owned by Digital inputs; clear that assignment first";
                for(const auto& binding:button.applied.bindings)for(const auto& source:binding.sources)if(button.applied.identity.hardware_key==row.draft.device && cockpit_source_overlap(source,target.encoder,row.draft))row.pending="Hardware source owned by Digital inputs; clear that assignment first";
            }
            for(const auto& [other_id,other]:c.rows)if(other_id!=id && other.applied.enabled){
                const auto other_definition=active_catalogue().cockpit.find(other_id);
                if(other_definition!=active_catalogue().cockpit.end() && cockpit_destination_overlap(other_definition->second->target,target.target))row.pending="Destination already owned by another cockpit assignment";
                const auto& x=other.applied;const auto& y=row.draft;
                const bool same_kind=other_definition!=active_catalogue().cockpit.end() && (other_definition->second->encoder?x.software?1:2:0)==(target.encoder?y.software?1:2:0);
                if(x.device==y.device && same_kind && (x.a==y.a || (x.software && (x.b==y.a || x.a==y.b || x.b==y.b))))row.pending="Hardware source already assigned";
            }
        }
        if(row.pending.empty())row.applied=row.draft;
        else if(const auto old=previous.rows.find(id);old!=previous.rows.end())row.applied=old->second.applied;
        else row.applied={};
    }
    if(!module_conflicts(module_library_,modules(),b,c,h,p,error,a.get()))return false;
    if(!save_assignments(options_.data_dir+L"\\assignments.cfg",p,d,*a,error,&b,&h,&c))return false;
    bool changed=false;for(const auto& [id,row]:c.rows){const auto old=previous.rows.find(id);if(old==previous.rows.end()?row.applied.enabled:row.applied!=old->second.applied)changed=true;}
    for(const auto& [id,row]:previous.rows)if(row.applied.enabled && !c.rows.count(id))changed=true;
    {std::lock_guard lock(mutex_);cockpit_=std::move(c);if(changed)++cockpit_revision_;}SetEvent(change_);return true;
}
bool Runtime::test_output(const std::string& id,bool on,std::string& error) {
    const auto definition=active_catalogue().digital_outputs.find(id);
    if(definition==active_catalogue().digital_outputs.end() || !definition->second->qualified){error="Output contract unavailable";return false;}
    const auto configuration=hardware();const HardwareBinding* binding=nullptr;
    for(const auto& b:configuration.outputs)if(b.observation==id){if(binding){error="Test requires one assigned destination";return false;}binding=&b;}
    if(!binding || !binding->enabled || binding->transform!=OutputTransform::boolean || binding->minimum!=0 || binding->maximum!=1){error="Assign a Boolean output before testing";return false;}
    {std::lock_guard lock(mutex_);if(output_identification_card_.device==binding->device){error="Output identification currently owns this card";return false;}}
    if(std::any_of(configuration.devices.begin(),configuration.devices.end(),[&](const auto& card){return card.key==binding->device && !card.enabled;})){error="Card disabled in HWIO Hardware options";return false;}
    if(!hardware_resource_enabled(configuration,binding->device,binding->resource)){error="PoExtBus outputs disabled in HWIO Hardware options";return false;}
    auto health=ethernet_.health();const auto fds_health=fds_.generic_health();health.insert(health.end(),fds_health.begin(),fds_health.end());const auto phidget_health=phidget_io_.health();health.insert(health.end(),phidget_health.begin(),phidget_health.end());bool configured=false;
    for(const auto& card:health)if(card.key==binding->device && card.connected && card.configured){
        if(std::any_of(card.inhibited_outputs.begin(),card.inhibited_outputs.end(),[&](const auto& issue){return issue.first==binding->id;}))continue;
        configured=std::any_of(card.resources.begin(),card.resources.end(),[&](const auto& resource){return resource.output && resource.minimum==0 && resource.maximum==1 && resource.kind==binding->resource && resource.channel==binding->channel &&
            (resource.kind==pokeys::Channel::digital || resource.kind==pokeys::Channel::led || resource.kind==pokeys::Channel::poext || resource.kind==ResourceKind::boolean_output || resource.kind==ResourceKind::dataline_output);});
        if(configured && binding->device.starts_with("fds-") && binding->resource==ResourceKind::boolean_output)for(const auto& resource:card.resources)if(resource.output && resource.kind==ResourceKind::boolean_output && (resource.channel-1)%8==(binding->channel-1)%8)
            if(std::none_of(configuration.outputs.begin(),configuration.outputs.end(),[&](const auto& owner){return owner.enabled && owner.device==binding->device && owner.resource==resource.kind && owner.channel==resource.channel;}))configured=false;
        if(configured && binding->device.starts_with("fds-") && binding->resource==ResourceKind::boolean_output && std::any_of(card.resources.begin(),card.resources.end(),[](const auto& resource){return resource.output && resource.kind==ResourceKind::integral_display;}))configured=false;
        if(configured && binding->device.starts_with("fds-") && binding->resource==ResourceKind::dataline_output)for(const auto& resource:card.resources)if(resource.output && resource.kind==ResourceKind::dataline_output)
            if(std::none_of(configuration.outputs.begin(),configuration.outputs.end(),[&](const auto& owner){return owner.enabled && owner.device==binding->device && owner.resource==resource.kind && owner.channel==resource.channel;}))configured=false;
    }
    if(!configured){error="Assigned Boolean channel unavailable";return false;}
    {std::lock_guard lock(mutex_);output_tests_[id]=on;++output_test_revision_;}
    SetEvent(change_);return true;
}
void Runtime::clear_output_tests(){
    {std::lock_guard lock(mutex_);if(output_tests_.empty())return;output_tests_.clear();++output_test_revision_;}
    SetEvent(change_);
}
std::map<std::string,bool> Runtime::output_tests(){std::lock_guard lock(mutex_);return output_tests_;}
std::optional<IdentificationCard> Runtime::output_identification_card(const std::string& device,std::string& error){
    const auto h=hardware();const auto settings=this->settings();
    const auto configured=std::find_if(h.devices.begin(),h.devices.end(),[&](const auto& d){return d.key==device;});
    const bool fds=device.starts_with("fds-"),phidget=device.starts_with("phidget:");
    if(!(fds?settings.fds_enabled:phidget?settings.phidgets_enabled:settings.pokeys_enabled) || configured==h.devices.end() || !configured->enabled){error="Device support or this card is disabled";return {};}
    const auto m=modules();for(const auto& [key,row]:m.rows)if(!row.applied.profile.empty()){
        if(key==device || std::any_of(row.applied.associations.begin(),row.applied.associations.end(),[&](const auto& a){return a.second==device;})){error="A complete module owns this card";return {};}
    }
    const auto health=fds?fds_.generic_health():phidget?phidget_io_.health():ethernet_.health();
    const auto card=std::find_if(health.begin(),health.end(),[&](const auto& d){return d.key==device;});
    if(card==health.end() || !card->connected || !card->configured){error="Card is not connected with a current resource inventory";return {};}
    auto devices=fds?fds_.devices():phidget?phidget_io_.devices():ethernet_.devices();const auto identity=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.identity.hardware_key==device;});
    if(identity==devices.end() || !identity->connected || !identity->generation || !card->revision){error="Device identity is not current";return {};}
    if(fds && std::any_of(card->resources.begin(),card->resources.end(),[](const auto& r){return r.output && r.kind==ResourceKind::integral_display;})){error="Generic identification is unavailable for a shared integral-display bank";return {};}
    IdentificationCard result{device,identity->generation,card->revision,{}};
    for(const auto& r:card->resources)if(r.output && r.minimum==0 && r.maximum==1 && hardware_resource_enabled(h,device,r.kind) &&
        (r.kind==pokeys::Channel::digital || r.kind==pokeys::Channel::led || r.kind==pokeys::Channel::poext || r.kind==ResourceKind::boolean_output || r.kind==ResourceKind::dataline_output)){
        const unsigned rank=r.kind==pokeys::Channel::digital?0:r.kind==pokeys::Channel::poext?2:1;
        result.endpoints.push_back({static_cast<std::uint16_t>(r.kind),r.channel,UINT16_MAX,rank*65536u+static_cast<unsigned>(r.kind)*1024u+r.channel,r.name});
    }
    if(result.endpoints.empty()){error="No usable Boolean output channels";return {};}
    std::sort(result.endpoints.begin(),result.endpoints.end(),[](const auto& a,const auto& b){return a.physical_order<b.physical_order;});error.clear();return result;
}
bool Runtime::identify_outputs(IdentificationRequest r,const IdentificationCard& card,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    if(r.kind==IdentificationRequestKind::acquire){
        {std::lock_guard lock(mutex_);if(output_identification_session_){error="Another output-identification session is still restoring its card";return false;}}
        const auto current=output_identification_card(card.device,error);if(!current)return false;
        if(current->connection_generation!=r.connection_generation || current->resource_generation!=r.resource_generation || current->endpoints.size()!=card.endpoints.size()){error="Output inventory changed; reopen this card";return false;}
        for(std::size_t n=0;n<card.endpoints.size();++n){const auto& a=current->endpoints[n];const auto& b=card.endpoints[n];if(a.resource!=b.resource || a.channel!=b.channel || a.bit!=b.bit){error="Output addresses changed";return false;}}
        clear_output_tests();clear_module_tests();
        {std::lock_guard lock(mutex_);output_identification_card_=card;output_identification_session_=r.session;}
    }else {std::lock_guard lock(mutex_);if(output_identification_session_!=r.session || output_identification_card_.device!=card.device){
        // Provider disablement/loss can finish before the UI requests release.
        // Echo its terminal evidence under this request identity, without
        // changing a newer session or inventing successful restoration.
        if(r.kind==IdentificationRequestKind::release){
            const bool known=output_identification_retired_ && output_identification_retired_->request.session==r.session;
            output_identification_reply_=IdentificationCompletion{r,known && output_identification_retired_->success,true,known?output_identification_retired_->reason:"Identification session expired; normal output restoration unconfirmed"};return true;
        }
        error="Output-identification session has expired";return false;
    }}
    const bool accepted=card.device.starts_with("fds-")?fds_.identify_outputs(r,card):card.device.starts_with("phidget:")?phidget_io_.identify_outputs(r,card):ethernet_.identify_outputs(r,card);
    if(!accepted){if(r.kind==IdentificationRequestKind::acquire){std::lock_guard lock(mutex_);output_identification_session_=0;output_identification_card_={};}error="Provider identification mailbox is busy";return false;}
    return true;
}
std::optional<IdentificationCompletion> Runtime::identification_completion(std::uint64_t session){
    bool fds,phidget;{std::lock_guard lock(mutex_);if(output_identification_reply_ && output_identification_reply_->request.session==session){auto reply=std::move(output_identification_reply_);output_identification_reply_.reset();return reply;}if(session!=output_identification_session_)return {};fds=output_identification_card_.device.starts_with("fds-");phidget=output_identification_card_.device.starts_with("phidget:");}
    auto result=fds?fds_.identification_completion(session):phidget?phidget_io_.identification_completion(session):ethernet_.identification_completion(session);
    if(result && result->ownership_released){std::lock_guard lock(mutex_);if(output_identification_session_==session){output_identification_retired_=*result;output_identification_session_=0;output_identification_card_={};}}
    return result;
}
bool Runtime::save_identified_outputs(const IdentificationCard& card,const std::vector<IdentificationChoice>& choices,const HardwareConfiguration& original,std::string& error){
    std::lock_guard configuration_lock(configuration_mutex_);
    if(choices.size()!=card.endpoints.size()){error="Output draft does not match its card";return false;}
    auto next=hardware();auto outputs=[&](const HardwareConfiguration& h){HardwareConfiguration c;for(const auto& b:h.outputs)if(b.device==card.device)c.outputs.push_back(b);return hardware_encode(c);};
    if(outputs(next)!=outputs(original)){error="This card's assignments changed during identification; draft retained";return false;}
    std::uint64_t id=1;for(const auto& b:next.outputs)id=std::max(id,b.id+1);
    for(std::size_t n=0;n<choices.size();++n)if(choices[n].kind==IdentificationChoiceKind::assigned){
        const auto& e=card.endpoints[n];const auto definition=active_catalogue().digital_outputs.find(choices[n].output);
        if(definition==active_catalogue().digital_outputs.end() || !definition->second->qualified){error="Selected illumination definition is unavailable";return false;}
        HardwareBinding binding;binding.id=id++;binding.device=card.device;binding.resource=static_cast<pokeys::Channel>(e.resource);binding.channel=e.channel;binding.bit=e.bit;binding.name=definition->second->name;binding.observation=choices[n].output;binding.enabled=true;binding.transform=OutputTransform::boolean;
        const auto old=std::find_if(next.outputs.begin(),next.outputs.end(),[&](const auto& b){return b.device==card.device && static_cast<unsigned>(b.resource)==e.resource && b.channel==e.channel && b.bit==e.bit;});
        if(old==next.outputs.end())next.outputs.push_back(std::move(binding));else {binding.id=old->id;*old=std::move(binding);}
    }
    return save_hardware(std::move(next),error);
}
static Timing timing(std::vector<double> data);
RuntimeView Runtime::snapshot() {
    RuntimeView view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view = view_;
    }
    // Percentile sorting belongs to the reader, never the mapping worker.
    view.map_timing = timing(std::move(view.timing_samples[0]));
    view.send_timing = timing(std::move(view.timing_samples[1]));
    view.filtered_map_timing = timing(std::move(view.timing_samples[2]));
    view.filtered_send_timing = timing(std::move(view.timing_samples[3]));
    return view;
}
void Runtime::request_snapshot(HWND window, UINT message) {
    view_window_ = window;
    view_message_ = message;
    view_requested_ = true;
    SetEvent(change_);
}
std::vector<Profile> Runtime::profiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_;
}
DisplayAssignments Runtime::display_assignments() {
    std::lock_guard<std::mutex> lock(mutex_);
    return display_assignments_;
}
AxisConfiguration Runtime::axes() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = axes_;
    for (std::size_t i = 0; i < flight_slots; ++i) {
        auto &row = result.rows[i];
        if (row.managed || !row.pending.empty())
            continue;
        row = {};
        const auto &ref = display_assignments_[i];
        if (ref.choice == DisplayChoice::none)
            continue;
        // A unique automatic source is imported once, on CPT. It is still shared graphically.
        if (ref.choice == DisplayChoice::automatic && ((i >= 4 && i < 8) || i >= 16))
            continue;
        const Profile *chosen_profile = nullptr;
        const Binding *chosen = nullptr;
        unsigned matches = 0;
        for (const auto &p : profiles_)
            for (const auto &b : p.bindings)
                if (flight_binding_matches(i, b) &&
                    (ref.choice == DisplayChoice::binding ? ref.profile == p.id && ref.binding == b.id : b.enabled)) {
                    chosen_profile = &p;
                    chosen = &b;
                    ++matches;
                }
        if (matches == 1 && chosen && !chosen->sources.empty() && chosen->sources[0].key.kind == ControlKind::axis) {
            row.replaces = {DisplayChoice::binding, chosen_profile->id, chosen->id};
            row.draft = axis_from_binding(*chosen_profile, *chosen, profiles_);
            row.applied = row.draft;
            row.active = chosen->enabled;
        }
    }
    return result;
}
void Runtime::learn_override() {
    learn();
    std::lock_guard<std::mutex> lock(mutex_);
    learning_button_ = true;
}
ButtonConfiguration Runtime::buttons() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = buttons_;
    button_import_legacy(result, profiles_);
    return result;
}
bool Runtime::save_buttons(ButtonConfiguration configuration, std::string &error, const std::string &replace_action) {
    std::lock_guard configuration_lock(configuration_mutex_);
    auto existing = profiles();
    const auto axis_configuration = axes();
    const auto previous = buttons();
    const auto cockpit_configuration=cockpit();
    bool legacy_changed=false;
    for(const auto &draft:configuration.toggle_drafts) {
        if(draft.second)configuration.toggles.insert(draft.first);else configuration.toggles.erase(draft.first);
        for(const auto &a:button_catalogue())if(a.group==draft.first)configuration.rows[a.id].managed=true;
    }
    for (auto &item : configuration.rows) {
        auto &row = item.second;
        if (!row.managed) continue;
        const auto *action = button_action(item.first);
        if (!action) { row.pending="Catalogue action unavailable; assignment retained"; continue; }
        row.pending.clear();
        if (!row.draft.id.empty() && row.draft.bindings.empty()) row.pending = "Select a hardware button";
        if (!row.draft.id.empty() && !button_source_valid(row.draft)) row.pending = "Invalid hardware button";
        if (!row.draft.id.empty() && row.pending.empty()) {
            for(const auto& [id,cockpit_row]:cockpit_configuration.rows)if(cockpit_row.applied.enabled){const auto definition=active_catalogue().cockpit.find(id);if(definition==active_catalogue().cockpit.end())continue;
                if(cockpit_destination_overlap(action->target,definition->second->target))row.pending="Owned by Encoders/Analogue inputs; clear that assignment first";
                // The same physical phase cannot command an encoder and a panel
                // button. Use the provider's resource kind for both Apply paths.
                for(const auto& source:row.draft.bindings[0].sources)if(row.draft.identity.hardware_key==cockpit_row.applied.device &&
                    cockpit_source_overlap(source,definition->second->encoder,cockpit_row.applied))row.pending="Hardware source owned by Encoders; clear that assignment first";
            }
            for (std::size_t i=0;i<flight_slots;++i) {
                const auto &axis = axis_configuration.rows[i];
                if (axis.managed && axis.active && button_overlap(*action,axis_target(i)))
                    row.pending = "Owned by axis pane: " + utf8(flight_slot_name(i)) + "; clear that axis assignment first";
            }
            for (auto &p : existing) for (auto &b : p.bindings) {
                if (!b.enabled || !button_overlap(*action,b.target)) continue;
                if (replace_action == item.first) {
                    b.enabled = false;
                    legacy_changed=true;
                    row.replaces.push_back({p.id,b.id});
                } else row.pending = "Owned by " + p.name + " / " + b.name + "; use Replace existing assignment";
            }
        }
        // Clearing a recognized existing row explicitly removes that legacy assignment.
        if(row.draft.id.empty() && row.pending.empty())for(const auto &old:row.replaces)
            for(auto &p:existing)if(p.id==old.first)for(auto &b:p.bindings)if(b.id==old.second&&b.enabled){b.enabled=false;legacy_changed=true;}
        if (row.pending.empty()) row.applied = row.draft;
        else {
            const auto old = previous.rows.find(item.first);
            row.applied = old == previous.rows.end() ? Profile{} : old->second.applied;
            row.managed = old != previous.rows.end() && old->second.managed;
        }
    }
    // A Toggle edit is one group transaction. Conflicts must not change the active mode
    // or remove its other position while the proposed replacement remains pending.
    std::set<std::string> toggle_groups=configuration.toggles;
    toggle_groups.insert(previous.toggles.begin(),previous.toggles.end());
    for(const auto &draft:configuration.toggle_drafts)toggle_groups.insert(draft.first);
    for(const auto &group:toggle_groups) {
        const bool desired=configuration.toggles.count(group)!=0;
        bool pending=false;
        for(const auto &a:button_catalogue())if(a.group==group) {
            const auto row=configuration.rows.find(a.id);
            pending=pending||(row!=configuration.rows.end()&&!row->second.pending.empty());
        }
        if(pending && (desired!=(previous.toggles.count(group)!=0)||configuration.toggle_drafts.count(group))) {
            configuration.toggle_drafts[group]=desired;
            if(previous.toggles.count(group))configuration.toggles.insert(group);else configuration.toggles.erase(group);
            for(const auto &a:button_catalogue())if(a.group==group) {
                auto &row=configuration.rows[a.id];const auto old=previous.rows.find(a.id);
                row.applied=old==previous.rows.end()?Profile{}:old->second.applied;
                row.managed=old!=previous.rows.end()&&old->second.managed;
                if(row.pending.empty())row.pending="Toggle change pending in this switch group";
            }
        } else configuration.toggle_drafts.erase(group);
    }
    if (!module_conflicts(module_library_,modules(),configuration,cockpit(),hardware(),existing,error,&axis_configuration))return false;
    if (!button_configuration_valid(configuration,error) ||
        !save_assignments(options_.data_dir + L"\\assignments.cfg",existing,display_assignments(),axis_configuration,error,&configuration)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiles_ = std::move(existing);
        buttons_ = std::move(configuration);
        ++button_revision_;
        bool ownership_changed=false;
        for (const auto &p:profiles_) for (const auto &b:p.bindings)
            ownership_changed = ownership_changed || button_replaces(previous,p.id,b.id) != button_replaces(buttons_,p.id,b.id);
        if (ownership_changed || legacy_changed || !replace_action.empty()) ++revision_;
    }
    journal_.producer(0,psx::diagnostics::Component::application)
        .emit(psx::diagnostics::Code::configuration,psx::diagnostics::Severity::info,{},psx::diagnostics::ErrorDomain::none,0,
              "Button assignment configuration saved");
    SetEvent(change_);
    return true;
}
bool Runtime::save_axes(AxisConfiguration configuration, std::string &error) {
    std::lock_guard configuration_lock(configuration_mutex_);
    const auto existing = profiles();
    std::unique_ptr<AxisConfiguration> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::make_unique<AxisConfiguration>(axes_);
    }
    const auto log = journal_.producer(0, psx::diagnostics::Component::application);
    const auto failed = [&] {
        log.emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::warning, {},
                 psx::diagnostics::ErrorDomain::application, 0, error);
        return false;
    };
    // Enrolling a second seat also enrolls the existing enabled first-seat owner.
    for (std::size_t i = 0; i < flight_slots; ++i) {
        const int partner = axis_partner(i);
        if (!configuration.rows[i].managed || partner < 0)
            continue;
        auto &other = configuration.rows[static_cast<std::size_t>(partner)];
        if (other.active && !other.managed)
            other.managed = true;
    }
    for (std::size_t i = 0; i < flight_slots; ++i) {
        const auto &row = configuration.rows[i];
        if (!row.managed || !row.active)
            continue;
        const auto target = axis_target(i);
        for(const auto& [id,cockpit_row]:cockpit().rows)if(cockpit_row.applied.enabled){
            const auto definition=active_catalogue().cockpit.find(id);if(definition==active_catalogue().cockpit.end())continue;
            if(cockpit_destination_overlap(target,definition->second->target)){error="Target already owned by Encoders/Analogue inputs";return failed();}
            auto source_conflict=[&](const Profile& physical,const Source& source){
                const Profile* owner=source.profile.empty() || source.profile==physical.id?&physical:nullptr;if(!owner)for(const auto& profile:existing)if(profile.id==source.profile)owner=&profile;
                return owner && owner->identity.hardware_key==cockpit_row.applied.device && cockpit_source_overlap(source,definition->second->encoder,cockpit_row.applied);
            };
            for(const auto& binding:row.applied.source.bindings){
                for(const auto& source:binding.sources)if(source_conflict(row.applied.source,source)){error="Hardware source already owned by Encoders/Analogue inputs";return failed();}
                if(binding.throttle_layout==ThrottleLayout::separate_forward_reverse && source_conflict(row.applied.reverse,binding.reverse_source)){error="Reverse source already owned by Encoders/Analogue inputs";return failed();}
            }
        }
        for (const auto &button : buttons().rows) {
            const auto *action = button_action(button.first);
            if (action && button.second.managed && !button.second.applied.id.empty() && button_overlap(*action,target)) {
                error = "Target already owned by button assignment: " + action->name;
                return failed();
            }
        }
        for (const auto &p : existing)
            for (const auto &b : p.bindings)
                if (b.enabled && overlaps(target, b.target) && !axis_replaces(configuration, p.id, b.id)) {
                    error = "Target already owned by " + p.name + " / " + b.name;
                    return failed();
                }
    }
    if(!module_conflicts(module_library_,modules(),buttons(),cockpit(),hardware(),existing,error,&configuration))return failed();
    if (!save_assignments(options_.data_dir + L"\\assignments.cfg", existing, display_assignments(), configuration,
                          error))
        return failed();
    const auto before = std::make_unique<AxisConfiguration>();
    const auto after = std::make_unique<AxisConfiguration>();
    for (std::size_t i = 0; i < flight_slots; ++i) {
        before->rows[i] = previous->rows[i];
        after->rows[i] = configuration.rows[i];
        const bool unchanged = axis_configuration_encode(*before) == axis_configuration_encode(*after);
        before->rows[i] = {};
        after->rows[i] = {};
        if (unchanged)
            continue;
        const auto &row = configuration.rows[i];
        std::ostringstream message;
        message << utf8(flight_slot_name(i)) << ": "
                << (row.pending.empty() ? "calibration/assignment applied" : row.pending)
                << "; captures=" << row.draft.captured << "; deadzone=" << row.draft.deadzone << '%';
        log.emit(psx::diagnostics::Code::configuration,
                 row.pending.empty() ? psx::diagnostics::Severity::info : psx::diagnostics::Severity::warning, {},
                 psx::diagnostics::ErrorDomain::none, 0, message.str());
        if (!row.draft.source.bindings.empty()) {
            const auto &b = row.draft.source.bindings[0];
            const auto &c = b.calibration;
            message.str({});
            message << "Raw min/centre/max=" << c.minimum << '/' << c.center << '/' << c.maximum
                    << "; reversed=" << c.inverted << "; source=" << row.draft.source.identity.name;
            log.emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::info, {},
                     psx::diagnostics::ErrorDomain::none, 0, message.str(), {i});
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        axes_ = std::move(configuration);
        ++axis_revision_;
    }
    SetEvent(change_);
    return true;
}
bool Runtime::save_display(const DisplayAssignments &assignments, std::string &error) {
    std::lock_guard configuration_lock(configuration_mutex_);
    if (!save_assignments(options_.data_dir + L"\\assignments.cfg", profiles(), assignments, axes(), error))
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        display_assignments_ = assignments;
        ++display_revision_;
    }
    SetEvent(change_);
    return true;
}
bool Runtime::save(Profile profile, std::string &error) {
    std::lock_guard configuration_lock(configuration_mutex_);
    if (!validate(profile, error))
        return false;
    const auto configuration = axes();
    const auto button_configuration = buttons();
    const auto cockpit_configuration=cockpit();
    for(const auto& binding:profile.bindings)if(binding.enabled)for(const auto& [id,row]:cockpit_configuration.rows)if(row.applied.enabled){
        const auto definition=active_catalogue().cockpit.find(id);if(definition!=active_catalogue().cockpit.end() && cockpit_destination_overlap(binding.target,definition->second->target)){error="Target is owned by Encoders/Analogue inputs";return false;}
        if(definition!=active_catalogue().cockpit.end())for(const auto& source:binding.sources){
            const Profile* owner=source.profile.empty() || source.profile==profile.id?&profile:nullptr;const auto saved=profiles();if(!owner)for(const auto& candidate:saved)if(candidate.id==source.profile)owner=&candidate;
            if(!owner || owner->identity.hardware_key!=row.applied.device)continue;
            if(cockpit_source_overlap(source,definition->second->encoder,row.applied)){error="Hardware source is owned by Encoders/Analogue inputs";return false;}
        }

    }
    for (const auto &b : profile.bindings) if (b.enabled)
        for (const auto &row : button_configuration.rows) {
            const auto *action = button_action(row.first);
            if (action && row.second.managed && !row.second.applied.id.empty() && button_overlap(*action,b.target) &&
                !button_replaces(button_configuration,profile.id,b.id)) {
                error = "Target is owned by the button assignment pane"; return false;
            }
        }
    for (const auto &b : profile.bindings)
        if (b.enabled)
            for (const auto &row : configuration.rows)
                if (row.managed && row.active && overlaps(b.target, row.applied.source.bindings[0].target) &&
                    !axis_replaces(configuration, profile.id, b.id)) {
                    error = "Target is owned by the axis assignment pane";
                    return false;
                }
    auto existing = profiles();
    for (const auto &other : existing)
        if (other.id != profile.id)
            for (const auto &a : other.bindings)
                for (const auto &b : profile.bindings)
                    if (a.enabled && b.enabled && overlaps(a.target, b.target)) {
                        error = "Field already owned by profile " + other.name;
                        return false;
                    }
    auto changed = std::find_if(existing.begin(), existing.end(), [&](const auto &p) { return p.id == profile.id; });
    if (changed == existing.end())
        existing.push_back(std::move(profile));
    else
        *changed = std::move(profile);
    if(!module_conflicts(module_library_,modules(),buttons(),cockpit(),hardware(),existing,error,&configuration))return false;
    if (!save_assignments(options_.data_dir + L"\\assignments.cfg", existing, display_assignments(), configuration,
                          error))
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiles_ = std::move(existing);
        ++revision_;
    }
    SetEvent(change_);
    return true;
}
bool Runtime::import_profile(const std::wstring &file, std::string &error) {
    Profile p;
    if (!load_profile(file, p, error))
        return false;
    return save(std::move(p), error);
}
bool Runtime::export_profile(const std::string &id, const std::wstring &file, std::string &error) {
    for (const auto &p : profiles())
        if (p.id == id)
            return save_profile(file, p, error);
    error = "Select an assigned profile";
    return false;
}
bool Runtime::remove_binding(const std::string &profile, std::uint64_t id, std::string &error) {
    for (auto p : profiles())
        if (p.id == profile) {
            p.bindings.erase(
                std::remove_if(p.bindings.begin(), p.bindings.end(), [&](const auto &b) { return b.id == id; }),
                p.bindings.end());
            return save(std::move(p), error);
        }
    error = "Profile not found";
    return false;
}
void Runtime::connect(std::string host, std::uint16_t port) {
    std::lock_guard configuration_lock(configuration_mutex_);
    options_.host = host;
    options_.port = port;
    std::string error;
    if (!save_endpoint(options_.data_dir, host, port, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.problem = error;
    }
    disconnect_requested_=false;connect_requested_=true;SetEvent(change_);
}
void Runtime::disconnect() {
    std::lock_guard configuration_lock(configuration_mutex_);
    connect_requested_=false;disconnect_requested_=true;SetEvent(change_);
}
void Runtime::learn() {
    std::lock_guard<std::mutex> lock(mutex_);
    learned_.reset();
    learning_baseline_.clear();
    for (const auto &s : view_.controls)
        learning_baseline_[{s.device, s.control}] = s.value;
    learning_ = true;
    learning_button_ = false;
}
std::optional<Sample> Runtime::learned() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = learned_;
    learned_.reset();
    return result;
}
static Timing timing(std::vector<double> data) {
    Timing out;
    out.count = data.size();
    if (data.empty())
        return out;
    std::sort(data.begin(), data.end());
    out.p50 = data[(data.size() - 1) * 50 / 100];
    out.p95 = data[(data.size() - 1) * 95 / 100];
    out.p99 = data[(data.size() - 1) * 99 / 100];
    out.maximum = data.back();
    return out;
}
void Runtime::run() {
    // All manufacturer acquisition uses the same non-owning boundary. Protocol
    // workers and physical output ownership remain with their own adapters.
    const std::array providers{
        InputProvider::bind(input_,0,max_usb_devices,true),
        InputProvider::bind(ethernet_,max_usb_devices,first_fds_device,true),
        InputProvider::bind(fds_,first_fds_device,first_phidget_device),
        InputProvider::bind(fdstq_,first_phidget_device,first_generic_phidget_device),
        InputProvider::bind(phidget_io_,first_generic_phidget_device,first_cpflight_device),
        InputProvider::bind(cpflight_,first_cpflight_device,max_devices)};

    std::uint64_t phidget_inventory_seen=UINT64_MAX,phidget_io_seen=UINT64_MAX,phidget_hardware_seen=UINT64_MAX,phidget_settings_seen=UINT64_MAX,phidget_output_seen=UINT64_MAX,phidget_overflow_seen{};
    const auto startup_problem = view_.problem;
    Mapper mapper;std::set<std::string,std::less<>> trace_records(active_catalogue().observed_controls.begin(),active_catalogue().observed_controls.end());for(const auto& [id,definition]:active_catalogue().observations){(void)id;trace_records.insert(record_name(definition->record));}for(const auto& [id,output]:active_catalogue().digital_outputs){(void)id;for(const auto& branch:output->branches)for(const auto& operand:branch)trace_records.insert(record_name(operand.record));for(const auto& operand:output->requirements)trace_records.insert(record_name(operand.record));}
    for(const auto key:flightillusion::simulator_interests())mapper.watched_records.insert(record_name(key));
    std::uint64_t fi_sequence{},fi_demand_epoch{};flightillusion::Connection fi_connection;
    flightillusion::ExporterSource fi_exporter;
    std::set<std::string> fi_demanded,fi_blocked;
    flightillusion::Inputs fi_inputs;std::uint64_t fi_ownership_revision{UINT64_MAX};
    auto set_interests=[&](std::vector<RecordKey> interests){const auto& fi=flightillusion::simulator_interests();interests.insert(interests.end(),fi.begin(),fi.end());transport_.interests(interests);};
    FailureControl failures;FailurePipe failure_pipe;failure_pipe.open();
    MotorPipe motor_pipe;motor_pipe.open();MotorPipeV2 motor_pipe_v2;motor_pipe_v2.open();const auto motor_session=std::uint64_t(psx::diagnostics::qpc_now());ActuatorPipe actuator_pipe;actuator_pipe.open();ActuatorControl actuator_control(*this,motor_session);
    ThrustPipe thrust_pipe;const bool thrust_endpoint_available=thrust_pipe.open();ThrustControl thrust_control(motor_session);
    std::array<psx::hwio::motors::v2::Packet,32> motor_replies{};unsigned motor_reply_cursor{};std::uint64_t motor_request_highwater{};
    for(const auto* key:{"Qs120","Qs483","Qi129","Qh398","Qh399","Qh396","Qs425","Qs436","Qh388","Qi119","Qi226","Qh386","Qh58"})mapper.watched_records.insert(key);
    mapper.watched_records.insert("Qi239");
    std::vector<RecordKey> servo_interests;std::uint64_t servo_source_seen=UINT64_MAX,actuator_source_seen=UINT64_MAX;bool servo_baseline_request{};std::uint64_t actuator_demand_epoch{};std::set<std::string> actuator_demanded;
    auto servo_sources_update=[&]{const auto revision=servos_->source_revision(),actuator_revision=actuators_->revision();if(revision==servo_source_seen && actuator_revision==actuator_source_seen)return false;servo_baseline_request=servo_source_seen!=UINT64_MAX;servo_source_seen=revision;actuator_source_seen=actuator_revision;servo_interests.clear();auto names=servos_->source_records();const auto extra=actuators_->source_records();names.insert(names.end(),extra.begin(),extra.end());for(const auto& name:names){RecordKey key;if(parse_key(name,key)){servo_interests.push_back(key);mapper.watched_records.insert(name);}}return true;};
    (void)servo_sources_update(); // Register saved sources before draining startup data.
    const auto random_trim=[] {unsigned char bit{};return BCryptGenRandom(nullptr,&bit,1,BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0?((bit&1)?1:-1):0;};
    std::uint64_t failure_demand_epoch{};
    for(const auto id:{"observe.tla1","observe.tla2","observe.tla3","observe.tla4","observe.fdstq.speedbrake","observe.fdstq.stabtrim","observe.fdstq.trim-power"}){const auto definition=active_catalogue().observations.find(id);if(definition!=active_catalogue().observations.end())mapper.watched_records.insert(record_name(definition->second->record));}
    FlightDisplay flight;
    AxisRouter router;
    ButtonRouter button_router;
    HardwareRouter hardware_router;
    ModuleOutputs module_outputs;
    const ModuleSink module_sink=[this](const ModuleOutput& o){return o.slot>=first_cpflight_device?cpflight_.submit(o):fds_.submit(o);};
    std::uint64_t cpflight_revision_seen=UINT64_MAX,cpflight_overflow_seen{};
    ModuleCompilation module_compilation;
    std::map<std::string,std::string> auto_errors;bool modules_applied=false;std::uint64_t trace_checkpoint{},trace_second=UINT64_MAX;
    std::string identify_previous,identify_profile_previous;std::uint64_t identify_revision_seen=UINT64_MAX,identify_second=UINT64_MAX;
    std::uint64_t fdstq_revision_seen=UINT64_MAX,fdstq_overflow=0;
    DigitalOutputs fdstq_outputs;fdstq_outputs.configure(active_catalogue());
    std::uint64_t fds_revision_seen=UINT64_MAX,module_revision_seen=UINT64_MAX,module_tests_seen=UINT64_MAX;
    CockpitRouter cockpit_router;
    InputOverridePolicy override_policy(active_catalogue());
    std::uint64_t override_revision_seen=UINT64_MAX;
    auto apply_overrides=[&]{
        const auto revision=input_overrides_.revision();if(revision==override_revision_seen)return;
        override_policy.configure(input_overrides_.snapshot());override_revision_seen=revision;
        button_router.overrides(override_policy.blocked_actions(),mapper);
        for(const auto id:button_router.take_override_cancellations())transport_.cancel_binding(id);
        for(const auto id:cockpit_router.overrides(override_policy.blocked_encoders()))transport_.cancel_binding(id);
        for(const auto key:button_router.take_cancellations()){
            transport_.cancel_binding(button_command_id(key));
            const auto r=mapper.records.find(record_name(key));if(r!=mapper.records.end())r->second.effective=r->second.committed;
        }
    };
    auto capture_override=[&](const std::vector<std::string>& actions){
        if(!input_overrides_.capturing())return false;
        auto view=input_overrides_.snapshot();if(!view.capturing)return false;
        std::set<std::string> ids;
        for(const auto& action:actions){const auto id=action.starts_with("encoder:")?override_policy.encoder_control(action.substr(8)):override_policy.button_control(action);if(!id.empty())ids.insert(id);}
        if(ids.empty()){input_overrides_.status("No applied assignment; capture remains armed");return false;}
        view.configuration.controls.insert(ids.begin(),ids.end());std::string error;
        const bool saved=input_overrides_.replace(std::move(view.configuration),error,view.revision);
        input_overrides_.capture(false);
        std::string message=saved?"Blocked: ":"Save failed; identifying input discarded: ";
        for(const auto& id:ids){const auto& controls=override_policy.controls();const auto c=std::find_if(controls.begin(),controls.end(),[&](const auto& v){return v.id==id;});message+=(c==controls.end()?id:c->name)+"; ";}
        if(!saved)message+=error;input_overrides_.status(std::move(message));apply_overrides();return true;
    };
    button_router.capture_handler(capture_override);cockpit_router.capture_handler(capture_override);
    std::uint64_t cockpit_revision_seen=UINT64_MAX;
    std::uint64_t output_test_seen=UINT64_MAX;
    std::uint64_t output_configuration_seen=UINT64_MAX,output_inventory_seen=UINT64_MAX;
    ButtonConfiguration button_configuration;
    auto button_movement = std::make_unique<ButtonMovement>();
    std::uint64_t button_revision_seen = UINT64_MAX;
    auto movement = std::make_unique<AxisMovement>();
    std::uint64_t axis_revision_seen = UINT64_MAX, settings_revision_seen = UINT64_MAX;
    AxisConfiguration axis_configuration;
    std::uint64_t flight_profiles = UINT64_MAX, flight_devices = UINT64_MAX, flight_revision = UINT64_MAX;
    auto log = journal_.producer(2, psx::diagnostics::Component::decoder);
    std::array<MotorFollowingPhase,motor_slots> motor_reported_phase{};std::array<bool,motor_slots> motor_reported_major{};std::uint64_t motor_summary_at{};
    MotorAtDisconnect at_disconnect;std::string at_disconnect_reported;

    std::uint64_t profile_revision_seen = UINT64_MAX, device_revision = UINT64_MAX, input_overflow = 0,
                  network_overflow = 0;
    std::uint64_t ethernet_revision_seen=UINT64_MAX, hardware_revision_seen=UINT64_MAX, ethernet_settings_seen=UINT64_MAX;
    std::uint64_t ethernet_overflow=0,fds_overflow=0,output_fds_revision=UINT64_MAX;
    auto trace_devices=[&]{auto inventory=fds_.inventory();for(const auto& input:fdstq_.devices()){fds::Device d;d.input=input;d.key=input.identity.hardware_key;d.model="TQTQ";d.generic=true;d.baseline=input.connected;inventory.push_back(std::move(d));}return inventory;};
    auto latest=[&](std::size_t d,std::size_t c,Sample& sample){return provider_latest(providers,d,c,sample);};
    std::vector<DeviceInfo> devices; // First discovery must also publish identity/range events.
    auto source_storage = std::make_unique<std::array<std::array<SourceValue, max_controls>, max_devices>>();
    auto sequence_storage = std::make_unique<std::array<std::array<std::uint64_t, max_controls>, max_devices>>();
    auto &values = *source_storage;
    auto &sequences = *sequence_storage;
    auto clear_sources = [&]() {
        for (auto &row : values)
            row.fill(SourceValue{});
        for (auto &row : sequences)
            row.fill(0);
    };
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> runtime_ids;
    std::uint64_t next_runtime_id = 0;
    std::vector<double> mapping_times, send_times, filtered_mapping, filtered_send;
    mapping_times.reserve(4096);
    send_times.reserve(4096);
    filtered_mapping.reserve(4096);
    filtered_send.reserve(4096);
    std::size_t map_cursor = 0, send_cursor = 0, filtered_map_cursor = 0, filtered_send_cursor = 0;
    auto filtered = [&](std::uint64_t id) {
        for (const auto &m : mapper.mappings)
            if (m.binding.id == id)
                return m.binding.mode == MappingMode::axis &&
                       (m.binding.calibration.lowpass_ms > 0 || m.binding.calibration.median_three ||
                        (m.binding.throttle_layout == ThrottleLayout::separate_forward_reverse &&
                         (m.binding.reverse_calibration.lowpass_ms > 0 || m.binding.reverse_calibration.median_three)));
        return false;
    };
    auto add_time = [](std::vector<double> &times, std::size_t &cursor, double value) {
        if (value < 0)
            return;
        if (times.size() < 4096)
            times.push_back(value);
        else {
            times[cursor] = value;
            cursor = (cursor + 1) % 4096;
        }
    };
    const double frequency = static_cast<double>(psx::diagnostics::qpc_frequency());
    std::uint64_t snapshot_sequence = 0;
    std::vector<SourceValue> mapping_sources;
    mapping_sources.reserve(max_controls);
    bool reconfigure = true;
    bool session_changed = true;
    int app_settings_for_cockpit=25;
    AppSettings motion_settings;
    FdstqSpeedFollow tq_speed_follow;bool tq_speed_reserved{};
    std::uint64_t motor_mapping_revision_seen=UINT64_MAX;std::uint64_t feedback_revision=UINT64_MAX,stepper_feedback_revision=UINT64_MAX,actuator_feedback_revision=UINT64_MAX;MotorMappingInputs motor_inputs;MotorMappingDefinitions motor_definitions;bool motor_inputs_changed=true,motor_continuity_lost{};unsigned motor_last_suppressed{};bool motor_last_speed_suppressed{};
    std::uint64_t thrust_refresh_at{};
    auto refresh_thrust=[&](const MotorMappingFrame& frame){
        const auto device=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.slot==first_phidget_device && d.connected;});const auto generation=device==devices.end()?0:device->generation;
        const auto tq=fdstq_.thrust_status();auto released=motor_mappings_->thrust_released_groups(thrust_control.operation(),thrust_control.owns() && !thrust_control.drives());
        for(unsigned n=0;n<2;++n)if(tq[n].drive==MotorDriveState::off && (!thrust_control.owns() || tq[n].operation==thrust_control.operation() || !tq[n].available))released|=1u<<(20+n);
        const auto status=fdstq_thrust_status(motion_settings.fdstq,axis_configuration,mapper,generation,tq,motion_settings.fds_enabled);
        auto capabilities=thrust_frame(motor_mappings_->thrust_status(frame),status,motor_mappings_->definition_revision(),released);
        if(!thrust_endpoint_available)for(auto& l:capabilities.levers){l.flags&=~psx::hwio::thrust_control::ready;constexpr std::string_view reason="HWIO thrust command endpoint unavailable";std::copy(reason.begin(),reason.end(),l.reason.begin());}
        thrust_control.observe(capabilities,psx::diagnostics::qpc_now(),psx::diagnostics::qpc_frequency());
        thrust_refresh_at=GetTickCount64()+(thrust_control.owns()?20:100);
    };
    auto commands = [&](double ms) {
        apply_overrides();
        const auto failure_now=psx::diagnostics::qpc_now();
        failures.tick(mapper,failure_now,psx::diagnostics::qpc_frequency(),random_trim);
        const bool changed_axes=mapper.failed_axes!=failures.axes(),changed_trim=mapper.runaway_trim!=failures.trim();
        if(changed_axes || changed_trim)button_router.failure_state_changed();
        if(changed_axes)transport_.supersede_records({{RecordKind::qs,120}});
        if(changed_trim)transport_.supersede_records({{RecordKind::qh,398},{RecordKind::qh,399},{RecordKind::qh,396}});
        router.update(values, mapper, ms);
        const auto motor_now=GetTickCount64();const auto feedback_changed=motor_bench_->feedback_revision();if(motor_inputs_changed || feedback_changed!=feedback_revision || steppers_->revision()!=stepper_feedback_revision || actuators_->revision()!=actuator_feedback_revision){const auto motors=motor_bench_->feedback_definitions(),steps=steppers_->feedback_definitions();std::vector<MotorFeedbackDefinition> all(motors.begin(),motors.end());all.insert(all.end(),steps.begin(),steps.end());const auto gauges=actuators_->feedback_definitions();all.insert(all.end(),gauges.begin(),gauges.end());motor_feedback_.configure(all,devices,values);feedback_revision=feedback_changed;stepper_feedback_revision=steppers_->revision();actuator_feedback_revision=actuators_->revision();}if(motor_continuity_lost)motor_feedback_.invalidate();motor_feedback_.publish(motor_now);const auto mapping_revision=motor_mappings_->definition_revision();if(motor_inputs_changed || mapping_revision!=motor_mapping_revision_seen){motor_definitions=motor_mappings_->definitions();motor_inputs.configure(motor_definitions,axis_configuration,devices);motor_inputs.baseline(values);motor_mapping_revision_seen=mapping_revision;motor_inputs_changed=false;}if(motor_continuity_lost){motor_inputs.invalidate();motor_continuity_lost=false;}else motor_inputs.refresh(latest);auto motor_frame=motor_inputs.frame(mapper,motor_now);motor_frame.provider_enabled=motion_settings.phidgets_enabled;
        // Main forwards our injections to other clients, not to this socket.
        // A native demand equal to recent physical feedback is still authority.
        // Completed local writes stay in committed/transmitted, never observed.
        motor_frame.ios_owned=thrust_control.owns();motor_frame.ios_drive=thrust_control.drives();motor_frame.ios_token=thrust_control.token();motor_frame.ios_operation=thrust_control.operation();motor_frame.ios_percent=thrust_control.targets();
        for(unsigned i=0;i<motor_axes;++i)if(motor_definitions[i].enabled && fdstq_owns_axis(i,motion_settings.fdstq))motor_mappings_->inhibit(motor_definitions[i].motor,"Axis owned by the FDSTQ assembly; resolve the assignment conflict");motor_mappings_->following_logging(motion_settings.log_level!=LogLevel::off);auto motor_routing=motor_mappings_->tick(motor_frame);
        if(thrust_control.owns()){motor_routing.suppress_throttles=15;motor_routing.feedback_mask=0;}
        mapper.motor_throttle_suppression=motor_routing.suppress_throttles;mapper.motor_speedbrake_suppression=motor_routing.suppress_speedbrake;
        if(motor_last_suppressed!=motor_routing.suppress_throttles || motor_last_speed_suppressed!=motor_routing.suppress_speedbrake){std::vector<RecordKey> changed;if(motor_last_suppressed!=motor_routing.suppress_throttles)changed.push_back({RecordKind::qs,436});if(motor_last_speed_suppressed!=motor_routing.suppress_speedbrake)changed.push_back({RecordKind::qh,388});transport_.supersede_axes(changed);motor_last_suppressed=motor_routing.suppress_throttles;motor_last_speed_suppressed=motor_routing.suppress_speedbrake;}
        auto motor_command=motor_throttle_feedback(mapper,motor_routing,psx::diagnostics::qpc_now());
        if(at_disconnect.epoch()!=mapper.epoch){
            if(at_disconnect.motors())log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,"Motor A/T disconnect episode discarded at PSX session boundary; no stale pulse replay");
            at_disconnect.boundary(mapper.epoch);
        }
        for(unsigned n=0;n<motor_slots;++n)if(motor_routing.fault_mask&(1u<<n)){const auto cause="Motor "+std::to_string(n)+": "+motor_routing.fault_reasons[n]+((motor_routing.disconnect_faults&(1u<<n))?"; A/T disconnect requested":"; drive release requested");log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,cause);}
        if(at_disconnect.observe_inactive(mapper.epoch,motor_frame.psx_ready && motor_frame.authority==AtAuthority::physical,mapper.at_inactive_sequence))
            transport_.cancel_binding(at_disconnect.binding());
        // Retire the preceding episode before admitting a new motor fault from
        // this batch; an old inactive edge must not consume the new disconnect.
        if(motor_routing.disconnect_at)at_disconnect.request(motor_routing.disconnect_faults,mapper.epoch,mapper.at_inactive_sequence);
        std::optional<Command> motor_disconnect;
        if(at_disconnect.stage()==MotorAtDisconnect::Stage::pending)motor_disconnect=at_disconnect.next(mapper,button_action("qh386.left.1"),motor_now,psx::diagnostics::qpc_now());
        const auto delivery=at_disconnect.status();
        if(at_disconnect.motors())motor_mappings_->at_disconnect_delivery(at_disconnect.motors(),delivery);
        if(delivery!=at_disconnect_reported){if(!delivery.empty())log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,delivery);at_disconnect_reported=delivery;}
        const auto motor_views=motor_mappings_->views();
        for(unsigned n=0;n<motor_slots;++n){unsigned master=motor_axes;for(unsigned a=0;a<motor_axes;++a)if(motor_definitions[a].enabled && motor_definitions[a].motor==n){master=a;break;}if(master==motor_axes)continue;
            const auto& state=motor_views[master];const bool transition=state.phase!=motor_reported_phase[n] || state.major_fault!=motor_reported_major[n];
            if(transition || motor_now>=motor_summary_at){const auto physical=motor_bench().snapshot(n).live;const auto& sensor=motor_frame.sensors[master];std::ostringstream text;text<<std::setprecision(7)<<"Motor "<<n<<" phase "<<unsigned(state.phase)<<": AT="<<unsigned(motor_frame.authority)<<" off="<<motor_frame.at_inactive_sequence<<" rt="<<state.retarget_kind<<" dir="<<state.request_direction<<" c/l/r="<<state.pending_command<<'/'<<state.pending_limits<<'/'<<state.pending_release<<" ev="<<motor_drive_event_name(state.drive_event)<<" stop="<<state.stop_reason<<" svc="<<state.correction_service_ms<<" change="<<state.target_change<<" D="<<state.demand<<" A="<<state.target_anchor<<" E="<<state.arrival_band<<" edge="<<state.endpoint_edge<<" dir="<<state.request_direction<<" id="<<physical.target_command_id<<" P="<<sensor.raw<<"; "<<state.status<<"; encoder="<<physical.position<<" target="<<physical.target<<" sensor_raw="<<sensor.raw<<" sensor_fraction="<<sensor.fraction<<" accepted="<<physical.target_command_id<<" accepted_ms="<<physical.target_accepted_ms<<" expected_seq="<<physical.expected_sequence<<" expected_ms="<<physical.expected_observed_ms<<" sensor_ms="<<sensor.sample_ms<<" encoder_observed_ms="<<physical.observed_ms<<" encoder_checked_ms="<<physical.checked_ms<<" authority="<<unsigned(motor_frame.authority)<<" arm="<<(motor_frame.at_arm_valid?(motor_frame.at_armed?1:0):-1)<<" release_confirmed="<<state.release_confirmed;log.emit(psx::diagnostics::Code::message,state.major_fault?psx::diagnostics::Severity::warning:psx::diagnostics::Severity::info,{},psx::diagnostics::ErrorDomain::none,physical.fault_code,text.str());}
            motor_reported_phase[n]=state.phase;motor_reported_major[n]=state.major_fault;
        }if(motor_now>=motor_summary_at)motor_summary_at=motor_now+500;

        const auto tq_device=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.slot==first_phidget_device && d.connected;});
        const auto tq_generation=tq_device==devices.end()?0:tq_device->generation;
        const FdstqThrust ios_thrust{thrust_control.owns(),thrust_control.drives(),thrust_control.token(),thrust_control.operation(),thrust_control.targets()};
        auto tq_motion=fdstq_motion(motion_settings.fdstq,axis_configuration,mapper,tq_generation,&ios_thrust);
        if(!(fdstq_.motor_available()&4))tq_motion.enabled[2]=false;
        const auto& speed_source=values[first_phidget_device][10];
        bool reserve_speed=false;const auto speed_definition=active_catalogue().observations.find("observe.fdstq.speedbrake");
        if(speed_definition!=active_catalogue().observations.end()){
            const auto record=mapper.records.find(record_name(speed_definition->second->record));double observed{};std::string_view text;
            if(record!=mapper.records.end() && observation_value(*speed_definition->second,record->second.observed,observed,text))reserve_speed=tq_speed_follow.update(tq_motion,speed_source.available,speed_source.value,static_cast<int>(observed),record->second.sequence,mapper.epoch);
            else tq_speed_follow.reset();
        }else tq_speed_follow.reset();
        if(reserve_speed!=tq_speed_reserved){tq_speed_reserved=reserve_speed;if(const auto* definition=flight_target(8))transport_.supersede_axes({definition->descriptor.record});if(motion_settings.fds_enabled)fds_.trace.record(fds::TraceKind::routing,static_cast<std::uint32_t>(first_phidget_device),tq_generation,reserve_speed?"FDSTQ speedbrake motor reserves axis":"FDSTQ speedbrake motor releases axis");}
        if(reserve_speed)for(auto& m:mapper.mappings)if(m.externally_calculated && m.binding.target.id==axis_target(8).id){m.state.available=false;m.state.dirty=false;m.state.reason="FDSTQ speedbrake motor follows PSX";}
        if(stop_ || (!mapper.ready && !thrust_control.owns()) || !tq_generation){tq_speed_follow.reset();tq_motion={};}
        fdstq_.motion(tq_motion);
        if(motor_now>=thrust_refresh_at)refresh_thrust(motor_frame);
        cockpit_router.update(values,mapper,ms,app_settings_for_cockpit);
        if (router.authority_changed()) {
            std::vector<RecordKey> records;
            for (auto &m : mapper.mappings)
                if (m.externally_calculated) {
                    records.push_back(m.binding.target.record);
                    m.state.dirty = true;
                    auto r = mapper.records.find(record_name(m.binding.target.record));
                    if (r != mapper.records.end())
                        r->second.effective = r->second.committed;
                }
            transport_.supersede_axes(records);
        }
        failures.reconcile(mapper,button_router,failure_now);
        for (std::size_t count = 0; count < max_bindings + 1024; ++count) {
            auto command = failures.next();
            if(!command && motor_disconnect){command=std::move(motor_disconnect);motor_disconnect.reset();}
            if(!command && motor_command){command=std::move(motor_command);motor_command.reset();}
            if(!command)command=mapper.next(ms, psx::diagnostics::qpc_now());
            if (!command) command = button_router.next(values,mapper,ms,psx::diagnostics::qpc_now());
            if (!command) command = cockpit_router.next(mapper,ms,psx::diagnostics::qpc_now());
            for(const auto key:button_router.take_cancellations()) {
                transport_.cancel_binding(button_command_id(key));
                const auto record=mapper.records.find(record_name(key));
                if(record!=mapper.records.end())record->second.effective=record->second.committed;
            }
            if (!command)
                break;
            if(thrust_control.owns() && command->line.starts_with("Qs436="))continue;
            if(!failures.filter(*command,mapper))continue;
            if (command->input_qpc) {
                const bool f = filtered(command->binding);
                add_time(f ? filtered_mapping : mapping_times, f ? filtered_map_cursor : map_cursor,
                         (command->mapped_qpc - command->input_qpc) * 1000.0 / frequency);
            }
            if(modules_applied)fds_.trace.record(fds::TraceKind::routing,0,mapper.epoch,command->line);
            const bool tq_speed_write=fdstq_speed_write(axis_configuration,mapper,*command);
            const auto tq_speed_value=command->value;
            const bool disconnect_command=at_disconnect.owns(command->binding,command->epoch);
            if (!transport_.submit(std::move(*command))) {
                if(disconnect_command){at_disconnect.submitted(false);log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,"Motor A/T disconnect submission failed; request retained, no completed-send claim");}
                mapper.ready = false;
                break;
            }
            if(disconnect_command){at_disconnect.submitted(true);log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,"Motor A/T disconnect submitted to transport");}
            if(tq_speed_write)tq_speed_follow.written(tq_speed_value);
        }
    };
    while (!stop_) {
        const auto now = psx::diagnostics::qpc_now();
        const double ms = now * 1000.0 / frequency;
        std::uint64_t revision, display_revision, axis_revision, settings_revision, button_revision,cockpit_revision;
        AppSettings app_settings;
        bool filters_changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            app_settings = settings_;
            settings_revision = settings_revision_;
            revision = revision_;
            display_revision = display_revision_;
            axis_revision = axis_revision_;
            button_revision = button_revision_;
            cockpit_revision=cockpit_revision_;
        }
        if(app_settings.cpflight.enabled){const auto cp=cpflight_.snapshot();if(cp.connected && !cp.serial.empty() && (app_settings.cpflight.serial.empty() || (app_settings.cpflight.serial==cp.serial && app_settings.cpflight.port.com_name!=cp.com_name))){auto pinned=app_settings;pinned.cpflight.serial=cp.serial;for(const auto& p:cp.ports)if(p.com_name==cp.com_name && p.instance_id==pinned.cpflight.port.instance_id)pinned.cpflight.port=p;std::string reason;if(save_settings(pinned,reason))app_settings=pinned;else {app_settings.cpflight.enabled=false;cpflight_.configure(app_settings.cpflight);std::lock_guard lock(mutex_);view_.problem="Cannot pin CPflight identity: "+reason;}}}
        motion_settings=app_settings;
        app_settings_for_cockpit=app_settings.smoothing_ms;
        std::uint64_t hardware_revision,modules_revision,module_ui_revision;
        {std::lock_guard lock(mutex_);hardware_revision=hardware_revision_;modules_revision=module_applied_revision_;module_ui_revision=module_revision_;}
        const bool hardware_changed=hardware_revision!=hardware_revision_seen;
        if(!app_settings.network_selection_initialized){
            if(const auto inventory=ethernet_.adapters.snapshot();inventory && inventory->error.empty()){
                auto initial=app_settings;initial.network_interface=psx::network::initial_adapter(*inventory);
                initial.network_selection_initialized=true;std::string error;
                if(save_settings(initial,error))app_settings=initial;
            }
        }
        if(hardware_revision!=hardware_revision_seen || settings_revision!=ethernet_settings_seen){
            ethernet_.configure(app_settings.network_interface,hardware(),app_settings.pokeys_enabled);
            clear_module_tests();fds_.enabled(app_settings.fds_enabled);configure_phidgets(app_settings);hardware_revision_seen=hardware_revision;ethernet_settings_seen=settings_revision;
            fdstq_.configure(app_settings.fdstq,app_settings.fds_enabled);
            fds_.configure(app_settings.fds_api_path,app_settings.network_interface,disabled_fds(hardware()));
        }
        if (revision != profile_revision_seen || input_.metadata_revision != device_revision || ethernet_.metadata_revision!=ethernet_revision_seen || fds_.metadata_revision!=fds_revision_seen || fdstq_.metadata_revision!=fdstq_revision_seen || cpflight_.metadata_revision!=cpflight_revision_seen || modules_revision!=module_revision_seen)
            reconfigure = true;
        if(!reconfigure && modules_applied && (hardware_changed || axis_revision_seen!=axis_revision || cockpit_revision_seen!=cockpit_revision || button_revision_seen!=button_revision)) {
            auto current_axes=std::make_unique<AxisConfiguration>(axes());
            auto current=compile_module_devices(module_library_,modules(),module_inventory(),active_catalogue(),buttons(),cockpit(),hardware(),profiles(),current_axes.get());
            if(current.devices!=module_compilation.devices)reconfigure=true;
            else module_compilation=std::move(current);
        }
        {const auto inventory=phidgets_.inventory();if(inventory.revision!=phidget_inventory_seen || hardware_revision!=phidget_hardware_seen || settings_revision!=phidget_settings_seen){
            std::vector<std::string> reserved;if(app_settings.fds_enabled && app_settings.fdstq.enabled && app_settings.fdstq.serial>0){const auto& tq=app_settings.fdstq;reserved.push_back("phidget:"+std::string(tq.remote?tq.host+":"+std::to_string(tq.port):"local")+":"+std::to_string(tq.serial)+":-1");}
            phidget_io_.configure(inventory.channels,hardware(),app_settings.phidgets_enabled,std::move(reserved));phidget_inventory_seen=inventory.revision;phidget_hardware_seen=hardware_revision;phidget_settings_seen=settings_revision;
        }if(phidget_io_seen!=phidget_io_.metadata_revision){phidget_io_seen=phidget_io_.metadata_revision;reconfigure=true;}}
        if (reconfigure && (app_settings.fds_enabled || app_settings.cpflight.enabled)) {
            const auto cards=module_inventory();const auto assigned=modules();
            for(const auto& choice:module_device_auto_choices(module_library_,assigned,cards)){const auto key=choice.category+"/"+choice.role;std::string error;if(assign_module_role(choice.category,choice.role,choice.device,choice.profile,true,error))auto_errors.erase(key);else auto_errors[key]=error;}
        }
        if (reconfigure) {
            filters_changed = true;
            transport_.cancel_all();
            button_router.release_all(mapper);
            for (auto &m : mapper.mappings)
                if (m.state.held)
                    mapper.lost(m.device);
            commands(ms);
            tq_speed_follow.reset();
            transport_.drain(100);
            for (auto &record : mapper.records)
                record.second.effective = record.second.committed;
            const auto previous_devices = devices;
            devices.clear();
            for(const auto& provider:providers)provider.append_devices(devices);
            cpflight_revision_seen=cpflight_.metadata_revision;
            fdstq_revision_seen=fdstq_.metadata_revision;
            fds_revision_seen=fds_.metadata_revision;module_revision_seen=modules_revision;
            ethernet_revision_seen=ethernet_.metadata_revision;
            for (const auto &d : devices) {
                const auto old = std::find_if(previous_devices.begin(), previous_devices.end(),
                                              [&](const auto &x) { return x.slot == d.slot; });
                if (old != previous_devices.end() && old->generation == d.generation && old->connected == d.connected)
                    continue;
                log.emit(psx::diagnostics::Code::configuration,
                         d.connected ? psx::diagnostics::Severity::info : psx::diagnostics::Severity::warning, {},
                         psx::diagnostics::ErrorDomain::none, 0,
                         (d.connected ? "Device recognized: " : "Device disconnected: ") + d.identity.name,
                         {d.slot, d.generation, d.identity.vendor, d.identity.product});
                if (d.connected)
                    for (const auto &c : d.controls) {
                        if (c.key.kind != ControlKind::axis)
                            continue;
                        const std::string detail = c.name + " raw range=" + std::to_string(c.minimum) + ".." +
                                                   std::to_string(c.maximum) + "; bits=" + std::to_string(c.bits);
                        log.emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::info, {},
                                 psx::diagnostics::ErrorDomain::none, 0, detail, {d.slot, d.generation});
                    }
            }
            device_revision = input_.metadata_revision;
            profile_revision_seen = revision;
            auto saved = profiles();
            button_configuration = buttons();
            axis_configuration=axes();
            const auto module_configuration=modules();
            modules_applied=std::any_of(module_configuration.rows.begin(),module_configuration.rows.end(),[](const auto& row){return !row.second.applied.profile.empty();});
            module_compilation=compile_module_devices(module_library_,module_configuration,module_inventory(),active_catalogue(),button_configuration,cockpit(),hardware(),saved,&axis_configuration);
            button_configuration=module_compilation.buttons;
            module_outputs.configure_devices(module_library_,modules(),module_compilation,module_inventory(),active_catalogue(),module_sink);
            if(app_settings.fds_enabled)fds_.trace.record(fds::TraceKind::context,0,mapper.epoch,fds::context_encode(modules(),buttons(),cockpit(),trace_devices()));
            module_tests_seen=UINT64_MAX;clear_module_tests();
            mapper.mappings.clear();
            std::vector<DeviceIdentity> identities;
            std::vector<DeviceInfo> connected;
            for (const auto &d : devices)
                if (d.connected && !d.controls.empty()) {
                    identities.push_back(d.identity);
                    connected.push_back(d);
                }
            std::set<std::uint16_t> assigned;
            std::string problem = startup_problem;
            const auto conflicts = ownership_conflicts(saved);
            for (const auto &p : saved)
                for (const auto &b : p.bindings)
                    if (conflicts.count({p.id, b.id}))
                        problem += "Conflicting field ownership withheld: " + p.name + " / " + b.name + "; ";
            for (const auto &p : saved) {
                const int match = match_profile(p, identities);
                if (match == -2) {
                    problem = "Ambiguous device for " + p.name + "; assign a device explicitly";
                    continue;
                }
                if (match < 0)
                    continue;
                const auto &d = connected[static_cast<std::size_t>(match)];
                if (!assigned.insert(d.slot).second) {
                    problem = "Device assignment conflict for " + p.name;
                    continue;
                }
                for (const auto &b : p.bindings) {
                    if (!b.enabled || catalogue_target_missing(b.target) || axis_replaces(axis_configuration, p.id, b.id) || button_replaces(button_configuration,p.id,b.id))
                        continue;
                    if (conflicts.count({p.id, b.id}))
                        continue;
                    if (mapper.mappings.size() >= max_bindings) {
                        problem = "Configured binding count exceeds 512";
                        break;
                    }
                    Mapped m;
                    m.binding = b;
                    m.profile = p.id;
                    m.local_id = b.id;
                    m.device = d.slot;
                    m.generation = d.generation;
                    bool found = true;
                    const auto identity = std::make_pair(p.id, b.id);
                    auto rid = runtime_ids.find(identity);
                    if (rid == runtime_ids.end())
                        rid = runtime_ids.emplace(identity, ++next_runtime_id).first;
                    m.binding.id = rid->second;
                    auto sources = b.sources;
                    if (b.target.throttle_engine >= 0 && b.throttle_layout == ThrottleLayout::separate_forward_reverse)
                        sources.push_back(b.reverse_source);
                    for (const auto &source : sources) {
                        const DeviceInfo *source_device = &d;
                        if (!source.profile.empty() && source.profile != p.id) {
                            auto source_profile = std::find_if(saved.begin(), saved.end(), [&](const auto &item) {
                                return item.id == source.profile;
                            });
                            if (source_profile == saved.end()) {
                                found = false;
                                break;
                            }
                            const int source_match = match_profile(*source_profile, identities);
                            if (source_match < 0) {
                                found = false;
                                break;
                            }
                            source_device = &connected[static_cast<std::size_t>(source_match)];
                        }
                        auto c = std::find_if(source_device->controls.begin(), source_device->controls.end(),
                                              [&](const auto &item) { return same(source.key, item.key); });
                        if (c == source_device->controls.end()) {
                            found = false;
                            break;
                        }
                        m.controls.push_back(static_cast<std::uint16_t>(c - source_device->controls.begin()));
                        m.source_devices.push_back(source_device->slot);
                        m.source_generations.push_back(source_device->generation);
                    }
                    if (found)
                        mapper.mappings.push_back(std::move(m));
                    else
                        problem = "Stored HID source absent in " + p.name;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                view_.problem = problem;
            }
            // Preserve unaffected devices' samples and candidate filter histories on hotplug.
            for (std::size_t slot = 0; slot < max_devices; ++slot) {
                const auto old = std::find_if(previous_devices.begin(), previous_devices.end(),
                                              [&](const auto &d) { return d.connected && d.slot == slot; });
                const auto current = std::find_if(devices.begin(), devices.end(),
                                                  [&](const auto &d) { return d.connected && d.slot == slot; });
                if (old == previous_devices.end() || current == devices.end() ||
                    old->generation != current->generation) {
                    values[slot].fill(SourceValue{});
                    sequences[slot].fill(0);
                }
            }
            reconfigure = false;
            session_changed = true;
            axis_revision_seen = UINT64_MAX;
            button_revision_seen = UINT64_MAX;
            cockpit_revision_seen=UINT64_MAX;
            std::vector<RecordKey> interests;
            for (auto id = runtime_ids.begin(); id != runtime_ids.end();) {
                const bool active = std::any_of(mapper.mappings.begin(), mapper.mappings.end(), [&](const auto &m) {
                    return m.profile == id->first.first && m.local_id == id->first.second;
                });
                if (active)
                    ++id;
                else
                    id = runtime_ids.erase(id);
            }
            for (const auto &m : mapper.mappings)
                interests.push_back(m.binding.target.record);
            const auto button_interests = button_router.interests();
            interests.insert(interests.end(),button_interests.begin(),button_interests.end());
            interests.insert(interests.end(),servo_interests.begin(),servo_interests.end());
            set_interests(interests);
            log.emit(psx::diagnostics::Code::configuration, psx::diagnostics::Severity::info, {},
                     psx::diagnostics::ErrorDomain::none, 0, "HWIO device/profile assignment refreshed");
        }
        if (axis_revision_seen != axis_revision) {
            cockpit_revision_seen=UINT64_MAX;
            filters_changed = true;
            axis_configuration = axes();
            std::vector<RecordKey> affected;
            for (const auto &m : mapper.mappings)
                if (m.externally_calculated || axis_replaces(axis_configuration, m.profile, m.local_id))
                    affected.push_back(m.binding.target.record);
            transport_.supersede_axes(affected);
            for (const auto &key : affected) {
                auto r = mapper.records.find(record_name(key));
                if (r != mapper.records.end())
                    r->second.effective = r->second.committed;
            }
            mapper.mappings.erase(std::remove_if(mapper.mappings.begin(), mapper.mappings.end(),
                                                 [&](const auto &m) {
                                                     return !m.externally_calculated &&
                                                            axis_replaces(axis_configuration, m.profile, m.local_id);
                                                 }),
                                  mapper.mappings.end());
            router.configure(axis_configuration, devices, mapper);motor_inputs_changed=true;
            axis_revision_seen = axis_revision;
            std::vector<RecordKey> interests;
            for (const auto &m : mapper.mappings)
                interests.push_back(m.binding.target.record);
            const auto button_interests = button_router.interests();
            interests.insert(interests.end(),button_interests.begin(),button_interests.end());
            interests.insert(interests.end(),servo_interests.begin(),servo_interests.end());
            set_interests(interests);
        }
        if (button_revision_seen != button_revision) {
            button_configuration = buttons();
            module_compilation=compile_module_devices(module_library_,modules(),module_inventory(),active_catalogue(),button_configuration,cockpit(),hardware(),profiles(),&axis_configuration);
            button_configuration=module_compilation.buttons;
            const auto changed=button_router.changed_records(button_configuration);
            for(const auto key:changed){transport_.cancel_binding(button_command_id(key));for(const auto& a:button_catalogue())if(a.target.record==key)transport_.cancel_binding(button_control_command_id(a.id));}
            button_router.retire(changed,mapper);
            commands(ms);
            if(!changed.empty())transport_.drain(100);
            button_router.configure(button_configuration,devices,mapper);
            override_policy=InputOverridePolicy(active_catalogue());override_revision_seen=UINT64_MAX;apply_overrides();
            button_router.baseline(values,mapper);
            button_revision_seen = button_revision;
            auto interests = button_router.interests();
            for (const auto &m : mapper.mappings) interests.push_back(m.binding.target.record);
            interests.insert(interests.end(),servo_interests.begin(),servo_interests.end());
            set_interests(interests);
        }
        if (flight_profiles != revision || flight_devices != device_revision || flight_revision != display_revision) {
            filters_changed = true;
            flight.configure(profiles(), devices, display_assignments());
            flight_profiles = revision;
            flight_devices = device_revision;
            flight_revision = display_revision;
        }
        if(cockpit_revision_seen!=cockpit_revision){
            for(const auto id:cockpit_router.command_ids())transport_.cancel_binding(id);
            cockpit_router.configure(modules_applied?module_compilation.cockpit:cockpit(),devices,mapper,module_compilation.devices);cockpit_revision_seen=cockpit_revision;
            override_policy=InputOverridePolicy(active_catalogue());override_revision_seen=UINT64_MAX;apply_overrides();
            auto interests=cockpit_router.interests();for(const auto& m:mapper.mappings)interests.push_back(m.binding.target.record);
            const auto buttons=button_router.interests();interests.insert(interests.end(),buttons.begin(),buttons.end());set_interests(interests);
        }
        if (filters_changed || settings_revision != settings_revision_seen) {
            trim_displays_.configure(app_settings.trim_displays);
            cpflight_.configure(app_settings.cpflight);
            for (auto &m : mapper.mappings)
                flight_filter(m.binding, app_settings.smoothing_ms);
            router.filter_interval(app_settings.smoothing_ms);
            flight.filter_interval(app_settings.smoothing_ms);
            settings_revision_seen = settings_revision;
        }
        if ((phidget_io_.overflow!=phidget_overflow_seen || cpflight_.overflow!=cpflight_overflow_seen) || input_.overflow != input_overflow || ethernet_.overflow!=ethernet_overflow || fds_.overflow!=fds_overflow || fdstq_.overflow!=fdstq_overflow || transport_.overflow != network_overflow || mapper.overflow || button_router.overflow || cockpit_router.overflow) {
            const bool physical_loss=(phidget_io_.overflow!=phidget_overflow_seen || cpflight_.overflow!=cpflight_overflow_seen) || input_.overflow!=input_overflow || ethernet_.overflow!=ethernet_overflow || fds_.overflow!=fds_overflow || fdstq_.overflow!=fdstq_overflow;
            if(physical_loss)motor_continuity_lost=true;
            input_overflow = input_.overflow;
            ethernet_overflow=ethernet_.overflow;
            fds_overflow=fds_.overflow;fdstq_overflow=fdstq_.overflow;phidget_overflow_seen=phidget_io_.overflow;cpflight_overflow_seen=cpflight_.overflow;
            network_overflow = transport_.overflow;
            transport_.cancel_all();
            button_router.release_all(mapper);
            commands(ms);
            transport_.drain(100);
            transport_.resynchronize();
            mapper.boundary(mapper.epoch + 1);
            button_router.boundary(mapper.epoch);
            cockpit_router.boundary(mapper.epoch);
            clear_sources();
            session_changed = true;
            for(const auto& provider:providers)provider.discard_batch();
            log.emit(psx::diagnostics::Code::events_lost, psx::diagnostics::Severity::error, {},
                     psx::diagnostics::ErrorDomain::application, 0,
                     "Queue overflow: pending work discarded; reconnect and fresh baseline");
        }
        if(servo_sources_update()){std::vector<RecordKey> interests=servo_interests;for(const auto& m:mapper.mappings)interests.push_back(m.binding.target.record);const auto buttons=button_router.interests();interests.insert(interests.end(),buttons.begin(),buttons.end());set_interests(interests);}
        // bang is the documented read-only snapshot request, not a Q-variable write.
        if(servo_baseline_request && mapper.ready){Command query;query.epoch=mapper.epoch;query.event=true;query.line="bang\n";if(transport_.submit(std::move(query)))servo_baseline_request=false;}
        NetworkEvent event;
        const auto network_slice=GetTickCount64();unsigned network_serviced{};
        while (network_serviced<64 && (network_serviced==0 || GetTickCount64()-network_slice<8) && transport_.incoming.pop(event)) {
            ++network_serviced;
            if (event.kind == NetworkEventKind::boundary || event.kind == NetworkEventKind::load_boundary) {
                const bool loading=event.kind==NetworkEventKind::load_boundary;
                failures.boundary("PSX load/loss boundary");failure_demand_epoch=0;
                if(app_settings.fds_enabled)fds_.trace.record(fds::TraceKind::session,0,event.epoch,loading?"load-boundary":"boundary",static_cast<std::uint64_t>(event.qpc));
                mapper.boundary(event.epoch,loading);
                button_router.boundary(event.epoch,loading);
                cockpit_router.boundary(event.epoch);
                session_changed = true;
                if(!loading)clear_sources();
                for(const auto& provider:providers)provider.discard_batch();;
            } else if (event.epoch == mapper.epoch) {
                if (event.kind == NetworkEventKind::line) {
                    if(app_settings.fds_enabled){const std::string_view captured(event.text.data());const auto key=captured.substr(0,captured.find('='));if(trace_records.count(key))fds_.trace.record(fds::TraceKind::psx_record,0,event.epoch,captured,static_cast<std::uint64_t>(event.qpc));}
                    mapper.receive(event.text.data(), event.qpc);
                    const std::string_view line(event.text.data());
                    button_router.received(line.substr(0,line.find('=')));
                } else if (event.kind == NetworkEventKind::ready) {
                    if(app_settings.fds_enabled)fds_.trace.record(fds::TraceKind::session,0,event.epoch,"ready",static_cast<std::uint64_t>(event.qpc));
                    mapper.ready = true;
                    session_changed = true;
                } else if (event.kind == NetworkEventKind::unavailable) {
                    mapper.records.erase(event.text.data());
                    for (auto &m : mapper.mappings)
                        if (record_name(m.binding.target.record) == event.text.data()) {
                            m.state.reason = "PSX target exceeds bounded record size";
                            m.state.pending = false;
                        }
                } else if (event.kind == NetworkEventKind::sent) {
                    if(app_settings.fds_enabled)fds_.trace.record(fds::TraceKind::session,0,event.epoch,std::string("sent ")+event.text.data(),static_cast<std::uint64_t>(event.qpc));
                    if(at_disconnect.completed(event.binding,event.epoch)){log.emit(psx::diagnostics::Code::message,psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,"Motor A/T disconnect completed outgoing transport send; PSX application not acknowledged");}
                    mapper.sent_record(event.text.data(), event.qpc);
                    button_router.sent(event.text.data(),event.binding,event.qpc);
                    if (event.input_qpc) {
                        const bool f = filtered(event.binding);
                        add_time(f ? filtered_send : send_times, f ? filtered_send_cursor : send_cursor,
                                 (event.qpc - event.input_qpc) * 1000.0 / frequency);
                    }
                }
            }
        }
        // Initial PSX synchronization must not starve physical acquisition or
        // the motor ownership heartbeat. Preserve queued record order.
        if(network_serviced>=64 || GetTickCount64()-network_slice>=8)SetEvent(change_);
        const auto release_connection=[&](const char* reason){
            failures.clear(reason);button_router.release_all(mapper);
            for(auto& mapping:mapper.mappings)if(mapping.state.held)mapper.lost(mapping.device);
            commands(ms);transport_.drain(100);mapper.ready=false;
        };
        if(disconnect_requested_.exchange(false)){release_connection("HWIO Stop");transport_.disconnect();}
        // The latest Connect survives a Stop's bounded release/drain interval.
        if(connect_requested_.exchange(false)){release_connection("PSX connection changed");const auto [host,port]=endpoint();transport_.connect(host,port);}
        // Demand subscriptions are session controls, kept separate from coalesced axis records.
        if(mapper.ready && failures.bound() && failure_demand_epoch!=mapper.epoch){
            if(transport_.submit({FailureControl::binding_id,mapper.epoch,0,now,"demand=Qs483\n",true}))failure_demand_epoch=mapper.epoch;
        }
        for(unsigned serviced=0;serviced<8;++serviced){
            const auto* request=failure_pipe.poll();
            if(failure_pipe.take_loss())failures.detach("IOS pipe disconnected");
            if(!request)break;
            const auto [host,port]=endpoint();
            if(connect_requested_ || disconnect_requested_){auto reply=failures.status(mapper,host,port,now,psx::diagnostics::qpc_frequency());reply.operation=request->operation;reply.request_id=request->request_id;reply.result=psx::hwio::failures::Result::unavailable;reply.data.flags&=~(psx::hwio::failures::ready|psx::hwio::failures::bound);failure_pipe.reply(reply);continue;}
            const int direction=request->operation==psx::hwio::failures::Operation::set && request->data.trim && !failures.trim()?random_trim():0;
            failure_pipe.reply(failures.dispatch(*request,mapper,host,port,now,psx::diagnostics::qpc_frequency(),direction));
        }
        auto service_motor_pipe=[&]<class Packet>(MotorPipeChannel<Packet>& pipe){for(unsigned serviced=0;serviced<4;++serviced){
            namespace m=psx::hwio::motors;const auto* p=pipe.poll();if(!p)break;
            auto reply=motor_mappings().motor_status_v2(motor_session);reply.operation=p->operation;reply.request_id=p->request_id;reply.motor=p->motor;reply.operation_id=p->request_id;
            const auto [host,port]=endpoint();reply.port=port;std::copy_n(host.begin(),std::min(host.size(),reply.host.size()-1),reply.host.begin());
            if(!motor_wire_valid(*p) || (p->operation!=m::Operation::status && !p->request_id))reply.result=m::Result::malformed;
            else if(p->operation!=m::Operation::status && p->motor>=p->motors.size())reply.result=m::Result::malformed;
            else if(p->operation!=m::Operation::status){
                const auto duplicate=std::find_if(motor_replies.begin(),motor_replies.end(),[&](const auto& r){return r.request_id==p->request_id && r.session==p->session;});
                if(duplicate!=motor_replies.end()){if(duplicate->operation!=p->operation || duplicate->motor!=p->motor || duplicate->fault_revision!=p->fault_revision || duplicate->revision!=p->revision)reply.result=m::Result::conflict;else {reply.result=duplicate->result;reply.operation_id=duplicate->operation_id;reply.fault_revision=duplicate->fault_revision;reply.message=duplicate->message;}}
                else if(p->session!=motor_session)reply.result=m::Result::session_mismatch;
                else if(p->request_id<=motor_request_highwater)reply.result=m::Result::conflict;
                else if(p->motor>=motor_slots)reply.result=m::Result::malformed;
                else if(p->revision!=reply.revision || (p->operation==m::Operation::reset_home && p->fault_revision!=reply.motors[p->motor].fault_revision))reply.result=m::Result::conflict;
                else {motor_request_highwater=p->request_id;std::string error;if(p->operation==m::Operation::stop)motor_mappings().stop(p->motor,p->request_id);else if(!motor_mappings().reset_home(p->motor,p->fault_revision,p->request_id,error)){reply.result=m::Result::unavailable;std::copy_n(error.begin(),std::min(error.size(),reply.message.size()-1),reply.message.begin());}
                    reply.fault_revision=p->fault_revision;motor_replies[motor_reply_cursor++%motor_replies.size()]=reply;}
            }pipe.reply(motor_packet<Packet>(reply));
        }};service_motor_pipe(motor_pipe);service_motor_pipe(motor_pipe_v2);
        auto map_sources = [&](Mapped &m, bool edge, bool baseline, bool relative) {
            mapping_sources.clear();
            for (std::size_t i = 0; i < m.controls.size(); ++i)
                mapping_sources.push_back(values[m.source_devices[i]][m.controls[i]]);
            mapper.input(m, mapping_sources, ms, edge, baseline, relative);
        };
        auto process = [&](const Sample &sample, bool edge) {
            if (sample.device >= max_devices || sample.control >= max_controls)
                return;
            const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto &d) {
                return d.slot == sample.device && d.connected && d.generation == sample.generation;
            });
            if (device == devices.end())
                return;
            if (sample.control >= device->controls.size())
                return;
            if(sample.device>=first_fds_device && sample.device<=first_phidget_device && app_settings.fds_enabled){std::ostringstream captured;captured<<sample.device<<' '<<sample.control<<' '<<sample.generation<<' '<<sample.sequence<<' '<<sample.qpc<<' '<<sample.value<<' '<<sample.baseline<<' '<<sample.relative<<' '<<sample.invalid<<' '<<edge;fds_.trace.record(fds::TraceKind::input,sample.device,sample.generation,captured.str());}
            if(!sample.invalid){movement->input(*device, sample);if(!sample.relative)button_movement->input(*device,sample);}
            const auto old = values[sample.device][sample.control];
            values[sample.device][sample.control] = {sample.value, !sample.invalid, sample.qpc, sample.sequence};
            motor_inputs.observe(sample);motor_feedback_.observe(sample);
            sequences[sample.device][sample.control] = sample.sequence;
            if (device->controls[sample.control].key.kind == ControlKind::button)
                button_router.input(sample,values,mapper,edge);
            cockpit_router.input(sample,mapper);
            if(input_overrides_.capturing() && edge && !sample.invalid && !sample.baseline &&
               ((sample.relative && sample.value!=0) || (!sample.relative && old.available && old.value==0 && sample.value!=0 && device->controls[sample.control].key.kind==ControlKind::button)))
                input_overrides_.status("No applied assignment; capture remains armed");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (learning_ && !sample.invalid && !sample.relative && !sample.baseline && old.available &&
                    (!learning_button_ ||
                     (device->controls[sample.control].key.kind == ControlKind::button && sample.value != 0))) {
                    const auto key = std::make_pair(sample.device, sample.control);
                    auto anchor = learning_baseline_.find(key);
                    if (anchor == learning_baseline_.end())
                        anchor = learning_baseline_.emplace(key, old.value).first;
                    const auto &c = device->controls[sample.control];
                    if (sample.value != anchor->second &&
                        (c.key.kind != ControlKind::axis ||
                         std::abs(static_cast<double>(sample.value) - anchor->second) >
                             std::max(1.0, (static_cast<double>(c.maximum) - c.minimum) * 0.01))) {
                        learned_ = sample;
                        learning_ = false;
                    }
                }
            }
            for (auto &m : mapper.mappings) {
                if(m.profile=="cockpit")continue;
                bool affected = false;
                for (std::size_t i = 0; i < m.controls.size(); ++i)
                    if (m.source_devices[i] == sample.device && m.controls[i] == sample.control)
                        affected = true;
                if (!affected)
                    continue;
                if (edge && m.binding.mode == MappingMode::selector && m.controls.size() > 1)
                    continue;
                map_sources(m, edge, sample.baseline || session_changed, sample.relative);
            }
            if (edge) {
                commands(ms);
            }
        };
        if(app_settings.fds_enabled && GetTickCount64()>=trace_checkpoint){trace_checkpoint=GetTickCount64()+10000;fds_.trace.record(fds::TraceKind::context,0,mapper.epoch,fds::context_encode(modules(),buttons(),cockpit(),trace_devices()));fds_.trace.record(fds::TraceKind::session,0,mapper.epoch,"boundary");for(const auto& [key,record]:mapper.records)if(trace_records.count(key))fds_.trace.record(fds::TraceKind::psx_record,0,mapper.epoch,key+"="+record.observed);if(mapper.ready)fds_.trace.record(fds::TraceKind::session,0,mapper.epoch,"ready");for(const auto& d:devices)if(d.slot>=first_fds_device && d.slot<=first_phidget_device && d.connected)for(std::size_t c=0;c<d.controls.size();++c){const auto& value=values[d.slot][c];if(!value.available)continue;std::ostringstream captured;captured<<d.slot<<' '<<c<<' '<<d.generation<<' '<<value.sequence<<' '<<value.qpc<<' '<<value.value<<" 1 0 0 0";fds_.trace.record(fds::TraceKind::input,d.slot,d.generation,captured.str());}}
        Sample sample;
        // Fair service at report boundaries; never mix half of a selector report
        // with the other producer. Each pass is bounded even under sustained input.
        unsigned cp_roles{};if(app_settings.cpflight.enabled && !app_settings.cpflight.serial.empty())for(const auto& d:cpflight_.inventory())if(module_compilation.devices.count(d.key) && d.input.connected)cp_roles|=1u<<unsigned(d.input.slot-first_cpflight_device);
        cpflight_.publication(mapper.ready,mapper.epoch,cp_roles);
        for(const auto& provider:providers) {
        unsigned serviced=0;
        while (provider.pop(sample)) {
            if (sample.report_end) {
                button_router.end_report();
                cockpit_router.report(sample.device,values,mapper);
                for (auto &m : mapper.mappings)
                    if (m.binding.mode == MappingMode::selector && m.controls.size() > 1 &&
                        std::find(m.source_devices.begin(), m.source_devices.end(), sample.device) !=
                            m.source_devices.end())
                        map_sources(m, true, session_changed, false);
                commands(ms);
                if(++serviced>=64){SetEvent(change_);break;}
            } else {
                process(sample, true);
                if(!provider.complete_reports && ++serviced>=256){SetEvent(change_);break;}
            }
        }
        }
        for (const auto &d : devices)
            if (d.connected) {
                std::array<Sample,max_controls> baselines{};std::array<bool,max_controls> present{};
                const auto report_revision=[&]{return d.slot<max_usb_devices?input_.report_revision(d.slot):d.slot<first_fds_device?ethernet_.report_revision(d.slot):0;};
                const auto before=report_revision();
                for (std::size_t c = 0; c < d.controls.size(); ++c) {
                    if(d.controls[c].key.kind==ControlKind::button) {
                        // Ordinary button changes belong exclusively to complete ordered reports.
                        // Latest-state recovery is a coherent device snapshot, never half a report.
                        if(!(before&1) && (session_changed || !values[d.slot][c].available) && latest(d.slot,c,baselines[c]) && baselines[c].generation==d.generation)present[c]=true;
                        continue;
                    }
                    if (latest(d.slot, c, sample) && sample.generation == d.generation &&
                        (sample.sequence != sequences[d.slot][c] || session_changed)) {
                        sample.baseline = session_changed || !values[d.slot][c].available;
                        process(sample, false);
                    }
                }
                if(!(before&1) && before==report_revision()){for(std::size_t c=0;c<d.controls.size();++c)if(present[c]){baselines[c].baseline=true;process(baselines[c],false);}if(d.coherent_reports)cockpit_router.report(d.slot,values,mapper);}
            }
        session_changed = false;
        button_router.end_report();
        mapper.tick(ms);
        {
            const auto fi_now=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            auto state=flightillusion_->snapshot();
            fi_connection.update(*flightillusion_,app_settings.flightillusion_enabled,fi_now);
            if(mapper.epoch!=fi_demand_epoch){fi_demand_epoch=mapper.epoch;fi_demanded.clear();}
            if(app_settings.flightillusion_enabled&&state.connected&&mapper.ready)for(const auto* key:{"Qi273","Qi274"})if(!fi_demanded.contains(key)){Command request;request.epoch=mapper.epoch;request.event=true;request.line=std::string("demand=")+key+"\n";if(transport_.submit(std::move(request)))fi_demanded.insert(key);}
            const auto pc_utc_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            auto sources=flightillusion::host_sources(mapper,++fi_sequence,fi_now,pc_utc_ms,flightillusion::local_pc_epoch(pc_utc_ms));
            const auto [fi_host,fi_port]=endpoint();
            fi_exporter.configure({fi_host,fi_port,mapper.epoch,app_settings.flightillusion_enabled&&state.connected&&mapper.ready});
            fi_exporter.apply(sources,std::uint64_t(pc_utc_ms),fi_now);
            if(!app_settings.flightillusion_enabled){sources.values={};sources.clocks={};sources.pc_utc={};sources.pc_local={};sources.controls_ready=false;}
            flightillusion_->publish(sources);
            const auto configuration_revision=revision+axis_revision+button_revision+cockpit_revision+modules_revision;
            if(configuration_revision!=fi_ownership_revision||session_changed){fi_ownership_revision=configuration_revision;fi_blocked.clear();
                const auto block=[&](RecordKey key){const auto name=record_name(key);if(name=="Qi38"||name=="Qh136"||name=="Qs124"||name=="Qs125")fi_blocked.insert(name);};
                for(const auto& mapping:mapper.mappings)if(mapping.binding.enabled)block(mapping.binding.target.record);
                for(const auto& [id,row]:button_configuration.rows)if(!row.applied.bindings.empty()){if(const auto action=button_action(id))block(action->target.record);}
                for(const auto& [id,row]:cockpit().rows)if(row.applied.enabled){const auto definition=active_catalogue().cockpit.find(id);if(definition!=active_catalogue().cockpit.end()){block(definition->second->target.record);if(definition->second->encoder)if(const auto action=button_action(definition->second->decrease))block(action->target.record);}}
            }
            const auto inputs=flightillusion_->drain_inputs();auto input_view=flightillusion_->snapshot();if(!app_settings.flightillusion_enabled)input_view.connected=false;
            auto outgoing=fi_inputs.update(mapper,input_view,inputs,fi_blocked,fi_now,now,std::int64_t(frequency));
            for(const auto id:fi_inputs.cancellations())transport_.cancel_binding(id);
            for(auto& command:outgoing)transport_.submit(std::move(command));
        }
        if(servos_){servos_->provider_enabled(app_settings.phidgets_enabled);servos_->observe(mapper,phidget_io_.readings());}
        steppers_->provider_enabled(app_settings.phidgets_enabled);actuators_->observe(mapper,app_settings.phidgets_enabled);
        if(mapper.ready){if(actuator_demand_epoch!=mapper.epoch){actuator_demand_epoch=mapper.epoch;actuator_demanded.clear();}auto demand=actuators_->demand_records();const auto servo_demand=servos_->demand_records();demand.insert(demand.end(),servo_demand.begin(),servo_demand.end());for(const auto& name:demand)if(!actuator_demanded.contains(name)){Command request;request.epoch=mapper.epoch;request.event=true;request.line="demand="+name+"\n";if(transport_.submit(std::move(request)))actuator_demanded.insert(name);}}

        commands(ms);
        // Command submission takes priority over display-only calculation.
        for(unsigned serviced=0;serviced<4;++serviced){const auto* request=thrust_pipe.poll();if(!request)break;
            auto frame=motor_inputs.frame(mapper,GetTickCount64());frame.provider_enabled=motion_settings.phidgets_enabled;refresh_thrust(frame);
            auto reply=thrust_control.dispatch(*request,thrust_pipe.client());const auto [host,port]=endpoint();std::copy_n(host.begin(),std::min(host.size(),reply.host.size()-1),reply.host.begin());reply.port=port;
            if(reply.result==psx::hwio::thrust_control::Result::ok && request->operation==psx::hwio::thrust_control::Operation::set_hold){transport_.supersede_records({{RecordKind::qs,436}});mapper.motor_throttle_suppression=15;}
            thrust_pipe.reply(reply);
        }
        for(unsigned serviced=0;serviced<4;++serviced){const auto* request=actuator_pipe.poll();if(!request)break;
            auto reply=actuator_control.dispatch(*request);unsigned bench_throttles{};std::vector<Profile> bench_sources;
            auto exclude=[&](const MotorFeedbackDefinition& feedback){if(feedback.kind!=MotorPositionSource::analogue)return;Profile source;std::string error;if(profile_decode(feedback.input_profile,source,error))bench_sources.push_back(std::move(source));};
            for(unsigned slot=0;slot<motor_slots;++slot){const auto state=motor_bench_->snapshot(slot);if(!state.manual_page)continue;exclude(state.draft.feedback);for(unsigned axis=0;axis<4;++axis)if(motor_definitions[axis].enabled && motor_definitions[axis].motor==slot)bench_throttles|=1u<<axis;}
            for(unsigned slot=0;slot<stepper_slots;++slot){const auto state=steppers_->snapshot(slot);if(state.bench)exclude(state.draft.feedback);}
            const auto physical=SituationControls::snapshot(mapper,router,button_router,values,devices,bench_throttles,psx::diagnostics::qpc_now(),psx::diagnostics::qpc_frequency(),bench_sources);
            psx::hwio::situation_controls::put(reply,physical);psx::hwio::thrust_control::put(reply,thrust_control.snapshot(actuator_pipe.client()));actuator_pipe.reply(reply);
        }
        // Copy delivery state after the current submission decisions as well.
        flight.update(mapper, values, ms);
        if(output_configuration_seen!=hardware_revision_seen || output_inventory_seen!=ethernet_revision_seen || output_fds_revision!=fds_revision_seen || phidget_output_seen!=phidget_io_seen){
            clear_output_tests();
            auto inventory=ethernet_.health();auto fds_inventory=fds_.generic_health();inventory.insert(inventory.end(),fds_inventory.begin(),fds_inventory.end());auto native=phidget_io_.health();inventory.insert(inventory.end(),native.begin(),native.end());
            hardware_router.configure(hardware(),devices,active_catalogue(),inventory);
            output_configuration_seen=hardware_revision_seen;output_inventory_seen=ethernet_revision_seen;
            output_fds_revision=fds_revision_seen;phidget_output_seen=phidget_io_seen;
        }
        std::map<std::string,bool> tests;
        bool tests_changed=false;
        {std::lock_guard lock(mutex_);if(output_test_seen!=output_test_revision_){output_test_seen=output_test_revision_;tests=output_tests_;tests_changed=true;}}
        if(tests_changed){hardware_router.clear_tests();for(const auto& [id,on]:tests)if(!hardware_router.set_test(id,on)){hardware_router.clear_tests();clear_output_tests();break;}}
        hardware_router.update(mapper,ethernet_,&fds_,&phidget_io_);
        fdstq_outputs.update(mapper);std::array<bool,5> tq_output{};
        for(unsigned n=0;n<5;++n)if(const auto* value=fdstq_outputs.find(app_settings.fdstq.outputs[n]);value && value->available)tq_output[n]=value->on;
        fdstq_.outputs(tq_output);
        std::map<std::string,bool> module_tests;bool changed_tests=false;
        {std::lock_guard lock(mutex_);if(module_tests_seen!=module_test_revision_){module_tests_seen=module_test_revision_;module_tests=module_tests_;changed_tests=true;}}
        if(changed_tests){module_outputs.clear_tests();for(const auto& [id,on]:module_tests)module_outputs.test(id,on);}
        std::string identifying,identify_profile;std::uint64_t identify_started,identify_revision,identify_generation;
        bool identify_allow_blanking{};
        {std::lock_guard lock(mutex_);identifying=identification_device_;identify_profile=identification_profile_;identify_started=identification_started_;identify_generation=identification_generation_;identify_revision=identification_revision_;
            identify_allow_blanking=std::none_of(modules_.rows.begin(),modules_.rows.end(),[&](const auto& row){return !row.second.applied.profile.empty() && (row.first==identifying || std::any_of(row.second.applied.associations.begin(),row.second.applied.associations.end(),[&](const auto& child){return child.second==identifying;}));});}
        const auto identify_phase=(GetTickCount64()-identify_started)/1000;
        const auto* identify_definition=module_profile(module_library_,identify_profile);
        if(!identifying.empty() && std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d.connected && d.identity.hardware_key==identifying && d.generation==identify_generation;})){clear_module_tests();identifying.clear();identify_definition=nullptr;}
        if(identify_previous!=identifying || identify_revision_seen!=identify_revision || (!identifying.empty() && identify_second!=identify_phase)) {
            if(!identify_previous.empty() && identify_previous!=identifying)if(const auto* old=module_profile(module_library_,identify_profile_previous))fds_.identify({identify_previous,old->identification_lamps,old->identification_brightness,false,false});
            // Stop is serviced before a subsequent identification starts: only one test is active.
            if(!identifying.empty() && identify_definition)fds_.identify({identifying,identify_definition->identification_lamps,identify_definition->identification_brightness,fds::identification_on(GetTickCount64()-identify_started),true,identify_generation,identify_allow_blanking});
            module_outputs.identification(identifying,identify_definition?std::span<const unsigned>(identify_definition->identification_lamps):std::span<const unsigned>{});
            identify_previous=identifying;identify_profile_previous=identify_profile;identify_revision_seen=identify_revision;identify_second=identify_phase;
        }
        const auto module_second=GetTickCount64()/1000;if(app_settings.fds_enabled && module_second!=trace_second){trace_second=module_second;fds_.trace.record(fds::TraceKind::session,0,mapper.epoch,"seconds "+std::to_string(module_second));}module_outputs.update_to(mapper,module_second,module_sink,&hardware_router.evaluated_outputs());
        trim_displays_.publish(trim_source(mapper));
        if (view_requested_.exchange(false)) {
            RuntimeView view;
            view.input_overrides=input_overrides_.snapshot();
            view.flight = flight.snapshot();
            router.display(view.flight, mapper);
            view.axes = router.status();
            view.module_auto_errors=auto_errors;view.last_axis = movement->snapshot(devices);
            view.last_button = button_movement->snapshot(devices);
            view.buttons = button_router.shared_snapshot(button_configuration,mapper);
            view.button_revision = button_revision_seen;
            view.axis_revision = axis_revision_seen;
            view.profile_revision = profile_revision_seen;
            view.snapshot_sequence = ++snapshot_sequence;
            view.snapshot_qpc = psx::diagnostics::qpc_now();
            view.network = transport_.status();
            view.trim_displays=trim_displays_.snapshot();view.cpflight=cpflight_.snapshot();
            view.flightillusion=flightillusion_->snapshot();
            for(const auto& d:devices)if(!d.complete_module)view.devices.push_back(d);
            view.hardware=ethernet_.health();view.hardware_revision=hardware_revision_seen;
            {auto inventory=fds_.generic_health();view.hardware.insert(view.hardware.end(),inventory.begin(),inventory.end());auto native=phidget_io_.health();view.hardware.insert(view.hardware.end(),native.begin(),native.end());}
            view.fdstq_status=fdstq_.status();view.fdstq_testing=fdstq_.identifying();view.fdstq_identification_available=fdstq_.identification_available();view.phidgets=phidgets_.inventory();view.phidget_readings=phidget_io_.readings();{std::lock_guard lock(mutex_);view.identification=identification_device_;}view.modules=module_compilation.views;view.module_revision=module_ui_revision+fds_revision_seen;view.fds=fds_.health();
            const auto module_configuration=modules();
            for(auto& module:view.modules){const auto row=module_configuration.rows.find(module.device);module.pending=row!=module_configuration.rows.end() && (row->second.draft!=row->second.applied || !row->second.pending.empty());}
            for(auto& module:view.modules)module.source=module_outputs.source_status(module.device,mapper.ready);
            view.hardware_outputs=hardware_router.snapshot();
            view.digital_outputs=hardware_router.digital_snapshot();
            view.cockpit=cockpit_router.snapshot(mapper);view.cockpit_revision=cockpit_revision_seen;
            view.input_reports = input_.reports+ethernet_.reports+fds_.reports+fdstq_.reports+phidget_io_.reports+cpflight_.reports;
            view.input_rejected = input_.rejected+ethernet_.rejected;
            view.input_overflow = input_.overflow+ethernet_.overflow+fds_.overflow+fdstq_.overflow+phidget_io_.overflow+cpflight_.overflow;
            view.buffered_packets = input_.buffered_packets;
            view.buffer_reads = input_.buffer_reads;
            for (const auto &d : devices)
                view.input_activity[d.slot] = d.slot<max_usb_devices?input_.activity(d.slot):d.slot<first_fds_device?ethernet_.activity(d.slot):d.slot<first_phidget_device?fds_.activity(d.slot):InputActivity{};
            view.edge_queue=0;for(const auto& provider:providers)view.edge_queue+=provider.queued();
            view.journal = journal_.status();
            view.timing_samples = {mapping_times, send_times, filtered_mapping, filtered_send};
            for (const auto &d : devices)
                if (d.connected)
                    for (std::size_t c = 0; c < d.controls.size(); ++c) {
                        Sample sample_value;
                        if (latest(d.slot, c, sample_value) && sample_value.generation == d.generation)
                            view.controls.push_back(sample_value);
                    }
            for (const auto &m : mapper.mappings) {
                BindingView b;
                b.id = m.local_id;
                b.profile = m.profile;
                b.name = m.binding.name;
                b.target = m.binding.target.name;
                b.available = m.state.available;
                b.raw = m.controls.empty() ? 0 : values[m.source_devices[0]][m.controls[0]].value;
                b.calibrated = m.state.normalized;
                b.desired = m.state.desired;
                b.sent = m.state.sent;
                b.delivery = m.state.delivery;
                b.reason = m.state.reason;
                if(mapper.failed_axes && m.binding.target.record==RecordKey{RecordKind::qs,120} && (mapper.failed_axes&(1u<<m.binding.target.field)))b.reason="IOS failure: output held at neutral";
                if(mapper.runaway_trim && m.binding.target.record.kind==RecordKind::qh && (m.binding.target.record.index==398 || m.binding.target.record.index==399 || (m.binding.target.record.index==396 && m.binding.target.owned_mask&mapper.trim_owned_mask)))b.reason="IOS trim runaway: physical trim output suppressed";
                const auto r = mapper.records.find(record_name(m.binding.target.record));
                if (r != mapper.records.end())
                    b.observed = extract(m.binding.target, r->second.observed);
                view.bindings.push_back(std::move(b));
            }
            for (unsigned i = 0; i < 4; ++i) {
                view.fuel[i] = fuel_indicator(mapper, i);
                const auto *definition=flight_target(9+i);
                if (!definition) continue;
                const auto &d=definition->descriptor;
                const auto r=mapper.records.find(record_name(d.record));
                if (r != mapper.records.end()) {
                    auto value = extract(d, r->second.observed);
                    if (value)
                        view.motors[i] = {mapper.epoch, r->second.sequence, r->second.qpc, static_cast<double>(*value),
                                          true};
                }
            }
            auto ip = mapper.records.find("Qs323"), pp = mapper.records.find("Qs324");
            if (ip != mapper.records.end())
                view.indicator_power = ip->second.observed;
            if (pp != mapper.records.end())
                view.panel_power = pp->second.observed;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                view.problem = view_.problem;
                view_ = std::move(view);
            }
            if (const auto window = view_window_.load())
                PostMessageW(window, view_message_, 0, 0);
        }
        bool ticking = false;
        for (const auto &m : mapper.mappings)
            if (m.state.available &&
                (m.binding.calibration.lowpass_ms > 0 || m.binding.reverse_calibration.lowpass_ms > 0 ||
                 m.binding.mode == MappingMode::relative || m.binding.mode == MappingMode::momentary))
                ticking = true;
        ticking = ticking || flight.settling() || router.settling() || button_router.pending();
        const HANDLE handles[] = {change_, input_.available_event, transport_.available_event, ethernet_.available_event,fds_.available_event,fdstq_.available_event,phidget_io_.available_event,cpflight_.available_event,failure_pipe.event(),motor_pipe.event(),motor_pipe_v2.event(),actuator_pipe.event(),thrust_pipe.event()};
        WaitForMultipleObjects(DWORD(std::size(handles)), handles, FALSE, ticking ? 2 : 50);
    }
    thrust_control.shutdown();
    module_outputs.release_to(module_sink);
    failure_pipe.close();failures.clear("HWIO Exit");
    apply_overrides();
    transport_.cancel_all();
    button_router.release_all(mapper);
    for (auto &m : mapper.mappings)
        if (m.state.held)
            mapper.lost(m.device);
    commands(psx::diagnostics::qpc_now() * 1000.0 / frequency);
    if (!transport_.drain(500))
        log.emit(psx::diagnostics::Code::shutdown_result, psx::diagnostics::Severity::warning, {},
                 psx::diagnostics::ErrorDomain::application, 0, "Held release did not complete before exit deadline");
}
} // namespace hwio

namespace hwio {
bool Runtime::assign_module_role(const std::string& category,const std::string& role,const std::string& device,const std::string& profile,bool automatic,std::string& error,bool additional) {
    std::lock_guard transaction(configuration_mutex_);auto configuration=modules();std::set<std::string> applied_rows;
    if(additional && (category!="CDU" || !configuration.additional_cdu_roles.count(role) || automatic)){error="Enable the additional CDU assignment for this role first";return false;}
    const auto* chosen=device.empty()?nullptr:module_profile(module_library_,profile);
    if(!device.empty() && (!chosen || chosen->category!=category || std::find(chosen->roles.begin(),chosen->roles.end(),role)==chosen->roles.end())){error="Incompatible module role";return false;}
    if(chosen){const auto current=configuration.rows.find(device);if(current!=configuration.rows.end() && !current->second.applied.profile.empty() && current->second.applied.additional_cdu!=additional){error="This CDU is already selected for the other assignment; unassign it there first";return false;}}
    for(auto& [id,row]:configuration.rows){(void)id;const auto* previous=module_profile(module_library_,row.applied.profile);if(previous && previous->category==category && row.applied.role==role && row.applied.additional_cdu==additional){row.draft={};row.pending="apply";applied_rows.insert(id);}}
    if(!additional)configuration.automatic[category+"/"+role]=automatic;
    if(chosen){auto& row=configuration.rows[device];const auto* previous=module_profile(module_library_,row.applied.profile);if(!row.applied.profile.empty() && (row.applied.role!=role || (previous && previous->category!=category))){error="Device already assigned to another role; unassign it first";return false;}const auto cards=module_inventory();const auto card=std::find_if(cards.begin(),cards.end(),[&](const auto& d){return d.key==device;});if(card!=cards.end()){if(card->model!=chosen->model || card->input.identity.backend!=chosen->backend){error="Discovered device does not match the selected profile";return false;}row.update=card->update;}else if(row.model.empty() || row.model!=chosen->model){error="Absent device has no compatible saved identity";return false;}row.device=device;row.model=chosen->model;row.draft={chosen->id,role,chosen->version,row.applied.associations};row.pending="apply";applied_rows.insert(device);}
    if(chosen)configuration.rows.at(device).draft.additional_cdu=additional;
    return save_modules(std::move(configuration),error,&applied_rows);
}
bool Runtime::additional_cdu(const std::string& role,bool enabled,std::string& error){
    if(role!="Left" && role!="Centre" && role!="Right"){error="Invalid CDU role";return false;}
    std::lock_guard transaction(configuration_mutex_);auto configuration=modules();std::set<std::string> applied_rows;
    if(enabled)configuration.additional_cdu_roles.insert(role);
    else {configuration.additional_cdu_roles.erase(role);for(auto& [id,row]:configuration.rows)if((row.applied.additional_cdu && row.applied.role==role) || (row.draft.additional_cdu && row.draft.role==role)){row.draft={};row.pending="apply";applied_rows.insert(id);}}
    return save_modules(std::move(configuration),error,&applied_rows);
}
bool Runtime::identify_module(const std::string& device,const std::string& profile,std::string& error) {
    {std::lock_guard lock(mutex_);if(output_identification_card_.device==device){error="Output identification currently owns this card";return false;}}
    fdstq_.identify(false);
    const auto* definition=module_profile(module_library_,profile);const auto cards=fds_.inventory();const auto card=std::find_if(cards.begin(),cards.end(),[&](const auto& d){return d.key==device && d.input.connected;});
    if(!definition || card==cards.end() || definition->model!=card->model || definition->backend!=card->input.identity.backend){error="Selected connected compatible device required";return false;}
    if(definition->identification_lamps.empty() && !definition->identification_brightness){error="No recovered identification lighting contract for this model";return false;}
    if(definition->identification_brightness && !(card->caps.features&0x10000)){error="Device does not advertise identification brightness";return false;}
    for(const auto lamp:definition->identification_lamps)if(std::none_of(card->resources.begin(),card->resources.end(),[&](const auto& r){return r.output && r.kind==ResourceKind::boolean_output && r.channel==lamp;})){error="Identification backlight is outside advertised capabilities";return false;}
    const auto configuration=hardware();for(const auto& output:configuration.outputs)if(output.enabled && output.device==device && std::find(definition->identification_lamps.begin(),definition->identification_lamps.end(),output.channel)!=definition->identification_lamps.end()){error="Identification light is owned by a generic output";return false;}
    std::lock_guard lock(mutex_);if(!settings_.fds_enabled){error="FDS support disabled";return false;}
    if(identification_device_!=device && !definition->identification_lamps.empty() && !card->readouts_known && std::any_of(modules_.rows.begin(),modules_.rows.end(),[&](const auto& row){return !row.second.applied.profile.empty() && (row.first==device || std::any_of(row.second.applied.associations.begin(),row.second.applied.associations.end(),[&](const auto& child){return child.second==device;}));})){error="Assigned module identification awaits known shared readout values";return false;}
    if(identification_device_==device){identification_device_.clear();identification_profile_.clear();}else{identification_device_=device;identification_profile_=profile;identification_started_=GetTickCount64();identification_generation_=card->input.generation;}++identification_revision_;SetEvent(change_);return true;
}
bool Runtime::export_fds_diagnostics(const std::filesystem::path& directory,std::string& error) {
    std::lock_guard transaction(configuration_mutex_);
    try {std::filesystem::create_directories(directory);fds::Trace::write(directory/L"trace.fdst",fds_.trace.snapshot());unsigned n{};for(const auto& [reason,entries]:fds_.trace.faults()){fds::Trace::write(directory/(L"fault-"+std::to_wstring(n)+L".fdst"),entries);std::ofstream why(directory/(L"fault-"+std::to_wstring(n++)+L".txt"));why<<reason;}
        {std::ofstream tuning(directory/L"motor-autotune.txt",std::ios::binary);tuning<<motor_bench().tuning_report();if(!tuning)throw std::runtime_error("Cannot write motor-tuning diagnostic report");}
        {std::ofstream report(directory/L"actuators.txt",std::ios::binary);for(unsigned slot_index=0;slot_index<stepper_slots;++slot_index){const auto s=steppers().snapshot(slot_index);report<<"Stepper "<<slot_index<<" serial="<<s.draft.address.serial<<" port="<<s.draft.address.hub_port<<" channel="<<s.draft.address.channel<<" run="<<s.draft.run_mode<<" generation="<<s.live.generation<<" generated_or_measured_position="<<s.live.position<<" status="<<s.status<<" fault="<<s.live.fault<<'\n';}for(unsigned slot_index=0;slot_index<actuator_mapping_slots;++slot_index){const auto s=actuators().snapshot(slot_index);if(s.definition.enabled)report<<"Mapping "<<slot_index<<" kind="<<unsigned(s.definition.output.kind)<<" slot="<<s.definition.output.slot<<" phase="<<unsigned(s.phase)<<" reference_quality="<<unsigned(s.quality)<<" position="<<s.position<<" target="<<s.target<<" status="<<s.status<<'\n';}if(!report)throw std::runtime_error("Cannot write actuator diagnostic report");}
        {std::ofstream servo_report(directory/L"servos.txt",std::ios::binary);for(unsigned i=0;i<servo_slots;++i){const auto s=servos().snapshot(i);servo_report<<"Servo "<<i<<" "<<s.draft.name<<" address="<<s.draft.address.device<<" channel="<<s.draft.address.channel<<" phase="<<unsigned(s.phase)<<" generation="<<s.live.generation<<" revision="<<s.live.revision<<" drive="<<unsigned(s.live.drive)<<" accepted="<<s.live.accepted.value_or(-1)<<" profile="<<s.live.profile_position.value_or(-1)<<" current_A="<<s.live.current.value_or(-1)<<" status="<<s.status<<" fault="<<s.live.fault<<" code="<<s.live.fault_code<<"\n";}if(!servo_report)throw std::runtime_error("Cannot write servo diagnostic report");}
        motor_mappings().export_following(directory);const auto view=snapshot();std::filesystem::path resolved_phidget,resolved_fds;try{resolved_phidget=vendor_library(VendorLibrary::phidgets,settings().phidget_api_path);}catch(const std::exception&){}try{resolved_fds=vendor_library(VendorLibrary::fds,settings().fds_api_path);}catch(const std::exception&){}std::ofstream metadata(directory/L"manifest.txt");LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);metadata<<"HWIO " HWIO_VERSION "\nTrace FDST2 / 256-byte records / 16 MiB ring / QPC frequency "<<frequency.QuadPart<<"\nSDK path "<<utf8(settings().fds_api_path)<<"\n"<<view.fds.usb<<"\n"<<view.fds.ethernet<<"\n";for(const auto& m:view.modules)metadata<<m.device<<' '<<m.model<<' '<<m.update<<' '<<m.connection<<' '<<m.completeness<<'\n';const auto executable=std::filesystem::path(executable_directory())/L"psx-hwio.exe";for(const auto& identity:{executable,resolved_fds,resolved_phidget})if(!identity.empty() && std::filesystem::exists(identity))metadata<<"Identity "<<utf8(identity.wstring())<<" size="<<std::filesystem::file_size(identity)<<" SHA256="<<fds::sha256_file(identity)<<'\n';metadata<<"FDS reports "<<fds_.reports<<" rejected "<<fds_.rejected<<" overflow "<<fds_.overflow<<"\n";metadata<<"Input overflow "<<view.input_overflow<<"\nTrace records "<<fds_.trace.written()<<"\n";if(!metadata)throw std::runtime_error("Cannot write diagnostic manifest");
        for(const auto* name:{L"settings.cfg",L"assignments.cfg",L"motor-settings.cfg",L"motor-mappings.cfg",L"servo-settings.cfg",L"stepper-settings.cfg",L"stepper-settings.cfg.bak",L"actuator-mappings.cfg",L"actuator-mappings.cfg.bak",L"catalogue.xml",L"fds-modules.xml"}) {const auto input=std::filesystem::path(name==std::wstring_view(L"settings.cfg") || name==std::wstring_view(L"assignments.cfg") || name==std::wstring_view(L"motor-settings.cfg") || name==std::wstring_view(L"motor-mappings.cfg") || name==std::wstring_view(L"servo-settings.cfg") || name==std::wstring_view(L"stepper-settings.cfg") || name==std::wstring_view(L"stepper-settings.cfg.bak") || name==std::wstring_view(L"actuator-mappings.cfg") || name==std::wstring_view(L"actuator-mappings.cfg.bak")?data_dir():executable_directory())/name;if(!std::filesystem::exists(input)){metadata<<"Absent snapshot "<<utf8(input.wstring())<<'\n';continue;}if(std::filesystem::file_size(input)>32*1024*1024)throw std::runtime_error("Diagnostic configuration exceeds 32 MiB");std::ifstream source(input,std::ios::binary);std::ofstream destination(directory/name,std::ios::binary|std::ios::trunc);if(!source)throw std::runtime_error("Cannot read diagnostic configuration snapshot");destination<<source.rdbuf();if(!destination)throw std::runtime_error("Cannot write diagnostic configuration snapshot");}
        for(const auto& file:std::filesystem::directory_iterator(directory))if(file.is_regular_file() && file.path().filename()!=L"manifest.txt")metadata<<"Bundle "<<utf8(file.path().filename().wstring())<<" size="<<file.file_size()<<" SHA256="<<fds::sha256_file(file.path())<<'\n';metadata.flush();if(!metadata)throw std::runtime_error("Cannot finish diagnostic manifest");
        return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
}

namespace hwio {
std::vector<ModuleDevice> Runtime::module_inventory(){auto out=module_devices(fds_.inventory());auto cp=cpflight_.inventory();out.insert(out.end(),cp.begin(),cp.end());return out;}
bool Runtime::select_cpflight_port(const psx::hardware::SerialPort& port,std::string& error){auto s=settings();s.provider_selection_initialized=true;s.cpflight.port=port;s.cpflight.serial.clear();s.cpflight.enabled=!port.com_name.empty();return save_settings(s,error);}
}
