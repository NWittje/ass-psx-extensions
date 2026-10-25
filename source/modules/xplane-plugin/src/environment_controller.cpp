#include "psx_plugin/environment_controller.hpp"
#include "psx_plugin/environment_time.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <bit>

namespace psx::xp {
namespace {
constexpr std::uint32_t presented=env::cloud_fields|env::bit(env::Field::wind_from_deg_true)|env::bit(env::Field::wind_speed_mps)|
    env::bit(env::Field::temperature_c)|env::bit(env::Field::visibility_m)|env::bit(env::Field::precipitation_level);
bool matches(const env::Control& c,const env::Sample& s) noexcept {return c.producer==s.producer&&c.lifecycle==s.lifecycle;}
bool same_native(float a,float b) noexcept {return a==b||std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);}
bool same_weather(float a,float b) noexcept {
    static_assert(std::numeric_limits<float>::is_iec559&&sizeof(float)==sizeof(std::uint32_t));
    if(!std::isfinite(a)||!std::isfinite(b))return false;
    constexpr std::uint32_t sign=0x80000000u,magnitude=0x7fffffffu;
    const auto aa=std::bit_cast<std::uint32_t>(a),bb=std::bit_cast<std::uint32_t>(b);
    const auto am=aa&magnitude,bm=bb&magnitude;
    if(aa==bb||(!am&&!bm))return true;
    if(((aa^bb)&sign)&&am&&bm)return false;
    // Order finite float encodings numerically, merging the two representations of
    // zero. Only adjacent representable values count; no absolute epsilon is used.
    const auto ordered_a=(aa&sign)?sign-am:sign+am,ordered_b=(bb&sign)?sign-bm:sign+bm;
    return (ordered_a>ordered_b?ordered_a-ordered_b:ordered_b-ordered_a)<=2;
}
envp::VisibilityPolicy policy(const EnvironmentSample& input,const env::Control& c) noexcept {
    if(input.presentation_available)return input.visibility;
    envp::VisibilityPolicy p;p.owner=c.owner;p.lifecycle=input.sample.lifecycle;p.source_sequence=input.sample.sequence;
    if(input.sample.flags&env::visibility_unlimited){p.kind=envp::PolicyKind::sentinel_finite;p.visibility_m=9999;}
    return p;
}
double isa(double h) noexcept { // US Standard Atmosphere1976 temperature layers; local MSL/profile approximation.
    if(h<=11000)return 15-.0065*h;if(h<=20000)return -56.5;
    if(h<=32000)return -56.5+.001*(h-20000);if(h<=47000)return -44.5+.0028*(h-32000);
    if(h<=51000)return -2.5;if(h<=71000)return -2.5-.0028*(h-51000);
    return -58.5-.002*(h-71000);
}
bool measured(unsigned field,double v) noexcept {
    if(!std::isfinite(v))return false;
    if(field>=5&&field<=13)return field%3==2?v>=0&&v<=1:v>=-2000&&v<=100000;
    switch(field){case 14:return v>=0&&v<360;case 15:return v>=0&&v<=300;case 16:return v>=-150&&v<=100;
    case 17:return v>=10000&&v<=150000;case 18:return v>=0&&v<=1000000;case 20:return v>0&&v<=150000;case 21:return v>=0&&v<=1;default:return false;}
}
bool clouds_settled(const env::Applied& r) noexcept {
    if(!(r.accepted_mask&env::cloud_fields))return false;
    std::array<unsigned,3> expected{},actual{};unsigned expected_count{},actual_count{};
    for(unsigned layer=0;layer<3;++layer){const auto field=5+layer*3,mask=7u<<field;
        if(!(r.accepted_mask&mask))continue;if((r.accepted_mask&mask)!=mask)return false;
        if(r.accepted[field]==0||r.accepted[field+1]==r.accepted[field+2])continue;
        if(r.accepted[field+1]>r.accepted[field+2])return false;expected[expected_count++]=field;}
    for(unsigned layer=0;layer<3;++layer){const auto field=5+layer*3,mask=7u<<field;
        // Unknown coverage cannot establish absence of an additional active layer.
        // A known clear slot needs no meaningful base/top readback.
        if(!(r.actual_mask&(1u<<field)))return false;if(r.actual[field]==0)continue;
        if((r.actual_mask&mask)!=mask)return false;if(r.actual[field+1]==r.actual[field+2])continue;
        if(r.actual[field+1]>r.actual[field+2])return false;actual[actual_count++]=field;}
    if(expected_count!=actual_count)return false;
    // Aircraft cloud slots carry no PSX Upper/Lower/CB identity. Test the at most
    // six bijections; greedy matching can reuse the only match for another layer.
    std::array<unsigned,3> order{0,1,2};
    do {bool match=true;for(unsigned i=0;i<expected_count&&match;++i){const auto e=expected[i],a=actual[order[i]];
        match=std::abs(r.actual[a]-r.accepted[e])<=.01&&std::abs(r.actual[a+1]-r.accepted[e+1])<=1.0&&std::abs(r.actual[a+2]-r.accepted[e+2])<=1.0;}
        if(match)return true;
    }while(std::next_permutation(order.begin(),order.begin()+actual_count));return false;
}
}
bool EnvironmentController::begin(SdkApi& sdk,motion::InstanceId provider,std::uint64_t frequency,std::int64_t (*clock)()) noexcept {
    if(!sdk.on_thread()||!motion::valid(provider)||!frequency||!clock)return false;
    retire();for(unsigned id=system_time;id<=time;++id)if(saved_[id].mask)return false;
    sdk_=&sdk;provider_=provider;frequency_=frequency;clock_=clock;floor_=clock();generation_=1;connection_=0;
    reports_={};policies_={};claims_={};temperature_target_={};release_reasons_={};owned_=0;last_weather_=last_readback_=held_utc_=0;
    // Deferred native restoration may outlive Disable. Keep only its fixed original/target ledger;
    // unrelated bindings and time can start independently after handles are freshly qualified below.
    for(auto& saved:saved_)if(!saved.mask)saved={};
    failures_={};failure_head_=failure_size_=0;failure_seen_={};failures_lost_=0;
    settling_=false;settling_begin_=settling_end_=0;
    struct Spec {const char* name;XPLMDataTypeID type;unsigned length;bool writable;};
    constexpr Spec specs[]{
        {"sim/time/use_system_time",xplmType_Int,1,true},{"sim/time/zulu_date_days",xplmType_Int,1,true},{"sim/time/zulu_time_sec",xplmType_Float,1,true},
        {"sim/weather/region/change_mode",xplmType_Int,1,true},{"sim/weather/region/update_immediately",xplmType_Int,1,true},
        {"sim/weather/region/cloud_type",xplmType_FloatArray,3,true},{"sim/weather/region/cloud_coverage_percent",xplmType_FloatArray,3,true},
        {"sim/weather/region/cloud_base_msl_m",xplmType_FloatArray,3,true},{"sim/weather/region/cloud_tops_msl_m",xplmType_FloatArray,3,true},
        {"sim/weather/region/wind_speed_msc",xplmType_FloatArray,13,true},{"sim/weather/region/wind_direction_degt",xplmType_FloatArray,13,true},
        {"sim/weather/region/temperatures_aloft_deg_c",xplmType_FloatArray,13,true},{"sim/weather/region/sealevel_temperature_c",xplmType_Float,1,true},
        {"sim/weather/region/qnh_pas",xplmType_Float,1,true},{"sim/weather/region/visibility_reported_sm",xplmType_Float,1,true},
        {"sim/weather/region/rain_percent",xplmType_Float,1,true},{"sim/weather/region/atmosphere_alt_levels_m",xplmType_FloatArray,13,false},
        {"sim/flightmodel/position/latitude",xplmType_Double,1,false},{"sim/flightmodel/position/longitude",xplmType_Double,1,false},
        {"sim/flightmodel/position/elevation",xplmType_Double,1,false},{"sim/weather/aircraft/wind_now_speed_msc",xplmType_Float,1,false},
        {"sim/weather/aircraft/wind_now_direction_degt",xplmType_Float,1,false},{"sim/weather/aircraft/temperature_ambient_deg_c",xplmType_Float,1,false},
        {"sim/weather/aircraft/qnh_pas",xplmType_Float,1,false},{"sim/weather/aircraft/visibility_reported_sm",xplmType_Float,1,false},
        {"sim/weather/aircraft/cloud_coverage_percent",xplmType_FloatArray,3,false},{"sim/weather/aircraft/cloud_base_msl_m",xplmType_FloatArray,3,false},
        {"sim/weather/aircraft/cloud_tops_msl_m",xplmType_FloatArray,3,false},{"sim/weather/aircraft/barometer_current_pas",xplmType_Float,1,false},
        {"sim/flightmodel2/position/pressure_altitude",xplmType_Double,1,false}};
    static_assert(std::size(specs)==count);
    for(unsigned i=0;i<count;++i){const auto& s=specs[i];auto ref=XPLMFindDataRef(s.name);auto& b=bindings_[i];
        b={};b.type=s.type;b.length=s.length;b.writable=s.writable;b.status=sdk.inspect(ref,s.type,s.writable);
        const bool inspectable=b.status==Status::ok||b.status==Status::type_mismatch||b.status==Status::read_only;
        b.actual_type=ref&&inspectable?static_cast<unsigned>(XPLMGetDataRefTypes(ref)):0;
        const auto length=ref&&s.type==xplmType_FloatArray&&(b.actual_type&xplmType_FloatArray)?XPLMGetDatavf(ref,nullptr,0,0):0;
        b.actual_length=s.type==xplmType_FloatArray?static_cast<unsigned>((std::max)(0,length)):ref?1u:0u;
        if((b.status==Status::ok||b.status==Status::read_only)&&b.actual_length!=s.length)b.status=Status::short_read;
        // A typed weather handle can become writable after re-enable. Keep it for
        // pending-original recovery; every actual submission rechecks writability.
        if(b.status==Status::ok||(i>=cloud_type&&i<=rain&&b.status==Status::read_only))b.ref=ref;
        if(b.status!=Status::ok)fail(static_cast<Id>(i),0,FailureStage::binding,b.status);}
    weather_getter_=reinterpret_cast<WeatherGetter>(XPLMFindSymbol("XPLMGetWeatherAtLocation"));
    return true; // Missing optional field bindings never gate time or other services.
}
void EnvironmentController::fail(Id id,unsigned index,FailureStage stage,Status status,float requested,float actual) noexcept {
    static_assert(static_cast<unsigned>(FailureStage::owner_mismatch)<16);
    if(index>=13)return;const auto bit=static_cast<std::uint16_t>(1u<<static_cast<unsigned>(stage));
    auto& seen=failure_seen_[id][index];if(seen&bit)return;seen|=bit;
    if(failure_size_==failures_.size()){++failures_lost_;return;}
    const auto& b=bindings_[id];auto& f=failures_[(failure_head_+failure_size_++)%failures_.size()];
    f={static_cast<unsigned>(id),index,stage,status,std::bit_cast<std::uint32_t>(requested),std::bit_cast<std::uint32_t>(actual),
       static_cast<unsigned>(b.type),b.actual_type,b.length,b.actual_length,
       claims_[id<=time?0:1].input.sample.sequence,connection_};
}
bool EnvironmentController::failure(Failure& f) noexcept {
    if(!sdk_||!sdk_->on_thread()||!failure_size_)return false;
    f=failures_[failure_head_];failure_head_=(failure_head_+1)%failures_.size();--failure_size_;return true;
}
void EnvironmentController::retire() noexcept {
    if(!sdk_||!sdk_->on_thread())return;for(unsigned g=0;g<2;++g)release(g,env::Reason::stopped);
    if(restoration_pending())return; // Preserve unresolved originals across Disable/re-enable retries.
    sdk_=nullptr;bindings_={};saved_={};weather_getter_=nullptr;
}
bool EnvironmentController::restoration_pending() const noexcept {for(const auto& saved:saved_)if(saved.mask)return true;return false;}
bool EnvironmentController::read(Id id,unsigned index,float& value,bool raw,Status* outcome) noexcept {
    const auto& b=bindings_[id];const auto status=b.ref?sdk_->inspect(b.ref,b.type,false):b.status;
    if(outcome)*outcome=status;
    if(!b.ref||index>=b.length||status!=Status::ok){fail(id,index,FailureStage::read,status);return false;}
    if(b.type==xplmType_Int)value=static_cast<float>(XPLMGetDatai(b.ref));
    else if(b.type==xplmType_Float)value=XPLMGetDataf(b.ref);
    else if(b.type==xplmType_Double)value=static_cast<float>(XPLMGetDatad(b.ref));
    else {const auto received=XPLMGetDatavf(b.ref,&value,static_cast<int>(index),1);
        if(received!=1){if(outcome)*outcome=Status::short_read;fail(id,index,FailureStage::read,Status::short_read,1,static_cast<float>(received));return false;}}
    if(!std::isfinite(value)){if(outcome)*outcome=Status::non_finite;fail(id,index,FailureStage::read,Status::non_finite,0,value);return raw;}return true;
}
bool EnvironmentController::save(Id id,unsigned index) noexcept {
    auto& s=saved_[id];if(s.mask&(1u<<index))return true;
    if(!read(id,index,s.values[index]))return false;s.mask|=static_cast<std::uint16_t>(1u<<index);return true;
}
bool EnvironmentController::write(Id id,unsigned index,float value,bool force) noexcept {
    const auto& b=bindings_[id];if(!std::isfinite(value)){fail(id,index,FailureStage::baseline,Status::non_finite,value);return false;}
    if(!b.writable||!save(id,index)){fail(id,index,FailureStage::baseline,b.ref?Status::unavailable:b.status,value);return false;}
    const auto status=sdk_->inspect(b.ref,b.type,true);if(status!=Status::ok){fail(id,index,FailureStage::baseline,status,value);return false;}
    // A qualified exact readback needs no setter, including on a new target or
    // Follow. Keep its baseline for competing-writer/restoration checks.
    if(!force){float current{};if(read(id,index,current)&&same_native(current,value)){
        saved_[id].written[index]=current;saved_[id].written_mask|=static_cast<std::uint16_t>(1u<<index);return true;}}
    saved_[id].uncertain_mask|=static_cast<std::uint16_t>(1u<<index);
    TimingScope timing(*this,id==system_time?EnvironmentStage::system_time:id==date?EnvironmentStage::date:id==time?EnvironmentStage::clock:id<=immediate?EnvironmentStage::settings:EnvironmentStage::regional);
    if(b.type==xplmType_Int)XPLMSetDatai(b.ref,static_cast<int>(value));else if(b.type==xplmType_Float)XPLMSetDataf(b.ref,value);
    else XPLMSetDatavf(b.ref,&value,static_cast<int>(index),1);
    float actual{};const auto readable=read(id,index,actual,true);
    const auto ok=readable&&(id>=cloud_type&&id<=rain?same_weather(actual,value):std::isfinite(actual)&&std::abs(actual-value)<.001f);
    if(!ok)fail(id,index,readable?FailureStage::setter_mismatch:FailureStage::setter_read,readable?Status::out_of_range:Status::unavailable,value,actual);
    // Ownership evidence is the observed result of our setter, including rejected/NaN results;
    // intended-value acceptance is a separate test. Never compare a new own write to its predecessor.
    if(readable){saved_[id].written[index]=actual;saved_[id].written_mask|=static_cast<std::uint16_t>(1u<<index);
        saved_[id].uncertain_mask&=static_cast<std::uint16_t>(~(1u<<index));}return ok;
}
bool EnvironmentController::write_array(Id id,const std::array<float,13>& values,bool* submitted,std::uint16_t force) noexcept {
    if(submitted)*submitted=false;
    const auto& b=bindings_[id];const auto status=b.ref?sdk_->inspect(b.ref,b.type,true):b.status;
    if(!b.ref||b.type!=xplmType_FloatArray||b.length!=13||!b.writable||status!=Status::ok){fail(id,0,FailureStage::array_before,status);return false;}
    std::array<float,13> current{};auto received=XPLMGetDatavf(b.ref,current.data(),0,13);
    if(received!=13){fail(id,0,FailureStage::array_before,Status::short_read,13,static_cast<float>(received));return false;}
    for(unsigned i=0;i<13;++i){if(!std::isfinite(current[i])||!std::isfinite(values[i])){fail(id,i,FailureStage::array_before,Status::non_finite,values[i],current[i]);return false;}
        if(!(saved_[id].mask&(1u<<i)))saved_[id].values[i]=current[i];}
    saved_[id].mask=0x1fff;
    // Submit only contiguous changed indices. Thirteen comparisons/runs is an
    // explicit bound; already matching bands never trigger a native setter.
    for(unsigned first=0;first<13;){if(!(force&(1u<<first))&&same_native(current[first],values[first])){++first;continue;}unsigned end=first+1;
        while(end<13&&((force&(1u<<end))||!same_native(current[end],values[end])))++end;
        TimingScope timing(*this,EnvironmentStage::regional);
        for(unsigned i=first;i<end;++i)saved_[id].uncertain_mask|=static_cast<std::uint16_t>(1u<<i);
        XPLMSetDatavf(b.ref,const_cast<float*>(values.data()+first),static_cast<int>(first),static_cast<int>(end-first));first=end;
    }
    // Also acknowledge a qualified already-matching intent to the fixed target
    // ledger; this flag is not an SDK-setter count (Capture times those calls).
    if(submitted)*submitted=true;
    received=XPLMGetDatavf(b.ref,current.data(),0,13);
    for(int i=0;i<(std::min)(13,(std::max)(0,received));++i){saved_[id].written[i]=current[i];saved_[id].written_mask|=static_cast<std::uint16_t>(1u<<i);
        saved_[id].uncertain_mask&=static_cast<std::uint16_t>(~(1u<<i));}
    if(received!=13){fail(id,0,FailureStage::array_after,Status::short_read,13,static_cast<float>(received));return false;}
    for(unsigned i=0;i<13;++i)if(!same_weather(current[i],values[i])){
        fail(id,i,FailureStage::array_mismatch,std::isfinite(current[i])?Status::out_of_range:Status::non_finite,values[i],current[i]);return false;}
    saved_[id].written=current;saved_[id].written_mask=0x1fff;return true;
}
bool EnvironmentController::confirm(Id id,unsigned index,float actual) noexcept {
    auto& s=saved_[id];const auto bit=static_cast<std::uint16_t>(1u<<index);
    // Native weather arithmetic can round a target by one float step. Mapping
    // tolerance is much wider and cannot acknowledge a queued native setter.
    if(!(s.target_mask&bit)||!same_weather(actual,s.target[index]))return false;
    if((s.restoring_mask&bit)&&callback_<=s.submitted_callback[index])return false;
    // Other setters in this callback can couple native weather fields. Retain pending
    // ownership until a later callback confirms the complete submitted batch.
    if(callback_>s.submitted_callback[index]){s.pending_mask&=static_cast<std::uint16_t>(~bit);s.delayed_mask&=static_cast<std::uint16_t>(~bit);}
    s.written[index]=actual;s.written_mask|=bit;s.uncertain_mask&=static_cast<std::uint16_t>(~bit);return true;
}
bool EnvironmentController::submit(Id id,unsigned index,float value) noexcept {
    auto& s=saved_[id];const auto bit=static_cast<std::uint16_t>(1u<<index);
    if(s.restoring_mask&bit)return false;
    if((s.target_mask&bit)&&same_native(s.target[index],value)){float actual{};return read(id,index,actual,true)&&confirm(id,index,actual);}
    const auto& b=bindings_[id];if(!b.ref||!b.writable||sdk_->inspect(b.ref,b.type,true)!=Status::ok||!save(id,index))return false;
    const bool superseded=(s.pending_mask&bit)!=0;
    s.target[index]=value;s.target_mask|=bit;s.pending_mask|=bit;s.submitted_callback[index]=callback_;
    const bool immediate=write(id,index,value,superseded)&&same_weather(s.written[index],value);
    if(immediate)s.delayed_mask&=static_cast<std::uint16_t>(~bit);else s.delayed_mask|=bit;
    return immediate;
}
bool EnvironmentController::submit_array(Id id,const std::array<float,13>& values) noexcept {
    auto& s=saved_[id];if(s.restoring_mask)return false;bool same=s.target_mask==0x1fff;
    for(unsigned i=0;i<13&&same;++i)same=same_native(s.target[i],values[i]);
    if(same){const auto& b=bindings_[id];std::array<float,13> actual{};
        if(!b.ref||sdk_->inspect(b.ref,b.type,false)!=Status::ok||XPLMGetDatavf(b.ref,actual.data(),0,13)!=13)return false;
        bool complete=true;for(unsigned i=0;i<13;++i)complete=confirm(id,i,actual[i])&&complete;return complete;}
    const auto& b=bindings_[id];if(!b.ref||!b.writable||b.length!=13||sdk_->inspect(b.ref,b.type,true)!=Status::ok)return false;
    std::uint16_t superseded=s.pending_mask;
    for(unsigned i=0;i<13;++i)if(!(s.target_mask&(1u<<i))||same_native(s.target[i],values[i]))superseded&=static_cast<std::uint16_t>(~(1u<<i));
    bool submitted=false;bool immediate=write_array(id,values,&submitted,superseded);
    for(unsigned i=0;i<13&&immediate;++i)immediate=same_weather(s.written[i],values[i]);
    if(submitted){s.target=values;s.target_mask=s.pending_mask=0x1fff;s.delayed_mask=immediate?0:0x1fff;for(auto& stamp:s.submitted_callback)stamp=callback_;}
    return immediate;
}
bool EnvironmentController::restore(Id id,unsigned index) noexcept {
    auto& s=saved_[id];if(!(s.mask&(1u<<index)))return true;const auto previous=s.values[index];
    TimingScope timing(*this,EnvironmentStage::restoration,id<=time?1u:2u);
    const auto bit=static_cast<std::uint16_t>(1u<<index);
    float current{};const bool observed=read(id,index,current,true);
    if(id>=cloud_type&&id<=rain){
        if((s.restoring_mask&bit)&&s.submitted_callback[index]){if(!observed||!confirm(id,index,current))return false;}
        else {
            const bool pending=(s.pending_mask&bit)!=0;
            const bool qualified_target=(s.target_mask&bit)&&!(s.restoring_mask&bit);
            const bool foreign=!pending&&(s.written_mask&bit)&&observed&&!same_weather(current,qualified_target?s.target[index]:s.written[index]);
            if(foreign){s.mask&=static_cast<std::uint16_t>(~bit);s.written_mask&=static_cast<std::uint16_t>(~bit);
                s.target_mask&=static_cast<std::uint16_t>(~bit);s.pending_mask&=static_cast<std::uint16_t>(~bit);s.delayed_mask&=static_cast<std::uint16_t>(~bit);s.uncertain_mask&=static_cast<std::uint16_t>(~bit);return false;}
            if(!pending&&observed&&same_native(current,previous)){}
            else {const bool delayed=(s.delayed_mask&bit)!=0;s.target[index]=previous;s.target_mask|=bit;s.pending_mask|=bit;s.restoring_mask|=bit;s.submitted_callback[index]=0;
                const auto& b=bindings_[id];if(!b.ref||sdk_->inspect(b.ref,b.type,true)!=Status::ok)return false;
                s.submitted_callback[index]=callback_+1;
                // Submit the original once even when a queued target still reads as the original.
                // Otherwise that delayed target could arrive after Stop and escape restoration.
                const bool immediate=write(id,index,previous,true)&&same_weather(s.written[index],previous);
                if(delayed||!immediate)return false;}
        }
        s.mask&=static_cast<std::uint16_t>(~bit);s.written_mask&=static_cast<std::uint16_t>(~bit);s.uncertain_mask&=static_cast<std::uint16_t>(~bit);
        s.target_mask&=static_cast<std::uint16_t>(~bit);s.pending_mask&=static_cast<std::uint16_t>(~bit);s.delayed_mask&=static_cast<std::uint16_t>(~bit);s.restoring_mask&=static_cast<std::uint16_t>(~bit);return true;
    }
    // An unobserved setter result is unresolved, not permission to overwrite a later unknown edit.
    if(s.uncertain_mask&(1u<<index)){if(!observed||!same_native(current,previous))return false;}
    const bool foreign=id!=time&&id!=date&&(s.written_mask&(1u<<index))&&observed&&!same_native(current,s.written[index]);
    const auto ok=observed&&same_native(current,previous)?true:!foreign&&write(id,index,previous);
    if(ok||foreign){s.mask&=static_cast<std::uint16_t>(~(1u<<index));s.written_mask&=static_cast<std::uint16_t>(~(1u<<index));
        s.uncertain_mask&=static_cast<std::uint16_t>(~(1u<<index));}return ok;
}
env::Reason EnvironmentController::ownership_issue(unsigned group) noexcept {
    const unsigned first=group==0?system_time:change,last=group==0?system_time:rain;
    for(unsigned id=first;id<=last;++id)for(unsigned i=0;i<bindings_[id].length;++i)if((saved_[id].written_mask&(1u<<i))&&!(saved_[id].pending_mask&(1u<<i))){
        float current{};Status status{};if(!read(static_cast<Id>(id),i,current,false,&status)){
            fail(static_cast<Id>(id),i,FailureStage::owner_read,status,saved_[id].written[i],current);return env::Reason::binding;}
        const auto& saved=saved_[id];const auto bit=static_cast<std::uint16_t>(1u<<i);
        const bool qualified_target=(saved.target_mask&bit)&&!(saved.restoring_mask&bit);
        const bool weather=id>=cloud_type&&id<=rain;const auto expected=weather&&qualified_target?saved.target[i]:saved.written[i];
        const bool same=weather?same_weather(current,expected):same_native(current,expected);
        if(!same){fail(static_cast<Id>(id),i,FailureStage::owner_mismatch,Status::out_of_range,expected,current);return env::Reason::owner;}
    }return env::Reason::none;
}
void EnvironmentController::retry_restoration() noexcept {
    for(unsigned id=cloud_type;id<=rain;++id)for(unsigned i=0;i<13;++i)if(saved_[id].restoring_mask&(1u<<i))restore(static_cast<Id>(id),i);
    bool pending=false;for(unsigned id=change;id<=rain;++id)pending=pending||saved_[id].mask;
    if(!pending&&reports_[1].phase==env::Phase::released&&reports_[1].reason==env::Reason::binding)reports_[1].reason=release_reasons_[1];
}
void EnvironmentController::release(unsigned g,env::Reason reason) noexcept {
    if(!sdk_||!sdk_->on_thread())return;bool restored=true;
    TimingScope timing(*this,EnvironmentStage::lease,1u<<g);
    release_reasons_[g]=reason;
    // Restore settings last, after data: system-time ownership / change-mode resume afterward.
    const unsigned first=g==0?date:cloud_type,last=g==0?time:rain;
    for(unsigned id=first;id<=last;++id)for(unsigned i=0;i<13;++i)if(saved_[id].mask&(1u<<i))restored=restore(static_cast<Id>(id),i)&&restored;
    if(g==0)restored=restore(system_time,0)&&restored;
    else {restored=restore(immediate,0)&&restored;restored=restore(change,0)&&restored;settling_=false;prepared_weather_target_=0;}
    owned_&=~(1u<<g);claims_[g].pending=claims_[g].have_sample=false;
    auto& r=reports_[g];r.groups=1u<<g;r.phase=env::Phase::released;r.reason=restored?reason:env::Reason::binding;
    r.accepted_mask=r.actual_mask=r.approximation_mask=r.location_mask=0;r.accepted={};r.actual={};r.location={};
    r.location_begin_qpc=r.location_end_qpc=0;r.flags=0;r.applied_qpc=clock_();r.readback_qpc=0;
    if(g==0){r.accepted_utc_millis=r.actual_utc_millis=0;held_utc_=0;}
    if(reason==env::Reason::owner)r.reason=reason;
}
void EnvironmentController::connection(std::uint64_t value) noexcept {
    if(!sdk_||!sdk_->on_thread()||connection_==value)return;for(unsigned g=0;g<2;++g)release(g,env::Reason::disconnected);
    // The worker can receive valid controls before the first SDK callback sees this connection.
    // Connection tags retire old traffic; callback-observation time is not the worker's admission floor.
    connection_=value;floor_=0;failure_seen_={};
}
void EnvironmentController::boundary() noexcept {if(sdk_&&sdk_->on_thread()){
    ++generation_;prepared_weather_target_=0;
    // An actual destination replacement requires one reapplication, not a cloud reset.
    for(unsigned id=cloud_type;id<=rain;++id){saved_[id].target_mask=saved_[id].pending_mask=saved_[id].delayed_mask=0;}
}}
env::Ack EnvironmentController::command(const EnvironmentCommand& input) noexcept {
    env::Ack ack;ack.control=input.control;const auto& c=input.control;
    if(!sdk_||!sdk_->on_thread()){ack.reason=env::Reason::binding;return ack;}
    if(input.connection!=connection_){ack.reason=env::Reason::identity;return ack;}
    const auto now=clock_();if(input.received_qpc<=0||input.received_qpc<floor_||input.received_qpc>now){ack.reason=env::Reason::stale;return ack;}
    for(unsigned g=0;g<2;++g)if(c.groups&(1u<<g)){
        auto& s=claims_[g];if(s.command.control.producer==c.producer&&c.lifecycle<s.command.control.lifecycle){ack.reason=env::Reason::lifecycle;continue;}
        if(c.action==env::Action::acquire&&s.pending&&now<s.expires&&s.command.control.owner!=c.owner){ack.reason=env::Reason::owner;continue;}
        if(s.command.connection==input.connection&&c.operation<=s.command.control.operation&&!input.source_loss){ack.reason=env::Reason::owner;continue;}
        const bool same=s.command.control.producer==c.producer&&s.command.control.owner==c.owner&&
            s.command.control.lifecycle==c.lifecycle&&c.configuration>=s.command.control.configuration;
        if(c.action==env::Action::release){if(s.pending&&!same){ack.reason=env::Reason::owner;continue;}
            release(g,input.source_loss?env::Reason::stale:env::Reason::stopped);s.command=input;ack.accepted_groups|=1u<<g;continue;}
        if(g==0&&(!bindings_[system_time].ref||!bindings_[date].ref||!bindings_[time].ref)){ack.reason=env::Reason::binding;continue;}
        if(g==1&&(!bindings_[change].ref||!bindings_[immediate].ref)){ack.reason=env::Reason::binding;continue;}
        if(c.action==env::Action::renew&&(!s.pending||!same||now>=s.expires)){ack.reason=same&&now>=s.expires?env::Reason::expired:env::Reason::owner;continue;}
        if(c.action==env::Action::acquire){
            release(g,env::Reason::none);
            const bool restoration_failed=reports_[g].reason==env::Reason::binding;
            // Retirement may retain the old source/owner for a legitimate Stop
            // report. It must not become a freshly timed result of this Acquire.
            reports_[g]={};policies_[g]={};
            if(restoration_failed&&(g==0||saved_[change].mask||saved_[immediate].mask)){ack.reason=env::Reason::binding;continue;}
            s.command=input;s.pending=true;
        }
        else s.command.control=c;
        s.expires=input.received_qpc+static_cast<std::int64_t>(frequency_*c.duration_ms/1000);ack.accepted_groups|=1u<<g;
    }
    ack.phase=c.action==env::Action::release?env::Phase::released:env::Phase::observing;return ack;
}
void EnvironmentController::sample(const EnvironmentSample& input) noexcept {
    if(!sdk_||!sdk_->on_thread()||!env::valid(input.sample)||input.sample.received_qpc<=0||input.sample.received_qpc<floor_||
        input.sample.received_qpc>clock_()||input.sample.sample_end_qpc>clock_())return;
    if(input.presentation_available&&(!input.presentation_revision||!envp::valid(envp::Presentation{{input.sample,input.context,{},input.local_visibility},input.visibility})||
        input.context.observation_end_qpc>clock_()))return;
    for(unsigned g=0;g<2;++g){auto& s=claims_[g];
        const bool ordered=!s.have_sample||((!input.presentation_available||(s.input.presentation_available&&
            input.presentation_revision>s.input.presentation_revision&&input.visibility.selection_revision>=s.input.visibility.selection_revision))&&input.sample.configuration>=s.input.sample.configuration&&
            (input.sample.sequence>s.input.sample.sequence||
             (input.presentation_available&&s.input.presentation_available&&input.sample.sequence==s.input.sample.sequence&&
              input.presentation_revision>s.input.presentation_revision&&envp::same_sample(input.sample,s.input.sample)&&input.context==s.input.context)));
        if(s.pending&&s.command.connection==input.connection&&s.command.revision==input.revisions[g]&&matches(s.command.control,input.sample)&&ordered&&
           (!input.presentation_available||input.visibility.owner==s.command.control.owner)){
            if(g==1&&(!input.weather.identity||!envp::valid(envp::Presentation{{input.sample,input.context,{0,0,0,1,0},input.local_visibility},policy(input,s.command.control),input.weather}))) {release(1,env::Reason::unsupported);continue;}
            if(g==1&&s.have_sample&&input.weather.identity<s.input.weather.identity)continue;
            s.input=input;s.have_sample=true;}}
}
void EnvironmentController::apply_time(std::int64_t now,bool fresh) noexcept {
    auto& c=claims_[0];auto& r=reports_[0];const auto s=fresh?c.input.sample:r.source;
    if(!fresh&&!(owned_&1))return;
    r.source=s;r.provider=provider_;r.owner=c.command.control.owner;r.provider_generation=generation_;r.groups=1;
    if(fresh)policies_[0]=policy(c.input,c.command.control);
    if(!(s.flags&env::utc_valid)){release(0,env::Reason::unsupported);return;}
    auto utc=fresh?s.utc_millis:held_utc_;
    if(!environment_time::valid(utc)){release(0,env::Reason::unsupported);return;}
    if(fresh&&(s.valid_mask&1)&&!(s.flags&env::paused)&&
       !environment_time::project(utc,now-s.sample_end_qpc,frequency_,s.values[0],utc)){
        release(0,env::Reason::unsupported);return;}
    environment_time::Date d;
    if(!environment_time::split(utc,d)){release(0,env::Reason::unsupported);return;}
    if(!write(system_time,0,0)||!write(date,0,static_cast<float>(d.ordinal))||!write(time,0,d.seconds)){release(0,env::Reason::binding);return;}
    const auto applied=clock_();
    float system{},day{},seconds{};std::int64_t actual{};
    if(!read(system_time,0,system)||system!=0||!read(date,0,day)||!read(time,0,seconds)||
       !environment_time::join(d.year,day,seconds,actual)||day!=static_cast<float>(d.ordinal)||
       std::abs(actual-utc)>1000){release(0,env::Reason::binding);return;}
    const auto readback=clock_();
    if(readback>=c.expires){release(0,env::Reason::expired);return;}
    const bool current=fresh&&readback>=s.sample_end_qpc&&readback-s.sample_end_qpc<static_cast<std::int64_t>(frequency_);
    held_utc_=utc;owned_|=1;r={};r.source=s;r.provider=provider_;r.owner=c.command.control.owner;r.provider_generation=generation_;
    r.groups=1;r.phase=env::Phase::owned;r.reason=current?env::Reason::none:env::Reason::stale;r.flags=env::calendar_year_approximate;
    r.applied_qpc=applied;r.readback_qpc=readback;r.accepted_utc_millis=utc;
    if(s.valid_mask&1){r.accepted_mask=1;r.accepted[0]=current&&!(s.flags&env::paused)?s.values[0]:0;}
    r.actual_utc_millis=actual;
}
void EnvironmentController::apply_weather(std::int64_t now) noexcept {
    // Reserve this cadence even for a failed attempt. Stop/Acquire must not reset it.
    last_weather_=now;
    reported_weather_target_=claims_[1].input.weather.identity;
    const auto& s=claims_[1].input.weather.source.sample;auto& r=reports_[1];const auto previous=r.accepted;const auto previous_mask=r.accepted_mask;
    r={};r.source=s;r.provider=provider_;r.owner=claims_[1].command.control.owner;
    policies_[1]=claims_[1].input.weather.visibility;const auto& visibility_policy=policies_[1];
    r.provider_generation=generation_;r.groups=2;r.applied_qpc=now;r.phase=env::Phase::owned;
    if(!write(change,0,3)||!write(immediate,0,1)){release(1,env::Reason::binding);last_weather_=clock_();return;}
    bool failed=false;
    for(unsigned layer=0;layer<3;++layer){const auto field=5+layer*3;const auto mask=7u<<field;
        if((s.valid_mask&mask)!=mask||s.values[field+1]<0||s.values[field+2]<0){
            for(auto id:{cloud_type,coverage,base,top})failed=!restore(id,layer)||failed;continue;}
        const auto disabled=s.values[field]==0||s.values[field+1]==s.values[field+2];
        const auto cover=disabled?0.0f:static_cast<float>(s.values[field]);
        const bool type_ok=submit(cloud_type,layer,layer==2?3.0f:1.0f),coverage_ok=submit(coverage,layer,cover);
        const bool base_ok=submit(base,layer,static_cast<float>(s.values[field+1])),top_ok=submit(top,layer,static_cast<float>(s.values[field+2]));
        if(type_ok&&coverage_ok&&base_ok&&top_ok){
            r.accepted_mask|=mask;r.accepted[field]=cover;r.accepted[field+1]=static_cast<float>(s.values[field+1]);r.accepted[field+2]=static_cast<float>(s.values[field+2]);r.approximation_mask|=mask;r.flags|=env::opacity_approximate;
        }
    }
    for(auto pair:{std::pair{env::Field::wind_from_deg_true,wind_direction},std::pair{env::Field::wind_speed_mps,wind_speed}}){
        const auto field=static_cast<unsigned>(pair.first);if(s.valid_mask&(1u<<field)){std::array<float,13> values;values.fill(static_cast<float>(s.values[field]));
            if(field==14&&values[0]>=360)values.fill(0);
            if(submit_array(pair.second,values)){r.accepted_mask|=1u<<field;r.accepted[field]=values[0];r.approximation_mask|=1u<<field;r.flags|=env::local_profile_approximate;}}
        else for(unsigned i=0;i<13;++i)failed=!restore(pair.second,i)||failed;
    }
    const auto temp=env::bit(env::Field::temperature_c);
    // Retain the destination pressure-grid mapping for this immutable target.
    // Reconnection/reconciliation must not re-anchor it to a moving aircraft.
    const auto target=claims_[1].input.weather.identity,owner=claims_[1].command.control.owner;
    const bool cached=temperature_target_.identity==target&&temperature_target_.owner==owner&&temperature_target_.producer==s.producer&&temperature_target_.lifecycle==s.lifecycle&&temperature_target_.sequence==s.sequence;
    auto profile=temperature_target_.profile;auto pressure_m=temperature_target_.pressure_m;auto sea=temperature_target_.sea;bool temp_ok=temperature_target_.valid;
    if(!cached){pressure_m=0;profile={};sea=0;
    const auto& anchor=bindings_[pressure_altitude];
    temp_ok=(s.valid_mask&temp)&&anchor.ref&&sdk_->inspect(anchor.ref,xplmType_Double,false)==Status::ok;
    if((s.valid_mask&temp)&&!temp_ok)fail(pressure_altitude,0,FailureStage::pressure_anchor,anchor.ref?Status::unavailable:anchor.status);
    if(temp_ok){pressure_m=XPLMGetDatad(anchor.ref)*.3048;temp_ok=std::isfinite(pressure_m)&&pressure_m>=-2000&&pressure_m<=84852;
        if(!temp_ok)fail(pressure_altitude,0,FailureStage::pressure_anchor,std::isfinite(pressure_m)?Status::out_of_range:Status::non_finite,0,static_cast<float>(pressure_m));}
    std::array<float,13> heights{};const auto delta=s.values[16]-isa(pressure_m);sea=static_cast<float>(15+delta);
    if(temp_ok){const auto& b=bindings_[levels];const auto status=b.ref?sdk_->inspect(b.ref,xplmType_FloatArray,false):b.status;
        const auto received=b.ref&&status==Status::ok?XPLMGetDatavf(b.ref,heights.data(),0,13):0;temp_ok=b.ref&&status==Status::ok&&received==13;
        if(!temp_ok)fail(levels,0,FailureStage::array_before,status==Status::ok?Status::short_read:status,13,static_cast<float>(received));
        for(unsigned i=0;i<13&&temp_ok;++i){temp_ok=std::isfinite(heights[i])&&heights[i]>=0&&heights[i]<=84852;
            if(!temp_ok)fail(levels,i,FailureStage::profile_height,std::isfinite(heights[i])?Status::out_of_range:Status::non_finite,0,heights[i]);
            else if(i&&heights[i]<=heights[i-1]){temp_ok=false;fail(levels,i,FailureStage::profile_order,Status::out_of_range,heights[i-1],heights[i]);}
            profile[i]=static_cast<float>(isa(heights[i])+delta);}}
      temperature_target_={s.producer,owner,target,s.lifecycle,s.sequence,profile,pressure_m,sea,temp_ok};
    }
    prepared_pressure_m_=pressure_m;
    const bool profile_ok=temp_ok&&submit_array(temperatures,profile),sea_ok=temp_ok&&submit(sea_temperature,0,sea);
    if(profile_ok&&sea_ok){
        r.accepted_mask|=temp|env::bit(env::Field::pressure_altitude_m);r.accepted[16]=s.values[16];r.accepted[23]=pressure_m;
        r.approximation_mask|=temp;r.flags|=env::local_profile_approximate;}
    else if(!temp_ok){for(unsigned i=0;i<13;++i)failed=!restore(temperatures,i)||failed;failed=!restore(sea_temperature,0)||failed;}
    // A source cloud-height datum does not qualify its QNH pressure reference. The loaded
    // public-dataref probe found a stable destination offset with native qnh_base_elevation.
    // Keep source/aircraft QNH evidence; do not write or claim an unqualified pressure pair.
    if(s.valid_mask&env::bit(env::Field::qnh_pa)){float native{};read(qnh,0,native);fail(qnh,0,FailureStage::qnh_reference,Status::unavailable,static_cast<float>(s.values[17]),native);}
    for(auto pair:{std::pair{env::Field::visibility_m,visibility},std::pair{env::Field::precipitation_level,rain}}){
        const auto f=static_cast<unsigned>(pair.first);const bool selected=f==18&&visibility_policy.kind!=envp::PolicyKind::native;
        // The native sentinel remains in r.source; only the destination policy
        // gives it a finite ceiling. Never invent a native finite observation.
        if((s.valid_mask&(1u<<f))||selected){const auto wanted=f==18?(selected?visibility_policy.visibility_m:s.values[f])/1609.344:f==19?s.values[f]/3:s.values[f];
            if(submit(pair.second,0,static_cast<float>(wanted))){r.accepted_mask|=1u<<f;r.accepted[f]=f==19?s.values[f]:static_cast<float>(wanted)*(f==18?1609.344:1.0);
                if(f==19){r.approximation_mask|=1u<<f;float ratio{};if(read(rain,0,ratio)){r.accepted_mask|=env::bit(env::Field::precipitation_ratio);r.accepted[21]=ratio;r.approximation_mask|=env::bit(env::Field::precipitation_ratio);}}}
            else if(!(saved_[pair.second].pending_mask&1))failed=selected||failed;
        }else failed=!restore(pair.second,0)||failed;
    }
    // Weather setters can update companion fields. Observe the completed submission batch
    // before treating any changed getter as foreign on the next callback.
    for(unsigned id=cloud_type;id<=rain;++id)for(unsigned i=0;i<bindings_[id].length;++i){auto& saved=saved_[id];const auto bit=static_cast<std::uint16_t>(1u<<i);
        if(!(saved.target_mask&bit)||(saved.restoring_mask&bit))continue;
        float actual{};if(read(static_cast<Id>(id),i,actual,true)){saved.written[i]=actual;saved.written_mask|=bit;saved.uncertain_mask&=static_cast<std::uint16_t>(~bit);
            if(same_weather(actual,saved.target[i])){saved.pending_mask&=static_cast<std::uint16_t>(~bit);saved.delayed_mask&=static_cast<std::uint16_t>(~bit);}
            else {saved.pending_mask|=bit;saved.delayed_mask|=bit;}}}
    std::uint32_t pending_fields{};
    for(unsigned l=0;l<3;++l)for(auto id:{cloud_type,coverage,base,top})if(saved_[id].pending_mask&(1u<<l))pending_fields|=7u<<(5+l*3);
    if(saved_[wind_direction].pending_mask)pending_fields|=env::bit(env::Field::wind_from_deg_true);
    if(saved_[wind_speed].pending_mask)pending_fields|=env::bit(env::Field::wind_speed_mps);
    if(saved_[temperatures].pending_mask||saved_[sea_temperature].pending_mask)pending_fields|=temp;
    if(saved_[visibility].pending_mask)pending_fields|=env::bit(env::Field::visibility_m);
    if(saved_[rain].pending_mask)pending_fields|=env::bit(env::Field::precipitation_level);
    r.accepted_mask&=~pending_fields;r.approximation_mask&=r.accepted_mask;
    if(!(r.accepted_mask&temp)){r.accepted_mask&=~env::bit(env::Field::pressure_altitude_m);r.accepted[23]=0;}
    if(!(r.accepted_mask&env::bit(env::Field::precipitation_level))){r.accepted_mask&=~env::bit(env::Field::precipitation_ratio);r.approximation_mask&=r.accepted_mask;}
    for(unsigned i=0;i<r.accepted.size();++i)if(!(r.accepted_mask&(1u<<i)))r.accepted[i]=0;
    if(!r.accepted_mask&&!pending_fields){release(1,env::Reason::unsupported);last_weather_=clock_();return;}
    owned_|=2;r.phase=r.accepted_mask?env::Phase::owned:env::Phase::observing;
    r.reason=failed?env::Reason::binding:(s.valid_mask&presented&~r.accepted_mask&~pending_fields)?env::Reason::unsupported:env::Reason::none;
    bool cloud_changed=((previous_mask^r.accepted_mask)&env::cloud_fields)!=0;
    for(unsigned f=5;f<14;++f)if((r.accepted_mask&(1u<<f))&&r.accepted[f]!=previous[f])cloud_changed=true;
    if(cloud_changed&&(r.accepted_mask&env::cloud_fields)){settling_=true;settling_begin_=clock_();settling_end_=0;}
    // Cloud realization is native and may settle slowly. Follow, edits and recovery never force a full reset.
    r.settling_begin_qpc=settling_begin_;r.settling_end_qpc=settling_end_;last_weather_=clock_();
}
void EnvironmentController::confirm_weather() noexcept {
    auto& r=reports_[1];const auto& source=r.source;
    const auto accepted=[&](Id id,unsigned index){auto& saved=saved_[id];const auto bit=static_cast<std::uint16_t>(1u<<index);
        if(!(saved.target_mask&bit)||(saved.restoring_mask&bit))return false;
        float actual{};return read(id,index,actual,true)&&confirm(id,index,actual)&&!(saved.pending_mask&bit);};
    r.accepted={};r.accepted_mask=r.approximation_mask=0;
    r.flags&=~(env::opacity_approximate|env::local_profile_approximate);
    for(unsigned layer=0;layer<3;++layer){const auto field=5+3*layer,mask=7u<<field;
        if((source.valid_mask&mask)!=mask)continue;
        bool complete=true;for(auto id:{cloud_type,coverage,base,top})complete=accepted(id,layer)&&complete;
        if(complete){r.accepted_mask|=mask;r.approximation_mask|=mask;r.flags|=env::opacity_approximate;
            r.accepted[field]=saved_[coverage].target[layer];r.accepted[field+1]=saved_[base].target[layer];r.accepted[field+2]=saved_[top].target[layer];}}
    for(auto pair:{std::pair{14u,wind_direction},std::pair{15u,wind_speed}}){bool complete=true;
        for(unsigned i=0;i<13;++i)complete=accepted(pair.second,i)&&complete;
        if(complete){r.accepted_mask|=1u<<pair.first;r.approximation_mask|=1u<<pair.first;r.accepted[pair.first]=saved_[pair.second].target[0];r.flags|=env::local_profile_approximate;}}
    bool temperature=accepted(sea_temperature,0);for(unsigned i=0;i<13;++i)temperature=accepted(temperatures,i)&&temperature;
    if(temperature){const auto mask=env::bit(env::Field::temperature_c)|env::bit(env::Field::pressure_altitude_m);
        r.accepted_mask|=mask;r.approximation_mask|=env::bit(env::Field::temperature_c);r.accepted[16]=source.values[16];r.accepted[23]=prepared_pressure_m_;r.flags|=env::local_profile_approximate;}
    if(accepted(visibility,0)){r.accepted_mask|=env::bit(env::Field::visibility_m);r.accepted[18]=saved_[visibility].target[0]*1609.344;}
    if(accepted(rain,0)){const auto mask=env::bit(env::Field::precipitation_level)|env::bit(env::Field::precipitation_ratio);
        r.accepted_mask|=mask;r.approximation_mask|=mask;r.accepted[19]=source.values[19];r.accepted[21]=saved_[rain].target[0];}
    r.phase=r.accepted_mask?env::Phase::owned:env::Phase::observing;
}
envp::AppliedPresentation EnvironmentController::weather_report() const noexcept {
    envp::AppliedPresentation result{reports_[1],policies_[1]};
    const auto& input=claims_[1].input.sample;
    if(reported_weather_target_&&input.sequence){result.weather_target=reported_weather_target_;result.current_lifecycle=input.lifecycle;
        result.current_sequence=input.sequence;result.current_source_end_qpc=input.sample_end_qpc;result.weather_observed_qpc=clock_();}
    return result;
}
void EnvironmentController::readback(std::int64_t) noexcept {
    TimingScope timing(*this,EnvironmentStage::readback);
    auto& r=reports_[1];r.actual={};r.actual_mask=0;r.location={};r.location_mask=0;r.location_begin_qpc=r.location_end_qpc=0;
    r.flags&=~env::location_readback_detailed;const auto assign=[&](unsigned field,Id id,unsigned index,double scale=1.0){float v{};
        if(read(id,index,v)){const auto value=field==14&&v==360?0:v*scale;if(measured(field,value)){r.actual[field]=value;r.actual_mask|=1u<<field;}}};
    for(auto pair:{std::pair{14u,actual_wind_direction},std::pair{15u,actual_wind_speed},std::pair{16u,actual_temperature},std::pair{17u,actual_qnh},std::pair{20u,actual_pressure}})assign(pair.first,pair.second,0);
    assign(18,actual_visibility,0,1609.344);
    const auto& pressure_binding=bindings_[pressure_altitude];
    if(pressure_binding.ref&&sdk_->inspect(pressure_binding.ref,xplmType_Double,false)==Status::ok){const auto height=XPLMGetDatad(pressure_binding.ref)*.3048;
        if(std::isfinite(height)&&height>=-2000&&height<=100000){r.actual[23]=height;r.actual_mask|=env::bit(env::Field::pressure_altitude_m);}}
    for(unsigned l=0;l<3;++l){assign(5+l*3,actual_coverage,l);assign(6+l*3,actual_base,l);assign(7+l*3,actual_top,l);}
    double lat{},lon{},alt{};bool position=true;
    for(auto pair:{std::pair{latitude,&lat},std::pair{longitude,&lon},std::pair{altitude,&alt}}){const auto& b=bindings_[pair.first];
        if(!b.ref||sdk_->inspect(b.ref,xplmType_Double,false)!=Status::ok){position=false;break;}*pair.second=XPLMGetDatad(b.ref);position=std::isfinite(*pair.second)&&position;}
    if(position&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180&&alt>=-2000&&alt<=100000){r.actual_mask|=0xe;r.actual[1]=lat;r.actual[2]=lon;r.actual[3]=alt;
        if(weather_getter_){XPLMWeatherInfo_t info{};info.structSize=sizeof(info);const auto nan=std::numeric_limits<float>::quiet_NaN();
            info.temperature_alt=info.pressure_alt=info.precip_rate_alt=info.wind_dir_alt=info.wind_spd_alt=info.visibility=nan;
            for(auto& cloud:info.cloud_layers)cloud={nan,nan,nan,nan};
            r.location_begin_qpc=clock_();const auto detailed=weather_getter_(lat,lon,alt,&info);r.location_end_qpc=clock_();
            if(detailed)r.flags|=env::location_readback_detailed;
            // Return0 still supplies best data. Query visibility is the SDK's zero-altitude base, not aircraft visibility.
            r.location_mask=0xe;r.location[1]=lat;r.location[2]=lon;r.location[3]=alt;
            const auto add=[&](unsigned f,double v){if(f==14&&v==360)v=0;if(measured(f,v)){r.location[f]=v;r.location_mask|=1u<<f;}};
            add(14,info.wind_dir_alt);add(15,info.wind_spd_alt);add(16,info.temperature_alt);add(18,info.visibility);
            add(20,info.pressure_alt);add(21,info.precip_rate_alt);
            for(unsigned l=0;l<3;++l){const auto& cloud=info.cloud_layers[l];add(5+l*3,cloud.coverage);add(6+l*3,cloud.alt_base);add(7+l*3,cloud.alt_top);}
        }}
    if(settling_&&clouds_settled(r)){settling_=false;settling_end_=clock_();}
    r.flags=settling_?r.flags|env::clouds_settling:r.flags&~env::clouds_settling;r.settling_end_qpc=settling_end_;
    r.readback_qpc=clock_();last_readback_=r.readback_qpc;
}
void EnvironmentController::tick(bool connected) noexcept {
    if(!sdk_||!sdk_->on_thread())return;++callback_;const auto now=clock_();
    if(timing_.generation){if(last_timing_tick_&&now-last_timing_tick_>static_cast<std::int64_t>(frequency_/10)&&!have_gap_timing_){gap_timing_=timing_;gap_timing_.observed=last_timing_tick_;have_gap_timing_=true;}last_timing_tick_=now;}
    const bool weather_due=!last_weather_||now-last_weather_>=static_cast<std::int64_t>(frequency_);
    if(weather_due){bool restoring=false;for(unsigned id=cloud_type;id<=rain;++id)restoring=restoring||saved_[id].restoring_mask;
        if(restoring){retry_restoration();if(!claims_[1].pending)last_weather_=clock_();}}
    for(unsigned g=0;g<2;++g){auto& s=claims_[g];if(!s.pending)continue;
        if(!connected||now>=s.expires){release(g,!connected?env::Reason::disconnected:env::Reason::expired);continue;}
        if((owned_&(1u<<g))&&(g==0||prepared_weather_target_)){const auto issue=ownership_issue(g);if(issue!=env::Reason::none){release(g,issue);continue;}}
        if(!s.have_sample)continue;const auto context_end=s.input.context.observation_end_qpc;
        const bool context_fresh=s.input.visibility.kind!=envp::PolicyKind::extended||
            (now>=context_end&&now-context_end<static_cast<std::int64_t>(frequency_));
        const bool fresh=now>=s.input.sample.sample_end_qpc&&now-s.input.sample.sample_end_qpc<static_cast<std::int64_t>(frequency_)&&(g==0||context_fresh);
        if(g==1){if(fresh&&s.input.weather.identity&&s.input.weather.identity!=prepared_weather_target_&&(!last_weather_||now-last_weather_>=static_cast<std::int64_t>(frequency_))){
                apply_weather(now);if(s.pending)prepared_weather_target_=s.input.weather.identity;
            }
            else if(fresh&&(owned_&2)&&weather_due){confirm_weather();last_weather_=clock_();}
            else if(!fresh&&(owned_&2))reports_[1].reason=env::Reason::stale;
            if((owned_&2)&&(!last_readback_||now-last_readback_>=static_cast<std::int64_t>(frequency_)))readback(now);
        }
    }
    if(claims_[0].pending&&claims_[0].have_sample){const auto end=claims_[0].input.sample.sample_end_qpc;const auto current=clock_();
        if(current>=claims_[0].expires)release(0,env::Reason::expired);
        else apply_time(current,current>=end&&current-end<static_cast<std::int64_t>(frequency_));}
    if(claims_[1].pending&&clock_()>=claims_[1].expires)release(1,env::Reason::expired);
}
void EnvironmentController::capture(std::uint64_t generation) noexcept {
    if(!sdk_||!sdk_->on_thread()||timing_.generation==generation)return;
    timing_={};timing_.generation=generation;timing_.connection=connection_;gap_timing_={};have_gap_timing_=false;last_timing_publish_=last_timing_tick_=0;
}
void EnvironmentController::trace(EnvironmentStage stage,std::int64_t begin,unsigned groups) noexcept {
    if(!timing_.generation)return;auto& slot=timing_.stages[static_cast<unsigned>(stage)];if(slot.callback!=callback_){slot.begin=begin;slot.groups=0;}slot.end=clock_();slot.callback=callback_;slot.groups|=groups;
    slot.source=slot.groups==3?0:claims_[slot.groups==1?0:1].input.sample.sequence;slot.target=slot.groups&2?reported_weather_target_:0;++slot.count;
}
bool EnvironmentController::timing(EnvironmentTiming& output) noexcept {
    if(!sdk_||!sdk_->on_thread()||!timing_.generation)return false;
    if(have_gap_timing_){output=gap_timing_;have_gap_timing_=false;return true;}
    const auto now=clock_();if(last_timing_publish_&&now-last_timing_publish_<static_cast<std::int64_t>(frequency_/4))return false;
    output=timing_;output.observed=now;last_timing_publish_=now;return true;
}
}
