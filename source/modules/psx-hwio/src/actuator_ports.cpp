#include "hwio/actuators.hpp"
#include "hwio/servo.hpp"
namespace hwio {
namespace {
MotorCommandResult unsupported(){return {0,MotorResultStage::rejected,MotorError::unsupported,0,"Operation is not available for this actuator"};}
class MotorPort final:public PositionMotorBackend {
    MotorBench& bank_;unsigned n_;
public:
    MotorPort(MotorBench& b,unsigned n):bank_(b),n_(n){}~MotorPort(){bank_.release_mapping(n_);}
    MotorCommandResult inspect()override{return {0,MotorResultStage::host_applied};}MotorCommandResult configure(MotorParameters)override{return inspect();}
    MotorCommandResult tuning(double,double,double)override{return unsupported();}
    MotorCommandResult limits(double v,double a)override{return bank_.mapping_limits(n_,v,a);}MotorCommandResult target(double v)override{return bank_.mapping_target(n_,v);}
    MotorCommandResult engage()override{return bank_.mapping_engage(n_);}MotorCommandResult reference(double,double,double)override{return unsupported();}
    MotorCommandResult jog(double)override{return unsupported();}MotorCommandResult wave(MotorWaveform,double)override{return unsupported();}
    MotorCommandResult renew()override{bank_.mapping_heartbeat(n_);return {0,MotorResultStage::host_applied};}
    MotorCommandResult stop()override{return bank_.mapping_stop(n_,false);}MotorCommandResult disengage()override{return bank_.mapping_stop(n_,true);}MotorCommandResult close()override{return bank_.release_mapping(n_);}
    void guard(MotorGuardSnapshot g)override{bank_.guard(n_,g);}
    MotorSnapshot snapshot()const override{const auto v=bank_.snapshot(n_);auto s=v.live;if(!v.mapping_owned){s.configured=false;s.fault=v.manual_page?"Manual page owns this motor":"Motor mapping ownership withdrawn";}return s;}
    std::optional<MotorCommandResult> result(std::uint64_t id)const override{return id?bank_.mapping_result(n_,id):std::optional(MotorCommandResult{0,MotorResultStage::host_applied});}
};
class ServoPort final:public PositionMotorBackend {
    ServoIndicators& bank_;unsigned n_;std::shared_ptr<ServoBackend> servo_;
public:
    ServoPort(ServoIndicators& b,unsigned n,std::shared_ptr<ServoBackend> s):bank_(b),n_(n),servo_(std::move(s)){}
    ~ServoPort(){std::string error;bank_.external(n_,false,error);}
    bool target_before_engage()const noexcept override{return true;}
    bool released_at_target()const override{return servo_->snapshot().released_at_target;}
    MotorCommandResult inspect()override{return servo_->inspect();}MotorCommandResult configure(MotorParameters)override{return {0,MotorResultStage::host_applied};}
    MotorCommandResult tuning(double,double,double)override{return unsupported();}
    MotorCommandResult limits(double v,double a)override{auto p=servo_->snapshot().applied;p.velocity=v;p.acceleration=a;return servo_->configure(p);}
    MotorCommandResult target(double p)override{return bank_.external_active(n_)?servo_->target(p):unsupported();}
    MotorCommandResult engage()override{return bank_.external_active(n_)?servo_->engage():unsupported();}
    MotorCommandResult reference(double,double,double)override{return unsupported();}MotorCommandResult jog(double)override{return unsupported();}MotorCommandResult wave(MotorWaveform,double)override{return unsupported();}
    MotorCommandResult renew()override{return {0,MotorResultStage::host_applied};}
    MotorCommandResult stop()override{return servo_->hold();}MotorCommandResult disengage()override{return servo_->disengage();}MotorCommandResult close()override{return servo_->disengage();}
    MotorSnapshot snapshot()const override{const auto v=servo_->snapshot();MotorSnapshot s;s.attached=v.attached;s.configured=v.configured && bank_.external_active(n_);s.feedback_valid=v.profile_position.has_value();s.position=v.profile_position.value_or(v.accepted.value_or(0));s.target=v.accepted.value_or(0);s.desired_target=v.desired.value_or(0);s.target_revision_valid=v.accepted.has_value();s.target_attained=v.moving.has_value() && !*v.moving;s.target_command_id=v.accepted_id;s.target_accepted_ms=v.accepted_ms;s.checked_ms=v.checked_ms;s.generation=v.generation;s.configuration_revision=v.revision;s.drive=v.drive;s.engaged=v.drive==MotorDriveState::on;s.fault=v.fault;if(!bank_.external_active(n_))s.fault="Servo ownership withdrawn";s.capabilities.minimum_position=v.applied.pulse_min;s.capabilities.maximum_position=v.applied.pulse_max;s.applied.velocity_limit=v.applied.velocity.value_or(v.capabilities.velocity_max);s.applied.acceleration=v.applied.acceleration.value_or(v.capabilities.acceleration_max);return s;}
    std::optional<MotorCommandResult> result(std::uint64_t id)const override{return id?servo_->result(id):std::optional(MotorCommandResult{0,MotorResultStage::host_applied});}
};
}
std::shared_ptr<PositionMotorBackend> actuator_motor_port(MotorBench& bank,unsigned n,std::string& error){if(!bank.claim_mapping(n,error)){const auto state=bank.snapshot(n);if(!state.pending && !state.live.attached && !state.mapping_owned && !state.manual_page)bank.activate(n);return {};}return std::make_shared<MotorPort>(bank,n);}
std::shared_ptr<PositionMotorBackend> actuator_servo_port(ServoIndicators& bank,unsigned n,std::string& error){auto backend=bank.external(n,true,error);return backend?std::make_shared<ServoPort>(bank,n,std::move(backend)):nullptr;}
}
