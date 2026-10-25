#pragma once
#include "motor_types.hpp"
#include <array>
#include <future>
#include <span>
#include <vector>

namespace hwio {
struct MotorLoopGains {
    double kp{},ki{},kd{},vff{},aff{};
    bool operator==(const MotorLoopGains&)const=default;
    void apply(MotorParameters& p)const noexcept {p.kp=kp;p.ki=ki;p.kd=kd;p.vff=vff;p.aff=aff;}
    static MotorLoopGains from(const MotorParameters& p)noexcept{return {p.kp,p.ki,p.kd,p.vff,p.aff};}
};
enum class MotorTunePhase { idle,noise,probe,identify,analysis,validate,returning,finished,failed,cancelled };
struct MotorTuningRequest {
    std::string motor_identity,feedback_identity;
    MotorDriveMode mode{MotorDriveMode::pwm};
    std::uint64_t run{},session{},coordinate{},draft_revision{};
    double travel{},buffer{},quantum{1},code_count{};
};
struct MotorTuningSample {
    std::uint64_t ms{},sequence{},generation{},accepted_ms{};
    double position{},requested{},accepted{},applied{};
    unsigned traversal{};
    bool fresh{},applied_valid{};
    MotorTunePhase phase{};std::uint64_t observed_ms{},requested_ms{};
    bool telemetry_available{}; // SDK getter output; not an independent arrival event.
    double raw_position{};
};
struct MotorIdentifiedModel {
    std::array<double,2> gain{},bias{}; // Positive/negative normalized drive -> selected sensor counts/s.
    double tau{},delay{},rms{},validation_rms{},interval{.05},quantum{1},pwm_ramp{},backlash{};
    bool inertia_resolved{};
    std::array<double,32> timing{};unsigned timing_count{};
};
struct MotorTuningResult {
    bool success{},restored{},aff_identified{};
    MotorLoopGains gains;
    MotorLoopGains retry_gains;bool retry_available{};
    MotorIdentifiedModel model;
    unsigned evaluations{},observations{},retries{};
    double retry_phase_margin{},retry_gain_margin_db{};
    double phase_margin{},gain_margin_db{},tracking_rms{},overshoot{},drive_variation{},covered_travel{},covered_speed{},settling_seconds{};
    std::string reason;
};
struct MotorTuningStatus {
    std::string motor_identity,feedback_identity;
    MotorParameters parameters;MotorDriveMode mode{MotorDriveMode::pwm};double code_count{};
    MotorTunePhase phase{MotorTunePhase::idle};
    std::uint64_t run{},session{},coordinate{},draft_revision{},started_ms{},elapsed_ms{};
    double centre{},low{},high{},jitter{},quantum{},buffer{},tolerance{},target{};
    bool active{},direct_drive{},command_valid{},finalizing{};
    MotorTuningResult result;
    std::string message;
};
// Pure, bounded analysis. No SDK or hardware access; also used by authored plants.
MotorTuningResult analyse_motor_tuning(std::span<const MotorTuningSample>,const MotorParameters&,double travel,double tolerance,double quantum,MotorDriveMode=MotorDriveMode::pwm);
struct MotorTuningAction {bool active{},direct{},stop{};double value{};std::optional<MotorLoopGains> gains;};
// Serviced by the motor worker. Only analysis runs on a second thread; sampling
// storage is reserved before excitation and never grows beyond its fixed cap.
class MotorAutoTuner {
    MotorTuningStatus status_;
    MotorTuningRequest request_;
    MotorParameters parameters_;
    std::vector<MotorTuningSample> samples_;
    std::vector<MotorTuningSample> trace_;
    std::future<MotorTuningResult> analysis_;
    std::uint64_t phase_ms_{},last_sequence_{},last_generation_{},settled_ms_{},fresh_ms_{};
    double noise_low_{},noise_high_{},start_{},effort_{.025},breakaway_{},last_position_{},last_velocity_{},direct_{},last_drive_{},validation_seconds_{};
    double fresh_position_{},measured_velocity_{},settling_position_{},command_latency_{};
    unsigned probe_{},traversal_{},validation_{},retry_{};
    bool gain_pending_{},stopping_{};
    void phase(MotorTunePhase,std::uint64_t,const char*);
    MotorTuningAction finish(bool,std::string);
public:
    MotorAutoTuner();~MotorAutoTuner();
    bool start(MotorTuningRequest,const MotorSnapshot&,std::uint64_t,std::string&);
    MotorTuningAction tick(const MotorSnapshot&,std::uint64_t,double dt);
    void command_accepted(std::uint64_t sample_ms,std::uint64_t requested_ms,std::uint64_t accepted_ms,double requested,double accepted);
    void cancel(std::string reason);
    const MotorTuningStatus& status()const noexcept{return status_;}
    std::span<const MotorTuningSample> trace()const noexcept{return trace_;}
    bool analysis_pending()const;
    void wait_analysis()const {if(analysis_.valid())analysis_.wait();}
    void restoration(bool ok,std::string reason={});
    void finalizing(bool value){status_.finalizing=value;}
    void completed_at(std::uint64_t now){if(status_.run && now>=status_.started_ms)status_.elapsed_ms=now-status_.started_ms;}
};
}
