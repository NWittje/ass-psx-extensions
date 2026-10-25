#pragma once
#include "phidget_session.hpp"
#include "motor_types.hpp"
#include "motor_motion.hpp"
#include <cstdint>
#include <optional>

namespace hwio {
struct MotorAddress {
    PhidgetMotorResource resource;
    bool remote{};
    PhidgetServer server;
    int model{},minimum_firmware{}; // Zero accepts the discovered compatible model/revision.
};
// Callers persist parameters. SDK acceptance and measured attainment are
// separate results; neither establishes physical lever/clutch correspondence.
class PhidgetMotor {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    PhidgetMotor(std::shared_ptr<PhidgetSession>,MotorAddress,MotorParameters,std::shared_ptr<MotorClock> clock={});
    ~PhidgetMotor();
    PhidgetMotor(const PhidgetMotor&)=delete;
    PhidgetMotor& operator=(const PhidgetMotor&)=delete;
    MotorCommandResult open();
    MotorCommandResult inspect(); // Open/read capabilities and feedback, without applying tuning or energizing.
    MotorCommandResult update(MotorParameters);
    MotorCommandResult update_tuning(double kp,double ki,double kd);
    MotorCommandResult update_limits(double velocity,double acceleration);
    MotorCommandResult start_wave(MotorWaveform,double amplitude);
    MotorCommandResult set_reference(double coordinate,double minimum,double maximum);
    MotorCommandResult engage();
    MotorCommandResult move_to(double);
    MotorCommandResult move_by(double);
    MotorCommandResult jog(double units_per_second);
    MotorCommandResult renew_jog();
    MotorCommandResult stop();
    MotorCommandResult disengage();
    MotorCommandResult close();
    void provider_enabled(bool);
    void guard(MotorGuardSnapshot);
    void wake();
    MotorSnapshot snapshot() const;
    std::optional<MotorCommandResult> result(std::uint64_t id) const;
};
}
