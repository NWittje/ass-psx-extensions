#pragma once
#include "core.hpp"
#include "phidget_motor.hpp"
#include "motor_feedback.hpp"
#include "phidget_io.hpp"
#include "servo.hpp"
#include "stepper.hpp"
#include <thread>
#include <mutex>
#include <atomic>
namespace hwio {
class SoftwarePositionMotor;
struct PhidgetDevice {int serial{},model{},version{};unsigned channels{};std::string name,key;bool operator==(const PhidgetDevice&) const=default;};
struct PhidgetInventory {std::vector<PhidgetDevice> devices;std::string status{"Disabled"};std::uint64_t revision{};std::vector<MotorDeviceEntry> position_motors,velocity_motors,encoders;std::vector<PhidgetChannel> channels;};
// A declared counter on this exact motor, not an unrelated hub/USB encoder.
// Discovery cannot establish that an encoder is physically wired or moving.
bool phidget_motor_shaft_available(const PhidgetInventory&,const MotorDeviceAddress&) noexcept;
// Informational Manager channels never open motors. Explicit internal callers
// create and open a position-controller object through this service.
class PhidgetDiscovery {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit PhidgetDiscovery(std::wstring library={});~PhidgetDiscovery();void enable(bool);PhidgetInventory inventory();
    std::shared_ptr<PhidgetMotor> create_motor(MotorAddress,MotorParameters,std::shared_ptr<MotorClock> clock={});
    std::shared_ptr<PositionMotorBackend> position_backend(const MotorDeviceAddress&);
    std::shared_ptr<PositionMotorBackend> software_backend(const MotorDeviceAddress&,std::function<MotorPositionSample(const MotorFeedbackDefinition&)>);
    MotorBench::ShaftReader shaft_reader(const MotorDeviceAddress&);
    std::shared_ptr<ServoBackend> servo_backend(const MotorDeviceAddress&);
    std::shared_ptr<PositionMotorBackend> stepper_backend(const StepperDefinition&,std::function<MotorPositionSample(const MotorFeedbackDefinition&)>);
    void servers(const std::vector<PhidgetServer>&); // Explicit caller configuration, not automatic server discovery.
};
}
