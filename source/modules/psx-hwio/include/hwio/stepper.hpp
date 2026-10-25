#pragma once
#include "software_motor.hpp"
#include "actuator_motion.hpp"
#include <array>
namespace hwio {
constexpr unsigned stepper_slots=20;
struct StepperDefinition {
    std::string name;MotorDeviceAddress address;MotorFeedbackDefinition feedback;
    bool run_mode{};double current{},holding_current{},velocity{},acceleration{};
    double full_steps{},gearing{1}; // Motor full steps/rev; motor revs/output rev.
    double kp{},ki{},kd{},vff{},aff{},feedback_velocity{},feedback_acceleration{},backlash{};
    unsigned interval_ms{50},failsafe_ms{1000};
    bool operator==(const StepperDefinition&)const=default;
    double output_counts()const noexcept{return full_steps*16*gearing;}
};
bool stepper_definition_valid(const StepperDefinition&,bool motion,std::string&);
bool load_stepper_settings(const std::wstring&,std::array<StepperDefinition,stepper_slots>&,std::string&);
bool save_stepper_settings(const std::wstring&,const std::array<StepperDefinition,stepper_slots>&,std::string&);
std::shared_ptr<PositionMotorBackend> phidget_stepper_backend(std::shared_ptr<PhidgetSession>,const StepperDefinition&,std::optional<PhidgetServer>,std::function<MotorPositionSample(const MotorFeedbackDefinition&)>);
struct StepperView {StepperDefinition draft,saved;MotorSnapshot live;bool dirty{},bench{};std::string status;};
class Steppers {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    using Factory=std::function<std::shared_ptr<PositionMotorBackend>(const StepperDefinition&)>;
    Steppers(std::wstring,Factory);~Steppers();
    StepperView snapshot(unsigned)const;
    bool edit(unsigned,StepperDefinition,std::string&);bool save(unsigned,std::string&);void revert(unsigned);void clear(unsigned);
    void inspect(unsigned);void bench(int);void drive(unsigned,int,bool begin=false);void hold(unsigned);void stop(unsigned);void close();void provider_enabled(bool);
    bool move_to(unsigned,double,std::string&);bool zero(unsigned,std::string&);
    std::shared_ptr<PositionMotorBackend> acquire(unsigned,std::string&);
    MotorParameters parameters(unsigned)const;
    std::array<MotorFeedbackDefinition,stepper_slots> feedback_definitions()const;std::uint64_t revision()const;
    std::array<StepperDefinition,stepper_slots> saved()const;
};
}
