#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hwio {
struct MotorCorrectionInput {
    std::uint64_t ms{};
    double raw{},encoder{},counts_per_raw{},velocity{},quantum{1},band{};
    bool ready{},attained{},hold_confirmed{};
    std::uint64_t sensor_ms{},sensor_sequence{},after_ms{},after_sequence{};
};
struct MotorCorrectionAction {
    bool move{},hold{},complete{};double target{},learn_raw{},learn_encoder{};int direction{};bool learn{};
};
// One bounded accepted target lead from actual encoder feedback, not another PID.
// Exact encoder attainment is not required to keep correcting physical error.
class MotorCorrection {
    bool active_{},step_{},holding_{},settling_{},stationary_pair_{};
    double goal_{},target_{},step_goal_{},origin_{},budget_{},stationary_encoder_{},mean_raw_{},mean_encoder_{},settled_low_{},settled_high_{};
    std::uint64_t serviced_{},stationary_{};unsigned samples_{};int direction_{},step_direction_{};
public:
    void reset()noexcept {*this={};}
    bool active()const noexcept {return active_;}
    bool pending_step()const noexcept {return step_;}
    double target()const noexcept {return target_;}
    double goal()const noexcept {return goal_;}
    std::uint64_t serviced_ms()const noexcept {return serviced_;}
    void begin(double raw_goal,std::uint64_t now,int direction)noexcept {reset();active_=true;goal_=raw_goal;serviced_=now;direction_=direction;}
    // Retargeting does not cancel an accepted step or replenish its time budget.
    // Keep only settling evidence which also fits the latest physical band.
    void retarget(double raw_goal,double band)noexcept {
        goal_=raw_goal;
        if(settling_ && (settled_low_<goal_-band || settled_high_>goal_+band))settling_=false;
    }
    MotorCorrectionAction step(const MotorCorrectionInput& x)noexcept {
        MotorCorrectionAction a;
        if(!active_)return a;
        if(step_ && (x.encoder-origin_)*step_direction_>=.5*x.quantum)direction_=step_direction_;
        if(std::abs(goal_-x.raw)<=x.band){
            // Holding is a priority cancellation, including while a preceding
            // movement or limit operation is awaiting its receipt.
            if(!holding_){holding_=true;settling_=false;a.hold=true;return a;}
            if(!x.hold_confirmed)return a;
            // Cached pre-move readings may cancel movement, but do not prove
            // that the load arrived after the accepted movement.
            const bool observed=!x.after_ms || (x.sensor_sequence && x.after_sequence?
                x.sensor_sequence>x.after_sequence:x.sensor_ms>=x.after_ms);
            if(!observed){settling_=false;return a;}
            if(!settling_){settling_=true;stationary_pair_=true;stationary_=x.ms;stationary_encoder_=x.encoder;samples_=0;mean_raw_=mean_encoder_=0;settled_low_=settled_high_=x.raw;}
            settled_low_=std::min(settled_low_,x.raw);settled_high_=std::max(settled_high_,x.raw);
            if(std::abs(x.encoder-stationary_encoder_)>=.5*x.quantum)stationary_pair_=false;
            ++samples_;mean_raw_+=(x.raw-mean_raw_)/samples_;mean_encoder_+=(x.encoder-mean_encoder_)/samples_;
            if(x.ms-stationary_>=200){a.complete=true;a.learn_raw=mean_raw_;a.learn_encoder=mean_encoder_;a.direction=direction_;a.learn=stationary_pair_;active_=false;}
            return a;
        }
        // Stop replaced the old step with a measured-position holding target.
        // If the sensor leaves the arrival band before settling, start a fresh
        // bounded step rather than waiting for the cancelled pre-Stop target.
        if(holding_){step_=false;budget_=0;serviced_=x.ms;}
        holding_=settling_=false;
        if(!x.ready)return a;
        if(x.ms<serviced_ || x.ms-serviced_<100)return a;
        const auto elapsed=std::min<std::uint64_t>(100,x.ms-serviced_);serviced_=x.ms;
        if(!(x.quantum>0) || !(x.velocity>0) || !std::isfinite(x.counts_per_raw))return a;
        // Missed timer intervals are not queued for later movement. Retain only
        // the sub-encoder-count budget needed for a representable slow step.
        budget_=std::min(budget_+x.velocity*double(elapsed)/1000.,x.quantum+x.velocity*.1);
        const double remaining=(goal_-x.raw)*x.counts_per_raw;
        const double distance=std::floor(std::min({std::abs(remaining),budget_,std::max(x.quantum,x.velocity*.1)})/x.quantum+1e-12)*x.quantum;
        if(!(distance>0))return a;
        a.move=true;a.direction=remaining>0?1:-1;a.target=x.encoder+a.direction*distance;
        if(step_ && a.direction==step_direction_ && (goal_-step_goal_)*a.direction*x.counts_per_raw>=0){
            // Keep an unfinished accepted lead through reverse encoder jitter.
            // Only new net shaft progress permits its next forward increment;
            // an unchanged/stationary shaft cannot accumulate command distance.
            const double progress=std::floor(std::max(0.,(x.encoder-origin_)*a.direction)/x.quantum+1e-12)*x.quantum;
            const double advance=std::clamp((a.target-target_)*a.direction,0.,progress);
            a.target=target_+a.direction*advance;
            // A shrinking physical error may legitimately shorten the lead.
            if((a.target-x.encoder)*a.direction>std::abs(remaining))a.target=x.encoder+a.direction*distance;
            if(progress>0)origin_+=a.direction*progress;
        }else origin_=x.encoder;
        // ready means the preceding SDK receipt was consumed. Replacement of
        // its unfinished target remains bounded from this measured encoder.
        target_=a.target;step_goal_=goal_;step_direction_=a.direction;step_=true;budget_-=distance;
        return a;
    }
};
}
