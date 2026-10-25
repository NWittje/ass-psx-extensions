#pragma once
#include "motor_bench.hpp"
#include "hid.hpp"
#include "flight_display.hpp"

namespace hwio {
// One acquisition-owned cache; quiet absolute values retain validity. Selection
// and device generation changes are independent of simulator reconnection.
class MotorFeedbackBus {
    static constexpr unsigned capacity=256;
    struct Input {MotorFeedbackDefinition definition;unsigned slot{},control{};std::uint64_t generation{};bool resolved{};SourceValue physical;double code_count{};
        std::array<SourceValue,64> history{};unsigned history_count{},history_next{};std::uint64_t discarded{};};
    mutable std::mutex mutex_;
    std::array<Input,capacity> inputs_;
    std::array<MotorPositionSample,capacity> samples_;
public:
    void configure(const std::array<MotorFeedbackDefinition,motor_slots>&,const std::vector<DeviceInfo>&,const SourceBank&);
    void configure(const std::vector<MotorFeedbackDefinition>&,const std::vector<DeviceInfo>&,const SourceBank&);
    void observe(const Sample&) noexcept;
    void publish(std::uint64_t);
    void invalidate();
    MotorPositionSample sample(unsigned)const;
    MotorPositionSample sample(const MotorFeedbackDefinition&)const;
    unsigned events(const MotorFeedbackDefinition&,std::uint64_t after,MotorPositionSample*,unsigned capacity,bool& lost)const;
};
}
