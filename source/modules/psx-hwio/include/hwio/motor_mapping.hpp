#pragma once
#include "motor_bench.hpp"
#include "motor_homing_log.hpp"
#include "motor_following.hpp"
#include "motor_alignment.hpp"
#include "motor_correction.hpp"
#include "motor_following_log.hpp"
#include <psx/hwio/motors.hpp>
#include <psx/hwio/thrust_control.hpp>
#include "axis_setup.hpp"
#include <mutex>
#include <condition_variable>
#include <thread>

namespace hwio {
enum class MotorAxis:unsigned { throttle1,throttle2,throttle3,throttle4,speedbrake,flap_left_outboard,flap_left_inboard,flap_right_inboard,flap_right_outboard,trim_left,trim_right,count };
constexpr unsigned motor_axes=unsigned(MotorAxis::count);
const wchar_t* motor_axis_name(MotorAxis) noexcept;
struct MotorMappingDefinition {
    bool enabled{},auto_home{};unsigned motor{};
    unsigned analogue_output{UINT_MAX}; // Presentation identity, independent of the motor/axis number.
    Profile feedback; // Indicators only: local sensor, never a pilot PSX binding.
    int low{},high{};unsigned captured{};
    double following_error{.05};unsigned following_timeout_ms{500},sensor_response_counts{20};
    unsigned response_delay_ms{250},lag_allowance_ms{2000};
    // Wiring direction is reusable; absolute encoder endpoints are not.
    std::string direction_key;int learned_direction{};
};
using MotorMappingDefinitions=std::array<MotorMappingDefinition,motor_axes>;
using MotorAlignments=std::array<MotorAlignment,motor_slots>;
struct MotorAxisFeedback {
    bool valid{},calibrated{};double fraction{},resolution{};
    std::uint64_t generation{},calibration_revision{},sample_ms{};
    double raw{},raw_low{},raw_high{};bool raw_valid{};
    std::uint64_t source_identity{};bool source_exact{};double code_count{4096};
    std::uint64_t sample_sequence{};
};
struct MotorMappingFrame {
    std::array<MotorAxisFeedback,motor_axes> sensors;
    std::array<double,motor_axes> targets{};std::array<int,4> pilot_values{};std::array<bool,4> pilot_valid{};
    std::array<bool,motor_axes> target_valid{};
    std::array<std::uint64_t,motor_axes> target_sequence{};
    std::uint64_t psx_generation{},stamp_ms{};
    AtAuthority authority{AtAuthority::unavailable};bool psx_ready{},provider_enabled{true};
    std::uint64_t at_inactive_sequence{};
    bool at_arm_valid{},at_armed{};
    std::array<bool,4> combined_throttles{};
    bool ios_owned{},ios_drive{};std::uint64_t ios_token{},ios_operation{};
    std::array<double,4> ios_percent{};
};
enum class HomingState { idle,waiting,probe,escape,seeking_first,settling_first,departing_second,seeking_second,settling_second,parking,following,stopped,fault };
// Diagnostic state names describe SDK outcomes, never measured shaft torque.
inline const char* motor_drive_event_name(unsigned event)noexcept {
    static constexpr const char* names[]{"none","hold-request","release-request","SDK-off","engage-accepted","physical-held","hold-accepted"};
    return event<std::size(names)?names[event]:"unknown";
}
struct MotorMappingView {
    HomingState state{};bool homed{},engaged{};double encoder_low{},encoder_high{},sensor{},demand{},correction{};
    std::string status;std::uint64_t revision{};
    double target_anchor{},arrival_band{};unsigned target_change{}; // 0 unchanged, 1 new episode, 2 retarget.
    int endpoint_edge{},request_direction{};
    unsigned retarget_kind{},stop_reason{},drive_event{};
    std::uint64_t pending_command{},pending_limits{},pending_release{},correction_service_ms{};
    MotorFollowingPhase phase{MotorFollowingPhase::waiting};bool major_fault{},reset_pending{},release_confirmed{},reset_failed{};
    std::uint64_t fault_revision{},operation_id{};std::string cause;
};
struct MotorMappingRouting {
    unsigned suppress_throttles{},feedback_mask{},fault_mask{},disconnect_faults{};bool suppress_speedbrake{},disconnect_at{};
    std::array<std::string,motor_slots> fault_reasons;
    std::array<int,4> throttle_values{};
};
// Compose one complete TLA record, preserving engines outside the owned mask.
std::optional<Command> motor_throttle_feedback(Mapper&,const MotorMappingRouting&,std::int64_t mapped_qpc);
bool load_motor_mappings(const std::wstring&,MotorMappingDefinitions&,std::string&,MotorAlignments* =nullptr,std::string* alignment_warning=nullptr);
bool save_motor_mappings(const std::wstring&,const MotorMappingDefinitions&,std::string&,const MotorAlignments* =nullptr);
std::string motor_mappings_encode(const MotorMappingDefinitions&,const MotorAlignments&);
bool validate_motor_mappings(const MotorMappingDefinitions&,std::string&);
MotorMappingFrame motor_mapping_frame(const MotorMappingDefinitions&,const AxisConfiguration&,const std::vector<DeviceInfo>&,const SourceBank&,const Mapper&,std::uint64_t);
class MotorMappingInputs {
    struct Sensor {unsigned slot{},control{};bool resolved{},calibrated{},exact{},combined{};double low{},inverse_span{},codes{4096};std::uint64_t generation{},revision{},identity{};SourceValue physical;};
    std::array<Sensor,motor_axes> sensors_;
    MotorMappingFrame make_frame(const SourceBank*,const Mapper&,std::uint64_t)const;
public:
    void configure(const MotorMappingDefinitions&,const AxisConfiguration&,const std::vector<DeviceInfo>&);
    // Physical continuity belongs to the device generation, not the PSX session.
    // Preserve original sample timestamps across simulator routing resets.
    void baseline(const SourceBank&);
    void observe(const Sample&) noexcept;
    // Refresh only physical motor feedback before decisions. Ordinary pilot
    // routing still consumes the same samples through its existing ordered path.
    template<class Latest> void refresh(Latest&& latest) noexcept {
        for(auto& sensor:sensors_)if(sensor.resolved){Sample sample;
            if(latest(sensor.slot,sensor.control,sample) && sample.generation==sensor.generation && !sample.relative && !sample.report_end)
                sensor.physical={sample.value,!sample.invalid,sample.qpc,sample.sequence};
        }
    }
    void invalidate() noexcept;
    MotorMappingFrame frame(const Mapper&,std::uint64_t)const;
    MotorMappingFrame frame(const SourceBank&,const Mapper&,std::uint64_t)const;
};
// Timestamp/value equality is a candidate correlation, not proved causality.
// Consume at most one fresh matching observation per completed local send.
// Stepped by the acquisition worker. UI commands are bounded flags; all
// physical operations enqueue onto the per-motor worker, never into callbacks.
class MotorMappings {
    struct Session {
        MotorMappingView view;MotorOwner owner{MotorOwner::stopped};MotorHome home;MotorSessionMap map;MotorResponseMonitor response;MotorCorrection correction;
        MotorDisconnectWindow disconnect_window;MotorTrackingObservation tracking;
        bool correction_hold{},goal_known{},force_correction{},physical_hold{},correction_moved{},limit_hold{},accepted_movement{};
        bool ios_owned{},ios_drive{},ios_handover{};std::uint64_t ios_operation{};
        double goal_raw{},episode_anchor{},nominal_target{},episode_target{},episode_offset{},command_raw{},accepted_raw{},accepted_velocity{},accepted_acceleration{};
        std::uint64_t goal_psx_generation{},accepted_id{},episode{},command_episode{},accepted_episode{};std::string alignment_identity,correction_note;
        std::uint64_t arrival_after_ms{},arrival_after_sequence{},command_sensor_sequence{};
        bool command_target{},command_hold{},initial_requested{},reversal_hold{},home_hold{},home_requested{},stop_requested{},automatic{},auto_attempted{},claimed{},latched{},opening{},log_started{},reset_requested{},await_rearm{},inactive_seen{},finishing{},manual_page{},resume_page{};
        std::uint64_t tick_ms{},opening_ms{},failure_revision{},motor_generation{},coordinate{},sensor_generation{},calibration{},source_identity{},psx_generation{},feedback_ms{},sent_ms{},accepted_ms{},last_ms{};
        int last_feedback{-100000},edge{},movement_direction{};double target{},accepted{},limit_velocity{},limit_acceleration{},last_count{},measured_velocity{};
        std::string identity,disconnect_delivery;MotorCommandResult command,limits,release;
        std::uint64_t rearm_psx_generation{},rearm_inactive_sequence{};
        // A rejected request is not lost coordinates. Remember its input so an
        // unchanged invalid value cannot flood the worker or erase Home.
        MotorCommandResult rejection;unsigned rejection_kind{};
        double rejected_target{},rejected_velocity{},rejected_acceleration{};
        std::uint64_t rejected_configuration{},retry_ms_{};
    };
    MotorBench& bench_;std::wstring directory_;mutable std::mutex mutex_;
    MotorMappingDefinitions definitions_;MotorAlignments alignments_;MotorAlignmentSave alignment_save_;std::string alignment_warning_;std::array<Session,motor_slots> sessions_;
    std::unique_ptr<MotorHomingLog> homing_log_;
    std::unique_ptr<MotorFollowingLog> following_log_;
    struct LegacyDirection {std::string device;unsigned axis{motor_axes};std::uint64_t calibration{};int polarity{};};
    std::array<LegacyDirection,motor_slots> legacy_directions_;
    std::mutex save_mutex_,persistence_mutex_;std::condition_variable persistence_wake_;
    std::thread persistence_thread_;bool persistence_pending_{},persistence_stop_{};
    std::string direction_storage_error_;
    bool logging_tick_{};
    std::string storage_error_;bool hidden_{};std::uint64_t definition_revision_{1},assignment_revision_{1};
    void end(unsigned,Session&,std::string,bool fault=false);
    void persist_directions();void queue_direction_save();
public:
    MotorMappings(MotorBench&,std::wstring);
    ~MotorMappings();
    MotorMappingDefinitions definitions()const;
    std::uint64_t definition_revision()const;
    bool assign(MotorMappingDefinitions,std::string&);
    void home(unsigned);void stop(unsigned,std::uint64_t operation=0);void inhibit(unsigned,std::string);void hide_manual();void shutdown();
    void manual_page(int slot);
    std::array<MotorMappingView,motor_axes> views()const;
    MotorMappingRouting tick(const MotorMappingFrame&);
    psx::hwio::motors::Packet motor_status(std::uint64_t session)const;
    psx::hwio::motors::v2::Packet motor_status_v2(std::uint64_t session)const;
    std::array<psx::hwio::thrust_control::Lever,4> thrust_status(const MotorMappingFrame&)const;
    std::uint32_t thrust_released_groups(std::uint64_t operation=0,bool releasing=false)const;
    bool reset_home(unsigned,std::uint64_t fault_revision,std::uint64_t operation,std::string&);
    void following_logging(bool on){following_log_->enabled(on);}
    void at_disconnect_delivery(unsigned motors,std::string_view status);
    void export_following(const std::filesystem::path& p){following_log_->export_to(p);}
};
}
