#pragma once
#include "fdstq.hpp"
#include "axis_setup.hpp"
#include <psx/hwio/thrust_control.hpp>
namespace hwio {
FdstqMotion fdstq_motion(const FdstqConfiguration&,const AxisConfiguration&,const Mapper&,std::uint64_t generation,const FdstqThrust* =nullptr);
std::array<psx::hwio::thrust_control::Lever,4> fdstq_thrust_status(const FdstqConfiguration&,const AxisConfiguration&,const Mapper&,std::uint64_t,const std::array<FdstqDriveStatus,2>&,bool provider_enabled);
double fdstq_velocity(double target,double feedback,double zero,double full,bool reversed,double limit) noexcept;
bool fdstq_speed_write(const AxisConfiguration&,const Mapper&,const Command&) noexcept;
// Suppress our own input echoes; reserve the lever before the worker starts moving.
class FdstqSpeedFollow {
    std::array<int,64> echoes_{};std::size_t begin_{},end_{};
    std::uint64_t generation_{},epoch_{},sequence_{};
    int observed_{};bool known_{},following_{},overflow_{};
public:
    void reset() noexcept {*this={};}
    void written(int value) noexcept;
    bool update(FdstqMotion&,bool available,double feedback,int observed,std::uint64_t sequence,std::uint64_t epoch) noexcept;
};
}
