#pragma once
#include "motor_types.hpp"
#include "motor_backlash.hpp"
#include <algorithm>
#include <cmath>

namespace hwio {
// This loop's coordinate is measured sensor counts. It does not estimate
// position by integrating motor duty or motor velocity.
class MotorPositionLoop {
    double integral_{},measured_{},filtered_velocity_{};
    double shaft_position_{},width_{-1},goal_{};int flank_{};bool goal_known_{};
    MotorPlay reference_;
    bool baseline_{};
public:
    double position{},velocity{},acceleration{};
    void reset(double measured) noexcept {integral_=filtered_velocity_=velocity=acceleration=0;measured_=position=shaft_position_=measured;width_=-1;flank_=0;goal_known_=false;baseline_=true;}
    double shaft_position()const noexcept{return shaft_position_;}
    bool taking_up()const noexcept{return width_>0 && reference_.remaining()>1e-9 && (velocity!=0 || (goal_known_ && goal_!=position));}
    double step(double target,double measured,double dt,const MotorParameters& p,double output_limit=1) noexcept {
        if(!baseline_)reset(measured);
        if(!(dt>0) || !std::isfinite(dt))return 0;
        if(width_!=p.backlash){width_=std::max(0.,p.backlash);reference_.reset(shaft_position_,position,width_);goal_known_=false;}
        if(!goal_known_ || target!=goal_){const double travel=target-position;if(travel!=0)flank_=travel>0?1:-1;goal_=target;goal_known_=true;}
        const double shaft_target=target+flank_*width_*.5;
        const double old_velocity=velocity,delta=shaft_target-shaft_position_;
        const double wanted=std::copysign(std::min(p.velocity_limit,std::sqrt(2*p.acceleration*std::abs(delta))),delta);
        velocity+=std::clamp(wanted-velocity,-p.acceleration*dt,p.acceleration*dt);
        const double advance=(old_velocity+velocity)*.5*dt;
        // During a reversal, retain real braking rather than snapping the
        // reference to a target on the other side of its current velocity.
        if(delta*advance>=0 && std::abs(advance)>=std::abs(delta)){shaft_position_=shaft_target;velocity=0;}
        else shaft_position_+=advance;
        position=reference_.step(shaft_position_);
        acceleration=(velocity-old_velocity)/dt;
        const double raw_velocity=(measured-measured_)/dt;measured_=measured;
        filtered_velocity_+=dt/(.1+dt)*(raw_velocity-filtered_velocity_);
        const double error=position-measured;
        const double base=p.kp*error-p.kd*filtered_velocity_+p.vff*velocity+p.aff*acceleration;
        const double next=std::clamp(integral_+p.ki*error*dt,-output_limit,output_limit);
        const double candidate=base+next;
        if(std::abs(candidate)<=output_limit || (candidate>output_limit && next<integral_) || (candidate<-output_limit && next>integral_))integral_=next;
        return std::clamp(base+integral_,-output_limit,output_limit);
    }
    double integral()const noexcept{return integral_;}
};
}
