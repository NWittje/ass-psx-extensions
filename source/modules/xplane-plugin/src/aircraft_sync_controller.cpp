#include "psx_plugin/aircraft_sync_controller.hpp"
#include <psx/sync/runtime.hpp>
#include <cmath>
#include <algorithm>
#include <bit>

namespace psx::xp {
namespace {
constexpr std::array<unsigned,5> gear_native{0,4,3,2,1};
constexpr std::array<unsigned,6> le_native{2,1,1,1,1,2};
constexpr std::array<std::uint32_t,6> visible{0x39f,0x2f5,0x3ff,0x3ff,15,0x367f};
constexpr const char* leading_names[]{"psx_ios/aircraft_sync/leading_edge_0","psx_ios/aircraft_sync/leading_edge_1","psx_ios/aircraft_sync/leading_edge_2","psx_ios/aircraft_sync/leading_edge_3","psx_ios/aircraft_sync/leading_edge_4","psx_ios/aircraft_sync/leading_edge_5"};
constexpr const char* gear_names[]{"psx_ios/aircraft_sync/gear_0","psx_ios/aircraft_sync/gear_1","psx_ios/aircraft_sync/gear_2","psx_ios/aircraft_sync/gear_3","psx_ios/aircraft_sync/gear_4"};
constexpr const char* door_names[]{"psx_ios/aircraft_sync/door_0","psx_ios/aircraft_sync/door_1","psx_ios/aircraft_sync/door_2","psx_ios/aircraft_sync/door_3","psx_ios/aircraft_sync/door_4"};
constexpr const char* reverse_names[]{"psx_ios/aircraft_sync/reverser_0","psx_ios/aircraft_sync/reverser_1","psx_ios/aircraft_sync/reverser_2","psx_ios/aircraft_sync/reverser_3"};
constexpr const char* light_names[]{"psx_ios/aircraft_sync/light_0","psx_ios/aircraft_sync/light_1","psx_ios/aircraft_sync/light_2","psx_ios/aircraft_sync/light_3","psx_ios/aircraft_sync/light_4","psx_ios/aircraft_sync/light_5","psx_ios/aircraft_sync/light_6"};
constexpr const char* spill_names[]{"psx_ios/aircraft_sync/light_spill_0","psx_ios/aircraft_sync/light_spill_1","psx_ios/aircraft_sync/light_spill_2","psx_ios/aircraft_sync/light_spill_3","psx_ios/aircraft_sync/light_spill_4","psx_ios/aircraft_sync/light_spill_5","psx_ios/aircraft_sync/light_spill_6","psx_ios/aircraft_sync/light_spill_7","psx_ios/aircraft_sync/light_spill_8","psx_ios/aircraft_sync/light_spill_9","psx_ios/aircraft_sync/light_spill_10","psx_ios/aircraft_sync/light_spill_11","psx_ios/aircraft_sync/light_spill_12","psx_ios/aircraft_sync/light_spill_13"};
constexpr const char* active_names[]{"psx_ios/aircraft_sync/active_light_0","psx_ios/aircraft_sync/active_light_1","psx_ios/aircraft_sync/active_light_2","psx_ios/aircraft_sync/active_light_3","psx_ios/aircraft_sync/active_light_4","psx_ios/aircraft_sync/active_light_5","psx_ios/aircraft_sync/active_light_6","psx_ios/aircraft_sync/active_light_7","psx_ios/aircraft_sync/active_light_8","psx_ios/aircraft_sync/active_light_9","psx_ios/aircraft_sync/active_light_10","psx_ios/aircraft_sync/active_light_11","psx_ios/aircraft_sync/active_light_12","psx_ios/aircraft_sync/active_light_13"};
bool domain(const aircraft::Control& c,const aircraft::Sample& s) noexcept {
    return c.producer==s.producer&&c.lifecycle==s.lifecycle&&c.configuration==s.configuration;
}
float mapped(unsigned group,unsigned component,float value) noexcept {
    if(group==2&&component>=6){
        // PSX actual extents at detents; installed Sparky OBJ keys use the flap labels.
        // This spatial profile approximation has no clock or animation response.
        constexpr std::array<float,6> ext{0,250,750,1250,1750,2250},label{0,5,10,20,25,30};
        const auto position=value*2250;
        for(unsigned i=1;i<6;++i)if(position<=ext[i])return label[i-1]+(position-ext[i-1])*(label[i]-label[i-1])/(ext[i]-ext[i-1]);
        return 30;
    }
    if(group==4)return value>0?1.0f:0.0f;
    if(group==5)return value>=4?1.0f:0.0f; // Powered heating levels never emit a beam.
    return value;
}
float unmapped_te(float value) noexcept {
    constexpr std::array<float,6> ext{0,250,750,1250,1750,2250},label{0,5,10,20,25,30};
    for(unsigned i=1;i<6;++i)if(value<=label[i])return (ext[i-1]+(value-label[i-1])*(ext[i]-ext[i-1])/(label[i]-label[i-1]))/2250;
    return 1;
}
}
bool AircraftController::begin(SdkApi& sdk,motion::InstanceId provider,std::uint64_t frequency,std::int64_t (*clock)()) noexcept {
    if(!sdk.on_thread()||!clock||!frequency||!motion::valid(provider))return false;
    retire();for(unsigned g=0;g<6;++g)if(restoration_pending(g))return false;
    sdk_=&sdk;provider_=provider;frequency_=frequency;clock_=clock;floor_=clock();generation_=0;connection_=0;groups_={};applied_={};
    lifecycle_pending_=have_pending_profile_=false;pending_generation_=0;lifecycle_retry_qpc_=0;pending_profile_={};
    owned_ref_=XPLMRegisterDataAccessor("psx_ios/aircraft_sync/owned_groups",xplmType_Int,0,read_owned,nullptr,nullptr,nullptr,
        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,this,nullptr);
    if(!owned_ref_){retire();return false;}
    components_ref_=XPLMRegisterDataAccessor("psx_ios/aircraft_sync/owned_components",xplmType_FloatArray,0,nullptr,nullptr,nullptr,nullptr,
        nullptr,nullptr,nullptr,nullptr,read_components,nullptr,nullptr,nullptr,this,nullptr);
    if(!components_ref_){retire();return false;}
    // Register fixed plugin-owned accessors at enable, before scheduling frame callbacks.
    profile({});
    return true;
}
void AircraftController::retire() noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    lifecycle_pending_=have_pending_profile_=false;
    for(unsigned g=0;g<6;++g)release(g,aircraft::Reason::stopped);
    for(unsigned g=0;g<6;++g)if(restoration_pending(g))return;
    for(std::size_t i=0;i<proxy_count_;++i)if(proxies_[i].ref)XPLMUnregisterDataAccessor(proxies_[i].ref);
    if(owned_ref_)XPLMUnregisterDataAccessor(owned_ref_);
    if(components_ref_)XPLMUnregisterDataAccessor(components_ref_);
    owned_ref_=components_ref_=nullptr;proxy_count_=0;bindings_={};proxies_={};capabilities_={};qualified_=false;sdk_=nullptr;
}
void AircraftController::lifecycle(std::uint64_t generation,bool loading) noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    if(generation<(lifecycle_pending_?pending_generation_:generation_))return;
    if(!lifecycle_pending_||generation!=pending_generation_)have_pending_profile_=false;
    lifecycle_pending_=true;pending_generation_=generation;pending_loading_=loading;
    qualified_=false;loading_=true;floor_=clock_();resume_lifecycle(true);
}
bool AircraftController::resume_lifecycle(bool force) noexcept {
    if(!lifecycle_pending_)return true;const auto now=clock_();
    if(!force&&now<lifecycle_retry_qpc_)return false;
    // A transient restoration fault retries at most 4 Hz in ordinary callbacks.
    // New profile/connection/control input may resolve it immediately, without a wait.
    lifecycle_retry_qpc_=now+static_cast<std::int64_t>((std::max)(std::uint64_t{1},frequency_/4));
    for(unsigned g=0;g<6;++g)release(g,aircraft::Reason::loading);
    for(unsigned g=0;g<6;++g)if(restoration_pending(g))return false;
    generation_=pending_generation_;loading_=pending_loading_;qualified_=false;floor_=clock_();lifecycle_pending_=false;
    for(auto& group:bindings_)for(auto& b:group)b={};capabilities_={};
    for(std::size_t i=0;i<proxy_count_;++i)proxies_[i].fallback=nullptr;
    if(have_pending_profile_){const auto current=pending_profile_;have_pending_profile_=false;profile(current);}return true;
}
void AircraftController::connection(std::uint64_t value) noexcept {
    if(!sdk_||!sdk_->on_thread()||value==connection_)return;
    for(unsigned g=0;g<6;++g)release(g,aircraft::Reason::disconnected);
    connection_=value;floor_=clock_();resume_lifecycle(true);
}
void AircraftController::bind(const char* name,unsigned group,unsigned component,int index) noexcept {
    auto ref=XPLMFindDataRef(name);const auto type=index<0?xplmType_Float:xplmType_FloatArray;
    if(!ref||sdk_->inspect(ref,type,true)!=Status::ok)return;
    if(index>=0&&XPLMGetDatavf(ref,nullptr,0,0)<=index)return;
    bindings_[group][component]={ref,index};capabilities_[group]|=1u<<component;
}
bool AircraftController::proxy(const char* name,unsigned group,unsigned component,const char* fallback,int index,bool door,bool spill,bool active) noexcept {
    Proxy* p=nullptr;
    // Profiles rebind native consumers while the plugin-owned accessors retain their addresses.
    for(std::size_t i=0;i<proxy_count_;++i)if(proxies_[i].group==group&&proxies_[i].component==component&&
        proxies_[i].door==door&&proxies_[i].spill==spill&&proxies_[i].active==active){p=&proxies_[i];break;}
    if(!p){if(proxy_count_==proxies_.size())return false;p=&proxies_[proxy_count_++];
        p->owner=this;p->group=group;p->component=component;p->door=door;p->spill=spill;p->active=active;
        p->ref=XPLMRegisterDataAccessor(name,spill?xplmType_FloatArray:xplmType_Float,0,nullptr,nullptr,
            spill?nullptr:read_proxy,nullptr,nullptr,nullptr,nullptr,nullptr,spill?read_spill:nullptr,nullptr,nullptr,nullptr,p,nullptr);
    }
    if(!p->ref)return false;
    p->fallback=fallback?XPLMFindDataRef(fallback):nullptr;p->fallback_index=index;
    if(!spill&&!active){if(!p->fallback||sdk_->inspect(p->fallback,index<0?xplmType_Float:xplmType_FloatArray,false)!=Status::ok)return false;
        if(index>=0&&XPLMGetDatavf(p->fallback,nullptr,0,0)<=index)return false;
        bindings_[group][component]={p->ref,-1};capabilities_[group]|=1u<<component;
    }
    return true;
}
void AircraftController::profile(const MotionProfile& profile) noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    if(lifecycle_pending_){if(profile.generation!=pending_generation_)return;pending_profile_=profile;have_pending_profile_=true;resume_lifecycle(true);return;}
    if(profile.generation!=generation_)return;
    for(unsigned g=0;g<6;++g)if(restoration_pending(g)&&!(owned_&(1u<<g)))return;
    qualified_=profile.qualified&&profile.aircraft_state_qualified;
    constexpr const char* controls[]{"laminar/B747/flt_ctrls/l_aileron_outer","laminar/B747/flt_ctrls/l_aileron_inner","laminar/B747/flt_ctrls/r_aileron_inner","laminar/B747/flt_ctrls/r_aileron_outer",
        "laminar/B747/flt_ctrls/l_elev_outer",nullptr,nullptr,"laminar/B747/flt_ctrls/r_elev_outer","laminar/B747/flt_ctrls/rudder_upr_pos","laminar/B747/flt_ctrls/rudder_lwr_pos"};
    for(unsigned i=0;i<10;++i)if(controls[i])bind(controls[i],0,i);
    // Unsupported inboard-elevator meshes and stabilizer datum remain unavailable.
    for(unsigned i=0;i<11;++i)bind("laminar/B747/flt_ctrls/spoilers",1,i,static_cast<int>(i<5?i+1:i==5?6:i+2));
    for(unsigned i=6;i<10;++i)bind("laminar/B747/flt_ctrls/flaps",2,i,static_cast<int>(i-5));
    for(unsigned i=0;i<6;++i)proxy(leading_names[i],2,i,le_native[i]==1?"sim/flightmodel2/controls/slat1_deploy_ratio":"sim/flightmodel2/controls/slat2_deploy_ratio",-1);
    for(unsigned i=0;i<5;++i){proxy(gear_names[i],3,i,"sim/flightmodel2/gear/deploy_ratio",static_cast<int>(gear_native[i]));
        proxy(door_names[i],3,i+5,"sim/flightmodel2/gear/deploy_ratio",static_cast<int>(gear_native[i]),true);}
    for(unsigned i=0;i<4;++i)proxy(reverse_names[i],4,i,"sim/flightmodel2/engines/thrust_reverser_deploy_ratio",static_cast<int>(i));
    for(unsigned i=0;i<14;++i)proxy(spill_names[i],5,i,nullptr,-1,false,true);
    for(unsigned i=0;i<14;++i)proxy(active_names[i],5,i,nullptr,-1,false,false,true);
    // Native flags retain simulator flash timing and ordinary light state, with targeted Lua guards.
    constexpr std::array<int,4> land{0,3,1,2};
    for(unsigned i=0;i<4;++i)bind("sim/cockpit2/switches/landing_lights_switch",5,i,land[i]);
    bind("sim/cockpit2/switches/generic_lights_switch",5,4,0);bind("sim/cockpit2/switches/generic_lights_switch",5,5,1);
    // Int flags use a dedicated checked path below; preserve exact prior values on release.
    constexpr const char* integer_names[]{"sim/cockpit2/switches/taxi_light_on","sim/cockpit2/switches/beacon_on",
        "sim/cockpit2/switches/beacon_on","sim/cockpit2/switches/navigation_lights_on","sim/cockpit2/switches/navigation_lights_on",
        "sim/cockpit2/switches/strobe_lights_on"};
    for(unsigned i=6;i<12;++i){auto ref=XPLMFindDataRef(integer_names[i-6]);if(ref&&sdk_->inspect(ref,xplmType_Int,true)==Status::ok){
        bindings_[5][i]={ref,-2};capabilities_[5]|=1u<<i;}}
    bind("sim/cockpit2/switches/generic_lights_switch",5,12,2);bind("sim/cockpit2/switches/generic_lights_switch",5,13,3);
    // Read-only mesh light proxies do not replace the native flags' readback bindings.
    for(unsigned i=0;i<7;++i){auto saved=bindings_[5][i];
        auto source=i<4?"sim/flightmodel2/lights/landing_lights_brightness_ratio":i<6?"sim/flightmodel2/lights/generic_lights_brightness_ratio":"sim/flightmodel2/lights/taxi_lights_brightness_ratio";
        proxy(light_names[i],5,i,source,i<4?land[i]:i<6?static_cast<int>(i-4):0);bindings_[5][i]=saved;}
    for(unsigned g=0;g<6;++g)capabilities_[g]&=visible[g];loading_=false;
}
bool AircraftController::read(const Binding& b,float& value,bool raw) noexcept {
    if(!b.ref)return false;auto type=b.index==-2?xplmType_Int:b.index<0?xplmType_Float:xplmType_FloatArray;
    if(sdk_->inspect(b.ref,type,false)!=Status::ok)return false;
    if(b.index==-2)value=static_cast<float>(XPLMGetDatai(b.ref));
    else if(b.index<0)value=XPLMGetDataf(b.ref);
    else if(XPLMGetDatavf(b.ref,&value,b.index,1)!=1)return false;
    return raw||std::isfinite(value);
}
bool AircraftController::write(Binding& b,float value) noexcept {
    if(!b.ref||!std::isfinite(value))return false;
    // Proxies are plugin-owned state: do not attempt SDK writes into read-only providers.
    for(std::size_t i=0;i<proxy_count_;++i)if(proxies_[i].ref==b.ref){b.have_written=read(b,b.written,true);b.uncertain=!b.have_written;return true;}
    auto type=b.index==-2?xplmType_Int:b.index<0?xplmType_Float:xplmType_FloatArray;
    if(sdk_->inspect(b.ref,type,true)!=Status::ok)return false;
    if(b.index==-2)XPLMSetDatai(b.ref,static_cast<int>(value));
    else if(b.index<0)XPLMSetDataf(b.ref,value);
    else XPLMSetDatavf(b.ref,&value,b.index,1);
    b.have_written=read(b,b.written,true);b.uncertain=!b.have_written;
    return true;
}
bool AircraftController::restoration_pending(unsigned group) const noexcept {for(const auto& b:bindings_[group])if(b.have_previous)return true;return false;}
bool AircraftController::restore(Binding& b) noexcept {
    if(!b.have_previous)return true;
    // Plugin-owned proxies never changed native fallback state.
    for(std::size_t i=0;i<proxy_count_;++i)if(proxies_[i].ref==b.ref){b.have_previous=b.have_written=b.uncertain=false;return true;}
    if(!b.have_written&&!b.uncertain){b.have_previous=false;return true;} // Baseline-only admission never modified this component.
    float current{};const bool observed=read(b,current,true);
    const auto same=[](float a,float z){return a==z||std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(z);};
    if(observed&&same(current,b.previous)){b.have_previous=b.have_written=b.uncertain=false;return true;}
    if(b.uncertain)return false;
    if(observed&&b.have_written&&!same(current,b.written)){b.have_previous=b.have_written=false;return false;}
    const bool restored=write(b,b.previous)&&b.have_written&&std::isfinite(b.written)&&std::abs(b.written-b.previous)<.0001f;
    if(restored)b.have_previous=b.have_written=b.uncertain=false;return restored;
}
void AircraftController::release(unsigned g,aircraft::Reason reason) noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    owned_&=~(1u<<g);
    bool restored=true;
    for(auto& b:bindings_[g])restored=restore(b)&&restored;
    groups_[g].pending=false;groups_[g].have_sample=false;groups_[g].history.clear();
    applied_[g].phase=aircraft::Phase::released;applied_[g].reason=restored?reason:aircraft::Reason::binding;applied_[g].readback_mask=0;applied_[g].valid_mask=0;applied_[g].approximation_mask=0;
    applied_[g].applied_qpc=clock_();
}
aircraft::Ack AircraftController::command(const AircraftCommand& input) noexcept {
    aircraft::Ack ack;ack.control=input.control;ack.reason=aircraft::Reason::none;
    if(!sdk_||!sdk_->on_thread()){ack.reason=aircraft::Reason::binding;return ack;}
    const auto& c=input.control;const auto now=clock_();
    ack.phase=c.action==aircraft::Action::release?aircraft::Phase::released:aircraft::Phase::observing;
    if(input.connection!=connection_){ack.reason=aircraft::Reason::disconnected;return ack;}
    if(lifecycle_pending_)resume_lifecycle(true);
    if(!qualified_&&c.action!=aircraft::Action::release){ack.reason=aircraft::Reason::unsupported;return ack;}
    for(unsigned g=0;g<6;++g)if(c.groups&(1u<<g)) {
        auto& s=groups_[g];
        if(s.command.control.producer==c.producer&&c.lifecycle<s.command.control.lifecycle){ack.reason=aircraft::Reason::lifecycle;continue;}
        if(s.command.connection==input.connection&&c.operation<=s.command.control.operation&&!input.source_loss){ack.reason=aircraft::Reason::owner;continue;}
        if(c.action==aircraft::Action::release){
            if(s.command.control.owner&&(!domain(c,s.input.sample)&&s.have_sample)){ack.reason=aircraft::Reason::owner;continue;}
            release(g,input.source_loss?aircraft::Reason::stale:aircraft::Reason::stopped);s.command=input;ack.accepted_groups|=1u<<g;continue;}
        if(!capabilities_[g])continue;
        if(c.action==aircraft::Action::renew){
            if(s.command.connection!=input.connection||s.command.control.producer!=c.producer||
                s.command.control.lifecycle!=c.lifecycle||s.command.control.configuration!=c.configuration||
                s.command.control.owner!=c.owner){ack.reason=aircraft::Reason::owner;continue;}
            if(now>=s.expires){ack.reason=aircraft::Reason::expired;continue;}
            if(!s.pending){
                // Loading may retire a matching claim before its first coherent
                // sample. Renew must report that retirement, not invent a rival
                // owner and permanently block fresh source reacquisition.
                ack.reason=applied_[g].reason;
                if(ack.reason==aircraft::Reason::none)ack.phase=aircraft::Phase::unavailable;
                continue;
            }
        }
        if(c.action==aircraft::Action::acquire){release(g,aircraft::Reason::none);
            // Restoration retains the prior claim's identity for real terminal
            // reports. Its new clock must not present that old Released result
            // as this pending Acquire before a matching sample reaches the SDK.
            applied_[g]={};
            if(restoration_pending(g)){ack.reason=aircraft::Reason::binding;continue;}s.command=input;s.pending=true;}
        else s.command.control=c;
        s.expires=input.received_qpc+static_cast<std::int64_t>(frequency_*c.duration_ms/1000);ack.accepted_groups|=1u<<g;
    }
    return ack;
}
void AircraftController::sample(const AircraftSample& input) noexcept {
    if(!sdk_||!sdk_->on_thread()||!aircraft::valid(input.sample))return;
    const auto g=static_cast<unsigned>(input.sample.group);auto& s=groups_[g];
    if(!s.pending||s.command.connection!=input.connection||s.command.revision!=input.revision||!domain(s.command.control,input.sample)||
        input.sample.received_qpc<floor_||(s.have_sample&&input.sample.sequence<=s.input.sample.sequence))return;
    s.input=input;s.have_sample=true;
    s.rendered=input.sample.values;
    if(psx::sync::enabled()){
        psx::sync::Values value;value.producer=input.sample.producer.high;value.producer_low=input.sample.producer.low;
        value.lifecycle=input.sample.lifecycle;value.configuration=input.sample.configuration;value.sequence=input.sample.sequence;
        value.qpc=input.sample.source==aircraft::Source::native?input.sample.sample_end_qpc:input.sample.received_qpc;
        value.valid=input.sample.valid_mask;value.continuous=g<4?value.valid:0;value.value=input.sample.values;s.history.append(value);
    }
}
void AircraftController::tick(bool connected,bool loading) noexcept {
    if(!sdk_||!sdk_->on_thread())return;if(lifecycle_pending_&&!resume_lifecycle())return;const auto now=clock_();
    for(unsigned g=0;g<6;++g) {
        auto& s=groups_[g];auto& report=applied_[g];
        if(!s.pending)continue;
        const auto& value=s.input.sample;
        if(!connected||loading||loading_||!qualified_||now>=s.expires){release(g,!connected?aircraft::Reason::disconnected:
            now>=s.expires?aircraft::Reason::expired:aircraft::Reason::loading);continue;}
        if(!s.have_sample)continue;
        if(!(owned_&(1u<<g))&&restoration_pending(g)){release(g,aircraft::Reason::binding);s.pending=true;continue;}
        if(value.source==aircraft::Source::native&&(psx::sync::freshness_now(now)<value.sample_end_qpc||psx::sync::freshness_now(now)-value.sample_end_qpc>=static_cast<std::int64_t>(frequency_*aircraft::native_stale_ms/1000))){
            // Keep a valid renewable pending claim, release only physical output ownership.
            release(g,aircraft::Reason::stale);s.pending=true;continue;}
        s.rendered=value.values;
        if(psx::sync::enabled()&&g<4){psx::sync::Presented presented;const auto delay=frequency_*(g<2?25:75)/1000;
            if(now>=static_cast<std::int64_t>(delay)&&s.history.evaluate(now-static_cast<std::int64_t>(delay),presented))for(unsigned i=0;i<32;++i)if(presented.sample.valid&value.valid_mask&(1u<<i))s.rendered[i]=presented.sample.value[i];}
        const auto previous_mask=report.valid_mask;
        report={};report.producer=value.producer;report.provider=provider_;report.owner=s.command.control.owner;
        report.lifecycle=value.lifecycle;report.configuration=value.configuration;report.sequence=value.sequence;
        report.aircraft_generation=generation_;report.applied_qpc=now;report.group=static_cast<aircraft::Group>(g);
        report.valid_mask=value.valid_mask&capabilities_[g];report.approximation_mask=(value.approximation_mask|
            (g==4||g==5?report.valid_mask:g==2?0x3c0u:0))&report.valid_mask;report.requested=s.rendered;
        if(!report.valid_mask){release(g,aircraft::Reason::binding);s.pending=true;continue;}
        for(unsigned i=0;i<aircraft::component_counts[g];++i)if((previous_mask&~report.valid_mask)&(1u<<i)) {
            auto& b=bindings_[g][i];restore(b);
        }
        // Save native values before activating proxies/guard. Shared light flags saved once per ref.
        bool baseline_failed=false;
        for(unsigned i=0;i<aircraft::component_counts[g];++i)if(report.valid_mask&(1u<<i)) {
            auto& b=bindings_[g][i];if(!b.have_previous)b.have_previous=read(b,b.previous);
            baseline_failed=!b.have_previous||baseline_failed;
        }
        if(g==1&&(report.valid_mask&(1u<<5))){auto& extra=bindings_[1][31];if(!extra.have_previous){extra={};extra.ref=bindings_[1][5].ref;extra.index=7;extra.have_previous=read(extra,extra.previous);}
            baseline_failed=!extra.have_previous||baseline_failed;}
        if(baseline_failed){release(g,aircraft::Reason::binding);s.pending=true;continue;}
        owned_|=1u<<g;bool failed=false;
        for(unsigned i=0;i<aircraft::component_counts[g];++i)if(report.valid_mask&(1u<<i)) {
            auto desired=mapped(g,i,s.rendered[i]);
            if(g==5&&(i==7||i==8))desired=mapped(g,7,value.values[7])||mapped(g,8,value.values[8])?1.0f:0.0f;
            if(g==5&&(i==9||i==10))desired=((report.valid_mask&(1u<<9))&&mapped(g,9,value.values[9]))||
                ((report.valid_mask&(1u<<10))&&mapped(g,10,value.values[10]))?1.0f:0.0f;
            if(!write(bindings_[g][i],desired)){failed=true;continue;}
            const auto& written=bindings_[g][i];const float actual=written.written;
            if(written.have_written&&std::isfinite(actual)&&std::abs(actual-desired)<.0001f){report.readback_mask|=1u<<i;
                report.readback[i]=g==2&&i>=6?unmapped_te(actual):actual;
                if(g==5){std::array<float,9> parameters{1,1,1,1,1,0,0,-1,1};
                    XPLMDataRef light{};for(std::size_t p=0;p<proxy_count_;++p)
                        if(proxies_[p].group==5&&proxies_[p].component==i&&proxies_[p].spill){light=proxies_[p].ref;break;}
                    if(light&&XPLMGetDatavf(light,parameters.data(),0,9)==9)
                        report.readback[i]=parameters[3]>0?9.0f:0.0f;
                    else {report.readback_mask&=~(1u<<i);failed=true;}}
            }else failed=true;
            if(g==1&&i==5){auto& extra=bindings_[1][31];if(!write(extra,desired)||!extra.have_written||!std::isfinite(extra.written)||std::abs(extra.written-desired)>=.0001f)failed=true;}
        }
        report.phase=aircraft::Phase::owned;report.reason=failed||report.valid_mask!=value.valid_mask?aircraft::Reason::binding:aircraft::Reason::none;
        if(failed){release(g,aircraft::Reason::binding);s.pending=true;}
    }
}
float AircraftController::proxy_value(const Proxy& p) noexcept {
    if(!sdk_||!sdk_->on_thread())return 0;
    if(p.active)return (owned_&(1u<<p.group))&&(applied_[p.group].valid_mask&(1u<<p.component))?1.0f:0.0f;
    if((owned_&(1u<<p.group))&&(applied_[p.group].valid_mask&(1u<<p.component)))
        return mapped(p.group,p.component,groups_[p.group].rendered[p.component]);
    float result{};if(!read({p.fallback,p.fallback_index},result))return 0;
    if(p.door)return result<.1f?result*10:result>.9f?(1-result)*10:1.0f;
    return result;
}
float AircraftController::read_proxy(void* context) noexcept {auto* p=static_cast<Proxy*>(context);return p&&p->owner?p->owner->proxy_value(*p):0;}
int AircraftController::read_spill(void* context,float* output,int offset,int count) noexcept {
    auto* p=static_cast<Proxy*>(context);if(!p||!p->owner||!p->owner->sdk_||!p->owner->sdk_->on_thread())return 0;
    if(!output)return 9;if(offset<0||count<0||offset>9)return 0;const auto length=(std::min)(count,9-offset);
    auto& owner=*p->owner;float alpha=(owner.owned_&(1u<<5))&&
        (owner.applied_[5].valid_mask&(1u<<p->component))&&owner.groups_[5].input.sample.values[p->component]>=4?1.0f:0.0f;
    // OBJ lighting accessors receive its nine parameters; modify alpha only, preserving recipe.
    if(offset<=3&&offset+length>3)output[3-offset]*=alpha;
    return length;
}
int AircraftController::read_owned(void* context) noexcept {
    auto* p=static_cast<AircraftController*>(context);return p&&p->sdk_&&p->sdk_->on_thread()?static_cast<int>(p->owned_):0;
}
int AircraftController::read_components(void* context,float* output,int offset,int count) noexcept {
    auto* owner=static_cast<AircraftController*>(context);if(!owner||!owner->sdk_||!owner->sdk_->on_thread())return 0;
    if(!output)return 6;if(offset<0||count<0||offset>6)return 0;const auto length=(std::min)(count,6-offset);
    for(int i=0;i<length;++i){const auto group=static_cast<unsigned>(offset+i);
        output[i]=(owner->owned_&(1u<<group))?static_cast<float>(owner->applied_[group].valid_mask):0;}
    return length;
}
}
