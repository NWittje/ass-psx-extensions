#include "psx_plugin/audio_model.hpp"
#include <charconv>
namespace psx::xp {
namespace {
bool field(const audio::State& s,audio::Group g,std::size_t i,float& out) noexcept {
    if(!(s.available_groups&audio::bit(g)))return false;const auto& snap=s.groups[static_cast<std::size_t>(g)];
    if(!(snap.flags&audio::coherent)||!snap.has(i)||!std::isfinite(snap.values[i]))return false;out=snap.values[i];return true;
}
int bracket(std::string_view name) noexcept {
    const auto at=name.find('[');if(at==name.npos)return -1;int value{};
    const auto end=name.find(']',at);if(end==name.npos)return -1;
    const auto parsed=std::from_chars(name.data()+at+1,name.data()+end,value);
    return parsed.ec==std::errc{}&&parsed.ptr==name.data()+end?value:-1;
}
}
Input parameter_source(std::string_view n,const EventSpec&) noexcept {
    using K=Input::Kind;using G=audio::Group;
    if(n=="sim/operation/sound/inside_any")return {K::constant,G::engines,0,1,1};
    if(n=="sim/graphics/view/view_is_external")return {K::constant,G::engines,0,1,0};
    if(n=="sim/time/total_flight_time_sec")return {K::flight_time};
    if(n=="laminar/B747/engines/type")return {K::family};
    if(n=="sim/graphics/view/pilots_head_y")return {K::eye_y};
    if(n=="sim/graphics/view/pilots_head_z")return {K::eye_z};
    if(n=="sim/flightmodel/position/true_airspeed")return {K::field,G::airflow,0};
    if(n=="sim/cockpit2/temperature/outside_air_LE_temp_degc")return {K::field,G::airflow,1};
    if(n=="sim/cockpit/engine/APU_N1")return {K::field,G::apu,0,0.1F};
    if(n.starts_with("laminar/B747/hydraulics/pressure_")&&n.size()&&n.back()>='1'&&n.back()<='4')return {K::field,G::hydraulic,static_cast<unsigned>(n.back()-'1')};
    const int i=bracket(n);if(i<0)return {};
    if(i<4&&(n.starts_with("sim/flightmodel2/engines/N1_percent[")||n.starts_with("sim/cockpit2/engine/indicators/N1_percent[")))return {K::field,G::engines,static_cast<unsigned>(i*6),0.1F};
    if(i<4&&n.starts_with("sim/flightmodel2/engines/thrust_reverser_deploy_ratio["))return {K::field,G::engines,static_cast<unsigned>(i*6+5)};
    if(i<5&&n.starts_with("sim/flightmodel2/gear/deploy_ratio[")){constexpr unsigned gear[]{0,4,3,2,1};return {K::field,G::gear,gear[i]};}
    return {};
}
bool input_value(const Input& in,const audio::State& state,const AudioHost& host,float& out) noexcept {
    switch(in.kind){
    case Input::Kind::constant:out=in.constant;break;
    case Input::Kind::field:if(!field(state,in.group,in.field,out))return false;out*=in.scale;break;
    case Input::Kind::flight_time:if(!host.flight_time_valid)return false;out=host.flight_time;break;
    case Input::Kind::family:switch(state.groups[0].family){case audio::Family::ge:out=1;break;case audio::Family::pw:out=0;break;case audio::Family::rr:out=2;break;default:return false;}break;
    case Input::Kind::eye_y:out=static_cast<float>(host.profile.eye.y);break;
    case Input::Kind::eye_z:out=static_cast<float>(host.profile.eye.z);break;
    default:return false;
    }return std::isfinite(out);
}
Evaluation evaluate(const EventSpec& e,const audio::State& s) noexcept {
    using G=audio::Group;float a{},b{};auto get=[&](G g,unsigned i,float& v){return field(s,g,i,v);};
    auto one=[&](G g,unsigned i,float threshold){return Evaluation{get(g,i,a),a>threshold};};
    const auto base=e.index*6;
    switch(e.trigger){
    case Trigger::engine_run:return one(G::engines,base,0);
    case Trigger::engine_crank:{const bool ok=get(G::engines,base+1,a)&&get(G::engines,base+3,b);return {ok,a>20&&b==0};}
    case Trigger::engine_ignite:return one(G::engines,base+3,0);
    case Trigger::engine_shutdown:{const bool ok=get(G::engines,base,a);return {ok,a<198};}
    case Trigger::reverse:return one(G::engines,base+5,0);
    case Trigger::gear:return one(G::gear,e.index,0.05F);
    case Trigger::hydraulic:return one(G::hydraulic,e.index,0);
    case Trigger::boolean:return one(e.group,e.index,0);
    case Trigger::pack:case Trigger::pack_stop:{const bool ok=get(G::packs,e.index,a)&&get(G::packs,e.index+3,b);const bool flow=a==1&&b>10;return {ok,e.trigger==Trigger::pack?flow:!flow};}
    case Trigger::fuel_stop:{const bool ok=get(G::fuel,e.index,a);return {ok,a==0};}
    case Trigger::apu_run:return one(G::apu,2,0);
    case Trigger::apu_start:return one(G::apu,2,0); // Authored runningWell acoustic edge, not a starter observation.
    case Trigger::apu_stop:{const bool ok=get(G::apu,2,a);return {ok,a==0};}
    case Trigger::wind:return one(G::airflow,0,0);
    case Trigger::flap_move:case Trigger::flap_stop:{bool moving=false;for(unsigned i=0;i<32;++i)if(e.index&(1u<<i)){if(!get(G::flaps,i,a))return {};moving=moving||a!=0;}return {true,e.trigger==Trigger::flap_move?moving:!moving};}
    case Trigger::snapshot:return {true,true};
    }return {};
}
bool transition_matches(const EventSpec& e,const audio::Transition& t) noexcept {
    if(t.group!=e.group)return false;
    switch(e.trigger){
    case Trigger::engine_crank:return t.field==e.index*6+1&&t.edge==audio::Edge::rising;
    case Trigger::engine_shutdown:return t.field==e.index*6&&t.edge==audio::Edge::rising;
    case Trigger::engine_ignite:return t.field==e.index*6+3&&t.edge==audio::Edge::rising;
    case Trigger::apu_start:return t.field==2&&t.edge==audio::Edge::rising;
    case Trigger::apu_stop:return t.field==2&&t.edge==audio::Edge::falling;
    case Trigger::flap_stop:return (e.index&(1u<<t.field))&&t.edge==audio::Edge::falling;
    case Trigger::fuel_stop:return t.field==e.index&&t.edge==audio::Edge::falling;
    case Trigger::pack_stop:return t.field==e.index&&t.edge==audio::Edge::falling;
    default:return false;
    }
}
bool AudioStateGate::accept(const audio::State& s,std::uint64_t connection) noexcept {
    if(s.connection_id!=connection||!audio::valid(s))return false;
    if(value_.connection_id==connection&&value_.audio_epoch){
        if(s.audio_epoch<value_.audio_epoch)return false;
        if(s.audio_epoch==value_.audio_epoch){
            if(s.producer_high!=value_.producer_high||s.producer_low!=value_.producer_low)return false;
            for(std::size_t i=0;i<audio::group_count;++i)if((s.available_groups&seen_)&(1u<<i)){
                const auto& a=s.groups[i];const auto& b=retained_[i];
                if(a.source!=b.source||a.family!=b.family||a.variant!=b.variant||a.lifecycle_epoch!=b.lifecycle_epoch||a.configuration_epoch!=b.configuration_epoch||
                    a.sequence<b.sequence||a.sample_end_qpc<b.sample_end_qpc||a.received_qpc<b.received_qpc)return false;
                if(a.sequence==b.sequence&&(a.sample_start_qpc!=b.sample_start_qpc||a.sample_end_qpc!=b.sample_end_qpc||
                    a.valid_fields!=b.valid_fields||a.values!=b.values))return false;
            }
        }
    }
    if(s.audio_epoch!=value_.audio_epoch||s.connection_id!=value_.connection_id){seen_=0;retained_={};}
    for(std::size_t i=0;i<audio::group_count;++i)if(s.available_groups&(1u<<i))retained_[i]=s.groups[i];
    seen_|=s.available_groups;value_=s;return true;
}
} // namespace psx::xp
