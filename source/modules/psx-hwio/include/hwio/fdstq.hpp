#pragma once
#include "hid.hpp"
#include "digital_outputs.hpp"
#include "motor_types.hpp"
namespace hwio {
namespace fds {class Trace;}
struct FdstqStepperConfiguration {
    bool enabled{}, reversed{};
    double current{}, acceleration{}, velocity{};
    bool operator==(const FdstqStepperConfiguration&) const=default;
};
// FDSTQ is a Phidget22 assembly, not an interfaceIT controller identity.
struct FdstqConfiguration {
    bool enabled{}, remote{true}, paired{true};
    int serial{}; std::string host; unsigned port{5661}, inverted_switches{};
    std::array<std::string,5> outputs{};
    int hub_serial{}, servo_serial{};
    std::array<FdstqStepperConfiguration,3> steppers{{{false,false,1.51},{false,false,1.51},{false,false,2.5}}};
    unsigned pair_follow{}; // 0 first engine, 1 average, 2 require agreement.
    bool trim_enabled{}, servo_enabled{};
    double trim_counts_per_degree{},trim_velocity{},trim_acceleration{.2};
    double servo_zero{},servo_full{},servo_velocity{},servo_acceleration{};
    bool operator==(const FdstqConfiguration&) const=default;
};
struct FdstqMotion {
    std::uint64_t generation{},psx_epoch{};
    std::array<bool,3> enabled{};
    std::array<double,3> target{}, zero{}, full{}; // Physical ADC counts.
    bool speed_follow{},trim{},servo{};double trim_degrees{};
    bool ios_thrust{};std::uint64_t ios_token{},ios_operation{};
};
struct FdstqThrust {bool owned{},drive{};std::uint64_t token{},operation{};std::array<double,4> percent{};};
struct FdstqDriveStatus {std::uint64_t generation{},operation{};bool available{},valid{},held{},limited{};MotorDriveState drive{MotorDriveState::off};double raw{},target{};std::string reason;};
bool fdstq_valid(const FdstqConfiguration&,std::string&);
DeviceInfo fdstq_device(int serial,std::uint64_t generation=0);
std::optional<int> fdstq_counts(double ratio) noexcept;
class FdstqInterface {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    HANDLE available_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    Spsc<Sample,4096> edges;
    std::atomic<std::uint64_t> metadata_revision{},overflow{},reports{};
    explicit FdstqInterface(std::wstring library={});~FdstqInterface();
    void configure(FdstqConfiguration,bool provider_enabled);
    std::vector<DeviceInfo> devices();
    bool latest(std::size_t,std::size_t,Sample&) const noexcept;
    void outputs(const std::array<bool,5>&);
    void motion(const FdstqMotion&); // Current-session intent, expires after 250 ms.
    void identify(bool enabled);
    bool identifying();
    bool identification_available();
    unsigned moving() const noexcept;
    unsigned motor_available() const noexcept;
    std::array<FdstqDriveStatus,2> thrust_status();
    void trace(fds::Trace*);
    std::string status();
};
}
