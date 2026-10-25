#pragma once
#include "motor_types.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace hwio {
enum class BacklashPhase { idle, feedback, forward, settling, reverse, releasing, complete, failed };
struct BacklashStatus {
    BacklashPhase phase{BacklashPhase::idle};
    bool active{},success{};
    double raw_counts{},shaft_counts{},raw_per_count{},jitter{};
    std::string message;
};
// Experiment geometry only. The owner executes/acknowledges actions and keeps
// its normal feedback, current, lease and explicit cancellation contracts.
// +1/-1: shaft direction; 0: stop; 2: remove drive.
class MotorBacklashFinder {
    BacklashStatus state_;
    std::uint64_t started_{},stationary_{},pot_generation_{},shaft_generation_{},sequence_{};
    double low_{},high_{},last_shaft_{},last_pot_{},first_pot_{},first_shaft_{},base_pot_{},base_shaft_{};
    bool flank_{};int direction_{},forward_shaft_sign_{},forward_pot_sign_{};unsigned action_revision_{};
    void phase(BacklashPhase p,int d,const char* text){state_.phase=p;direction_=d;state_.message=text;++action_revision_;stationary_=0;}
public:
    const BacklashStatus& status()const noexcept{return state_;}
    int direction()const noexcept{return direction_;}
    unsigned action_revision()const noexcept{return action_revision_;}
    void start(std::uint64_t now){*this={};state_.active=true;started_=now;phase(BacklashPhase::feedback,2,"Waiting for potentiometer and shaft feedback");}
    void cancel(const std::string& cause){state_.active=false;state_.success=false;phase(BacklashPhase::failed,2,cause.c_str());}
    void tick(const MotorPositionSample& pot,const MotorPositionSample& shaft,std::uint64_t now,bool action_accepted,bool released){
        if(!state_.active)return;
        const auto valid=[&](const MotorPositionSample& s){return s.valid && std::isfinite(s.position) && s.checked_ms<=now && now-s.checked_ms<=500;};
        if(!valid(pot) || !valid(shaft)){
            if(pot_generation_ || now-started_>=2000)cancel(!valid(pot)?"Backlash measurement lost potentiometer feedback":"Backlash measurement requires valid motor-shaft counts");
            return;
        }
        if(!pot_generation_){pot_generation_=pot.generation;shaft_generation_=shaft.generation;low_=high_=last_pot_=pot.position;last_shaft_=shaft.position;stationary_=now;}
        if(pot_generation_!=pot.generation || shaft_generation_!=shaft.generation){cancel("Backlash measurement feedback identity changed");return;}
        const bool fresh=pot.sequence!=sequence_;sequence_=pot.sequence;
        const double q=std::max(1e-9,pot.quantum),j=state_.jitter;
        if(state_.phase==BacklashPhase::feedback){
            low_=std::min(low_,pot.position);high_=std::max(high_,pot.position);
            if(!released || !action_accepted){stationary_=now;return;}
            if(now-stationary_>=300){state_.jitter=high_-low_;base_pot_=pot.position;phase(BacklashPhase::forward,1,"Driving forward to establish potentiometer-to-shaft scale");}
            return;
        }
        if(state_.phase==BacklashPhase::forward){
            if(!fresh || !action_accepted)return;
            if(!flank_ && std::abs(pot.position-base_pot_)>std::max(3*q,j+q)){first_pot_=pot.position;first_shaft_=shaft.position;flank_=true;}
            if(flank_ && std::abs(pot.position-first_pot_)>=std::max(8*q,2*j+3*q)){
                const double span=shaft.position-first_shaft_;
                if(std::abs(span)<q*1e-6){cancel("Potentiometer moved without measurable motor-shaft travel");return;}
                state_.raw_per_count=(pot.position-first_pot_)/span;forward_shaft_sign_=span>0?1:-1;forward_pot_sign_=pot.position>first_pot_?1:-1;last_pot_=pot.position;last_shaft_=shaft.position;
                phase(BacklashPhase::settling,0,"Stopping and stabilising before reversal");
            }
            return;
        }
        if(state_.phase==BacklashPhase::settling){
            if(!action_accepted)return;
            if(std::abs(shaft.position-last_shaft_)>.5 || std::abs(pot.position-last_pot_)>j+q){stationary_=now;last_pot_=pot.position;last_shaft_=shaft.position;}
            if(!stationary_)stationary_=now;
            if(now-stationary_>=200){
                if(!state_.success){base_pot_=pot.position;base_shaft_=shaft.position;phase(BacklashPhase::reverse,-1,"Reversing until the potentiometer responds");}
                else phase(BacklashPhase::releasing,2,"Removing motor drive after backlash measurement");
            }
            return;
        }
        if(state_.phase==BacklashPhase::reverse){
            if(!fresh || !action_accepted || std::abs(pot.position-base_pot_)<=j+q)return;
            const double shaft_delta=shaft.position-base_shaft_,pot_delta=pot.position-base_pot_;
            // Infer both signs from the positive drive, including inverted encoder
            // or potentiometer coordinates. Residual old-direction motion and
            // independently delayed samples are not a failed reversal: keep
            // driving negative until both measured responses have reversed.
            if(shaft_delta*forward_shaft_sign_>=0 || pot_delta*forward_pot_sign_>=-(j+q))return;
            state_.shaft_counts=std::max(0.,std::abs(shaft_delta)-std::abs(pot_delta/state_.raw_per_count));
            state_.raw_counts=state_.shaft_counts*std::abs(state_.raw_per_count);state_.success=true;
            last_pot_=pot.position;last_shaft_=shaft.position;phase(BacklashPhase::settling,0,"Stopping after potentiometer reversal response");return;
        }
        if(state_.phase==BacklashPhase::releasing && action_accepted && released){state_.active=false;phase(BacklashPhase::complete,2,"Backlash measured; result applied to unsaved settings");}
    }
};
}
