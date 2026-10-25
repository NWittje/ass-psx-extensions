#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace hwio {
struct MotorPositionSample {
    double position{};
    std::uint64_t generation{},sequence{},observed_ms{},checked_ms{};
    bool valid{};
    double quantum{1},code_count{};
};
enum class MotorDriveMode { position,pwm,velocity };
struct MotorDriveParameters {
    // PWM ramp has units duty/s; native velocity units belong to the drive's
    // encoder and must never be confused with the selected position sensor.
    double maximum_duty{.25},pwm_ramp{1};
    std::optional<double> native_velocity,native_acceleration,speed_kp,speed_ki,speed_kd,speed_deadband;
    std::optional<int> speed_feedback;
    bool operator==(const MotorDriveParameters&)const=default;
};
enum class MotorEncoderMode { push_pull=1,line_driver_2k2,line_driver_10k,open_collector_2k2,open_collector_10k };
enum class MotorFeedbackSource { encoder=1,hall=2 };
struct MotorParameters {
    MotorDriveParameters drive;
    double vff{},aff{}; // Host position loop only, normalized drive per count/s and count/s².
    double backlash{}; // Raw potentiometer gap; host load-reference geometry only, never an SDK PID property.
    double kp{},ki{},kd{},rescale{1},deadband{},velocity_limit{},acceleration{};
    std::optional<double> current_limit; // Required when the controller exposes current limiting.
    std::optional<bool> normalize_pid{true}; // Omit for devices with fixed PID normalization.
    std::string units{"encoder pulses"};
    std::optional<MotorEncoderMode> encoder_mode;
    std::optional<MotorFeedbackSource> feedback_source;
    std::optional<double> current_regulator_gain,feed_forward;
    double minimum{},maximum{},jog_lead{};
    bool numeric_range_only{}; // Explicit bench choice; these are SDK numeric bounds, not mechanical stops.
    unsigned feedback_interval_ms{100},failsafe_ms{1000},failsafe_refresh_ms{100},jog_interval_ms{100},jog_lease_ms{250};
};
struct MotorCapabilities {
    bool failsafe{true},braking{},back_emf{};
    bool software_position{},pwm_drive{};
    int model{},firmware{};
    bool position_controller{},current_limit{},normalize_pid{},encoder_mode{},feedback_source{},current_regulator_gain{},feed_forward{},expected_position{},duty_cycle{};
    double minimum_acceleration{},maximum_acceleration{},minimum_velocity{},maximum_velocity{},minimum_current{},maximum_current{},minimum_position{},maximum_position{},minimum_regulator_gain{},maximum_regulator_gain{};
    unsigned minimum_interval_ms{},maximum_interval_ms{},minimum_failsafe_ms{},maximum_failsafe_ms{};
};
enum class MotorState { closed,opening,unreferenced,ready,holding,moving,jogging,waving,fault };
enum class MotorDriveState { off,on,unknown };
enum class MotorFaultKind { none,soft_limit,tracking,continuity,sdk,unknown_drive };
enum class MotorError { none,invalid_argument,not_ready,unsupported,busy,queue_full,cancelled,sdk_error,soft_limit };
enum class MotorResultStage { rejected,queued,sdk_succeeded,attained,cancelled,limited,host_applied };
struct MotorCommandResult {
    std::uint64_t id{};
    MotorResultStage stage{MotorResultStage::rejected};
    MotorError error{MotorError::none};
    int sdk_code{};
    std::string reason;
    explicit operator bool() const noexcept{return stage!=MotorResultStage::rejected && error==MotorError::none;}
};
struct MotorSnapshot {
    std::uint64_t feedback_sequence{},feedback_generation{},drive_accepted_ms{},drive_requested_ms{};
    double feedback_quantum{1},feedback_code_count{},feedback_raw_position{};
    double reference_position{},reference_velocity{},reference_acceleration{},requested_drive{},accepted_drive{};
    double reference_shaft_position{};bool backlash_takeup{};
    std::optional<double> reported_drive;
    MotorState state{MotorState::closed};
    MotorDriveState drive{MotorDriveState::off};
    MotorFaultKind fault_kind{MotorFaultKind::none};
    MotorCapabilities capabilities;
    MotorParameters requested,applied;
    std::uint64_t generation{},configuration_revision{},coordinate_revision{},observed_ms{},checked_ms{};
    bool attached{},configured{},feedback_valid{},referenced{},engaged{},wave_active{};
    double wave_centre{},wave_period{};
    double position{},target{},desired_target{}; // Target is the SDK's encoder-representable value.
    std::uint64_t target_command_id{},target_accepted_ms{};bool target_revision_valid{},target_attained{};double target_start_position{};
    double target_velocity{},target_acceleration{}; // Limits in effect when this target was accepted.
    // Controller event arrival is distinct from an SDK cached getter and is
    // not a hardware acknowledgement of a particular target command.
    std::uint64_t expected_sequence{},expected_observed_ms{},expected_generation{},expected_coordinate{};
    bool expected_feedback_enabled{},expected_event_valid{};
    std::optional<double> expected_position,duty_cycle;
    MotorCommandResult last_result;
    MotorError fault_error{MotorError::none};
    int fault_code{}; // Native return/event code; zero for a software constraint.
    std::string last_fault;int last_fault_code{};std::uint64_t last_fault_ms{}; // Survives a failsafe channel reopen.
    std::uint64_t command_submissions{},command_coalesced{},command_overflows{},failsafe_renewed_ms{};
    bool watchdog_active{};
    std::string fault;
};
struct MotorClock {
    virtual ~MotorClock()=default;
    virtual std::uint64_t now_ms() const noexcept=0;
};
enum class MotorWaveform { sine,square };
double motor_wave_period(MotorWaveform,double amplitude,double velocity,double acceleration,double interval_seconds) noexcept;
}
