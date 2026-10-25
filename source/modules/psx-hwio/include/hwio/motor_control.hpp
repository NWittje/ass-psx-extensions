#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace hwio {
enum class MotorOwner { stopped,bench,homing,following };
enum class HomePhase { idle,jitter,probe,first,correct,capture,second,park,complete,fault };
struct MotorSensorContract {
    double low{},high{},codes{4096},jitter{},configured_buffer{},backlash{};
    double span()const noexcept{return std::abs(high-low);}
    double sign()const noexcept{return high>=low?1.:-1.;}
    double units(double raw)const noexcept{return (raw-low)*sign();}
    double buffer()const noexcept{return configured_buffer>0?configured_buffer:5+5*std::clamp((codes-512)/3584.,0.,1.);}
};
struct MotorSessionMap {
    bool valid{};double first_count{},first_raw{},second_count{},second_raw{};
    MotorSensorContract sensor;
    double target(double raw)const noexcept{return first_count+(raw-first_raw)*(second_count-first_count)/(second_raw-first_raw);}
    double backlash_offset(double raw,int direction,double width)const noexcept {
        if(!valid || !direction || !(width>0) || !std::isfinite(width))return 0;
        const double span=sensor.high-sensor.low,counts=target(sensor.high)-target(sensor.low);if(!span || !counts)return 0;
        const double gap=counts*width/(std::abs(span)+width),f=std::clamp((raw-sensor.low)/span,0.,1.);
        return gap*(direction*counts>0?1-f:-f);
    }
    double raw(double count)const noexcept{return first_raw+(count-first_count)*(second_raw-first_raw)/(second_count-first_count);}
    int polarity()const noexcept{return (second_count-first_count)*(second_raw-first_raw)*sensor.sign()>0?1:-1;}
};
// Monitoring only. These positions never replace the controller's PID target.
inline void motor_estimate_step(double& position,double& velocity,double target,double vmax,double acceleration,double dt) noexcept {
    if(dt<=0 || vmax<=0 || acceleration<=0)return;
    auto integrate=[&](double a,double time){position+=velocity*time+.5*a*time*time;velocity+=a*time;dt-=time;};
    // At most two preliminary braking segments precede the ordinary
    // accelerate/cruise/decelerate profile. Carry real starting velocity,
    // including unavoidable overshoot; a target at current position is not
    // evidence that a moving motor has already stopped.
    for(unsigned phase=0;phase<4 && dt>0;++phase){
        const double delta=target-position,distance=std::abs(delta),direction=delta<0?-1.:1.;
        const double v=velocity*direction;
        if(v<0){const double t=-v/acceleration;if(dt<t){integrate(direction*acceleration,dt);return;}integrate(direction*acceleration,t);velocity=0;continue;}
        if(v*v>2*acceleration*distance){const double t=v/acceleration;if(dt<t){integrate(-direction*acceleration,dt);return;}integrate(-direction*acceleration,t);velocity=0;continue;}
        if(v>vmax){const double t=(v-vmax)/acceleration;if(dt<t){integrate(-direction*acceleration,dt);return;}integrate(-direction*acceleration,t);velocity=direction*vmax;continue;}
        const double peak=std::min(vmax,std::sqrt(acceleration*distance+.5*v*v));
        if(peak==0){position=target;velocity=0;return;}
        const double up=(peak-v)/acceleration;
        const double cruise=std::max(0.,(distance-(v+peak)*.5*up-peak*peak/(2*acceleration))/peak);
        if(dt<up){integrate(direction*acceleration,dt);return;}integrate(direction*acceleration,up);velocity=direction*peak;
        if(dt<cruise){integrate(0,dt);return;}integrate(0,cruise);
        const double down=peak/acceleration;
        if(dt<down){integrate(-direction*acceleration,dt);return;}
        position=target;velocity=0;return;
    }
}
inline bool motor_sensor_responded(double progress,double required,double jitter) noexcept {
    return required<1 || (progress>jitter && progress+jitter+1e-9>=required);
}
struct HomeAcceptedMotion {
    double target{},velocity{},acceleration{},measured_velocity{};
    std::uint64_t accepted_ms{};
    std::optional<double> expected;
    std::uint64_t expected_ms{};
};
struct HomeObservation {
    std::uint64_t ms{};double raw{},count{},velocity{},acceleration{};bool drive_off{},accepted{};
    // Stop receipt and the holding snapshot must agree before stationary time
    // can qualify an endpoint. SDK queue acceptance alone is insufficient.
    bool stop_confirmed{true};
    std::optional<HomeAcceptedMotion> motion;
};
struct HomeAction {
    HomePhase phase{};int direction{};double speed_fraction{},target{};bool move{},hold{},release{},complete{};
    std::string error;
};
// All geometry uses raw ADC codes. Native encoder zero is untouched; first_count
// is the runtime software origin. The SDK remains responsible for encoder PID.
class MotorHome {
    HomePhase phase_{HomePhase::idle};MotorSensorContract sensor_;MotorSessionMap map_;
    std::uint64_t since_{},stationary_{},progress_ms_{};double noise_low_{},noise_high_{},origin_raw_{},origin_count_{},last_raw_{},last_count_{},progress_raw_{},progress_count_{};
    double leg_target_{},response_scale_{};int target_direction_{};bool leg_target_valid_{};
    double takeup_{};std::uint64_t takeup_until_{};bool load_started_{},scale_anchor_{};double scale_raw_{},scale_count_{};
    HomeAcceptedMotion motion_;bool motion_known_{};std::uint64_t motion_ms_{};
    double expected_position_{},expected_velocity_{},progress_expected_{};
    int direction_{1},polarity_{},destination_{1};bool reversed_{},started_{},slow_{},capture_started_{},second_leg_{};
    HomeAction failure(std::string reason){phase_=HomePhase::fault;return {phase_,0,0,0,false,false,true,false,std::move(reason)};}
    void progress(const HomeObservation& x){progress_ms_=x.ms;progress_raw_=x.raw;progress_count_=x.count;progress_expected_=expected_position_;}
    void observe_motion(const HomeObservation& x){
        if(!x.motion)return;
        const auto& current=*x.motion;
        if(x.drive_off){expected_position_=x.count;expected_velocity_=current.measured_velocity;motion_known_=true;motion_=current;motion_ms_=x.ms;return;}
        if(!motion_known_){expected_position_=x.count;expected_velocity_=current.measured_velocity;motion_known_=true;}
        else if(x.ms>motion_ms_){
            // A newly queued request is not accepted motion. Split a target
            // acknowledgement at its SDK timestamp; limit readback takes effect
            // when observed, never retrospectively at the requested fraction.
            const auto accepted=std::clamp(current.accepted_ms,motion_ms_,x.ms);
            if(current.target!=motion_.target && current.accepted_ms){
                motor_estimate_step(expected_position_,expected_velocity_,motion_.target,motion_.velocity,motion_.acceleration,double(accepted-motion_ms_)/1000.);
                motor_estimate_step(expected_position_,expected_velocity_,current.target,current.velocity,current.acceleration,double(x.ms-accepted)/1000.);
            }else motor_estimate_step(expected_position_,expected_velocity_,motion_.target,motion_.velocity,motion_.acceleration,double(x.ms-motion_ms_)/1000.);
        }
        if(current.expected && current.expected_ms>motion_.expected_ms){
            if(motion_.expected && current.expected_ms>motion_.expected_ms)
                expected_velocity_=(*current.expected-*motion_.expected)*1000/double(current.expected_ms-motion_.expected_ms);
            expected_position_=*current.expected;
        }
        motion_=current;motion_ms_=x.ms;
    }
    void transition(HomePhase p,const HomeObservation& x){phase_=p;since_=x.ms;stationary_=x.ms;last_raw_=x.raw;last_count_=x.count;origin_raw_=x.raw;origin_count_=x.count;progress(x);started_=slow_=capture_started_=false;leg_target_valid_=false;takeup_=sensor_.backlash;takeup_until_=0;load_started_=false;scale_anchor_=false;}
    bool settled(const HomeObservation& x){if(std::abs(x.count-last_count_)>=.5 || std::abs(x.raw-last_raw_)>sensor_.jitter){stationary_=x.ms;last_raw_=x.raw;last_count_=x.count;}return x.ms-stationary_>=200;}
    double advancing_target(const HomeObservation& x,double fraction){
        const double v=x.velocity*fraction;
        const double lead=std::max(1.,v*.25+v*v/(2*std::max(x.acceleration,1e-9)));
        const double candidate=x.count+direction_*lead;
        // A unidirectional Home leg must not request a reversal just because
        // measured encoder feedback retreats. Endpoint/phase changes still stop
        // or reverse explicitly; the two-second physical-response check remains.
        if(!leg_target_valid_ || target_direction_!=direction_){leg_target_=candidate;target_direction_=direction_;leg_target_valid_=true;}
        else if((candidate-leg_target_)*direction_>0)leg_target_=candidate;
        return leg_target_;
    }
    HomeAction movement(const HomeObservation& x,double fraction,double remaining_raw=std::numeric_limits<double>::infinity()){
        HomeAction a;a.phase=phase_;a.move=true;a.direction=direction_;a.speed_fraction=fraction;
        // Advancing targets are transport scheduling, not a travel budget. There
        // is no calibrated clipping during Home, including overshoot correction.
        a.target=advancing_target(x,fraction);
        const double dc=x.count-origin_count_,dr=std::abs(x.raw-origin_raw_);
        // A first Home has no physical conversion until observed travel supplies
        // one. Do not invent a buffer-sized expected response in that interval.
        // Retain the measured scale across Stop/reversal until both pairs exist.
        if(map_.valid)response_scale_=std::abs((map_.second_raw-map_.first_raw)/(map_.second_count-map_.first_count));
        else if(response_scale_==0 && dc!=0 && dr>sensor_.jitter){
            if(sensor_.backlash<=0){response_scale_=dr/std::abs(dc);progress(x);}
            else if(!scale_anchor_ && dr>sensor_.jitter+sensor_.buffer()){scale_anchor_=true;scale_raw_=x.raw;scale_count_=x.count;}
            else if(scale_anchor_ && std::abs(x.raw-scale_raw_)>sensor_.jitter+sensor_.buffer() && x.count!=scale_count_){response_scale_=std::abs((x.raw-scale_raw_)/(x.count-scale_count_));takeup_=0;progress(x);}
        }
        if(!started_){if(x.accepted){started_=true;progress(x);}return a;}
        if(takeup_>0 && !load_started_ && response_scale_>0){
            if(!takeup_until_){const double v=std::max(1e-9,x.velocity*fraction),acc=std::max(1e-9,x.acceleration),distance=takeup_/response_scale_;
                const double seconds=distance<v*v/(2*acc)?std::sqrt(2*distance/acc):distance/v+v/(2*acc);
                takeup_until_=x.ms+std::uint64_t(std::min(1e12,std::ceil(1000*seconds)));
            }
            // A sensor-coordinate backend can finish a short planned step
            // inside the known gap. Its allowance must still end; do not wait
            // forever for that finite reference to exceed the full gap.
            if(x.ms>=takeup_until_){takeup_=0;load_started_=true;progress_ms_=x.ms;}
        }
        const double directed=polarity_?(x.raw-progress_raw_)*sensor_.sign()*direction_*polarity_:std::abs(x.raw-progress_raw_);
        // A short accepted move can finish its estimated profile before its
        // acknowledgement is observed. Progress reset at that observation must
        // not erase the still-required physical response. At predicted rest,
        // retain the accepted displacement from the measured response baseline;
        // while moving, preserve the accepted trajectory including braking.
        const double travel=motion_known_ && expected_position_==motion_.target && expected_velocity_==0?
            motion_.target-progress_count_:expected_position_-progress_expected_;
        const double shaft_expected=response_scale_*std::abs(travel);
        if(takeup_>0 && !load_started_ && shaft_expected>takeup_){load_started_=true;progress_ms_=x.ms;}
        const double expected=std::max(0.,shaft_expected-takeup_);
        const double required=std::min({sensor_.buffer(),expected,remaining_raw});
        // A reversal can still be braking in the old direction. Judge physical
        // response against accepted expected travel, not the new desired sign.
        const double response=motion_known_ && travel!=0 && polarity_?(x.raw-progress_raw_)*sensor_.sign()*std::copysign(1.,travel)*polarity_:directed;
        const bool responded=motor_sensor_responded(response,required,sensor_.jitter);
        if(responded && required>=1){takeup_=0;progress(x);}
        if(x.ms-progress_ms_>=2000 && !responded)return failure("Homing response absent/wrong direction: raw="+std::to_string(x.raw)+", origin="+std::to_string(progress_raw_)+", progress="+std::to_string(response)+", required="+std::to_string(required)+", jitter="+std::to_string(sensor_.jitter)+", count="+std::to_string(x.count));
        return a;
    }
public:
    void begin(MotorSensorContract sensor,const HomeObservation& x){*this={};sensor_=sensor;noise_low_=noise_high_=x.raw;origin_raw_=x.raw;origin_count_=x.count;transition(HomePhase::jitter,x);}
    HomePhase phase()const noexcept{return phase_;}
    const MotorSessionMap& map()const noexcept{return map_;}
    const MotorSensorContract& sensor()const noexcept{return sensor_;}
    int polarity()const noexcept{return polarity_;}
    double probe_excursion(double raw)const noexcept{return (raw-origin_raw_)*sensor_.sign();}
    HomeAction step(const HomeObservation& x,double park_fraction,bool throttle_park=true){
        observe_motion(x);
        HomeAction a;a.phase=phase_;
        if(phase_==HomePhase::jitter){
            a.release=true;if(!x.drive_off){since_=x.ms;noise_low_=noise_high_=x.raw;return a;}
            if(std::abs(x.count-last_count_)>=.5){since_=x.ms;noise_low_=noise_high_=x.raw;last_count_=x.count;return a;}
            noise_low_=std::min(noise_low_,x.raw);noise_high_=std::max(noise_high_,x.raw);
            if(x.ms-since_<2000)return a;
            sensor_.jitter=noise_high_-noise_low_;origin_raw_=x.raw;origin_count_=x.count;
            const double u=sensor_.units(x.raw),band=sensor_.jitter+sensor_.buffer();
            if(u<=.05*sensor_.span()+band || u>=.95*sensor_.span()-band){destination_=u>sensor_.span()*.5?1:0;transition(HomePhase::probe,x);}else transition(HomePhase::first,x);
            return {phase_,0,0,0,false,true};
        }
        if(phase_==HomePhase::probe){
            const double excursion=(x.raw-origin_raw_)*sensor_.sign(),inward=destination_?-excursion:excursion;
            if(inward>sensor_.jitter+sensor_.buffer() && (x.count-origin_count_)*direction_>0){
                response_scale_=std::abs(excursion/(x.count-origin_count_));
                polarity_=excursion*(x.count-origin_count_)>0?1:-1;destination_=destination_?0:1;origin_raw_=x.raw;origin_count_=x.count;transition(HomePhase::first,x);return {phase_,0,0,0,false,true};
            }
            if(!started_ && x.accepted){started_=true;since_=x.ms;progress_raw_=x.raw;}
            if(started_ && (inward<-(sensor_.jitter+sensor_.buffer()/2) || (x.ms-since_>=2000 && std::abs(x.raw-progress_raw_)<=sensor_.jitter))){
                if(reversed_)return failure("Homing probe failed in both directions: raw="+std::to_string(x.raw)+", origin="+std::to_string(origin_raw_)+", jitter="+std::to_string(sensor_.jitter)+", buffer="+std::to_string(sensor_.buffer()));
                reversed_=true;direction_=-1;origin_raw_=x.raw;origin_count_=x.count;started_=false;since_=x.ms;return {phase_,0,0,0,false,true};
            }
            if(started_ && x.ms-since_>=2000){since_=x.ms;progress_raw_=x.raw;}
            return {phase_,direction_,.2,advancing_target(x,.2),true};
        }
        if(phase_==HomePhase::first || phase_==HomePhase::second){
            if(!polarity_){const double change=(x.raw-origin_raw_)*sensor_.sign();if(std::abs(change)>sensor_.jitter+sensor_.buffer() && (x.count-origin_count_)*direction_>0){polarity_=change*(x.count-origin_count_)>0?1:-1;destination_=direction_*polarity_>0?1:0;}}
            if(polarity_){
                const double u=sensor_.units(x.raw),remaining=destination_?sensor_.span()-u:u;
                if(remaining<0){second_leg_=phase_==HomePhase::second;direction_=-direction_;transition(HomePhase::correct,x);return {phase_,0,0,0,false,true};}
                if(remaining<=sensor_.jitter+sensor_.buffer()){second_leg_=phase_==HomePhase::second;transition(HomePhase::capture,x);return {phase_,0,0,0,false,true};}
                if(remaining<=.05*sensor_.span())slow_=true;
            }
            auto result=movement(x,slow_?.2:1.);
            // Before polarity qualification only net excursion establishes
            // response. Adjacent-code jitter cannot restart the two-second window.
            if(!polarity_ && std::abs(x.raw-progress_raw_)>sensor_.jitter+sensor_.buffer())progress(x);
            return result;
        }
        if(phase_==HomePhase::correct){
            const double u=sensor_.units(x.raw);if(destination_?u<=sensor_.span():u>=0){transition(HomePhase::capture,x);return {phase_,0,0,0,false,true};}
            return movement(x,.05);
        }
        if(phase_==HomePhase::capture){
            a.hold=true;
            if(!x.stop_confirmed){stationary_=x.ms;last_raw_=x.raw;last_count_=x.count;return a;}
            const double u=sensor_.units(x.raw);
            const double overshoot=destination_?u-sensor_.span():-u;
            // A real crossing is corrected immediately. Once stopped, measured
            // stationary ADC jitter is not another crossing/settling episode.
            if(overshoot>0 && (std::abs(x.count-last_count_)>=.5 || overshoot>sensor_.jitter)){direction_=destination_?-polarity_:polarity_;transition(HomePhase::correct,x);return {phase_,0,0,0,false,true};}
            if(!settled(x))return a;
            expected_position_=x.count;expected_velocity_=0;
            // Crossing back into the range is not an endpoint measurement.
            // A coarse/late sample can already be well inside the travel. Stop
            // first, then repeat the same endpoint approach if outside J+B.
            if(std::abs(destination_?sensor_.span()-u:u)>sensor_.jitter+sensor_.buffer()){
                direction_=destination_?polarity_:-polarity_;
                transition(second_leg_?HomePhase::second:HomePhase::first,x);
                return {phase_,0,0,0,false,true};
            }
            if(!second_leg_){map_.first_count=x.count;map_.first_raw=x.raw;direction_=destination_?-polarity_:polarity_;destination_=destination_?0:1;second_leg_=true;origin_raw_=x.raw;origin_count_=x.count;transition(HomePhase::second,x);return {phase_,0,0,0,false,true};}
            map_.second_count=x.count;map_.second_raw=x.raw;map_.sensor=sensor_;
            if(!std::isfinite(map_.first_count) || !std::isfinite(map_.second_count) || map_.second_raw==map_.first_raw || map_.second_count==map_.first_count)return failure("Homing endpoint pairs cannot define a finite sensor-to-motor conversion");
            map_.valid=true;transition(HomePhase::park,x);return {phase_,0,0,0,false,true};
        }
        if(phase_==HomePhase::park){
            const double wanted=sensor_.low+sensor_.sign()*(park_fraction==0 && throttle_park?sensor_.jitter:park_fraction*sensor_.span());
            const double u=sensor_.units(x.raw);
            const bool crossed=u<-sensor_.jitter || u>sensor_.span()+sensor_.jitter;
            if(!crossed && std::abs(x.raw-wanted)<=sensor_.jitter+sensor_.buffer()){a.hold=true;if(!x.stop_confirmed){capture_started_=false;return a;}if(!capture_started_){capture_started_=true;stationary_=x.ms;last_raw_=x.raw;last_count_=x.count;}if(settled(x)){transition(HomePhase::complete,x);return {phase_,0,0,0,false,false,true,true};}return a;}
            const int direction=(wanted-x.raw)*sensor_.sign()*polarity_>0?1:-1;
            if(capture_started_ || direction!=direction_){progress(x);started_=false;}
            capture_started_=false;direction_=direction;
            a=movement(x,crossed?.05:.2,std::abs(wanted-progress_raw_));
            // Parking is sensor-directed. A finite, already reached encoder
            // target cannot prove arrival or require an impossible extra move.
            if(a.move){const double counts_per_raw=(map_.second_count-map_.first_count)/(map_.second_raw-map_.first_raw);const double residual=(wanted-x.raw)*counts_per_raw+direction_*takeup_*std::abs(counts_per_raw);
                a.target=x.count+direction_*std::min(std::abs(a.target-x.count),std::abs(residual));}
            return a;
        }
        if(phase_==HomePhase::complete)return {phase_,0,0,0,false,false,true,true};
        return a;
    }
};

// The edge is a directional condition, never a motor fault. Hysteresis uses one
// ADC code beyond the measured jitter zone and never the homing arrival buffer.
inline int motor_sensor_edge(const MotorSensorContract& c,double raw,int previous) noexcept {
    const double u=c.units(raw),span=c.span();
    if(previous<0 && u<=c.jitter+1)return -1;
    if(previous>0 && u>=span-c.jitter-1)return 1;
    return u<=c.jitter?-1:u>=span-c.jitter?1:0;
}
inline bool motor_outward_direction(int edge,double signed_delta) noexcept {return edge && signed_delta*edge>=0;}
}
