#pragma once
#include "motor_feedback.hpp"
#include "motor_position_loop.hpp"
#include "phidget_session.hpp"
#include <stdexcept>

namespace hwio {
struct MotorDriveFailure:std::runtime_error {int code;MotorDriveFailure(int rc,const std::string& operation):std::runtime_error(operation+": native error "+std::to_string(rc)),code(rc){}};
struct MotorDriveObservation {bool attached{},release_confirmed{};std::optional<double> reported;};
// Called exclusively by the software motor worker, never an SDK callback/UI.
class MotorDrive {
public:
    virtual ~MotorDrive()=default;
    virtual MotorCapabilities open()=0;
    virtual void configure(const MotorParameters&)=0;
    virtual void command(double normalized)=0;
    virtual void release()=0;
    virtual void renew()=0;
    virtual bool hardware_watchdog_active() const noexcept {return true;}
    virtual MotorDriveObservation observation()=0;
    // Optional independent source. Analogue-feedback motors do not require or
    // open an encoder channel; providers without one return unavailable.
    virtual MotorPositionSample encoder(const MotorFeedbackDefinition&){return {};}
    virtual void close()noexcept=0;
};
std::unique_ptr<MotorDrive> phidget_motor_drive(std::shared_ptr<PhidgetSession>,MotorDeviceAddress,std::optional<PhidgetServer>,std::vector<PhidgetServer> = {});
class SoftwarePositionMotor final:public PositionMotorBackend {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    SoftwarePositionMotor(std::unique_ptr<MotorDrive>,std::function<MotorPositionSample(const MotorFeedbackDefinition&)>,std::shared_ptr<MotorClock> = {});
    ~SoftwarePositionMotor();
    void provider_enabled(bool);
    void feedback(const MotorFeedbackDefinition&)override;
    MotorCommandResult inspect()override;MotorCommandResult configure(MotorParameters)override;
    MotorCommandResult tuning(double,double,double)override;MotorCommandResult feedforward(double,double)override;
    MotorCommandResult gains(MotorLoopGains)override;
    MotorCommandResult auto_tune(MotorTuningRequest)override;MotorTuningStatus tuning_status()const override;
    MotorCommandResult diagnostic_drive(double)override;
    std::vector<MotorTuningSample> tuning_samples()const override;
    MotorCommandResult limits(double,double)override;MotorCommandResult target(double)override;
    MotorCommandResult reference(double,double,double)override;MotorCommandResult engage()override;
    MotorCommandResult jog(double)override;MotorCommandResult wave(MotorWaveform,double)override;
    MotorCommandResult renew()override;MotorCommandResult stop()override;MotorCommandResult disengage()override;MotorCommandResult close()override;
    void guard(MotorGuardSnapshot)override;bool quiescent()const override;
    MotorSnapshot snapshot()const override;std::optional<MotorCommandResult> result(std::uint64_t)const override;
};
}
