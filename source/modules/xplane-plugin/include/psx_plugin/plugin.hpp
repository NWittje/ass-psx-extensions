#pragma once
#include "sdk_api.hpp"
#include "observation_client.hpp"
#include "motion_controller.hpp"
#include "scenery_view_controller.hpp"
#include "audio_service.hpp"
#include "terrain_service.hpp"
#include "terrain_provider.hpp"
#include "terrain_capture.hpp"
#include "aircraft_sync_service.hpp"
#include "aircraft_sync_controller.hpp"
#include "environment_service.hpp"
#include "environment_controller.hpp"
#include "status_source.hpp"
#include "status_service.hpp"
#include "traffic_controller.hpp"
#include "runway_actor_controller.hpp"
#include <XPLMProcessing.h>

namespace psx::xp {
enum class State { stopped, started, enabled };
struct LifecycleSnapshot {
    State state{};
    std::uint64_t aircraft_generation{};
    std::uint64_t scenery_generation{};
    int callback_count{};
};

class Plugin final {
public:
    bool start();
    bool enable();
    void disable() noexcept;
    void stop() noexcept;
    void message(XPLMPluginID from, int message, void* parameter) noexcept;
    LifecycleSnapshot snapshot() const noexcept; // SDK thread only; unavailable otherwise
    SdkApi& sdk() noexcept { return sdk_; } // Local C++ extension seam; not a DLL export.
    const bridge::PluginStatus& observation_status() const noexcept { return observation_.status(); }
    const bridge::MotionStatus& motion_status() const noexcept {return motion_.report().status;}
    diagnostics::DiagnosticsHub* diagnostics_hub() const noexcept { return journal_.get(); } // Not a frame API.
    diagnostics::JournalStatus diagnostics_status() const { return journal_ ? journal_->status() : stopped_journal_; }
    std::uint64_t load_id() const noexcept { return load_id_; }
    std::uint64_t enable_id() const noexcept { return enable_id_; }
    const TerrainServiceStatus& terrain_status() const noexcept {return terrain_status_;}
    Plugin() = default;
    explicit Plugin(ObservationOptions options) : observation_options_(std::move(options)) {}
    Plugin(ObservationOptions options,TerrainOptions terrain,std::wstring aircraft_endpoint={},std::wstring environment_endpoint={},std::wstring status_endpoint={}) : observation_options_(std::move(options)),terrain_options_(std::move(terrain)),aircraft_endpoint_(std::move(aircraft_endpoint)),environment_endpoint_(std::move(environment_endpoint)),status_endpoint_(std::move(status_endpoint)) {}
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;
    static constexpr const char* diagnostic_name = "psx_ios/foundation/test_value";
    static constexpr const char* count_name = "psx_ios/foundation/callback_count";
    const status::Snapshot& readiness_snapshot() const noexcept {return status_source_.latest();}
private:
    friend struct EnvironmentPluginTestAccess;
    void traffic_start() noexcept;
    void traffic_handoff() noexcept;
    void actor_start() noexcept;
    void actor_handoff() noexcept;
    RunwayActorController actor_controller_;
    RunwayActorService actor_service_;
    TrafficController traffic_controller_;
    TrafficService traffic_service_;
    static float flight_loop(float, float, int, void*) noexcept;
    static float pre_flight_loop(float,float,int,void*) noexcept;
    static double read_diagnostic(void*) noexcept;
    static void write_diagnostic(void*, double) noexcept;
    static int read_count(void*) noexcept;
    SdkApi sdk_;
    State state_{State::stopped};
    XPLMDataRef diagnostic_{};
    XPLMDataRef count_{};
    XPLMFlightLoopID loop_{};
    XPLMFlightLoopID pre_loop_{};
    double diagnostic_value_{};
    int callback_count_{};
    std::uint64_t aircraft_generation_{};
    std::uint64_t scenery_generation_{};
    // ObservationClient borrows the hub: declaration order also protects exceptional destruction.
    std::unique_ptr<diagnostics::DiagnosticsHub> journal_;
    diagnostics::JournalStatus stopped_journal_{};
    diagnostics::DiagnosticProducer sdk_events_;
    std::uint64_t load_id_{}, enable_id_{};
    ObservationClient observation_;
    ObservationOptions observation_options_;
    ObservationLifecycle observation_lifecycle_{};
    MotionController motion_;
    SceneryViewController scenery_view_;
    void view_handoff() noexcept;
    AudioService audio_;
    TerrainProvider terrain_provider_;
    TerrainCapture terrain_capture_;
    TerrainService terrain_service_;
    TerrainOptions terrain_options_;
    TerrainServiceStatus terrain_status_{};
    std::uint64_t terrain_host_generation_{};
    bool terrain_enabled_{};
    AircraftController aircraft_controller_;
    AircraftService aircraft_service_;
    std::wstring aircraft_endpoint_;
    bool aircraft_enabled_{};
    EnvironmentController environment_controller_;
    EnvironmentService environment_service_;
    std::wstring environment_endpoint_;
    bool environment_enabled_{},environment_relocating_{};
    StatusSource status_source_;StatusService status_service_;
    StatusTransportReport status_transport_{};std::wstring status_endpoint_;
    static void status_sink(void*,const status::Snapshot&,bool) noexcept;
    static void placement_status(void*) noexcept;
    void status_handoff(bool forced=false,std::uint32_t notification=0,bool pending=false) noexcept;
    MotionProfile audio_profile_{};
    XPLMDataRef audio_flight_time_{};
    std::array<char,512> aircraft_path_{};
    bool motion_fault_seen_{};
    void motion_handoff() noexcept;
    void audio_start() noexcept;
    void audio_handoff() noexcept;
    void terrain_start();
    void terrain_handoff() noexcept;
    void aircraft_start() noexcept;
    void aircraft_handoff() noexcept;
    void environment_start() noexcept;
    void environment_handoff() noexcept;
    diagnostics::Context diagnostic_context() const noexcept;
    void event(diagnostics::Code, diagnostics::Severity=diagnostics::Severity::info,
               std::uint32_t error=0, std::string_view text={}) noexcept;
#ifdef _DEBUG
    static thread_local Plugin* error_owner_;
    static void sdk_error(const char*) noexcept;
#endif
    void identify_aircraft() noexcept;
};
} // namespace psx::xp
