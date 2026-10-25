#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hwio {
struct MotorHomingSample {
    std::uint64_t stamp_ms{},motor_generation{},sensor_generation{},calibration_revision{};
    double speed{},acceleration{},position{},target{},sensor{},resolution{},endpoint_band{};
    std::uint64_t motor_age_ms{},sensor_age_ms{};
    double raw{},raw_low{},raw_high{};bool raw_valid{},motor_valid{},sensor_valid{},engaged{};
    int polarity{},command_status{};double codes{},buffer{},jitter{},probe_excursion{};
    double scale{},measured_velocity{},remaining{},braking_margin{},requested_velocity{};
    double progress_sensor{},progress_encoder{},guard_target{},response_limit{20};std::uint64_t progress_age{},configuration_revision{},coordinate_revision{};int guard_decision{},drive_state{},release_stage{-1};
};
// One last-run CSV; bounded publication only on the acquisition worker.
// Directory creation, formatting, flushing and disk access belong to its writer.
class MotorHomingLog {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit MotorHomingLog(std::wstring directory);
    ~MotorHomingLog();
    MotorHomingLog(const MotorHomingLog&)=delete;
    MotorHomingLog& operator=(const MotorHomingLog&)=delete;
    void begin(unsigned motor,unsigned axis,std::string_view device,const MotorHomingSample&) noexcept;
    void update(unsigned motor,const MotorHomingSample&,std::string_view phase,std::string_view description,bool terminal) noexcept;
    void receipt(unsigned motor,const MotorHomingSample&,std::string_view description) noexcept;
    void finish(unsigned motor,std::string_view description,bool fault) noexcept;
    std::string error()const;
};
}
