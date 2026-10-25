#pragma once
#include "motor_control.hpp"
#include <optional>
namespace hwio {
// Existing IPC phase values are unchanged; Correcting uses recovering on the wire.
enum class MotorFollowingPhase { following,lag,recovering,limit,waiting,stopped,fault,correcting };
enum class MotorResponseKind { moving,arrived,missed_arrival,nonresponse };
struct MotorResponseResult {
    MotorResponseKind kind{MotorResponseKind::moving};std::string reason;
    bool empty()const noexcept {return reason.empty();}
};
// Schedules the initial encoder estimate's handoff. It never diagnoses override
// or disconnect: actual load position is measured only by the potentiometer.
class MotorResponseMonitor {
    bool initialized_{},prepared_{},expected_known_{};
    std::uint64_t window_{},last_{},expected_stamp_{};
    double prepared_count_{},prepared_velocity_{},origin_{},expected_origin_{},position_{},velocity_{},expected_position_{};
public:
    void reset() noexcept {*this={};}
    std::uint64_t response_age(std::uint64_t now)const noexcept{return initialized_ && now>=window_?now-window_:0;}
    void prepare(double,double count,double velocity=0) noexcept {prepared_=true;prepared_count_=count;prepared_velocity_=velocity;}
    MotorResponseResult observe(std::uint64_t now,const MotorSensorContract& c,double raw,double count,double accepted,double physical_target,
                         double counts_to_raw,double vmax,double acceleration,std::optional<double> expected={},std::uint64_t expected_stamp=0,bool encoder_attained=false){
        if(!initialized_){initialized_=true;window_=last_=now;origin_=raw;position_=expected_origin_=prepared_?prepared_count_:count;velocity_=prepared_?prepared_velocity_:0;prepared_=false;}
        const double dt=now>=last_?double(now-last_)/1000.:0;last_=now;
        if(expected){
            const auto stamp=expected_stamp?expected_stamp:now;
            if(!expected_known_ || stamp>expected_stamp_){
                if(expected_known_)velocity_=(*expected-expected_position_)*1000/double(stamp-expected_stamp_);
                expected_known_=true;expected_stamp_=stamp;expected_position_=*expected;
            }
            position_=*expected;if(position_==accepted)velocity_=0;
        }else motor_estimate_step(position_,velocity_,accepted,vmax,acceleration,dt);
        if(std::abs(raw-physical_target)<=c.jitter+c.buffer())return {MotorResponseKind::arrived,{}};
        // Encoder/profile completion only schedules correction of a missed load
        // arrival. Neither the encoder residual nor this hint is a motion fault.
        if(encoder_attained || std::abs(accepted-position_)*std::abs(counts_to_raw)<1 ||
           std::abs(accepted-count)*std::abs(counts_to_raw)<1)return {MotorResponseKind::missed_arrival,{}};
        if(now<window_ || now-window_<2000)return {};
        const double expected_travel=std::abs((position_-expected_origin_)*counts_to_raw);
        const double required=std::min(c.buffer(),std::max(0.,expected_travel-c.backlash));
        const double progress=(raw-origin_)*(physical_target>=origin_?1.:-1.);
        if(required>=1 && !motor_sensor_responded(progress,required,c.jitter))
            return {MotorResponseKind::nonresponse,"No measurable physical progress; continuing slow correction"};
        window_=now;origin_=raw;expected_origin_=position_;return {};
    }
};
struct MotorTrackingObservation {
    bool active{},disconnect{};
    std::uint64_t started{},elapsed{};
    double range{},error{},origin{},progress{};int direction{};
};
// Sole automatic motor-generated A/T-disconnect predicate. All distances use
// calibrated forward potentiometer units, never encoder or expected positions.
class MotorDisconnectWindow {
    bool active_{};std::uint64_t started_{};double origin_{},last_target_{};int direction_{};
public:
    void reset() noexcept {*this={};}
    MotorTrackingObservation observe(std::uint64_t now,double raw,double target,double range) noexcept {
        MotorTrackingObservation out;out.range=range;out.error=std::abs(target-raw);
        if(!std::isfinite(raw) || !std::isfinite(target) || !std::isfinite(range) || !(range>0) || out.error<=.10*range){reset();return out;}
        const int direction=target>raw?1:-1;
        if(!active_ || now<started_ || (target!=last_target_ && direction!=direction_)){active_=true;started_=now;origin_=raw;direction_=direction;}
        last_target_=target;
        out.active=true;out.started=started_;out.elapsed=now-started_;out.origin=origin_;out.direction=direction_;out.progress=(raw-origin_)*direction_;
        if(out.elapsed>=2000){
            out.disconnect=out.progress<.05*range;
            if(!out.disconnect){started_=now;origin_=raw;direction_=direction;}
        }
        return out;
    }
};
}
