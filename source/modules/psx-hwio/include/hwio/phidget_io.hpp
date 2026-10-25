#pragma once
#include "ethernet.hpp"
#include "motor_feedback.hpp"
#include "phidget_session.hpp"
#include "output_identification_port.hpp"

namespace hwio {
struct PhidgetChannel {
    MotorDeviceAddress address;
    int channel_class{},firmware{};
    std::string name;
    // Manager advertises alternative hub-port modes even without a VINT child.
    // These are not attached sensors and must not be opened by generic scanning.
    bool hub_port_device{};
    bool operator==(const PhidgetChannel&)const=default;
};
struct PhidgetReading {
    PhidgetChannel channel;
    std::optional<double> value;
    std::optional<std::int64_t> count;
    std::string unit,status;
    unsigned interval_ms{},minimum_interval_ms{},maximum_interval_ms{};
    std::uint64_t generation{},sequence{},observed_ms{},checked_ms{};
    bool valid{};
};
// Shared acquisition never resets the physical encoder. Each consumer owns its
// coordinate offset; quiet encoders keep getter-check and event times separate.
class PhidgetEncoderReader {
    struct Impl;std::unique_ptr<Impl> impl_;
    PhidgetEncoderReader(std::shared_ptr<PhidgetSession>,MotorDeviceAddress,std::optional<PhidgetServer>);
public:
    ~PhidgetEncoderReader();
    static std::shared_ptr<PhidgetEncoderReader> acquire(std::shared_ptr<PhidgetSession>,MotorDeviceAddress,std::optional<PhidgetServer>);
    PhidgetReading snapshot()const;
};
class PhidgetIo {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit PhidgetIo(std::wstring library={});~PhidgetIo();
    HANDLE available_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    Spsc<Sample,8192> edges;
    std::atomic<std::uint64_t> metadata_revision{},overflow{},reports{};
    void configure(std::vector<PhidgetChannel>,HardwareConfiguration,bool enabled,std::vector<std::string> excluded={});
    void stop();
    std::vector<DeviceInfo> devices()const;
    std::vector<EthernetHealth> health()const;
    std::vector<PhidgetReading> readings()const;
    bool latest(std::size_t,std::size_t,Sample&)const noexcept;
    bool submit(const EthernetOutput&)noexcept;
    bool identify_outputs(IdentificationRequest,const IdentificationCard&);
    std::optional<IdentificationCompletion> identification_completion(std::uint64_t);
};
int phidget_analogue_counts(double value,bool voltage)noexcept;
}
