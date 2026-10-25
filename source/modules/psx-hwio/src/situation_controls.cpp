#include "hwio/situation_controls.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
namespace hwio {
namespace {
using Value=psx::hwio::situation_controls::Value;
constexpr std::uint64_t seed=14695981039346656037ull;
void mix(std::uint64_t& h,std::uint64_t v){h=(h^v)*1099511628211ull;if(!h)h=1;}
void mix(std::uint64_t& h,std::string_view s){for(const unsigned char c:s)mix(h,c);mix(h,s.size());}
unsigned index(const TargetDescriptor& d){
    if(d.record==RecordKey{RecordKind::qh,170} && d.layout==ValueLayout::integer)return 0;
    if(d.record==RecordKey{RecordKind::qh,389} && d.layout==ValueLayout::integer)return 1;
    if(d.record==RecordKey{RecordKind::qs,436} && d.layout==ValueLayout::semicolon_field && d.field_count==4 && d.field<4)return 2+d.field;
    return 6;
}
struct Reader {
    const SourceBank& bank;const std::vector<DeviceInfo>& devices;std::int64_t now;const std::vector<Profile>& excluded;
    std::array<Value,6> values{};std::array<unsigned,6> owners{};
    void offer(unsigned n,Value v){if(n>=6)return;if(++owners[n]==1)values[n]=v;else values[n]={};}
    bool source(int d,int c,std::uint64_t generation,Value& v,int& raw)const{
        if(d<0 || c<0 || d>=int(max_devices) || c>=int(max_controls))return false;
        const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& a){return a.connected && a.slot==d && a.generation==generation;});
        if(found==devices.end() || unsigned(c)>=found->controls.size() || found->controls[unsigned(c)].relative)return false;
        for(const auto& p:excluded)if(!p.bindings.empty() && !p.bindings[0].sources.empty() && same(p.bindings[0].sources[0].key,found->controls[unsigned(c)].key) && match_profile(p,{found->identity})==0)return false;
        const auto& s=bank[unsigned(d)][unsigned(c)];const auto& control=found->controls[unsigned(c)];
        if(!s.available || !s.sequence || s.qpc<=0 || s.qpc>now || s.value<control.minimum || s.value>control.maximum)return false;
        auto identity=seed;const auto& a=found->identity;mix(identity,unsigned(a.backend));mix(identity,a.hardware_key);mix(identity,a.serial);mix(identity,a.container_id);mix(identity,a.collection_identity);mix(identity,a.instance_path);mix(identity,a.descriptor_signature);mix(identity,a.vendor);mix(identity,a.product);mix(identity,a.revision);mix(identity,a.usage_page);mix(identity,a.usage);
        const auto& key=control.key;mix(identity,unsigned(key.kind));mix(identity,key.usage_page);mix(identity,key.usage);mix(identity,key.report_id);mix(identity,key.data_index);mix(identity,key.link_collection);mix(identity,key.channel_kind);mix(identity,key.channel);
        mix(v.source,identity);mix(v.generation,generation);mix(v.sequence,s.sequence);v.input_qpc=std::max(v.input_qpc,s.qpc);raw=s.value;return true;
    }
    Value mapped(const Mapped& m)const{
        const auto& b=m.binding;const auto n=index(b.target);Value v;v.source=v.generation=v.sequence=v.mapping=seed;
        if(n>=6 || !b.enabled || b.target.semantics!=Semantics::absolute || m.controls.empty() || m.controls.size()!=m.source_devices.size() || m.controls.size()!=m.source_generations.size())return {};
        if(b.mode!=MappingMode::axis && b.mode!=MappingMode::selector && b.mode!=MappingMode::maintained)return {};
        if(b.mode==MappingMode::selector && m.controls.size()>1)for(const auto d:m.source_devices){
            const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& a){return a.connected && a.slot==d && a.coherent_reports;});
            if(d!=m.source_devices[0] || found==devices.end())return {};
        }
        mix(v.mapping,m.profile);mix(v.mapping,m.local_id);mix(v.mapping,b.id);mix(v.mapping,unsigned(b.mode));mix(v.mapping,b.target.id);mix(v.mapping,unsigned(b.throttle_layout));
        for(int x:{b.target.minimum,b.target.maximum,b.target.pressed,b.target.released})mix(v.mapping,unsigned(x));mix(v.mapping,m.reverse_unassigned);
        for(const auto* cal:{&b.calibration,&b.reverse_calibration}){for(double d:{cal->minimum,cal->center,cal->maximum})mix(v.mapping,std::bit_cast<std::uint64_t>(d));mix(v.mapping,cal->inverted);mix(v.mapping,cal->centered);}
        int raw{},reverse{},pattern{};
        for(std::size_t i=0;i<m.controls.size();++i){int value{};if(!source(m.source_devices[i],m.controls[i],m.source_generations[i],v,value))return {};if(i==0)raw=value;else reverse=value;if(i<31 && value)pattern|=1<<i;}
        if(b.mode==MappingMode::maintained){if(m.controls.size()!=1)return {};v.value=raw?b.target.pressed:b.target.released;}
        else if(b.mode==MappingMode::selector){
            if(!b.thresholds.empty()){
                if(m.controls.size()!=1 || !calibration_valid(b.calibration))return {};
                const auto physical=physical_fraction(raw,b.calibration);pattern=int(std::upper_bound(b.thresholds.begin(),b.thresholds.end(),physical)-b.thresholds.begin());
                for(double t:b.thresholds)if(std::abs(physical-t)<=b.hysteresis)return {};
            }else if(m.controls.size()==1)pattern=raw;
            for(const auto& [a,z]:b.selector){mix(v.mapping,unsigned(a));mix(v.mapping,unsigned(z));}
            for(double t:b.thresholds)mix(v.mapping,std::bit_cast<std::uint64_t>(t));
            mix(v.mapping,std::bit_cast<std::uint64_t>(b.hysteresis));
            const auto it=b.selector.find(pattern);if(it==b.selector.end())return {};v.value=it->second;
        }else{
            if(!calibration_valid(b.calibration))return {};
            const double physical=physical_fraction(raw,b.calibration);
            if(n>=2){
                if(b.throttle_layout==ThrottleLayout::separate_forward_reverse){
                    if(physical>0 || m.reverse_unassigned)v.value=int(std::llround(physical*5000));
                    else {if(m.controls.size()!=2 || !calibration_valid(b.reverse_calibration))return {};v.value=int(std::llround(physical_fraction(reverse,b.reverse_calibration)*-8925));}
                }else if(b.calibration.centered)v.value=int(std::llround(physical<0?-physical*b.target.minimum:physical*b.target.maximum));
                else v.value=int(std::llround(b.target.minimum+physical*(double(b.target.maximum)-b.target.minimum)));
            }else if(n==1){const double unit=b.calibration.centered?(physical+1)*.5:physical;v.value=int(std::llround(unit*6));}
            else return {};
        }
        if(v.value<(n==0?1:n==1?0:-8925) || v.value>(n==0?3:n==1?6:5000))return {};
        v.valid=1;return v;
    }
};
}
psx::hwio::situation_controls::Snapshot SituationControls::snapshot(const Mapper& mapper,
    const AxisRouter& axes,const ButtonRouter& buttons,const SourceBank& bank,
    const std::vector<DeviceInfo>& devices,unsigned bench,std::int64_t now,std::uint64_t frequency,const std::vector<Profile>& excluded){
    psx::hwio::situation_controls::Snapshot result;result.epoch=mapper.epoch;result.sampled_qpc=now;result.frequency=frequency;
    Reader r{bank,devices,now,excluded};
    for(const auto& m:mapper.mappings)if(!m.externally_calculated && m.binding.enabled)r.offer(index(m.binding.target),r.mapped(m));
    for(unsigned slot=9;slot<=13;++slot){const auto& row=axes.configuration_.rows[slot];if(!row.managed || !row.active)continue;
        const auto& c=axes.candidates_[slot];auto m=c.mapped;const auto n=index(m.binding.target);
        const unsigned required=slot==13 && row.applied.detents?127u:m.binding.calibration.centered?7u:5u;
        if((row.applied.captured&required)!=required){r.offer(n,{});continue;}
        m.controls={std::uint16_t(c.control)};m.source_devices={std::uint16_t(c.device)};m.source_generations={c.generation};
        if(m.binding.throttle_layout==ThrottleLayout::separate_forward_reverse && !m.reverse_unassigned){
            if((row.applied.reverse_captured&5)!=5){r.offer(n,{});continue;}
            m.controls.push_back(std::uint16_t(c.reverse_control));m.source_devices.push_back(std::uint16_t(c.reverse_device));m.source_generations.push_back(c.reverse_generation);
        }
        auto value=r.mapped(m);
        if(value.valid && slot==13 && row.applied.detents){int raw{};Value provenance;const bool available=r.source(c.device,c.control,c.generation,provenance,raw);unsigned matches{};
            const double width=std::abs(double(row.applied.points[6])-row.applied.points[0])*row.applied.deadzone/200;
            mix(value.mapping,std::bit_cast<std::uint64_t>(row.applied.deadzone));
            for(unsigned p=0;p<7;++p){mix(value.mapping,unsigned(row.applied.points[p]));if(available && std::abs(double(raw)-row.applied.points[p])<=width){value.value=int(p);++matches;}}
            if(matches!=1)value={};
        }
        r.offer(n,value);
    }
    for(const auto& g:buttons.groups_){
        if(g.entries.empty())continue;const auto n=index(buttons.entries_[g.entries.front()].action->target);if(n>1)continue;
        if(g.inhibited || g.capture_muted){r.offer(n,{});continue;}
        Value v;v.source=v.generation=v.sequence=v.mapping=seed;bool valid=true;int selected=-1;unsigned asserted{};
        for(const auto* a:g.positions){mix(v.mapping,a->id);mix(v.mapping,unsigned(a->value));}
        for(const auto i:g.entries){const auto& e=buttons.entries_[i];mix(v.mapping,e.action->id);mix(v.mapping,unsigned(e.action->value));for(auto state:e.states)mix(v.mapping,state);
            if(e.toggle || e.action->operation!=ButtonOperation::position){valid=false;break;}
            bool down{};
            if(e.contacts.empty()){int raw{};if(!r.source(e.device,e.control,e.generation,v,raw)){valid=false;break;}down=raw!=0;}
            else{unsigned mask{};for(std::size_t k=0;k<e.contacts.size();++k){const auto& c=e.contacts[k];int raw{};
                    const auto device=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.connected && d.slot==c.device && d.coherent_reports;});
                    if(k>=32 || c.device!=e.contacts[0].device || device==devices.end() || !r.source(c.device,c.control,c.generation,v,raw)){valid=false;break;}if(raw)mask|=1u<<k;
                }down=valid && std::find(e.states.begin(),e.states.end(),mask)!=e.states.end();}
            if(down){selected=e.action->value;++asserted;}
        }
        if(valid && asserted==0){unsigned missing{};for(const auto* a:g.positions)if(std::none_of(g.entries.begin(),g.entries.end(),[&](auto i){return buttons.entries_[i].action->id==a->id;})){selected=a->value;++missing;}
            if(g.down_only_gear_up){selected=g.down_only_gear_up->value;missing=1;}
            valid=missing==1 && std::all_of(g.entries.begin(),g.entries.end(),[&](auto i){const auto& e=buttons.entries_[i];return e.contacts.empty() && (g.entries.size()==1 || e.coherent);});
        }else valid=valid && asserted==1;
        if(valid){v.value=selected;v.valid=psx::hwio::situation_controls::valid;
            if(g.down_only_gear_up){v.valid|=psx::hwio::situation_controls::gear_down_only;result.version=2;}
        }else v={};r.offer(n,v);
    }
    for(unsigned n=0;n<4;++n)if(bench&(1u<<n))r.values[n+2]={};
    result.controls=r.values;return result;
}
}
