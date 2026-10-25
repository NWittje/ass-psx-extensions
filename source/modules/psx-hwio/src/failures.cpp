#include "hwio/failures.hpp"
#include "hwio/catalogue.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>

namespace hwio {
namespace {
using namespace psx::hwio::failures;
bool trim_record(RecordKey k) {return k.kind==RecordKind::qh && (k.index==396 || k.index==398 || k.index==399);}
std::int64_t unix_ms() {FILETIME f{};GetSystemTimePreciseAsFileTime(&f);ULARGE_INTEGER u{};u.LowPart=f.dwLowDateTime;u.HighPart=f.dwHighDateTime;return static_cast<std::int64_t>(u.QuadPart/10000)-11644473600000LL;}
template<std::size_t N> void text(std::array<char,N>& dst,std::string_view value) {std::memcpy(dst.data(),value.data(),std::min(N-1,value.size()));}
std::string endpoint(std::string_view host) {std::string s(host);std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(s=="localhost" || s=="::1" || s=="[::1]")s="127.0.0.1";return s;}
bool ias(const Mapper& m,std::int64_t now,std::int64_t hz,double& value,std::uint64_t& sequence) {
    (void)now;(void)hz; // Change-only Main TCP values remain current throughout their validated session.
    const auto r=m.records.find("Qs483");int n{};if(r==m.records.end() || !r->second.qpc)return false;
    const auto& s=r->second.observed;const auto end=s.find(';');if(!integer(std::string_view(s).substr(0,end),n) || n<0 || n>20000)return false;
    value=n*.1;sequence=r->second.sequence;return true;
}
bool running(const Mapper& m) {const auto r=m.records.find("Qi129");int n{};return r!=m.records.end() && integer(r->second.observed,n) && n>=0 && n<=3 && !(n&1);}
bool altitude_fresh(bool valid,std::int64_t sampled) {const auto now=unix_ms();return valid && sampled>0 && sampled<=now && now-sampled<=sample_max_age_ms;}
bool effect_baseline(const Mapper& m,std::uint32_t mask,bool trim) {
    if(mask){const auto r=m.records.find("Qs120");if(r==m.records.end())return false;
        for(std::size_t slot=0;slot<3;++slot){const auto* t=flight_target(slot);if(!t || !t->descriptor.writable)return false;const auto v=extract(t->descriptor,r->second.effective);if(!v || *v< -999 || *v>999)return false;}}
    if(trim){const auto* target=active_catalogue().target("trim-capt");const auto r=m.records.find("Qh398");int value{};
        if(!target || !target->writable || r==m.records.end() || !integer(r->second.effective,value) || value< -1 || value>1)return false;}
    return true;
}
}
void FailureControl::state(std::uint32_t axes,int trim) {
    if(axes_==axes && trim_==trim)return;
    axes_=axes;trim_=trim;outputs_.clear();++revision_;
}
void FailureControl::activate(const ArmInfo& a,int random) {state(axes_|a.axis_mask,a.trim?(trim_?trim_:random):trim_);}
void FailureControl::clear(std::string_view reason) {
    for(auto& a:arms_)a={};outputs_.clear();state(0,0);reason_=reason;++revision_;
    bound_=false;alt_valid_=false;alt_sequence_=0;
}
void FailureControl::boundary(std::string_view reason) {clear(reason);outputs_.clear();session_=0;}
Packet FailureControl::status(const Mapper& m,std::string_view host,std::uint16_t port,std::int64_t now,std::int64_t hz) const {
    Packet p;p.session=m.epoch;p.data.qpc=now;p.data.revision=revision_;p.data.axis_mask=axes_;p.data.trim=trim_;
    p.data.port=port;text(p.data.host,host);text(p.data.message,reason_);
    if(m.ready)p.data.flags|=Flag::ready;if(bound_ && session_==m.epoch)p.data.flags|=Flag::bound;
    double v{};std::uint64_t seq{};if(ias(m,now,hz,v,seq))p.data.flags|=Flag::ias_available;
    if(altitude_fresh(alt_valid_,alt_sampled_unix_ms_))p.data.flags|=Flag::altitude_available;
    for(const auto& a:arms_)if(a.info.id)p.data.arms[p.data.arm_count++]=a.info;
    return p;
}
Packet FailureControl::dispatch(const Packet& p,Mapper& m,std::string_view host,std::uint16_t port,std::int64_t now,std::int64_t hz,int random) {
    auto result=Result::ok;
    const auto finish=[&]{auto r=status(m,host,port,now,hz);r.operation=p.operation;r.request_id=p.request_id;r.result=result;return r;};
    if(!valid(p) || !p.request_id || p.request_id<=request_){result=Result::malformed;return finish();}
    request_=p.request_id;
    if(p.operation==Operation::status){if(bound_)owner_qpc_=now;return finish();}
    if(p.operation==Operation::bind){
        if(p.session!=m.epoch || !std::memchr(p.data.host.data(),0,p.data.host.size()) || endpoint(p.data.host.data())!=endpoint(host) || p.data.port!=port){result=Result::session_mismatch;return finish();}
        if(!m.ready){result=Result::unavailable;return finish();}
        bound_=true;session_=m.epoch;owner_qpc_=now;reason_.clear();return finish();
    }
    if(!bound_){result=Result::not_bound;return finish();}
    if(p.session!=session_ || m.epoch!=session_){result=Result::session_mismatch;return finish();}
    owner_qpc_=now;
    if(p.operation!=Operation::altitude_sample && p.data.revision!=revision_){result=Result::conflict;reason_="Failure state changed; read Status before a new command";return finish();}
    if(p.operation==Operation::clear_all){clear("IOS cleared failures");return finish();}
    if(!m.ready){result=Result::unavailable;return finish();}
    if(p.operation==Operation::altitude_sample){
        if(!(p.data.flags&Flag::sample_valid)){alt_valid_=false;for(auto& a:arms_)if(a.info.trigger==static_cast<unsigned>(Trigger::native_altitude))a={};++revision_;return finish();}
        if(!std::isfinite(p.data.value) || p.data.value< -10000 || p.data.value>100000 || !p.data.source_pid || !p.data.source_start_ms || !p.data.source_generation || !p.data.revision || !altitude_fresh(true,p.data.sampled_unix_ms)){result=Result::invalid_value;return finish();}
        const bool same=p.data.source_pid==alt_pid_ && p.data.source_start_ms==alt_start_ && p.data.source_generation==alt_generation_;
        if(same && (p.data.revision<=alt_sequence_ || p.data.sampled_unix_ms<=alt_sampled_unix_ms_)){result=Result::invalid_value;return finish();}
        if(!same && alt_sequence_)for(auto& a:arms_)if(a.info.trigger==static_cast<unsigned>(Trigger::native_altitude))a={};
        alt_pid_=p.data.source_pid;alt_start_=p.data.source_start_ms;alt_generation_=p.data.source_generation;alt_sequence_=p.data.revision;
        altitude_=p.data.value;alt_sampled_unix_ms_=p.data.sampled_unix_ms;alt_valid_=true;return finish();
    }
    if(p.operation==Operation::cancel_arm){for(auto& a:arms_)if(a.info.id==p.data.arm_id){a={};++revision_;}return finish();}
    if((p.data.axis_mask&~7u) || (p.data.trim!=0 && p.data.trim!=1)){result=Result::invalid_value;return finish();}
    if(p.operation==Operation::set){
        if(p.data.trim && !trim_ && random!=1 && random!=-1){result=Result::random_unavailable;return finish();}
        if(!effect_baseline(m,p.data.axis_mask,p.data.trim!=0)){result=Result::missing_baseline;return finish();}
        state(p.data.axis_mask,p.data.trim?(trim_?trim_:random):0);return finish();
    }
    if(!p.data.arm_id || (!p.data.axis_mask && !p.data.trim) || !std::isfinite(p.data.value) || (p.data.trigger!=Trigger::ias && p.data.trigger!=Trigger::native_altitude) || (p.data.trigger==Trigger::ias && (p.data.value<0 || p.data.value>2000)) || (p.data.trigger==Trigger::native_altitude && (p.data.value< -10000 || p.data.value>100000))){result=Result::invalid_value;return finish();}
    if(!effect_baseline(m,p.data.axis_mask,p.data.trim!=0)){result=Result::missing_baseline;return finish();}
    double value{};std::uint64_t seq{};
    if(p.data.trigger==Trigger::ias){if(!ias(m,now,hz,value,seq)){result=Result::missing_baseline;return finish();}}
    else {if(!altitude_fresh(alt_valid_,alt_sampled_unix_ms_)){result=Result::missing_baseline;return finish();}value=altitude_;seq=alt_sequence_;}
    const auto pause=m.records.find("Qi129");int pause_value{};
    if(pause==m.records.end() || !integer(pause->second.observed,pause_value) || pause_value<0 || pause_value>3){result=Result::missing_baseline;return finish();}
    Arm* slot{};
    for(auto& a:arms_){if(a.info.id==p.data.arm_id)slot=&a;else if(a.info.id && ((a.info.axis_mask&p.data.axis_mask) || (a.info.trim && p.data.trim))){result=Result::conflict;return finish();}}
    if(!slot)for(auto& a:arms_)if(!a.info.id){slot=&a;break;}
    if(!slot){result=Result::capacity;return finish();}
    slot->info={p.data.arm_id,p.data.value,p.data.axis_mask,static_cast<unsigned>(p.data.trigger),value<=p.data.value?1:-1,static_cast<unsigned>(p.data.trim)};
    slot->previous=value;slot->sequence=seq;++revision_;return finish();
}
void FailureControl::tick(Mapper& m,std::int64_t now,std::int64_t hz,int (*random_source)()) {
    if(bound_ && (!m.ready || m.epoch!=session_)){boundary("PSX session lost or changed");return;}
    if(bound_ && hz>0 && static_cast<double>(now-owner_qpc_)*1000/hz>owner_timeout_ms){clear("IOS owner lease expired");return;}
    for(auto& a:arms_)if(a.info.id){
        double value{};std::uint64_t seq{};const bool valid_sample=a.info.trigger==static_cast<unsigned>(Trigger::ias)?ias(m,now,hz,value,seq):altitude_fresh(alt_valid_,alt_sampled_unix_ms_);
        if(!valid_sample){a={};reason_="Armed trigger source unavailable";++revision_;continue;}
        if(a.info.trigger==static_cast<unsigned>(Trigger::native_altitude)){value=altitude_;seq=alt_sequence_;}
        if(seq==a.sequence || !running(m))continue;
        const bool passed=a.info.direction>0?a.previous<=a.info.threshold && value>=a.info.threshold:a.previous>=a.info.threshold && value<=a.info.threshold;
        a.previous=value;a.sequence=seq;
        if(passed){const int random=a.info.trim && !trim_?random_source():trim_;
            if(!effect_baseline(m,a.info.axis_mask,a.info.trim!=0))reason_="Failure destination baseline unavailable";
            else if(a.info.trim && !trim_ && random!=1 && random!=-1){reason_="Random trim direction unavailable";}else activate(a.info,random);a={};++revision_;}
    }
}
void FailureControl::reconcile(Mapper& m,const ButtonRouter& buttons,std::int64_t now) {
    if(!axes_ && !trim_ && !m.failed_axes && !m.runaway_trim)return;
    const bool axes_changed=m.failed_axes!=axes_,trim_changed=m.runaway_trim!=trim_;
    auto intents=buttons.trim_intents();
    for(const auto& mapping:m.mappings)if(mapping.binding.enabled && trim_record(mapping.binding.target.record))intents.emplace_back(mapping.binding.target,mapping.state.available && mapping.state.baseline?mapping.state.desired:mapping.binding.target.released);
    std::uint32_t owned{};bool fo{};std::map<std::string,std::string> physical;
    for(const auto& [target,value]:intents){const auto name=record_name(target.record);auto r=m.records.find(name);
        if(target.record.index==396)owned|=target.layout==ValueLayout::bit_field?target.owned_mask:15u;
        if(target.record.index==399)fo=true;
        if(r==m.records.end() && target.layout!=ValueLayout::integer)continue;
        auto [p,inserted]=physical.emplace(name,r==m.records.end()?"":r->second.effective);(void)inserted;
        if(const auto line=compose(target,p->second,value))p->second=line->substr(line->find('=')+1);else physical.erase(p);
    }
    m.failed_axes=axes_;m.runaway_trim=trim_;m.trim_owned_mask=owned;m.trim_fo_owned=fo;
    if(axes_changed)for(auto& mapping:m.mappings)if(mapping.binding.target.record==RecordKey{RecordKind::qs,120})mapping.state.dirty=true;
    const auto enqueue=[&](std::string name,std::string value,bool event){auto r=m.records.find(name);if(r==m.records.end() && name!="Qh398" && !(name=="Qh399" && fo))return;
        if(outputs_.size()>=8){clear("Failure output queue overflow");return;}
        if(r!=m.records.end())r->second.effective=value;outputs_.push_back({binding_id,m.epoch,0,now,name+"="+value+"\n",event});};
    if(trim_changed && m.ready){
        if(trim_){enqueue("Qh398","0",true);if(fo)enqueue("Qh399","0",true);
            if(owned){const auto r=m.records.find("Qh396");int v{};if(r!=m.records.end() && integer(r->second.effective,v))enqueue("Qh396",std::to_string(static_cast<unsigned>(v)&~owned),true);}
            enqueue("Qh398",std::to_string(trim_),true);
        }else{enqueue("Qh398",physical.count("Qh398")?physical["Qh398"]:"0",true);for(const auto& [name,value]:physical)if(name!="Qh398")enqueue(name,value,true);}
    }
    if(m.ready && axes_){const auto r=m.records.find("Qs120");if(r!=m.records.end()){auto value=r->second.effective;m.constrain_record({RecordKind::qs,120},value);if(value!=r->second.effective)enqueue("Qs120",value,false);}}
    if(m.ready && trim_){const auto r=m.records.find("Qh398");if(r!=m.records.end() && r->second.effective!=std::to_string(trim_))enqueue("Qh398",std::to_string(trim_),false);}
    if(m.ready && trim_ && fo){const auto r=m.records.find("Qh399");if(r!=m.records.end() && r->second.effective!="0")enqueue("Qh399","0",false);}
    if(m.ready && trim_ && owned){const auto r=m.records.find("Qh396");int v{};if(r!=m.records.end() && integer(r->second.effective,v) && (static_cast<unsigned>(v)&owned))enqueue("Qh396",std::to_string(static_cast<unsigned>(v)&~owned),false);}
}
std::optional<Command> FailureControl::next() {if(outputs_.empty())return {};auto r=std::move(outputs_.front());outputs_.pop_front();return r;}
bool FailureControl::filter(Command& c,Mapper& m) const {
    if(!axes_ && !trim_)return true;
    if(c.binding==binding_id)return true;
    const auto equal=c.line.find('=');RecordKey key{};if(equal==c.line.npos || !parse_key(std::string_view(c.line).substr(0,equal),key))return true;
    if(trim_ && key.kind==RecordKind::qh && (key.index==398 || key.index==399)){const auto r=m.records.find(record_name(key));if(r!=m.records.end())m.constrain_record(key,r->second.effective);return false;}
    auto value=c.line.substr(equal+1);if(!value.empty() && value.back()=='\n')value.pop_back();const auto before=value;m.constrain_record(key,value);
    if(before!=value){c.line=record_name(key)+"="+value+"\n";const auto r=m.records.find(record_name(key));if(r!=m.records.end())r->second.effective=value;}
    return true;
}
} // namespace hwio
