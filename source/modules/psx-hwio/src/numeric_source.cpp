#include "hwio/numeric_source.hpp"
#include "hwio/catalogue.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
namespace hwio {
std::optional<double> actuator_numeric(std::string_view text,int field,unsigned fields)noexcept{
    if(text.empty() || text.size()>4096 || field<-1 || fields<1 || fields>128 || (field>=0 && unsigned(field)>=fields))return {};
    std::string_view selected;unsigned count{};for(std::size_t begin=0;;){const auto end=text.find(';',begin);if((field<0 && count==0) || int(count)==field)selected=text.substr(begin,end==text.npos?text.size()-begin:end-begin);++count;if(end==text.npos)break;begin=end+1;}
    if((field<0 && count!=1) || (field>=0 && count!=fields) || selected.empty())return {};double v{};const auto r=std::from_chars(selected.data(),selected.data()+selected.size(),v);if(r.ec!=std::errc{} || r.ptr!=selected.data()+selected.size() || !std::isfinite(v))return {};return v;
}
std::optional<double> numeric_source_value(const Mapper& mapper,const NumericSource& s,NumericSourceCache& cache){
    if(cache.epoch!=mapper.epoch || cache.source!=s){cache={};cache.epoch=mapper.epoch;cache.source=s;}
    if(!mapper.ready || s.shift>31)return {};const auto found=mapper.records.find(s.record);if(found==mapper.records.end()){cache.sequence=UINT64_MAX;cache.number.reset();return {};}
    if(cache.sequence!=found->second.sequence){cache.sequence=found->second.sequence;cache.number=actuator_numeric(found->second.observed,s.field,s.fields);if(cache.number && s.record.size()>1 && s.record[1]!='s' && (*cache.number<INT32_MIN || *cache.number>INT32_MAX || std::floor(*cache.number)!=*cache.number || s.fields!=1))cache.number.reset();
        if(cache.number){auto& n=*cache.number;if(s.mask){if(n<0 || n>UINT32_MAX || std::floor(n)!=n)cache.number.reset();else n=double((std::uint32_t(n)&s.mask)>>s.shift);}if(cache.number){n=n*s.scale+s.offset;if(!std::isfinite(n))cache.number.reset();}}}
    auto result=cache.number;if(!s.power_record.empty()){const auto power=mapper.records.find(s.power_record);if(power==mapper.records.end()){cache.power_sequence=UINT64_MAX;cache.power.reset();return {};}if(cache.power_sequence!=power->second.sequence){cache.power_sequence=power->second.sequence;cache.power=actuator_numeric(power->second.observed,-1,1);}if(!cache.power || *cache.power<0 || *cache.power>UINT32_MAX || std::floor(*cache.power)!=*cache.power || !(std::uint32_t(*cache.power)&s.power_mask))return {};}
    return result;
}
std::vector<NumericSource> numeric_sources(){
    std::vector<NumericSource> out;
    // These menus supply mechanical instrument positions. Lighting observations
    // remain in the catalogue for lighting consumers and existing saved sources.
    for(const auto& [id,o]:active_catalogue().observations){if(o->encoding!=ObservationEncoding::number || o->minimum>=o->maximum || o->fields>128 || o->record==RecordKey{RecordKind::qs,323} || o->record==RecordKey{RecordKind::qs,324})continue;NumericSource s;s.id=id;s.name=o->name;s.record=record_name(o->record);s.unit=o->unit;s.field=o->fields>1?int(o->field):-1;s.fields=o->fields;s.minimum=double(o->minimum)/o->divisor;s.maximum=double(o->maximum)/o->divisor;s.scale=1./o->divisor;s.power_record=o->power_record;s.power_mask=o->power_mask;s.mask=o->mask;s.shift=o->shift;s.demand=o->demand;out.push_back(std::move(s));}const auto rank=[](const auto& source){return source.id=="servo.trim.captain"?0:source.id=="servo.trim.fo"?1:source.id=="servo.brake-accumulator"?2:3;};std::stable_sort(out.begin(),out.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});return out;
}
}
