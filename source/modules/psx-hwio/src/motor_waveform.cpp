#include "hwio/motor_types.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace hwio {
double motor_wave_period(MotorWaveform type,double amplitude,double velocity,double acceleration,double interval)noexcept {
    if(!std::isfinite(amplitude) || !std::isfinite(velocity) || !std::isfinite(acceleration) || !std::isfinite(interval) || amplitude<=0 || velocity<=0 || acceleration<=0 || interval<=0)return 0;
    double period{};
    if(type==MotorWaveform::sine)period=1.1*6.2831853071795864769*std::max(amplitude/velocity,std::sqrt(amplitude/acceleration));
    else if(type==MotorWaveform::square){const double distance=2*amplitude;const double transition=distance<=velocity*velocity/acceleration?2*std::sqrt(distance/acceleration):distance/velocity+velocity/acceleration;period=2*(transition+std::max(.25,.1*transition));}
    else return 0;
    period=std::max(40*interval,period);return std::isfinite(period) && period<=double(std::numeric_limits<std::uint64_t>::max())/2000.?period:0;
}
}
