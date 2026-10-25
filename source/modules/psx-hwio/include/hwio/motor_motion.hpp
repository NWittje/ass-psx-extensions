#pragma once
#include "motor_types.hpp"
#include "motor_control.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hwio {
// Physical fractions are unclamped: overshoot is a direction constraint, not a
// lost baseline. SDK numeric bounds remain hard constraints at the executor.
struct MotorGuardSnapshot {
    bool required{},valid{},mapped{},manual_page{};
    MotorOwner owner{MotorOwner::bench};double jitter{};
    double fraction{},resolution{};
    int polarity{},edge{2}; // 2: no authoritative hysteresis state supplied.
    std::uint64_t motor_generation{},sensor_generation{},calibration_revision{},coordinate_revision{},stamp_ms{},identity{};
};
enum class MotorMotionDecision { allow,limit,unavailable };
struct MotorGuardResult {MotorMotionDecision decision{MotorMotionDecision::allow};double target{};};

inline bool motor_inward(double measured,double target,int edge,double noise=0) noexcept {return edge && (measured-target)*edge>noise;}
inline MotorGuardResult motor_guard_target(const MotorGuardSnapshot& g,double position,double target,bool holding=false) noexcept {
    (void)holding; // A Stop target at measured position is allowed by direction, not by bypassing future holding checks.
    if(g.manual_page || !g.required)return {MotorMotionDecision::allow,target};
    if(!g.valid || !std::isfinite(g.fraction) || !std::isfinite(position) || !std::isfinite(target))return {MotorMotionDecision::unavailable,position};
    if(g.owner==MotorOwner::homing || !g.mapped)return {MotorMotionDecision::allow,target};
    if(!g.polarity)return {MotorMotionDecision::unavailable,position};
    const int edge=g.edge>=-1 && g.edge<=1?g.edge:g.fraction<=g.jitter?-1:g.fraction>=1-g.jitter?1:0;
    // The sensor determines whether an edge is occupied; encoder delta and the
    // learned polarity determine which way this command would drive the motor.
    if(target!=position && motor_outward_direction(edge,(target-position)*g.polarity))return {MotorMotionDecision::limit,position};
    return {MotorMotionDecision::allow,target};
}
inline double motor_operating_target(double position,double target,double low,double high) noexcept {
    if(position<low)return target>position?std::min(target,high):position;
    if(position>high)return target<position?std::max(target,low):position;
    return std::clamp(target,low,high);
}
}
