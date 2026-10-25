#include "hwio/digital_outputs.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace hwio {
void DigitalOutputs::configure(const Catalogue& catalogue) {
    sources_.clear();outputs_.clear();ids_.clear();epoch_=UINT64_MAX;ready_=false;
    std::map<std::string,std::size_t> sources;
    auto source=[&](RecordKey key,DigitalEncoding encoding=DigitalEncoding::numbers,const std::string& prefix=std::string{}) {
        const auto name=record_name(key);const auto layout=name+":"+std::to_string(static_cast<unsigned>(encoding))+":"+prefix;
        const auto [it,inserted]=sources.emplace(layout,sources_.size());
        if(inserted){Source s;s.record=name;s.encoding=encoding;
            // Qh58 is the mechanical ARM position, not an aircraft-computed
            // lamp contact. PSX distributes injections to other clients only.
            // Completed sends therefore participate in this switch's state;
            // queued intent and all electrical observations remain excluded.
            s.local_arm=name=="Qh58";
            std::size_t start=0;while(start<prefix.size()){const auto end=prefix.find('/',start);s.prefix.push_back(prefix.substr(start,end==prefix.npos?prefix.size()-start:end-start));if(end==prefix.npos)break;start=end+1;}
            sources_.push_back(std::move(s));}return it->second;
    };
    for(const auto& [id,definition]:catalogue.digital_outputs) {
        Output out;out.definition=definition;out.state.id=id;
        for(const auto& branch:definition->branches) {
            auto& result=out.branches.emplace_back();
            for(const auto& operand:branch){const auto index=source(operand.record,operand.encoding,operand.prefix);result.push_back({operand,index});out.dependencies.push_back(index);}
        }
        for(const auto& operand:definition->requirements){const auto index=source(operand.record,operand.encoding,operand.prefix);out.requirements.push_back({operand,index});out.dependencies.push_back(index);}
        for(const auto& predicate:definition->applicability){const auto index=source(predicate.first);out.applicability.push_back({index,predicate.second});out.dependencies.push_back(index);}
        std::sort(out.dependencies.begin(),out.dependencies.end());out.dependencies.erase(std::unique(out.dependencies.begin(),out.dependencies.end()),out.dependencies.end());
        ids_.emplace(id,outputs_.size());outputs_.push_back(std::move(out));
    }
    changed_.resize(sources_.size());
}
void DigitalOutputs::update(const Mapper& mapper) {
    const bool boundary=epoch_!=mapper.epoch || ready_!=mapper.ready;epoch_=mapper.epoch;ready_=mapper.ready;
    // At most one parse per changed record layout, independently of mapped route count.
    std::fill(changed_.begin(),changed_.end(),0);
    for(std::size_t i=0;i<sources_.size();++i) {
        auto& s=sources_[i];const auto it=mapper.records.find(s.record);
        const auto revision=it==mapper.records.end()?0:it->second.sequence;
        const auto completed=s.local_arm && it!=mapper.records.end()?it->second.transmitted_qpc:0;
        // Counter timestamps need not be unique. An ON/OFF pair completed in
        // one clock tick must still change the coil state without a self-echo.
        const std::string_view arm=s.local_arm && it!=mapper.records.end()?
            (it->second.committed.empty()?it->second.observed:it->second.committed):std::string_view{};
        const bool same_arm=!s.local_arm || (s.valid && s.fields==1 &&
            ((arm=="0" && s.values[0]==0) || (arm=="1" && s.values[0]==1)));
        if(s.initialized && !boundary && s.sequence==revision && s.completed_qpc==completed && same_arm)continue;
        changed_[i]=1;s.initialized=true;s.sequence=revision;s.epoch=mapper.epoch;s.valid=false;s.fields=0;
        s.completed_qpc=completed;
        if(!mapper.ready || it==mapper.records.end())continue;
        s.qpc=it->second.qpc;std::string_view payload=it->second.observed;
        if(s.local_arm && !it->second.committed.empty()){
            payload=it->second.committed;s.qpc=std::max(s.qpc,completed);
        }
        bool valid=true;
        if(s.encoding==DigitalEncoding::characters || s.encoding==DigitalEncoding::digits) {
            if(payload.empty() || payload.size()>s.values.size())continue;
            for(const unsigned char ch:payload){if(ch<32 || ch>126 || (s.encoding==DigitalEncoding::digits && (ch<'0' || ch>'9'))){valid=false;break;}s.values[s.fields++]=s.encoding==DigitalEncoding::digits?ch-'0':ch;}
            s.valid=valid;continue;
        }
        if(payload.size()<s.prefix.size())continue;
        for(std::size_t p=0;p<s.prefix.size();++p)if(s.prefix[p].find(payload[p])==std::string::npos)valid=false;
        if(!valid)continue;payload.remove_prefix(s.prefix.size());
        if(s.encoding==DigitalEncoding::trailing_numbers){if(payload.empty() || payload.back()!=';')continue;payload.remove_suffix(1);}
        for(;;) {
            if(s.fields==s.values.size()){valid=false;break;}
            const auto end=payload.find(';');const auto token=payload.substr(0,end);double value{};
            const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
            if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size() || !std::isfinite(value) || std::floor(value)!=value){
                if(s.encoding!=DigitalEncoding::mixed_fields || token.size()>64 || std::any_of(token.begin(),token.end(),[](unsigned char c){return c<32 || c>126;})){valid=false;break;}
                value=std::numeric_limits<double>::quiet_NaN();
            }
            s.values[s.fields++]=value;if(end==payload.npos)break;payload.remove_prefix(end+1);
        }
        s.valid=valid;
    }
    for(auto& out:outputs_) {
        if(!boundary && std::none_of(out.dependencies.begin(),out.dependencies.end(),[&](auto i){return changed_[i]!=0;}))continue;
        auto& state=out.state;state.available=false;state.on=false;state.qpc=0;
        if(!out.definition->qualified){state.reason=out.definition->unavailable;continue;}
        if(!mapper.ready){state.reason="PSX unavailable";continue;}
        bool valid=true,installed=true;
        for(const auto& p:out.applicability) {
            const auto& s=sources_[p.first];if(!s.valid || s.fields!=1){valid=false;continue;}
            if(std::find(p.second.begin(),p.second.end(),s.values[0])==p.second.end())installed=false;
        }
        auto evaluate=[&](const Operand& operand) {
            const auto& s=sources_[operand.source];const auto& d=operand.definition;
            const bool shape=d.length_min?s.fields>=d.length_min && s.fields<=d.length_max:s.fields==d.fields;
            if(!s.valid || !shape || !std::isfinite(s.values[d.field]) || s.values[d.field]<d.minimum || s.values[d.field]>d.maximum ||
               (!d.allowed.empty() && d.allowed.find(static_cast<char>(s.values[d.field]))==std::string::npos)){valid=false;return false;}
            state.qpc=std::max(state.qpc,s.qpc);const auto value=s.values[d.field];
            const auto bits=value>=INT32_MIN && value<=UINT32_MAX?static_cast<std::uint32_t>(static_cast<std::int64_t>(value)):0u;
            switch(d.test) {
            case DigitalTest::positive:return value>0;
            case DigitalTest::any:return (bits&d.mask)!=0;
            case DigitalTest::not_all:return (bits&d.mask)!=d.mask;
            case DigitalTest::clear:return (bits&d.mask)==0;
            case DigitalTest::equal:return value==d.value;
            case DigitalTest::greater:return value>d.value;
            case DigitalTest::at_most:return value<=d.value;
            case DigitalTest::valid:return true;
            }return false;
        };
        bool eligible=true;for(const auto& operand:out.requirements)eligible=evaluate(operand) && eligible;
        bool on=false;
        for(const auto& branch:out.branches) {
            bool all=true;for(const auto& operand:branch)all=evaluate(operand) && all;
            on=on || all;
        }
        state.available=valid && installed && eligible;state.on=state.available && on;
        state.reason=!valid?"Awaiting valid source baselines":!installed?"Not installed in this aircraft":!eligible?out.definition->requirement_reason:"";
    }
}
const DigitalOutputState* DigitalOutputs::find(std::string_view id) const noexcept {
    const auto it=ids_.find(id);return it==ids_.end()?nullptr:&outputs_[it->second].state;
}
std::vector<DigitalOutputState> DigitalOutputs::snapshot() const {
    std::vector<DigitalOutputState> result;result.reserve(outputs_.size());for(const auto& out:outputs_)result.push_back(out.state);return result;
}
} // namespace hwio
