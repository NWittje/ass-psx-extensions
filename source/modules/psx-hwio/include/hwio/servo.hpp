#pragma once
#include "motor_bench.hpp"
#include "phidget_session.hpp"
#include "numeric_source.hpp"
#include <array>
#include <functional>
#include <cmath>

namespace hwio {
class Mapper;
struct ServoAddress:MotorDeviceAddress {
    int minimum_firmware{};
    ServoAddress()=default;ServoAddress(MotorDeviceAddress a):MotorDeviceAddress(std::move(a)){}
};
// Pulse microseconds are command coordinates, never measured shaft degrees.
struct ServoParameters {
    double pulse_min{},pulse_max{};
    std::optional<double> velocity,acceleration,torque;
    std::optional<int> voltage;
    bool ramping{true};unsigned interval_ms{100},failsafe_ms{};
    bool operator==(const ServoParameters&)const=default;
};
struct ServoCapabilities {
    int model{},firmware{};double pulse_min{},pulse_max{},velocity_min{},velocity_max{},acceleration_min{},acceleration_max{},torque_min{},torque_max{};
    unsigned interval_min{},interval_max{},failsafe_min{},failsafe_max{};
    bool profile{},ramping{},velocity{},acceleration{},interval{},torque{},voltage{},failsafe{},startup_pulse_possible{};
};
struct ServoSnapshot {
    ServoCapabilities capabilities;ServoParameters requested,applied;
    bool attached{},configured{},released_at_target{};MotorDriveState drive{MotorDriveState::unknown};
    std::optional<double> desired,accepted,profile_position,profile_velocity,current;
    std::optional<bool> moving;
    std::uint64_t generation{},revision{},accepted_id{},accepted_ms{},position_event_ms{},velocity_event_ms{},checked_ms{},current_ms{};
    std::string fault;int fault_code{};MotorCommandResult last_result;
};
// Controller-profile completion is not shaft feedback. The fixed settling time
// only delays signal removal; a digital servo may still hold without pulses.
class ServoRelease {
    std::uint64_t command_{},since_{};bool stopped_{};
public:
    void reset()noexcept{stopped_=false;}
    bool due(const ServoSnapshot& s,std::uint64_t now)noexcept{
        if(!s.configured || s.drive!=MotorDriveState::on || !s.accepted || !s.profile_position || !s.moving || *s.moving || std::abs(*s.profile_position-*s.accepted)>1){reset();return false;}
        if(!stopped_ || command_!=s.accepted_id || now<since_){stopped_=true;command_=s.accepted_id;since_=now;return false;}
        return now-since_>=200;
    }
};
class ServoBackend {
public:
    virtual ~ServoBackend()=default;
    virtual MotorCommandResult inspect()=0;
    virtual MotorCommandResult configure(ServoParameters)=0;
    virtual MotorCommandResult target(double)=0;
    virtual MotorCommandResult engage()=0;
    virtual MotorCommandResult hold()=0;
    virtual MotorCommandResult disengage()=0;
    virtual MotorCommandResult close()=0;
    virtual ServoSnapshot snapshot()const=0;
    virtual std::optional<MotorCommandResult> result(std::uint64_t)const=0;
    virtual void provider_enabled(bool)=0;
};
class PhidgetServo final:public ServoBackend {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    PhidgetServo(std::shared_ptr<PhidgetSession>,ServoAddress,std::optional<PhidgetServer> = {});
    ~PhidgetServo();PhidgetServo(const PhidgetServo&)=delete;PhidgetServo& operator=(const PhidgetServo&)=delete;
    MotorCommandResult inspect()override;MotorCommandResult configure(ServoParameters)override;
    MotorCommandResult target(double)override;MotorCommandResult engage()override;MotorCommandResult hold()override;
    MotorCommandResult disengage()override;MotorCommandResult close()override;
    ServoSnapshot snapshot()const override;std::optional<MotorCommandResult> result(std::uint64_t)const override;void provider_enabled(bool)override;
};
using ServoSource=NumericSource;
struct ServoCalibrationPoint {
    double value{},pulse{};
    bool operator==(const ServoCalibrationPoint&)const=default;
};
struct ServoDefinition {
    std::string name;MotorDeviceAddress address;ServoSource source;ServoParameters parameters;
    std::optional<double> at_min,at_max;
    std::optional<double> at_centre;
    std::array<std::optional<ServoCalibrationPoint>,2> manual_points;
    bool manual_values{};
    unsigned update_ms{100};double deadband_us{1};
    bool operator==(const ServoDefinition&)const=default;
};
struct ServoCurve {
    std::array<ServoCalibrationPoint,5> points{};
    unsigned count{};
    double pulse_min{},pulse_max{};
    bool compile(const ServoDefinition&)noexcept;
    std::optional<double> map(double)const noexcept;
};
constexpr unsigned servo_slots=20;
enum class ServoPhase {unassigned,needs_calibration,ready,calibrating,following,source_unavailable,detached,fault};
struct ServoView {
    ServoDefinition draft,saved;ServoSnapshot live;ServoPhase phase{ServoPhase::unassigned};
    std::optional<double> source_value,manual;std::string status;bool dirty{};std::uint64_t revision{};
};
bool servo_definition_valid(const ServoDefinition&,std::string&);
std::optional<double> servo_numeric(std::string_view,int,unsigned)noexcept;
std::optional<double> servo_pulse(const ServoDefinition&,double)noexcept;
std::vector<ServoSource> servo_sources();
bool load_servo_settings(const std::wstring&,std::array<ServoDefinition,servo_slots>&,std::string&);
bool save_servo_settings(const std::wstring&,const std::array<ServoDefinition,servo_slots>&,std::string&);
class ServoIndicators {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    using Factory=std::function<std::shared_ptr<ServoBackend>(const MotorDeviceAddress&)>;
    ServoIndicators(std::wstring,Factory);~ServoIndicators();
    ServoView snapshot(unsigned)const;
    bool edit(unsigned,ServoDefinition,std::string&);
    bool save(unsigned,std::string&);void revert(unsigned);void clear(unsigned);
    void inspect(unsigned);void calibrate(unsigned);void manual(unsigned,double);void capture(unsigned,bool maximum);
    void manual_page(unsigned,bool);
    bool assign_point(unsigned,unsigned,std::optional<double>,std::string&);
    bool save_automatic(unsigned,std::string&);
    void follow(unsigned);void stop(unsigned);void cancel(unsigned);void hold(unsigned);
    void observe(Mapper&,const std::vector<struct PhidgetReading>&);
    std::uint64_t source_revision()const noexcept;
    std::vector<std::string> source_records()const;std::vector<std::string> demand_records()const;
    void provider_enabled(bool);void close();std::array<ServoDefinition,servo_slots> saved()const;
    std::shared_ptr<ServoBackend> external(unsigned,bool,std::string&);
    bool external_active(unsigned)const;
};
}
