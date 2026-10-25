#pragma once
#include "stepper.hpp"
#include "numeric_source.hpp"
#include <mutex>
namespace hwio {
constexpr unsigned actuator_mapping_slots=60;
struct ActuatorMapping {
    bool enabled{},auto_home{true};std::string name;ActuatorRef output;NumericSource source;
    unsigned analogue_output{UINT_MAX}; // Stable UI slot; legacy engine/IPC indices remain unchanged.
    ActuatorCalibration calibration{ActuatorCalibration::switches};
    std::array<MotorFeedbackDefinition,2> switches;std::array<bool,2> active_high{true,true};
    MotorFeedbackDefinition potentiometer;double sensor_min{},sensor_max{},command_min{},command_max{};
    SwitchHomeSettings home;
    RotaryUse rotary{RotaryUse::needle};NeedleStartup startup{NeedleStartup::full_signed};
    double source_revolution{1000},output_revolution{},index_window{.05};int direction{1};
    bool hold_source_loss{true};
    bool operator==(const ActuatorMapping&)const=default;
};
enum class ActuatorPhase:unsigned {stopped,waiting,homing,following,holding,fault,bench};
struct ActuatorView {
    ActuatorMapping definition;ActuatorPhase phase{ActuatorPhase::stopped};ReferenceQuality quality{};
    bool referenced{},release_confirmed{},latched{true},reset_pending{},major_fault{};double position{},target{},source{};
    std::uint64_t generation{},revision{},fault_revision{},operation_id{};std::string status;
};
bool actuator_mapping_valid(const ActuatorMapping&,std::string&);
bool load_actuator_mappings(const std::wstring&,std::array<ActuatorMapping,actuator_mapping_slots>&,std::string&);
bool save_actuator_mappings(const std::wstring&,const std::array<ActuatorMapping,actuator_mapping_slots>&,std::string&);
std::string actuator_mappings_encode(const std::array<ActuatorMapping,actuator_mapping_slots>&,std::string&);
// Thin existing-service adapters; they share the original physical owner.
std::shared_ptr<PositionMotorBackend> actuator_motor_port(MotorBench&,unsigned,std::string&);
class ServoIndicators;
std::shared_ptr<PositionMotorBackend> actuator_servo_port(ServoIndicators&,unsigned,std::string&);
class Actuators {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    using Factory=std::function<std::shared_ptr<PositionMotorBackend>(ActuatorRef,std::string&)>;
    using Sampler=std::function<MotorPositionSample(const MotorFeedbackDefinition&)>;
    using EdgeSampler=std::function<unsigned(const MotorFeedbackDefinition&,std::uint64_t,MotorPositionSample*,unsigned,bool&)>;
    Actuators(std::wstring,Factory,Sampler,EdgeSampler={},std::shared_ptr<MotorClock> = {});~Actuators();
    std::array<ActuatorMapping,actuator_mapping_slots> definitions()const;
    bool assign(unsigned,ActuatorMapping,std::string&);ActuatorView snapshot(unsigned)const;
    bool home(unsigned,std::uint64_t operation,std::uint64_t fault,std::string&);void stop(unsigned,std::uint64_t operation=0);
    void bench(ActuatorRef,bool);void observe(const Mapper&,bool enabled);void close();
    std::vector<MotorFeedbackDefinition> feedback_definitions()const;
    std::vector<std::string> source_records()const;std::vector<std::string> demand_records()const;std::uint64_t revision()const;
};
}
