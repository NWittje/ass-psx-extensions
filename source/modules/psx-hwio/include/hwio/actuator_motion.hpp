#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
namespace hwio {
enum class ActuatorKind:unsigned {motor,servo,stepper};
struct ActuatorRef {ActuatorKind kind{ActuatorKind::stepper};unsigned slot{};bool operator==(const ActuatorRef&)const=default;};
enum class ActuatorCalibration:unsigned {switches,potentiometer,rotation,legacy_servo,legacy_motor};
enum class RotaryUse:unsigned {compass,needle,drum};
enum class NeedleStartup:unsigned {full_signed,nearest_equivalent};
enum class ReferenceQuality:unsigned {none,measured,switch_estimate,pulse_command};
inline double nearest_periodic(double value,double reference,double period,int positive_direction=1)noexcept{
    if(!std::isfinite(value) || !std::isfinite(reference) || !(period>0))return value;
    // floor(x+.5) resolves exactly half a turn in positive output direction.
    const double turns=(reference-value)/period;return value+(positive_direction>0?std::floor(turns+.5):std::ceil(turns-.5))*period;
}
class RotaryCoordinate {
    bool initialized_{};double previous_source_{},unwrapped_{},offset_{};
public:
    void reset()noexcept{*this={};}
    double target(double source,double measured,double source_revolution,double output_revolution,int sign,RotaryUse use,NeedleStartup startup)noexcept{
        const double nominal=source/source_revolution*output_revolution*sign;
        if(!initialized_){initialized_=true;previous_source_=source;unwrapped_=nominal;offset_=use==RotaryUse::needle && startup==NeedleStartup::nearest_equivalent?nearest_periodic(nominal,measured,output_revolution,sign)-nominal:0;}
        else if(use==RotaryUse::compass){unwrapped_=nearest_periodic(nominal,unwrapped_,output_revolution,sign);previous_source_=source;}
        else {unwrapped_+=(source-previous_source_)/source_revolution*output_revolution*sign;previous_source_=source;}
        if(use==RotaryUse::compass)unwrapped_=nearest_periodic(nominal,measured,output_revolution,sign);
        return unwrapped_+offset_;
    }
};
struct ActuatorSwitchSample {
    bool valid{},active{};std::uint64_t generation{},sequence{},ms{};
};
struct ActuatorEdge {bool active{};std::uint64_t ms{},sequence{};};
// The candidate's original timestamp survives debounce; duplicate/out-of-order
// packets cannot create edges. Quiet current levels remain valid.
class ActuatorDebounce {
    bool initialized_{},level_{},candidate_{};std::uint64_t generation_{},sequence_{},candidate_ms_{},candidate_sequence_{};
public:
    void reset()noexcept{*this={};}
    std::optional<ActuatorEdge> sample(const ActuatorSwitchSample& s,std::uint64_t now,unsigned debounce)noexcept{
        if(!s.valid || s.ms>now)return {};
        if(!initialized_ || generation_!=s.generation){initialized_=true;level_=candidate_=s.active;generation_=s.generation;sequence_=s.sequence;candidate_ms_=s.ms;candidate_sequence_=s.sequence;return {};}
        std::optional<ActuatorEdge> edge;
        if(s.sequence>sequence_){if(candidate_!=level_ && s.ms>=candidate_ms_ && s.ms-candidate_ms_>=debounce){level_=candidate_;edge=ActuatorEdge{level_,candidate_ms_,candidate_sequence_};}sequence_=s.sequence;if(s.active!=candidate_){candidate_=s.active;candidate_ms_=s.ms;candidate_sequence_=s.sequence;}}
        if(candidate_!=level_ && now>=candidate_ms_ && now-candidate_ms_>=debounce){level_=candidate_;if(!edge || !edge->active)edge=ActuatorEdge{level_,candidate_ms_,candidate_sequence_};}return edge;
    }
    bool active()const noexcept{return level_;}
};
struct PositionCorrelation {double position{},uncertainty{};std::uint64_t sequence{};};
class ActuatorPositionHistory {
    struct Point {std::uint64_t ms{};double position{},speed{};};std::array<Point,128> data_{};unsigned count_{},next_{};
public:
    void reset()noexcept{count_=next_=0;}
    void add(std::uint64_t ms,double position,double speed)noexcept{if(!std::isfinite(position) || !std::isfinite(speed))return;if(count_ && ms<=data_[(next_+127)%128].ms)return;data_[next_]={ms,position,speed};next_=(next_+1)%128;count_=std::min(128u,count_+1);}
    std::optional<PositionCorrelation> at(std::uint64_t ms)const noexcept{
        if(count_<2)return {};for(unsigned i=1;i<count_;++i){const auto& a=data_[(next_+128-count_+i-1)%128];const auto& b=data_[(next_+128-count_+i)%128];if(ms<a.ms || ms>b.ms)continue;const double dt=double(b.ms-a.ms)/1000.;return PositionCorrelation{a.position+(b.position-a.position)*double(ms-a.ms)/double(b.ms-a.ms),.5*dt*std::max(std::abs(a.speed),std::abs(b.speed))+1};}return {};
    }
};
struct SwitchHomeSettings {
    bool two_switches{},rotation{};int direction{-1};double search_distance{},velocity{},latch_velocity{},clearance{},entered_travel{};unsigned timeout_ms{},debounce_ms{20};
    bool valid()const noexcept{return (direction==1 || direction==-1) && std::isfinite(search_distance) && search_distance>0 && std::isfinite(velocity) && velocity>0 && std::isfinite(latch_velocity) && latch_velocity>0 && latch_velocity<=velocity && std::isfinite(clearance) && clearance>0 && timeout_ms>0 && debounce_ms<=1000 && (rotation || two_switches || (std::isfinite(entered_travel) && entered_travel>2*clearance));}
    bool operator==(const SwitchHomeSettings&)const=default;
};
enum class SwitchHomePhase {idle,escape,seek,backoff,latch,settle,complete,fault};
struct SwitchHomeAction {bool move{},hold{},complete{},failed{};double target{},velocity{};std::string reason;};
class SwitchHome {
    std::uint64_t consumed_edge_{};SwitchHomeSettings config_;SwitchHomePhase phase_{};std::uint64_t start_{},stationary_{},generation_{};double origin_{},edge_{},first_{},second_{},last_{};int direction_{};bool second_leg_{};
    SwitchHomeAction fail(std::string text){phase_=SwitchHomePhase::fault;return {false,true,false,true,0,0,std::move(text)};}
public:
    SwitchHomePhase phase()const noexcept{return phase_;}double first()const noexcept{return first_;}double second()const noexcept{return second_;}
    void start(SwitchHomeSettings s,double position,bool active,std::uint64_t now,std::uint64_t generation){*this={};config_=s;start_=now;origin_=position;generation_=generation;direction_=s.direction;phase_=active?SwitchHomePhase::escape:SwitchHomePhase::seek;}
    SwitchHomeAction step(double position,bool moving,const ActuatorSwitchSample& input,const std::optional<PositionCorrelation>& edge,std::uint64_t now){
        if(phase_==SwitchHomePhase::complete)return {false,false,true};if(phase_==SwitchHomePhase::fault || phase_==SwitchHomePhase::idle)return {};
        if(!generation_)generation_=input.generation;
        if(!config_.valid() || !input.valid || input.generation!=generation_ || !std::isfinite(position))return fail("Homing switch or controller continuity unavailable");
        if(now-start_>=config_.timeout_ms || std::abs(position-origin_)>config_.search_distance+config_.clearance)return fail("Home search distance/time exceeded; inspect switch direction and travel");
        const double limit=origin_+direction_*config_.search_distance;
        switch(phase_){
        case SwitchHomePhase::escape:
            if(!input.active){phase_=SwitchHomePhase::seek;origin_=position;return {false,true};}
            return {true,false,false,false,origin_-direction_*config_.search_distance,config_.latch_velocity};
        case SwitchHomePhase::seek:
            if(input.active || (edge && edge->sequence && edge->sequence!=consumed_edge_)){if(!edge || edge->uncertainty>config_.clearance)return fail("Home switch correlation exceeds stopping clearance");consumed_edge_=edge->sequence;edge_=edge->position;phase_=SwitchHomePhase::backoff;return {true,true,false,false,edge_-direction_*config_.clearance,config_.latch_velocity};}
            return {true,false,false,false,limit,config_.velocity};
        case SwitchHomePhase::backoff:
            if(!moving && std::abs(position-(edge_-direction_*config_.clearance))<=1){if(input.active)return fail("Home switch remains active after backoff");phase_=SwitchHomePhase::latch;}
            else return {true,false,false,false,edge_-direction_*config_.clearance,config_.latch_velocity};
            [[fallthrough]];
        case SwitchHomePhase::latch:
            if(input.active || (edge && edge->sequence && edge->sequence!=consumed_edge_)){if(!edge || edge->uncertainty>config_.clearance)return fail("Latch edge correlation exceeds stopping clearance");consumed_edge_=edge->sequence;edge_=edge->position;phase_=SwitchHomePhase::settle;stationary_=0;last_=position;return {false,true};}
            return {true,false,false,false,limit,config_.latch_velocity};
        case SwitchHomePhase::settle:
            if(moving || std::abs(position-last_)>.5){stationary_=0;last_=position;return {false,true};}
            if(!stationary_)stationary_=now;if(now-stationary_<200)return {false,true};
            if(!second_leg_){first_=edge_;if(config_.two_switches || config_.rotation){second_leg_=true;generation_=0;consumed_edge_=0;direction_=-direction_;origin_=position;phase_=config_.rotation?SwitchHomePhase::escape:SwitchHomePhase::seek;return {false,true};}second_=first_-direction_*config_.entered_travel;}
            else second_=edge_;
            if(!config_.rotation && std::abs(second_-first_)<=2*config_.clearance)return fail("Endstop travel is smaller than stopping clearance");phase_=SwitchHomePhase::complete;return {false,true,true};
        default:return {};
        }
    }
    bool second_leg()const noexcept{return second_leg_;}
};
class ActuatorIndex {
    std::array<std::optional<double>,2> offsets_;double correction_{};
public:
    void reset()noexcept{*this={};}double correction()const noexcept{return correction_;}
    void qualify(int direction,double phase)noexcept{if(direction)offsets_[direction>0?1:0]=phase;}
    // A periodic index establishes phase only. Nearest equivalent retains the
    // already established revolution number; it cannot detect a whole lost turn.
    bool observe(int direction,PositionCorrelation hit,double revolution,double window,std::string& error)noexcept{
        if(!direction || !offsets_[direction>0?1:0]){error="Index approach direction is not calibrated";return false;}
        const double expected=nearest_periodic(*offsets_[direction>0?1:0],hit.position+correction_,revolution),delta=expected-(hit.position+correction_);
        if(std::abs(delta)+hit.uncertainty>window){error="Index discrepancy exceeds correction window; Reset-and-home required";return false;}correction_+=delta;error.clear();return true;
    }
};
}
