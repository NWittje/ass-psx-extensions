#pragma once
#include "hid.hpp"
#include "transport.hpp"
#include "flight_display.hpp"
#include "axis_setup.hpp"
#include "storage.hpp"
#include "ethernet.hpp"
#include "hardware_router.hpp"
#include <filesystem>
#include "phidgets.hpp"
#include "motor_mapping.hpp"
#include "actuators.hpp"
#include "input_overrides.hpp"
#include "card_assignments.hpp"
#include "flightillusion.hpp"

namespace hwio {
struct BindingView {
    std::uint64_t id{};
    std::string profile, name, target, reason;
    int raw{}, desired{}, sent{};
    double calibrated{};
    bool available{};
    DeliveryState delivery{};
    std::optional<int> observed;
};
struct Timing {
    std::uint64_t count{};
    double p50{}, p95{}, p99{}, maximum{};
};
struct RuntimeView {
    InputOverrideView input_overrides;
    std::map<std::string,std::string> module_auto_errors;
    LastAxisMovement last_axis;
    LastButtonInput last_button;
    std::shared_ptr<const std::vector<ButtonReadout>> buttons; // Immutable catalogue-bounded snapshot, shared across UI panes.
    std::uint64_t button_revision{};
    AxisStatus axes;
    std::uint64_t axis_revision{};
    FlightSnapshot flight;
    std::uint64_t profile_revision{}, snapshot_sequence{};
    std::int64_t snapshot_qpc{};
    TransportStatus network;
    std::vector<DeviceInfo> devices;
    std::vector<EthernetHealth> hardware;
    std::vector<HardwareOutputView> hardware_outputs;
    std::vector<DigitalOutputState> digital_outputs;
    std::vector<CockpitReadout> cockpit;
    std::uint64_t cockpit_revision{};
    std::uint64_t hardware_revision{};
    std::vector<ModuleView> modules;
    std::uint64_t module_revision{};
    fds::Health fds;
    PhidgetInventory phidgets;
    std::vector<PhidgetReading> phidget_readings;
    TrimDisplayView trim_displays;
    CpflightView cpflight;
    flightillusion::View flightillusion;
    std::string fdstq_status;
    bool fdstq_testing{},fdstq_identification_available{};
    std::string identification;
    std::vector<Sample> controls;
    std::array<InputActivity, max_devices> input_activity{};
    std::vector<BindingView> bindings;
    std::array<Indicator, 4> fuel;
    std::array<OutputObservation, 4> motors;
    std::string indicator_power, panel_power, problem;
    std::uint64_t input_reports{}, input_rejected{}, input_overflow{}, edge_queue{};
    std::uint64_t buffered_packets{}, buffer_reads{};
    Timing map_timing, send_timing, filtered_map_timing, filtered_send_timing;
    std::array<std::vector<double>, 4> timing_samples;
    psx::diagnostics::JournalStatus journal;
};
struct RuntimeOptions {
    std::wstring data_dir;
    std::string host{"127.0.0.1"};
    std::uint16_t port{default_main_port};
    bool autoconnect{true};
    bool endpoint_override{};
};
class Runtime {
    InputOverrides input_overrides_;
    friend struct RuntimeProbe;
    RuntimeOptions options_;
    AppSettings settings_;
    std::uint64_t settings_revision_{};
    HidInput input_;
    EthernetInput ethernet_;
    fds::Provider fds_;
    PhidgetDiscovery phidgets_;
    PhidgetIo phidget_io_;
    TrimDisplays trim_displays_;
    Cpflight cpflight_;
    std::unique_ptr<flightillusion::Provider> flightillusion_;
    std::vector<ModuleDevice> module_inventory();
    MotorFeedbackBus motor_feedback_;
    std::unique_ptr<MotorBench> motor_bench_;
    std::unique_ptr<MotorMappings> motor_mappings_;
    std::unique_ptr<ServoIndicators> servos_;
    std::unique_ptr<Steppers> steppers_;
    std::unique_ptr<Actuators> actuators_;
    std::vector<PhidgetServer> phidget_servers_;
    void configure_phidgets(const AppSettings&);
    FdstqInterface fdstq_;
    std::string identification_device_,identification_profile_;
    std::uint64_t identification_started_{},identification_generation_{},identification_revision_{};
    ModuleLibrary module_library_;
    ModuleConfiguration modules_;
    std::uint64_t module_revision_{};
    std::uint64_t module_applied_revision_{};
    std::map<std::string,bool> module_tests_;
    std::uint64_t module_test_revision_{};
    Transport transport_;
    psx::diagnostics::DiagnosticsHub journal_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> disconnect_requested_{false};
    std::atomic<bool> connect_requested_{false};
    HANDLE change_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex_;
    mutable std::recursive_mutex configuration_mutex_;
    std::vector<Profile> profiles_;
    std::uint64_t revision_{};
    DisplayAssignments display_assignments_;
    std::uint64_t display_revision_{};
    AxisConfiguration axes_;
    ButtonConfiguration buttons_;
    HardwareConfiguration hardware_;
    CockpitConfiguration cockpit_;
    std::uint64_t cockpit_revision_{};
    std::uint64_t hardware_revision_{};
    std::map<std::string,bool> output_tests_;
    std::uint64_t output_test_revision_{};
    IdentificationCard output_identification_card_;
    std::uint64_t output_identification_session_{};
    std::optional<IdentificationCompletion> output_identification_retired_,output_identification_reply_;
    std::uint64_t button_revision_{};
    std::uint64_t axis_revision_{};
    RuntimeView view_;
    std::atomic<bool> view_requested_{true};
    std::atomic<HWND> view_window_{nullptr};
    std::atomic<UINT> view_message_{0};
    bool learning_{};
    bool learning_button_{};
    std::optional<Sample> learned_;
    std::map<std::pair<std::uint16_t, std::uint16_t>, int> learning_baseline_;
    void run();
    void read_profiles();

  public:
    InputOverrideView input_overrides() const {return input_overrides_.snapshot();}
    bool save_input_overrides(InputOverrideConfiguration c,std::string& error,std::uint64_t expected_revision=UINT64_MAX){const auto ok=input_overrides_.replace(std::move(c),error,expected_revision);SetEvent(change_);return ok;}
    void capture_input_override(bool enabled){input_overrides_.capture(enabled);SetEvent(change_);}
    ~Runtime();
    void start(RuntimeOptions options);
    void stop();
    RuntimeView snapshot();
    AppSettings settings();
    bool save_settings(const AppSettings &settings, std::string &error);
    bool identify_trim_display(const TrimPort& p,std::string& error){return trim_displays_.identify(p,error);}
    bool assign_trim_display(unsigned role,std::uint64_t identity,std::string& error);
    bool select_cpflight_port(const psx::hardware::SerialPort&,std::string&);
    void refresh_cpflight(){cpflight_.refresh();}
    flightillusion::Provider& flightillusion(){return *flightillusion_;}
    bool enable_flightillusion(bool enabled,std::string& error){if(enabled&&(!flightillusion_||flightillusion_->snapshot().configuration.port.instance.empty())){error="Select a COM port in Instruments and calibration first";return false;}auto s=settings();s.flightillusion_enabled=enabled;return save_settings(s,error);}
    bool assign_fdstq(std::string& error);
    void identify_fdstq(bool enabled){clear_module_tests();fdstq_.identify(enabled);}
    HardwareConfiguration hardware();
    bool save_hardware(HardwareConfiguration configuration,std::string& error);
    std::optional<CardIdentity> card_identity(const std::string&,std::string&);
    std::unique_ptr<CardAssignments> card_configuration();
    bool export_card_assignments(const std::string&,const std::filesystem::path&,std::string&);
    bool prepare_card_import(const std::string&,const std::filesystem::path&,CardImport&,std::string&);
    bool import_card_assignments(const CardImport&,bool confirmed_model_only,std::string&);
    CockpitConfiguration cockpit();
    bool save_cockpit(CockpitConfiguration,std::string&);
    bool test_output(const std::string& id,bool on,std::string& error);
    void clear_output_tests();
    std::map<std::string,bool> output_tests();
    std::optional<IdentificationCard> output_identification_card(const std::string&,std::string&);
    bool identify_outputs(IdentificationRequest,const IdentificationCard&,std::string&);
    std::optional<IdentificationCompletion> identification_completion(std::uint64_t session);
    bool save_identified_outputs(const IdentificationCard&,const std::vector<IdentificationChoice>&,const HardwareConfiguration& original,std::string&);
    ModuleConfiguration modules();
    const ModuleLibrary& module_library() const noexcept {return module_library_;}
    bool save_modules(ModuleConfiguration,std::string&,const std::set<std::string>* applied_rows=nullptr);
    bool merge_output_catalogue(std::size_t&,std::string&);
    bool merge_module_library(std::size_t&,std::string&);
    bool test_module(const std::string&,bool,std::string&);
    bool identify_module(const std::string&,const std::string&,std::string&);
    bool assign_module_role(const std::string& category,const std::string& role,const std::string& device,const std::string& profile,bool automatic,std::string&,bool additional_cdu=false);
    bool additional_cdu(const std::string& role,bool enabled,std::string&);
    bool export_fds_diagnostics(const std::filesystem::path&,std::string&);
    void clear_module_tests();
    std::shared_ptr<const psx::network::Inventory> adapters() const {return ethernet_.adapters.snapshot();}
    void discover_hardware(){ethernet_.discover();fds_.discover();}
    bool refresh_hardware(const std::string& device){if(device.starts_with("fds-usb:") || device.starts_with("fds-net:")){fds_.discover();return true;}return ethernet_.refresh_card(device);}
    bool motor_hardware(const std::string& device,unsigned operation,unsigned axis){return ethernet_.motor_control(device,operation,axis);}
    std::vector<Discovery> discovered_hardware(){return ethernet_.discovered();}
    void request_snapshot(HWND window, UINT message);
    std::vector<Profile> profiles();
    DisplayAssignments display_assignments();
    AxisConfiguration axes();
    ButtonConfiguration buttons();
    bool save_buttons(ButtonConfiguration configuration, std::string &error, const std::string &replace_action = {});
    bool save_axes(AxisConfiguration configuration, std::string &error);
    void learn_override();
    bool save_display(const DisplayAssignments &assignments, std::string &error);
    bool save(Profile profile, std::string &error);
    bool import_profile(const std::wstring &file, std::string &error);
    bool export_profile(const std::string &id, const std::wstring &file, std::string &error);
    bool remove_binding(const std::string &profile, std::uint64_t id, std::string &error);
    void connect(std::string host, std::uint16_t port);
    void disconnect();
    void learn();
    std::optional<Sample> learned();
    std::wstring data_dir() const {
        return options_.data_dir;
    }
    MotorBench& motor_bench();
    Steppers& steppers();Actuators& actuators();
    bool assign_actuator(unsigned,ActuatorMapping,std::string&);
    MotorPositionSample actuator_input(const MotorFeedbackDefinition& d)const{return motor_feedback_.sample(d);}
    ServoIndicators& servos(){if(!servos_)servos_=std::make_unique<ServoIndicators>(options_.data_dir,[this](const MotorDeviceAddress& a){return phidgets_.servo_backend(a);});return *servos_;}
    MotorMappings& motor_mappings(){if(!motor_mappings_)motor_mappings_=std::make_unique<MotorMappings>(motor_bench(),options_.data_dir);return *motor_mappings_;}
    bool assign_motor_mappings(MotorMappingDefinitions,std::string&);
    std::pair<std::string, std::uint16_t> endpoint() const {
        std::lock_guard lock(configuration_mutex_);
        return {options_.host, options_.port};
    }
};
} // namespace hwio
