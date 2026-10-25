#include "psx_plugin/plugin.hpp"
#include <psx/sync/runtime.hpp>
#include <XPLMUtilities.h>
#include <XPLMPlanes.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstring>
#ifdef PSX_PLUGIN_OFFLINE_TEST
#include <Windows.h>
#endif

namespace psx::xp {
namespace d = diagnostics;
#ifdef _DEBUG
namespace { std::atomic<std::uint64_t> unowned_sdk_errors{}; }
thread_local Plugin* Plugin::error_owner_{};
void Plugin::sdk_error(const char* message) noexcept {
    // Never call the SDK here, and never submit to the SDK SPSC from another thread.
    auto* owner=error_owner_;
    if (!owner || !owner->journal_) { unowned_sdk_errors.fetch_add(1,std::memory_order_relaxed); return; }
    owner->journal_->count_lost(0,true,unowned_sdk_errors.exchange(0,std::memory_order_relaxed));
    std::size_t size{};
    if(message) while(size<159 && message[size]) ++size;
    owner->journal_->producer(0,d::Component::sdk).emit(d::Code::sdk_failure,d::Severity::error,
        owner->diagnostic_context(),d::ErrorDomain::sdk,0,std::string_view(message ? message : "",size));
}
#endif
d::Context Plugin::diagnostic_context() const noexcept {
    d::Context c; c.plugin_load_id=load_id_; c.enable_id=enable_id_;
    c.aircraft_generation=aircraft_generation_; c.scenery_generation=scenery_generation_;
    const auto& s=observation_.status();
    c.connection_id=s.connection_id; c.session_epoch=s.session_epoch;
    c.configuration_epoch=s.configuration_epoch; c.boost_generation=s.boost_generation;
    return c;
}
void Plugin::event(d::Code code,d::Severity severity,std::uint32_t error,std::string_view message) noexcept {
    sdk_events_.emit(code,severity,diagnostic_context(),error ? d::ErrorDomain::sdk : d::ErrorDomain::none,error,message);
}
bool Plugin::start() {
    sdk_.attach_thread();
    if (!sdk_.on_thread() || state_ != State::stopped) return false;
    try {
        load_id_=d::new_identity(); enable_id_=0;
        journal_=std::make_unique<d::DiagnosticsHub>();
#ifdef PSX_PLUGIN_OFFLINE_TEST
        d::Options options; options.component=d::Component::plugin; options.config.enabled=false;
        std::array<wchar_t,MAX_PATH+1> temporary{};
        const auto length=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());
        if(!length || length>=temporary.size()) { stop(); return false; }
        options.directory=std::wstring(temporary.data())+L"psx-ios-plugin-offline-"+std::to_wstring(load_id_);
        options.config_path=options.directory+L"\\diagnostics.ini";
#else
        auto options=d::default_options(d::Component::plugin);
#endif
        options.build="psx-xplane-0.20.0";
        journal_->start(options); // A failed sink must not disable observation.
        sdk_events_=journal_->producer(0,d::Component::plugin);
        event(d::Code::process_start);
#ifdef _DEBUG
        error_owner_=this; XPLMSetErrorCallback(sdk_error);
#endif
        auto failed=[&](std::uint32_t stage,std::string_view reason) {
            event(d::Code::startup_failed,d::Severity::error,stage,reason); stop(); return false;
        };
        int xp{}, sdk{}, host{};
        XPLMGetVersions(&xp, &sdk, &host);
        if (host != xplm_Host_XPlane || sdk < 400) return failed(1,"unsupported SDK/host");
        // Refuse a namespace collision without unregistering somebody else's dataref.
        if (XPLMFindDataRef(diagnostic_name) || XPLMFindDataRef(count_name)) return failed(2,"diagnostic namespace occupied");
        diagnostic_ = XPLMRegisterDataAccessor(diagnostic_name, xplmType_Double, 1,
            nullptr,nullptr,nullptr,nullptr, read_diagnostic,write_diagnostic,
            nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,this,this);
        if (!diagnostic_) return failed(3,"diagnostic accessor registration");
        count_ = XPLMRegisterDataAccessor(count_name, xplmType_Int, 0,
            read_count,nullptr,nullptr,nullptr,nullptr,nullptr,
            nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,this,nullptr);
        if (!count_) return failed(4,"callback accessor registration");
        state_ = State::started;
        return true;
    } catch (...) { event(d::Code::startup_failed,d::Severity::error,5,"startup exception"); stop(); return false; }
}
bool Plugin::enable() {
    if (!sdk_.on_thread() || state_ == State::stopped) return false;
    if (state_ == State::enabled) return true;
    if((motion_.report().status.flags&motion::override_owned) && !motion_.stop())return false;
    ++enable_id_;
    try {
#ifndef PSX_PLUGIN_OFFLINE_TEST
        if(!psx::sync::start_runtime(psx::sync::Role::plugin,psx::sync::read_settings(psx::sync::Role::plugin)))return false;
#endif
        XPLMCreateFlightLoop_t params{};
        params.structSize = sizeof(params);
        params.phase = xplm_FlightLoop_Phase_AfterFlightModel;
        params.callbackFunc = flight_loop;
        params.refcon = this;
        loop_ = XPLMCreateFlightLoop(&params);
        if (!loop_) { event(d::Code::sdk_failure,d::Severity::error,6,"flight loop creation"); disable(); return false; }
        params.phase=xplm_FlightLoop_Phase_BeforeFlightModel;params.callbackFunc=pre_flight_loop;
        pre_loop_=XPLMCreateFlightLoop(&params);
        if(!pre_loop_){disable();return false;}
        observation_options_.diagnostics=journal_.get();
        observation_options_.plugin_load_id=load_id_; observation_options_.enable_id=enable_id_;
#ifndef PSX_PLUGIN_OFFLINE_TEST
        {std::array<char,512> root{};XPLMGetSystemPath(root.data());
         if(root[0]&&!root.back())observation_options_.scenery_root=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(root.data()))).wstring();}
#endif
        if (!observation_.start(observation_options_)) { disable(); return false; }
        sdk_.activate();
        scenery_view_.begin(sdk_,observation_qpc,observation_qpc_frequency());

        auto boost_timeout=observation_options_.boost_timeout_ms;
#ifndef PSX_PLUGIN_OFFLINE_TEST
        const auto setting=motion_timeout_from_environment();boost_timeout=setting.milliseconds;
        sdk_events_.emit(d::Code::configuration,setting.valid?d::Severity::info:d::Severity::warning,diagnostic_context(),d::ErrorDomain::none,0,
            setting.valid?"Boost timeout milliseconds":"Invalid PSX_IOS_BOOST_TIMEOUT_MS; using 500 ms",{boost_timeout,setting.provided?1u:0u,0,0,0,0});
#endif
        motion_.begin(sdk_,load_id_,enable_id_,journal_->producer(0,d::Component::motion),
            observation_options_.callback_clock?observation_options_.callback_clock:observation_qpc,
            observation_qpc_frequency(),boost_timeout,journal_->options().config.delayed_ms);
        motion_fault_seen_=false;motion_.prepare_bindings();
        motion_.lifecycle(aircraft_generation_,scenery_generation_,false,false);
        identify_aircraft();
        audio_start();
        terrain_start();
        aircraft_start();
        environment_start();
        traffic_start();
        actor_start();
        state_ = State::enabled;
        callback_count_ = 0;
        status_source_.begin(sdk_,{load_id_,enable_id_},observation_qpc_frequency(),observation_qpc,status_sink,this);
#ifdef PSX_PLUGIN_OFFLINE_TEST
        if(!status_endpoint_.empty())status_service_.start({load_id_,enable_id_},status_endpoint_);
#else
        status_service_.start({load_id_,enable_id_},status_endpoint_);
#endif
        motion_.readiness_sink(placement_status,this);
        status_handoff(true);
        XPLMScheduleFlightLoop(loop_, -1.0F, 1);
        XPLMScheduleFlightLoop(pre_loop_,-1.0F,1);
        event(d::Code::enable);
        return true;
    } catch (...) { event(d::Code::sdk_failure,d::Severity::error,7,"enable exception"); disable(); return false; }
}
void Plugin::disable() noexcept {
    if (!sdk_.on_thread()) return;
    if(state_==State::enabled){
        const StatusContext disabled{false,observation_lifecycle_.aircraft[0]!=0,enable_id_,aircraft_generation_,
            scenery_generation_,terrain_enabled_?terrain_provider_.host().local_frame_generation:0,
            static_cast<std::uint64_t>(callback_count_)};
        status_source_.observe(disabled,motion_,true);
    }
    motion_.readiness_sink(nullptr,nullptr);status_service_.stop();
    traffic_controller_.retire();traffic_service_.stop();
    actor_controller_.retire();actor_service_.stop();
    if(state_==State::enabled || (motion_.report().status.flags&motion::override_owned)) {motion_.stop();motion_handoff();}
    motion_.retire_resources();
    environment_controller_.retire();environment_service_.stop();environment_enabled_=false;environment_relocating_=false;
    aircraft_controller_.retire();aircraft_service_.stop();aircraft_enabled_=false;
    terrain_capture_.retire();terrain_provider_.retire();terrain_service_.stop();terrain_enabled_=false;terrain_status_={};
    audio_.stop();audio_flight_time_=nullptr;audio_profile_={};
    // Override release/readback must precede SDK deactivation and worker retirement.
    scenery_view_.stop();
    sdk_.deactivate();
    if(pre_loop_){XPLMDestroyFlightLoop(pre_loop_);pre_loop_=nullptr;}
    if (loop_) { XPLMDestroyFlightLoop(loop_); loop_ = nullptr; }
    if(state_==State::enabled) event(d::Code::disable);
    observation_.stop(); // Cancel and join before code or owned SDK refs can unload.
#ifndef PSX_PLUGIN_OFFLINE_TEST
    psx::sync::stop_runtime();
#endif
    if (state_ == State::enabled) state_ = State::started;
}
void Plugin::stop() noexcept {
    if (!sdk_.on_thread()) return;
    disable();
    traffic_controller_.stop();
    actor_controller_.stop();
    if (count_) { XPLMUnregisterDataAccessor(count_); count_ = nullptr; }
    if (diagnostic_) { XPLMUnregisterDataAccessor(diagnostic_); diagnostic_ = nullptr; }
    diagnostic_value_ = 0;
    callback_count_ = 0;
    state_ = State::stopped;
#ifdef _DEBUG
    if(error_owner_==this) {
        XPLMSetErrorCallback(nullptr); error_owner_=nullptr;
        if(journal_) journal_->count_lost(0,true,unowned_sdk_errors.exchange(0,std::memory_order_relaxed));
    }
#endif
    if(journal_) { event(d::Code::process_stop); journal_->stop(); stopped_journal_=journal_->status(); journal_.reset(); sdk_events_={}; }
}
void Plugin::message(XPLMPluginID from, int message, void* parameter) noexcept {
    if (!sdk_.on_thread() || state_ == State::stopped || from != XPLM_PLUGIN_XPLANE) return;
    switch (message) {
    case XPLM_MSG_PLANE_LOADED:
    case XPLM_MSG_PLANE_UNLOADED:
    case XPLM_MSG_LIVERY_LOADED:
        if (reinterpret_cast<std::intptr_t>(parameter) != 0) return;
        // Snapshot at notification entry precedes existing potentially expensive lifecycle cleanup.
        if(message==XPLM_MSG_PLANE_UNLOADED)observation_lifecycle_.aircraft.fill(0);
        status_handoff(true,static_cast<std::uint32_t>(message),true);
        audio_.stop();audio_profile_={};audio_flight_time_=nullptr;
        ++aircraft_generation_;
        if(message!=XPLM_MSG_PLANE_UNLOADED)environment_controller_.boundary();
        aircraft_controller_.lifecycle(aircraft_generation_,true);
        terrain_provider_.lifecycle(aircraft_generation_,scenery_generation_,true);
        terrain_service_.invalidate(terrain_provider_.host());
        motion_.lifecycle(aircraft_generation_,scenery_generation_,true,message==XPLM_MSG_PLANE_UNLOADED);
        sdk_.invalidate();
        if (message == XPLM_MSG_PLANE_UNLOADED) observation_lifecycle_.aircraft.fill(0);
        else {motion_.prepare_bindings();identify_aircraft();if(state_==State::enabled)audio_start();}
        observation_lifecycle_.aircraft_generation=aircraft_generation_;
        observation_.invalidate_lifecycle(observation_lifecycle_);
        event(d::Code::aircraft_changed);
        status_handoff(true,static_cast<std::uint32_t>(message));
        break;
    case XPLM_MSG_AIRPORT_LOADED:
    case XPLM_MSG_SCENERY_LOADED:
        status_handoff(true,static_cast<std::uint32_t>(message),true);
        ++scenery_generation_;
        actor_controller_.scenery(scenery_generation_);
        // Ordinary scenery-tile streaming invalidates terrain/readiness, not weather content.
        if(message==XPLM_MSG_AIRPORT_LOADED)environment_controller_.boundary();
        terrain_provider_.lifecycle(aircraft_generation_,scenery_generation_,true);
        terrain_service_.invalidate(terrain_provider_.host());
        motion_.lifecycle(aircraft_generation_,scenery_generation_,false,false);
        sdk_.invalidate();
        observation_lifecycle_.scenery_generation=scenery_generation_;
        aircraft_controller_.lifecycle(aircraft_generation_,false);aircraft_controller_.profile(audio_profile_);
        observation_.invalidate_lifecycle(observation_lifecycle_);
        event(d::Code::scenery_changed);
        status_handoff(true,static_cast<std::uint32_t>(message));
        break;
    case 113: // XPLM_MSG_FMOD_BANK_UNLOADING, SDK 4.x master bank index 0.
        if(reinterpret_cast<std::intptr_t>(parameter)==0)audio_.stop();
        break;
    case 112: // XPLM_MSG_FMOD_BANK_LOADED.
        if(reinterpret_cast<std::intptr_t>(parameter)==0&&state_==State::enabled)audio_start();
        break;
    default: break;
    }
}
LifecycleSnapshot Plugin::snapshot() const noexcept {
    if (!sdk_.on_thread()) return {};
    return {state_, aircraft_generation_, scenery_generation_, callback_count_};
}
float Plugin::flight_loop(float, float, int, void* refcon) noexcept {
    if (!refcon) return 0;
    auto& self = *static_cast<Plugin*>(refcon);
    if (self.sdk_.ready() != Status::ok) return 0;
    const auto clock=self.observation_options_.callback_clock?self.observation_options_.callback_clock:observation_qpc;
    const auto begin=clock();
    if (self.callback_count_ < std::numeric_limits<int>::max()) ++self.callback_count_;
    self.observation_lifecycle_.aircraft_generation=self.aircraft_generation_;
    self.observation_lifecycle_.scenery_generation=self.scenery_generation_;
    self.observation_.callback(self.observation_lifecycle_);
    self.motion_.post(self.observation_.latest(),self.observation_.connected(),self.observation_.motion_window(),self.observation_.complete_window());
    self.motion_handoff();
    self.aircraft_handoff();
    if(self.terrain_enabled_) {
        TerrainCaptureResult capture;
        if(self.terrain_capture_.tick(self.terrain_service_.input(),capture))self.terrain_service_.publish_capture(capture);
    }
    self.status_handoff();
    const auto end=clock();if(end>=begin)self.motion_.callback_duration(false,static_cast<std::uint64_t>(end-begin));
    return -1.0F; // A count is diagnostic only: it is not time or display latency.
}
void Plugin::identify_aircraft() noexcept {
    aircraft_path_.fill(0);
    observation_lifecycle_.aircraft.fill(0);
    XPLMGetNthAircraftModel(0,observation_lifecycle_.aircraft.data(),aircraft_path_.data());
    observation_lifecycle_.aircraft.back()=0;
    aircraft_path_.back()=0;
    ProfileRequest request;request.generation=aircraft_generation_;request.path=aircraft_path_;observation_.request_profile(request);
}
void Plugin::motion_handoff() noexcept {
    bridge::MotionAck ack;for(std::size_t i=0;i<64 && motion_.pop_ack(ack);++i)
        if(!observation_.submit_motion_ack(ack))break;
    observation_.publish_motion(motion_.report());
}
void Plugin::status_sink(void* context,const status::Snapshot& snapshot,bool transition) noexcept {
    auto& self=*static_cast<Plugin*>(context);self.status_service_.publish(snapshot,transition);
}
void Plugin::placement_status(void* context) noexcept {static_cast<Plugin*>(context)->status_handoff(true);}
void Plugin::status_handoff(bool forced,std::uint32_t notification,bool pending) noexcept {
    const StatusContext context{state_==State::enabled,observation_lifecycle_.aircraft[0]!=0,enable_id_,
        aircraft_generation_,scenery_generation_,terrain_enabled_?terrain_provider_.host().local_frame_generation:0,
        static_cast<std::uint64_t>(callback_count_),pending};
    status_source_.observe(context,motion_,forced,notification);status_service_.poll(status_transport_);
}
void Plugin::audio_start() noexcept {
    void* studio{};
#ifndef PSX_PLUGIN_OFFLINE_TEST
    using GetStudio=void* (*)();
    const auto address=XPLMFindSymbol("XPLMGetFMODStudio");GetStudio get{};
    static_assert(sizeof(address)==sizeof(get));std::memcpy(&get,&address,sizeof(get));
    if(get)studio=get();
#endif
    audio_flight_time_=XPLMFindDataRef("sim/time/total_flight_time_sec");
    if(audio_flight_time_&&!(XPLMGetDataRefTypes(audio_flight_time_)&xplmType_Float))audio_flight_time_=nullptr;
    audio_.start(studio);
}
void Plugin::audio_handoff() noexcept {
    AudioHost host;host.connected=observation_.connected();
    if(host.connected&&observation_.latest())host.connection=observation_.latest()->session().connection_id;
    host.plugin_generation=enable_id_;host.aircraft_generation=aircraft_generation_;host.profile=audio_profile_;
    host.aircraft_path=aircraft_path_;host.follow=motion_.following();
    if(audio_flight_time_){host.flight_time=XPLMGetDataf(audio_flight_time_);host.flight_time_valid=std::isfinite(host.flight_time);}
    audio_.submit(host,observation_.audio_state());
    if(observation_.audio_transport_fault())audio_.fault();
    audio::Control c;for(std::size_t i=0;i<64&&observation_.pop_audio_control(c);++i)audio_.control(c);
    audio::Transition t;for(std::size_t i=0;i<audio::transition_capacity&&observation_.pop_audio_transition(t);++i)audio_.transition(t);
    audio::Status s;if(audio_.poll(s))observation_.publish_audio(s);
}
void Plugin::terrain_start() {
    terrain_enabled_=false;
#ifdef PSX_PLUGIN_OFFLINE_TEST
    // Existing fake-SDK/plugin integration fixtures must never bind the production terrain server.
    if(terrain_options_.endpoint.empty())return;
#endif
    const motion::InstanceId provider{load_id_,enable_id_};
    if(!terrain_provider_.begin(sdk_,provider,observation_qpc_frequency(),observation_qpc))return;
    terrain_provider_.lifecycle(aircraft_generation_,scenery_generation_,false);
    terrain_capture_.begin(terrain_provider_);
    auto options=terrain_options_;options.diagnostics=journal_.get();
    if(!terrain_service_.start(options,provider)){terrain_capture_.retire();terrain_provider_.retire();return;}
    terrain_enabled_=true;terrain_host_generation_=0;
}
void Plugin::terrain_handoff() noexcept {
    if(!terrain_enabled_)return;
    // First callback after a load remains invalid. Fresh physical queries must arrive after retirement.
    const bool loading=!observation_lifecycle_.aircraft[0]||motion_.report().status.phase==motion::Phase::relocating;
    if(terrain_provider_.host().loading!=loading) {
        terrain_provider_.lifecycle(aircraft_generation_,scenery_generation_,loading);
        terrain_service_.invalidate(terrain_provider_.host());
    }
    TerrainResult result;
    if(terrain_provider_.tick(terrain_service_.input(),result))terrain_service_.publish(result,terrain_provider_.host());
    if(terrain_host_generation_!=terrain_provider_.host().generation) {
        terrain_host_generation_=terrain_provider_.host().generation;terrain_service_.invalidate(terrain_provider_.host());
    }
    terrain_service_.poll(terrain_status_);
}
void Plugin::aircraft_start() noexcept {
#ifdef PSX_PLUGIN_OFFLINE_TEST
    if(aircraft_endpoint_.empty())return;
#endif
    try {
        if(!aircraft_controller_.begin(sdk_,{load_id_,enable_id_},observation_qpc_frequency(),observation_qpc))return;
        aircraft_controller_.lifecycle(aircraft_generation_,false);
        if(!aircraft_service_.start({load_id_,enable_id_},aircraft_endpoint_)){aircraft_controller_.retire();return;}
        aircraft_enabled_=true;
    }catch(...){aircraft_controller_.retire();aircraft_service_.stop();aircraft_enabled_=false;}
}
void Plugin::aircraft_handoff() noexcept {
    if(!aircraft_enabled_)return;
    aircraft_controller_.connection(aircraft_service_.connection());
    AircraftCommand command;for(unsigned i=0;i<aircraft::ordered_capacity&&aircraft_service_.command(command);++i)
        aircraft_service_.ack(aircraft_controller_.command(command),command.connection);
    AircraftSample sample;for(unsigned g=0;g<6;++g)if(aircraft_service_.sample(g,sample))aircraft_controller_.sample(sample);
    aircraft_controller_.tick(aircraft_service_.connected(),!observation_lifecycle_.aircraft[0]||motion_.report().status.phase==motion::Phase::relocating);
    aircraft_service_.publish(aircraft_controller_.applied());
}
void Plugin::environment_start() noexcept {
#ifdef PSX_PLUGIN_OFFLINE_TEST
    if(environment_endpoint_.empty())return;
#endif
    try {
        const auto clock=observation_options_.callback_clock?observation_options_.callback_clock:observation_qpc;
        if(!environment_controller_.begin(sdk_,{load_id_,enable_id_},observation_qpc_frequency(),clock))return;
        if(!environment_service_.start({load_id_,enable_id_},environment_endpoint_,journal_.get())){environment_controller_.retire();return;}
        environment_enabled_=true;environment_relocating_=false;
    }catch(...){environment_controller_.retire();environment_service_.stop();environment_enabled_=false;}
}
void Plugin::traffic_start() noexcept {
#ifndef PSX_PLUGIN_OFFLINE_TEST
    try {
        std::array<char,512> path{};XPLMGetSystemPath(path.data());
        if(!path[0]||path.back())return;
        const auto root=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data())));
        const auto cache=root/"Output/psx-ios/traffic-v1"/(std::to_string(load_id_)+"-"+std::to_string(enable_id_));
        if(!traffic_controller_.begin(sdk_,{load_id_,enable_id_},observation_qpc_frequency(),observation_qpc,root,cache))return;
        if(!traffic_service_.start({load_id_,enable_id_}))traffic_controller_.retire();
    }catch(...){traffic_controller_.retire();traffic_service_.stop();}
#endif
}
void Plugin::traffic_handoff() noexcept {
    TrafficCommand command;for(unsigned i=0;i<16&&traffic_service_.command(command);++i)
        traffic_service_.ack(traffic_controller_.command(command),command.transport);
    TrafficWindow window;if(traffic_service_.window(window))traffic_controller_.window(window);
    tv::Capture capture;if(traffic_service_.capture(capture))traffic_controller_.capture(capture);
    traffic_controller_.tick(traffic_service_.connection(),scenery_generation_,terrain_enabled_?terrain_provider_.host().local_frame_generation:0);
    traffic_service_.publish(traffic_controller_.report(),traffic_controller_.capabilities());
    for(std::size_t slot=0;slot<7;++slot){tv::Detail detail;if(traffic_controller_.detail(slot,detail))traffic_service_.detail(detail);}
}
void Plugin::actor_start() noexcept {
#ifndef PSX_PLUGIN_OFFLINE_TEST
    try {
        std::array<char,512> path{};XPLMGetSystemPath(path.data());path.back()=0;
        const auto root=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data())));
        const auto cache=root/"Output/psx-ios/runway-actor-v1"/(std::to_string(load_id_)+"-"+std::to_string(enable_id_));
        actor_controller_.begin(sdk_,observation_qpc,observation_qpc_frequency(),root,cache);
        actor_service_.start();
    }catch(...){actor_controller_.retire();actor_service_.stop();}
#endif
}
void Plugin::actor_handoff() noexcept {
    actor_controller_.selected_peer(observation_.selected_peer());
    const auto connection=actor_service_.connection();
    ActorCommand command;for(unsigned i=0;i<16&&actor_service_.command(command);++i)
        if(connection&&command.connection==connection)actor_service_.reply(actor_controller_.command(command),command.connection);
    actor_controller_.tick(connection,scenery_generation_,terrain_enabled_?terrain_provider_.host().local_frame_generation:0);
    nn::Result result;std::uint64_t owner{};for(unsigned i=0;i<32&&actor_controller_.result(result,owner);++i)actor_service_.reply(result,owner);
}
void Plugin::environment_handoff() noexcept {
    if(!environment_enabled_)return;
    const bool relocating=motion_.report().status.phase==motion::Phase::relocating;
    if(environment_relocating_&&!relocating&&(motion_.report().status.flags&motion::override_owned))environment_controller_.boundary();
    environment_relocating_=relocating;
    environment_controller_.connection(environment_service_.connection());
    environment_controller_.capture(environment_service_.capture_generation());
    EnvironmentCommand command;for(unsigned i=0;i<env::ordered_capacity&&environment_service_.command(command);++i)
        environment_service_.ack(environment_controller_.command(command),command.connection);
    EnvironmentSample sample;if(environment_service_.sample(sample))environment_controller_.sample(sample);
    environment_controller_.tick(environment_service_.connected());
    EnvironmentTiming timing;if(environment_controller_.timing(timing))observation_.publish_environment_timing(timing);
    EnvironmentController::Failure failure;
    for(unsigned i=0;i<8&&environment_controller_.failure(failure);++i){
        // Primitive SDK evidence only: writer thread formats/persists. Float bits preserve NaN and exact setter/readback differences.
        sdk_events_.emit(d::Code::sdk_failure,d::Severity::warning,diagnostic_context(),d::ErrorDomain::sdk,
            static_cast<std::uint32_t>(failure.status),"environment_sdk_failure",
            {failure.binding|(static_cast<std::uint64_t>(failure.index)<<8)|(static_cast<std::uint64_t>(failure.stage)<<16),
             failure.requested_bits|(static_cast<std::uint64_t>(failure.actual_bits)<<32),failure.source_sequence,failure.connection,
             failure.expected_type|(static_cast<std::uint64_t>(failure.actual_type)<<32),
             failure.expected_length|(static_cast<std::uint64_t>(failure.actual_length)<<32)});
    }
    environment_service_.publish(environment_controller_.applied(),environment_controller_.visibility_policies(),environment_controller_.weather_report());
}
float Plugin::pre_flight_loop(float,float,int,void* context) noexcept {
    if(!context)return 0;auto& self=*static_cast<Plugin*>(context);
    if(self.sdk_.ready()!=Status::ok)return 0;
    const auto clock=self.observation_options_.callback_clock?self.observation_options_.callback_clock:observation_qpc;
    const auto begin=clock();
    self.observation_.refresh();
    self.motion_.connection(self.observation_.connected()&&self.observation_.latest()?self.observation_.latest()->session().connection_id:0);
    MotionProfile profile;if(self.observation_.poll_profile(profile)){self.motion_.profile(profile);self.audio_profile_=profile;self.terrain_provider_.profile(profile);self.aircraft_controller_.profile(profile);}
    MotionControl control;for(std::size_t i=0;i<32 && self.observation_.pop_motion_control(control);++i) {
        self.observation_.refresh();
        self.motion_.connection(self.observation_.connected()&&self.observation_.latest()?self.observation_.latest()->session().connection_id:0);
        if(control.source_selection)self.motion_.selection(control.selection);
        else if(control.boundary)self.motion_.boundary(control.session);
        else self.motion_.request(control.request,self.observation_.latest(),self.observation_.connected(),self.observation_.complete_window());
    }
    if(self.observation_.motion_transport_fault())self.motion_.transport_overflow();
    self.motion_.pre(self.observation_.latest(),self.observation_.connected(),self.observation_.complete_window());self.motion_handoff();
    self.view_handoff();
    self.audio_handoff();
    self.terrain_handoff();
    self.aircraft_handoff();
    self.environment_handoff();
    self.traffic_handoff();
    self.actor_handoff();
    self.status_handoff();
    const auto end=clock();if(end>=begin)self.motion_.callback_duration(true,static_cast<std::uint64_t>(end-begin));return -1;
}
void Plugin::view_handoff() noexcept {
    const auto* latest=observation_.latest();
    const auto connection=observation_.connected()&&latest?latest->session().connection_id:0;
    scenery_view_.tick(); // Explicit selection survives transport loss, independent of Follow/Stop.
    bridge::ViewRequest request;
    for(unsigned i=0;i<32&&observation_.pop_view_request(request);++i) {
        if(!connection||request.connection_id!=connection)continue;
        const auto reply=scenery_view_.request(request,connection,load_id_,enable_id_);
        if(!observation_.submit_view_reply(reply))break;
    }
}
double Plugin::read_diagnostic(void* refcon) noexcept {
    if (!refcon) return 0;
    auto& self = *static_cast<Plugin*>(refcon);
    return self.sdk_.ready() == Status::ok ? self.diagnostic_value_ : 0;
}
void Plugin::write_diagnostic(void* refcon, double value) noexcept {
    if (!refcon) return;
    auto& self = *static_cast<Plugin*>(refcon);
    if (self.sdk_.ready() == Status::ok && std::isfinite(value) && std::abs(value) <= 1000000.0)
        self.diagnostic_value_ = value;
}
int Plugin::read_count(void* refcon) noexcept {
    if (!refcon) return 0;
    auto& self = *static_cast<Plugin*>(refcon);
    return self.sdk_.ready() == Status::ok ? self.callback_count_ : 0;
}
} // namespace psx::xp
