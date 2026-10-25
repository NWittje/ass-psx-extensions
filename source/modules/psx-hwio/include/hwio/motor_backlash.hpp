#pragma once
#include "motor_control.hpp"
#include "motor_alignment.hpp"

namespace hwio {
// Rate-independent play: shaft travel first crosses the known gap before the
// load moves. All three coordinates have the same units; no time integration,
// allocation or motor command is performed by this geometry.
class MotorPlay {
    double input_{},output_{},width_{};
public:
    void reset(double input,double output,double width)noexcept {
        width_=std::max(0.,width);input_=input;
        output_=std::clamp(output,input-width_*.5,input+width_*.5);
    }
    double step(double input)noexcept {input_=input;output_=std::clamp(output_,input-width_*.5,input+width_*.5);return output_;}
    double output()const noexcept{return output_;}
    double remaining()const noexcept{return std::max(0.,width_*.5-std::abs(input_-output_));}
};

// Home captures opposite load endpoints on opposite contact flanks. Removing
// the known gap from that signed span preserves both captured endpoints and
// avoids saving any absolute encoder origin in the backlash setting.
inline double motor_backlash_offset(const MotorSessionMap& map,double raw,int encoder_direction,double raw_width)noexcept {
    return map.backlash_offset(raw,encoder_direction,raw_width);
}
inline double motor_alignment_backlash(const MotorAlignment& alignment,const MotorSessionMap& map,double raw,int direction,double width)noexcept {
    const auto baseline=[&](double value){return motor_backlash_offset(map,value,direction,width);};
    if(!direction)return 0;
    const auto& table=alignment.directions[direction>0?1:0];
    if(alignment.identity.empty() || !table.count)return baseline(raw);
    // Full measured errors already include backlash. Beyond coverage retain the
    // nearest learned error; adding gap extrapolation would count it twice.
    return table.lookup(raw);
}
}
