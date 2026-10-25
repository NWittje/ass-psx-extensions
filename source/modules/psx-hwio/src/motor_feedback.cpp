#include "hwio/motor_feedback.hpp"
#include <psx/diagnostics/journal.hpp>

namespace hwio {
void MotorFeedbackBus::configure(const std::array<MotorFeedbackDefinition,motor_slots>& definitions,const std::vector<DeviceInfo>& devices,const SourceBank& bank){
    configure(std::vector<MotorFeedbackDefinition>(definitions.begin(),definitions.end()),devices,bank);
}
void MotorFeedbackBus::configure(const std::vector<MotorFeedbackDefinition>& definitions,const std::vector<DeviceInfo>& devices,const SourceBank& bank){
    std::lock_guard lock(mutex_);const auto previous=std::make_unique<decltype(inputs_)>(inputs_);for(auto& input:inputs_)input=Input{};
    std::vector<DeviceIdentity> identities;std::vector<const DeviceInfo*> connected;
    for(const auto& d:devices)if(d.connected){identities.push_back(d.identity);connected.push_back(&d);}
    for(unsigned n=0;n<std::min<std::size_t>(capacity,definitions.size());++n){auto& x=inputs_[n];x.definition=definitions[n];
        if(x.definition.kind!=MotorPositionSource::analogue)continue;
        Profile p;std::string error;if(!profile_decode(x.definition.input_profile,p,error) || p.bindings.empty() || p.bindings[0].sources.empty())continue;
        const int found=match_profile(p,identities);if(found<0)continue;const auto& device=*connected[unsigned(found)];if(device.slot>=max_devices)continue;
        for(unsigned c=0;c<device.controls.size() && c<max_controls;++c)if(same(device.controls[c].key,p.bindings[0].sources[0].key) && !device.controls[c].relative){
            x.resolved=true;x.slot=device.slot;x.control=c;x.generation=device.generation;x.code_count=double(device.controls[c].maximum)-double(device.controls[c].minimum)+1;
            const auto& old=(*previous)[n];if(old.resolved && old.slot==x.slot && old.control==c && old.generation==x.generation && old.definition==x.definition){x.physical=old.physical;x.history=old.history;x.history_count=old.history_count;x.history_next=old.history_next;x.discarded=old.discarded;}
            if(bank[x.slot][c].available)x.physical=bank[x.slot][c];break;
        }
    }
}
void MotorFeedbackBus::observe(const Sample& sample)noexcept{
    if(sample.relative || sample.report_end)return;std::lock_guard lock(mutex_);
    for(auto& x:inputs_)if(x.resolved && x.slot==sample.device && x.control==sample.control && x.generation==sample.generation){x.physical={sample.value,!sample.invalid,sample.qpc,sample.sequence};if(x.history_count==x.history.size())x.discarded=x.history[x.history_next].sequence;x.history[x.history_next]=x.physical;x.history_next=(x.history_next+1)%unsigned(x.history.size());x.history_count=std::min(unsigned(x.history.size()),x.history_count+1);}
}
void MotorFeedbackBus::publish(std::uint64_t now){
    std::lock_guard lock(mutex_);const auto qpc=psx::diagnostics::qpc_now();const auto frequency=psx::diagnostics::qpc_frequency();
    for(unsigned n=0;n<capacity;++n){const auto& x=inputs_[n];auto& sample=samples_[n];sample={};sample.checked_ms=now;
        if(!x.resolved || !x.physical.available)continue;sample.position=x.definition.inverted?-double(x.physical.value):double(x.physical.value);sample.generation=x.generation;sample.sequence=x.physical.sequence;sample.valid=true;sample.quantum=1;sample.code_count=x.code_count;
        const auto age=x.physical.qpc>0 && qpc>=x.physical.qpc && frequency>0?std::uint64_t(1000.*double(qpc-x.physical.qpc)/double(frequency)):0;sample.observed_ms=x.physical.qpc>0?now-std::min(now,age):0;
    }
}
void MotorFeedbackBus::invalidate(){std::lock_guard lock(mutex_);for(auto& x:inputs_){x.physical={};x.history_count=x.history_next=0;x.discarded=0;}samples_={};}
MotorPositionSample MotorFeedbackBus::sample(unsigned n)const{std::lock_guard lock(mutex_);return n<motor_slots?samples_[n]:MotorPositionSample{};}
MotorPositionSample MotorFeedbackBus::sample(const MotorFeedbackDefinition& d)const{std::lock_guard lock(mutex_);for(unsigned n=0;n<capacity;++n)if(inputs_[n].definition==d)return samples_[n];return {};}

unsigned MotorFeedbackBus::events(const MotorFeedbackDefinition& d,std::uint64_t after,MotorPositionSample* output,unsigned capacity_out,bool& lost)const{
    std::lock_guard lock(mutex_);lost=false;const auto now=GetTickCount64();const auto qpc=psx::diagnostics::qpc_now();const auto frequency=psx::diagnostics::qpc_frequency();
    for(const auto& x:inputs_)if(x.definition==d && x.resolved){lost=after && x.discarded && after<x.discarded;unsigned count{};for(unsigned n=0;n<x.history_count;++n){const auto& p=x.history[(x.history_next+unsigned(x.history.size())-x.history_count+n)%x.history.size()];if(p.sequence<=after)continue;if(count==capacity_out){lost=true;break;}auto& s=output[count++];s={};s.position=d.inverted?-double(p.value):double(p.value);s.generation=x.generation;s.sequence=p.sequence;s.valid=p.available;s.checked_ms=now;s.code_count=x.code_count;const auto age=p.qpc>0 && qpc>=p.qpc && frequency>0?std::uint64_t(1000.*double(qpc-p.qpc)/double(frequency)):0;s.observed_ms=now-std::min(now,age);}return count;}return 0;
}

}
