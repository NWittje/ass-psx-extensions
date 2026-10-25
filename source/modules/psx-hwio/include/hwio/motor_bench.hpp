#pragma once
#include "motor_types.hpp"
#include "motor_motion.hpp"
#include "motor_autotune.hpp"
#include "motor_backlash_find.hpp"
#include <array>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace hwio {
constexpr unsigned position_motor_slots=10,motor_slots=20;
struct MotorDeviceAddress {
    MotorDriveMode mode{MotorDriveMode::position};
    std::string provider{"phidget22"},device,server;
    int serial{},hub_port{-1},channel{-1},model{};
    unsigned port{5661};bool remote{};
    bool operator==(const MotorDeviceAddress&)const=default;
    bool assigned()const noexcept{return !device.empty() && channel>=0;}
    bool same_physical(const MotorDeviceAddress& other)const noexcept{
        if(!assigned() || !other.assigned() || provider!=other.provider)return false;
        if(provider=="phidget22")return serial==other.serial && ((hub_port<0)!=(other.hub_port<0) || (hub_port==other.hub_port && channel==other.channel));
        return device==other.device && channel==other.channel;
    }
};
struct MotorDeviceEntry {MotorDeviceAddress address;std::string name;bool connected{};bool operator==(const MotorDeviceEntry&)const=default;};
enum class MotorPositionSource { none,analogue,encoder };
struct MotorFeedbackDefinition {
    MotorPositionSource kind{MotorPositionSource::none};
    std::string input_profile; // Existing stable device/control profile encoding.
    MotorDeviceAddress encoder;
    bool inverted{};
    bool operator==(const MotorFeedbackDefinition&)const=default;
};
struct MotorDefinition {
    MotorFeedbackDefinition feedback;
    MotorDriveParameters drive;
    MotorDeviceAddress address;
    std::optional<double> minimum,maximum,maximum_velocity,maximum_acceleration,current_limit,vff,aff;
    std::optional<double> sensor_buffer; // Raw feedback ADC units; blank selects resolution-based Auto.
    double backlash{}; // Total directional take-up in raw potentiometer codes; zero disables it.
    // Onboard gains use standardized encoder units; software-loop gains use
    // selected feedback counts. Never migrate gains between those two modes.
    double velocity_fraction{.2},acceleration_percent{10},kp{},ki{},kd{},amplitude{10};
    MotorWaveform waveform{MotorWaveform::sine};
    bool operator==(const MotorDefinition&)const=default;
};
// One physical position loop, independent of manufacturer/UI. Backends queue
// commands; only their workers call SDKs. SDK acceptance is distinct from motion.
class PositionMotorBackend {
public:
    virtual ~PositionMotorBackend()=default;
    virtual MotorCommandResult inspect()=0;
    virtual MotorCommandResult configure(MotorParameters)=0;
    virtual MotorCommandResult tuning(double,double,double)=0;
    virtual MotorCommandResult feedforward(double,double){return {};}
    virtual MotorCommandResult gains(MotorLoopGains){return {0,MotorResultStage::rejected,MotorError::unsupported,0,"Atomic host gains unavailable"};}
    virtual MotorCommandResult auto_tune(MotorTuningRequest){return {0,MotorResultStage::rejected,MotorError::unsupported,0,"Automatic tuning unavailable"};}
    // Explicit Bench experiment only; normalized fraction of configured drive
    // ceiling. Never used by mapped following or an ordinary position target.
    virtual MotorCommandResult diagnostic_drive(double){return {0,MotorResultStage::rejected,MotorError::unsupported,0,"Direct drive measurement unavailable"};}
    virtual MotorTuningStatus tuning_status()const{return {};}
    virtual std::vector<MotorTuningSample> tuning_samples()const{return {};}
    virtual void feedback(const MotorFeedbackDefinition&) {}
    virtual MotorCommandResult limits(double,double)=0;
    virtual MotorCommandResult target(double)=0;
    virtual MotorCommandResult reference(double,double,double)=0;
    virtual MotorCommandResult engage()=0;
    virtual bool target_before_engage()const noexcept{return false;}
    // Servo signals may be deliberately removed after a completed command.
    // An unchanged mapped target must not immediately engage them again.
    virtual bool released_at_target()const{return false;}
    virtual MotorCommandResult jog(double)=0;
    virtual MotorCommandResult wave(MotorWaveform,double)=0;
    virtual MotorCommandResult renew()=0;
    virtual MotorCommandResult stop()=0;
    virtual MotorCommandResult disengage()=0;
    virtual MotorCommandResult close()=0;
    virtual MotorSnapshot snapshot()const=0;
    virtual void guard(MotorGuardSnapshot) {}
    virtual bool quiescent()const{return true;}
    virtual std::optional<MotorCommandResult> result(std::uint64_t)const=0;
};
using MotorBackendFactory=std::function<std::shared_ptr<PositionMotorBackend>(const MotorDeviceAddress&)>;
struct MotorTraceSample {std::uint64_t ms{};double command{},actual{};bool valid{};bool command_valid{true};};
struct MotorInitializationFailure {
    std::uint64_t revision{};bool known_session{},uncertain{};std::string reason;
};
struct MotorBenchView {
    BacklashStatus backlash_find;
    MotorTuningStatus automatic;
    MotorDefinition draft,saved;MotorSnapshot live;MotorCommandResult release;
    bool dirty{},pending{},running{},jogging{},mapping_owned{},operator_stopped{},manual_page{};std::string status;
    double centre{},period{5};std::uint64_t end_ms{},history_revision{},command_overflows{},initialization_failure_revision{};
};
bool motor_definition_valid(const MotorDefinition&,std::string&);
bool load_motor_settings(const std::wstring&,std::array<MotorDefinition,motor_slots>&,std::string&);
bool save_motor_settings(const std::wstring&,const std::array<MotorDefinition,motor_slots>&,std::string&);
class MotorBench {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    MotorBench(std::wstring directory,MotorBackendFactory,std::shared_ptr<MotorClock> = {});
    ~MotorBench();MotorBench(const MotorBench&)=delete;MotorBench& operator=(const MotorBench&)=delete;
    void activate(unsigned);void assign(unsigned,MotorDeviceAddress);void edit(unsigned,MotorDefinition,bool tuning_only=false);
    void clear(unsigned);void save(unsigned);void revert(unsigned);void zero(unsigned);
    void drive(unsigned,int direction);void play(unsigned);void hold(unsigned);void disengage(unsigned);
    void auto_tune(unsigned); // A second click cancels; only Motor10–19.
    using PositionSampler=std::function<MotorPositionSample(const MotorFeedbackDefinition&)>;
    using ShaftReader=std::function<MotorPositionSample()>;
    void measurement_sources(PositionSampler,std::function<ShaftReader(const MotorDeviceAddress&)>);
    void find_backlash(unsigned,MotorFeedbackDefinition temporary_potentiometer={});
    void heartbeat(unsigned);void hide();void stop();void wake();
    // Only the visible detail page owns this transient bypass. Changing pages
    // cancels queued motion before another owner can claim the motor.
    void manual_page(int slot);
    MotorBenchView snapshot(unsigned)const;
    std::uint64_t feedback_revision()const;
    std::array<MotorFeedbackDefinition,motor_slots> feedback_definitions()const;
    void guard(unsigned,MotorGuardSnapshot);
    // Mapping ownership excludes bench motion. Calls enqueue only; SDK work
    // stays on each physical motor's worker.
    // Recovery retains failure history. An old Home request must observe a
    // failure even when explicit channel recovery completed between its ticks.
    std::optional<MotorInitializationFailure> initialization_failure_since(unsigned,std::uint64_t,bool known_session_only=false)const;
    bool claim_mapping(unsigned,std::string&);
    MotorCommandResult release_mapping(unsigned);
    MotorCommandResult mapping_engage(unsigned);
    MotorCommandResult mapping_target(unsigned,double);
    MotorCommandResult mapping_limits(unsigned,double,double);
    MotorCommandResult mapping_stop(unsigned,bool disengage);
    void mapping_heartbeat(unsigned);
    std::optional<MotorCommandResult> mapping_result(unsigned,std::uint64_t)const;
    std::vector<MotorTraceSample> history(unsigned)const;
    std::string tuning_report()const;
};
}
