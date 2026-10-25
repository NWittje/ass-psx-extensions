#include "hwio/cockpit.hpp"
#include "hwio/pokeys.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hwio {
bool cockpit_source_overlap(const Source& source,bool encoder,const CockpitBinding& assignment) {
    const auto kind=static_cast<ResourceKind>(source.key.channel_kind);const auto channel=unsigned(source.key.channel);
    if(source.key.backend==BackendKind::phidget22 && assignment.device.starts_with("phidget:"))
        return encoder?assignment.software?source.key.channel_kind==5 && (channel==assignment.a || channel==assignment.b):source.key.channel_kind==8 && channel==assignment.a:(source.key.channel_kind==29 || source.key.channel_kind==31) && channel==assignment.a;
    if(source.key.backend==BackendKind::fds_usb || source.key.backend==BackendKind::fds_ethernet)
        return encoder?assignment.software?kind==ResourceKind::boolean_input && (channel==assignment.a || channel==assignment.b):kind==ResourceKind::rotary_delta && channel==assignment.a:kind==ResourceKind::analogue_input && channel==assignment.a;
    return encoder?assignment.software?kind==pokeys::Channel::digital && (channel==assignment.a || channel==assignment.b):(kind==pokeys::Channel::encoder && channel/2==assignment.a) || (kind==pokeys::Channel::encoder_count && channel==assignment.a):kind==pokeys::Channel::analog && channel==assignment.a;
}

namespace {
constexpr std::uint64_t mapping_base=0x3000000000000000ULL;
bool binding_valid(const CockpitBinding& b){
    const int limit=b.device.starts_with("phidget:")?16383:4095;
    if(b.device.size()>128 || b.counts<1 || b.counts>1024 || b.a>4095 || b.b>UINT16_MAX || (b.enabled && b.software && b.b>4095) || b.minimum<0 || b.maximum>limit || b.minimum>=b.maximum ||
       !std::isfinite(b.hysteresis) || b.hysteresis<0 || b.hysteresis>.25 || b.thresholds.size()>127)return false;
    double last=-1;for(const auto value:b.thresholds){if(!std::isfinite(value) || value<=0 || value>=1 || value<=last)return false;last=value;}
    return !b.enabled || (!b.device.empty() && (!b.software || b.a!=b.b));
}
void write_binding(std::ostream& out,const CockpitBinding& b){
    out<<std::quoted(b.device)<<' '<<b.a<<' '<<b.b<<' '<<b.enabled<<' '<<b.software<<' '<<b.inverted<<' '<<b.counts<<' '<<b.minimum<<' '<<b.maximum<<' '<<b.hysteresis<<' '<<b.thresholds.size();
    for(auto value:b.thresholds)out<<' '<<value;out<<'\n';
}
bool read_binding(std::istream& in,CockpitBinding& b){
    std::size_t count{};in>>std::quoted(b.device)>>b.a>>b.b>>b.enabled>>b.software>>b.inverted>>b.counts>>b.minimum>>b.maximum>>b.hysteresis>>count;
    if(!in || count>127)return false;b.thresholds.resize(count);for(auto& value:b.thresholds)in>>value;return bool(in) && binding_valid(b);
}
ButtonAction proxy(const CockpitDefinition& d){if(d.encoder)if(const auto* a=button_action(d.increase))return *a;ButtonAction a;a.target=d.target;a.applicability=d.applicability;return a;}
}
bool cockpit_valid(const CockpitConfiguration& c,std::string& error){
    if(c.rows.size()>512){error="Too many cockpit assignments";return false;}
    for(const auto& [id,row]:c.rows)if(id.empty() || id.size()>256 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:")!=id.npos ||
        row.pending.size()>1024 || !binding_valid(row.applied) || !binding_valid(row.draft)){error="Invalid cockpit assignment/calibration";return false;}
    return true;
}
bool cockpit_destination_overlap(const TargetDescriptor& a,const TargetDescriptor& b) noexcept {
    if(overlaps(a,b))return true;
    return a.record.kind==RecordKind::qh && b.record.kind==RecordKind::qh &&
        ((a.record.index<3 && b.record.index==a.record.index+3) || (b.record.index<3 && a.record.index==b.record.index+3));
}
std::string cockpit_encode(const CockpitConfiguration& c){
    std::ostringstream out;out<<std::setprecision(17)<<"cockpit-v1\n"<<c.rows.size()<<'\n';
    for(const auto& [id,row]:c.rows){out<<std::quoted(id)<<' '<<std::quoted(row.pending)<<'\n';write_binding(out,row.applied);write_binding(out,row.draft);}return out.str();
}
bool cockpit_decode(std::string_view text,CockpitConfiguration& out,std::string& error){
    if(text.size()>1024*1024){error="Cockpit assignments exceed bound";return false;}
    std::istringstream in{std::string(text)};std::string header,id;std::size_t count{};CockpitConfiguration c;
    if(!(in>>header>>count) || header!="cockpit-v1" || count>512){error="Invalid cockpit assignment header";return false;}
    for(std::size_t n=0;n<count;++n){CockpitRow row;if(!(in>>std::quoted(id)>>std::quoted(row.pending)) || !read_binding(in,row.applied) || !read_binding(in,row.draft) || !c.rows.emplace(id,std::move(row)).second){error="Invalid cockpit assignment";return false;}}
    in>>std::ws;if(!in.eof() || !cockpit_valid(c,error)){if(error.empty())error="Unexpected cockpit assignment data";return false;}out=std::move(c);return true;
}
int Quadrature::report(bool valid,int a,int b) noexcept {
    if(!valid || a<0 || a>1 || b<0 || b>1){reset();return 0;}
    const int current=(a<<1)|b;
    if(previous_<0){previous_=current;return 0;}
    const int old=previous_;previous_=current;
    if((old^current)==3){reset();return 0;}
    static constexpr int transitions[]{0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
    return transitions[(old<<2)|current];
}
void CockpitRouter::boundary(std::uint64_t epoch) noexcept {
    epoch_=epoch;turns_.clear();last_turn_.clear();shared_cdu_owner_.clear();overflow=false;
    for(auto& route:routes_){route.remainder=0;route.quadrature.reset();route.selector_filter.reset();route.sequence=0;route.movement_known=false;}
}
void CockpitRouter::configure(const CockpitConfiguration& c,const std::vector<DeviceInfo>& devices,Mapper& mapper,const std::set<std::string>& module_devices){
    mapper.mappings.erase(std::remove_if(mapper.mappings.begin(),mapper.mappings.end(),[](const auto& m){return m.profile=="cockpit";}),mapper.mappings.end());
    routes_.clear();boundary(mapper.epoch);std::uint64_t id=mapping_base;
    for(const auto& [key,row]:c.rows)if(row.applied.enabled){
        const auto found=active_catalogue().cockpit.find(key);if(found==active_catalogue().cockpit.end())continue;
        const auto record=found->second->target.record;
        const bool shared=!row.module_sources.empty() && !found->second->encoder && record.kind==RecordKind::qh && record.index>=404 && record.index<=406 && module_devices.count(row.applied.device);
        std::vector<const CockpitBinding*> sources{&row.applied};if(shared)for(const auto& source:row.module_sources)if(sources.size()<2 && source.enabled && module_devices.count(source.device))sources.push_back(&source);
        std::set<std::uint64_t> siblings;
        for(const auto* source:sources){
        Route route;route.definition=found->second;route.binding=*source;route.view.id=key;route.mapping=++id;route.shared_cdu=sources.size()>1;
        const auto& d=*route.definition;const auto& binding=route.binding;
        for(const auto& device:devices)if(device.connected && (!device.complete_module || module_devices.count(device.identity.hardware_key)) && device.identity.backend!=BackendKind::usb_hid && device.identity.hardware_key==binding.device){
            route.device=device.slot;route.generation=device.generation;
            for(std::size_t n=0;n<device.controls.size();++n){const auto& control=device.controls[n];const auto kind=static_cast<pokeys::Channel>(control.key.channel_kind);
                const bool phidget=device.identity.backend==BackendKind::phidget22 && device.slot>=first_generic_phidget_device;const bool fds=device.identity.backend==BackendKind::fds_usb || device.identity.backend==BackendKind::fds_ethernet || device.identity.backend==BackendKind::cpflight_serial;
                const auto expected=phidget?static_cast<ResourceKind>(d.encoder?(binding.software?5:8):31):d.encoder?(binding.software?(fds?ResourceKind::boolean_input:pokeys::Channel::digital):(fds?ResourceKind::rotary_delta:pokeys::Channel::encoder_count)):(fds?ResourceKind::analogue_input:pokeys::Channel::analog);
                if((kind==expected || (phidget && !d.encoder && unsigned(kind)==29)) && control.key.channel==binding.a)route.a=static_cast<int>(n);
                if(d.encoder && binding.software && kind==expected && control.key.channel==binding.b)route.b=static_cast<int>(n);
            }
            if(d.encoder && binding.software && !device.coherent_reports){route.a=-1;route.b=-1;route.view.reason="Software quadrature unavailable: transport has no qualified complete report boundary";}
        }
        if(route.view.reason.empty())route.view.reason=route.a<0 || (d.encoder && binding.software && route.b<0)?"Configured hardware source unavailable":"Waiting for current baselines";
        const bool conflict=std::any_of(mapper.mappings.begin(),mapper.mappings.end(),[&](const auto& m){return m.binding.enabled && cockpit_destination_overlap(m.binding.target,d.target) && !(route.shared_cdu && siblings.count(m.binding.id));});
        if(conflict){route.a=-1;route.view.reason="Destination already assigned to another input";}
        if(!d.encoder && route.a>=0 && mapper.mappings.size()<max_bindings){
            Mapped m;m.profile="cockpit";m.local_id=route.mapping;m.device=static_cast<std::uint16_t>(route.device);m.generation=route.generation;
            m.binding.id=route.mapping;m.binding.name=d.name;m.binding.target=d.target;
            auto& calibration=m.binding.calibration;calibration.minimum=binding.minimum;calibration.maximum=binding.maximum;calibration.center=(binding.minimum+binding.maximum)/2.;calibration.centered=false;calibration.inverted=binding.inverted;
            m.binding.mode=d.detents.empty()?MappingMode::axis:MappingMode::selector;m.binding.hysteresis=binding.hysteresis;
            for(std::size_t n=0;n<d.detents.size();++n)m.binding.selector[static_cast<int>(n)]=d.detents[n];
            m.binding.thresholds=binding.thresholds;if(m.binding.thresholds.empty() && !d.detents.empty())for(std::size_t n=1;n<d.detents.size();++n)m.binding.thresholds.push_back(double(n)/d.detents.size());
            m.controls.push_back(static_cast<std::uint16_t>(route.a));m.source_devices.push_back(m.device);m.source_generations.push_back(m.generation);mapper.mappings.push_back(std::move(m));siblings.insert(route.mapping);
        }
        routes_.push_back(std::move(route));
        }
    }
    for(const auto key:interests())mapper.watched_records.insert(record_name(key));
}
std::vector<std::uint64_t> CockpitRouter::overrides(const std::set<std::string>& ids){
    std::vector<std::uint64_t> affected;
    for(auto& r:routes_)if(override_encoders_.count(r.definition->id)!=ids.count(r.definition->id)){
        affected.push_back(r.mapping);r.remainder=0;
    }
    override_encoders_=ids;
    turns_.erase(std::remove_if(turns_.begin(),turns_.end(),[&](const auto& t){return ids.count(routes_[t.route].definition->id)!=0;}),turns_.end());return affected;
}
void CockpitRouter::turn(std::size_t index,int count,std::int64_t qpc,Mapper& mapper){
    auto& route=routes_[index];if(!count)return;
    if(capture_){std::vector<std::string> ids;
        for(const auto& r:routes_)if(r.definition->encoder && r.device==route.device && r.a==route.a && r.b==route.b)ids.push_back("encoder:"+r.definition->id);
        if(capture_(ids)){route.remainder=0;return;}
    }
    if(override_encoders_.count(route.definition->id)){route.remainder=0;return;}
    if(!mapper.ready)return;
    const auto* action=active_catalogue().action(count>0?route.definition->increase:route.definition->decrease);
    if(!action || !button_availability(*action,mapper).empty()){route.remainder=0;return;}
    route.remainder+=route.binding.inverted?-static_cast<std::int64_t>(count):count;
    const auto steps=route.remainder/route.binding.counts;route.remainder%=route.binding.counts;
    if(!steps)return;
    if(std::abs(steps)>100000 || turns_.size()>=1024){overflow=true;turns_.clear();route.remainder=0;return;}
    // Keep opposite turns ordered; adjacent equal-direction batches alone may merge.
    if(!turns_.empty() && turns_.back().route==index && ((turns_.back().count>0)==(steps>0)) && std::abs(static_cast<std::int64_t>(turns_.back().count)+steps)<=100000)
        turns_.back().count+=static_cast<int>(steps);
    else turns_.push_back({index,static_cast<int>(steps),qpc});
}
void CockpitRouter::input(const Sample& sample,Mapper& mapper){
    for(std::size_t n=0;n<routes_.size();++n){auto& r=routes_[n];if(!r.definition->encoder || r.binding.software || r.device!=sample.device || r.a!=sample.control || r.generation!=sample.generation)continue;
        r.view.available=!sample.invalid;r.view.raw=sample.value;
        if(sample.invalid || sample.baseline){r.remainder=0;turns_.erase(std::remove_if(turns_.begin(),turns_.end(),[&](const auto& t){return t.route==n;}),turns_.end());}
        else if(sample.relative)turn(n,sample.value,sample.qpc,mapper);
    }
}
void CockpitRouter::report(std::uint16_t device,const SourceBank& values,Mapper& mapper){
    for(std::size_t n=0;n<routes_.size();++n){auto& r=routes_[n];if(!r.definition->encoder || !r.binding.software || r.device!=device || r.a<0 || r.b<0)continue;
        const auto& a=values[device][static_cast<std::size_t>(r.a)];const auto& b=values[device][static_cast<std::size_t>(r.b)];
        r.view.available=a.available && b.available && a.value>=0 && a.value<=1 && b.value>=0 && b.value<=1;r.view.raw=r.view.available?(a.value<<1)|b.value:0;
        const auto delta=r.quadrature.report(r.view.available,a.value,b.value);if(!r.quadrature.known())r.remainder=0;
        turn(n,delta,std::max(a.qpc,b.qpc),mapper);
    }
}
void CockpitRouter::update(const SourceBank& values,Mapper& mapper,double ms,int smoothing){
    if(epoch_!=mapper.epoch)boundary(mapper.epoch);
    // A fresh sample with the same raw value is not a knob movement. Start on
    // the primary healthy source; hand over only to the latest actual change.
    std::map<std::string,std::vector<std::size_t>> valid;
    std::map<std::string,SharedCdu> moved;
    for(std::size_t n=0;n<routes_.size();++n){auto& route=routes_[n];if(!route.shared_cdu || route.device<0 || route.a<0)continue;
        const auto& source=values[static_cast<std::size_t>(route.device)][static_cast<std::size_t>(route.a)];
        if(!source.available || source.value<route.binding.minimum || source.value>route.binding.maximum){route.movement_known=false;continue;}
        valid[route.definition->id].push_back(n);
        if(route.movement_known && source.value!=route.movement_raw){auto& change=moved[route.definition->id];if(std::pair(source.qpc,source.sequence)>=std::pair(change.qpc,change.sequence))change={n,source.qpc,source.sequence};}
        route.movement_known=true;route.movement_raw=source.value;
    }
    for(const auto& [id,available]:valid){auto& selected=shared_cdu_owner_[id];if(std::find(available.begin(),available.end(),selected.route)==available.end())selected={available.front(),0,0};
        const auto change=moved.find(id);if(change!=moved.end() && std::pair(change->second.qpc,change->second.sequence)>=std::pair(selected.qpc,selected.sequence))selected=change->second;}
    for(auto& route:routes_){const auto& d=*route.definition;if(d.encoder)continue;
        const auto found=std::find_if(mapper.mappings.begin(),mapper.mappings.end(),[&](const auto& m){return m.binding.id==route.mapping;});if(found==mapper.mappings.end())continue;
        auto& m=*found;
        if(route.shared_cdu){const auto selected=shared_cdu_owner_.find(d.id);if(selected==shared_cdu_owner_.end() || &routes_[selected->second.route]!=&route){m.state.available=false;m.state.baseline=false;m.state.dirty=false;m.state.filter.reset();route.selector_filter.reset();route.sequence=0;m.state.reason="The other CDU brightness knob was moved last";continue;}}
        auto reason=button_availability(proxy(d),mapper);
        const auto baseline=mapper.records.find(record_name(d.target.record));
        if(baseline==mapper.records.end())reason="Awaiting current PSX baseline";
        else if(d.target.layout==ValueLayout::integer){int value{};if(!integer(baseline->second.effective,value) || value<d.baseline_minimum || value>d.baseline_maximum)reason="Invalid PSX baseline";}
        else if(!compose(d.target,baseline->second.effective,d.target.minimum))reason="Invalid packed PSX baseline";
        const auto& source=values[static_cast<std::size_t>(route.device)][static_cast<std::size_t>(route.a)];
        if(!reason.empty() || !source.available || source.value<0 || source.value>(route.binding.device.starts_with("phidget:")?16383:4095)){m.state.available=false;m.state.baseline=false;m.state.dirty=false;m.state.filter.reset();route.selector_filter.reset();route.sequence=0;m.state.reason=reason.empty()?"Hardware baseline unavailable":reason;continue;}
        m.binding.calibration.median_three=true;m.binding.calibration.lowpass_ms=smoothing;
        route.view.raw=source.value;route.source[0]=source;
        if(m.binding.mode==MappingMode::selector){auto calibration=m.binding.calibration;calibration.inverted=false;
            const double normalized=route.sequence!=source.sequence?route.selector_filter.sample(source.value,calibration,ms):route.selector_filter.settle(calibration,ms);route.sequence=source.sequence;
            route.source[0].value=static_cast<int>(std::llround(calibration.minimum+normalized*(calibration.maximum-calibration.minimum)));
        }
        mapper.input(m,route.source,ms,false,!m.state.baseline,false);
    }
}
std::optional<Command> CockpitRouter::next(Mapper& mapper,double ms,std::int64_t qpc){
    if(epoch_!=mapper.epoch)boundary(mapper.epoch);if(!mapper.ready)return {};
    std::set<std::string> blocked;
    for(auto it=turns_.begin();it!=turns_.end();){auto& route=routes_[it->route];const auto record=record_name(route.definition->target.record);
        if(blocked.count(record) || (last_turn_.count(record) && ms-last_turn_[record]<50)){blocked.insert(record);++it;continue;}
        const auto* action=active_catalogue().action(it->count>0?route.definition->increase:route.definition->decrease);
        route.view.reason=action?button_availability(*action,mapper):"Catalogue turn action missing";
        const int count=action?(action->wrap?std::clamp(it->count,1-action->wrap,action->wrap-1):std::clamp(it->count,action->target.minimum,action->target.maximum)):0;
        const auto current=mapper.records.find(record);
        const auto line=route.view.reason.empty()?button_compose(*action,current==mapper.records.end()?"":current->second.effective,count):std::nullopt;
        if(!line){it=turns_.erase(it);continue;}
        Command command{route.mapping,mapper.epoch,it->qpc,qpc,*line+"\n",true,0,count};command.minimum_interval_ms=50;
        it->count-=count;if(!it->count)turns_.erase(it);last_turn_[record]=ms;route.view.desired=count;return command;
    }return {};
}
std::vector<CockpitReadout> CockpitRouter::snapshot(const Mapper& mapper) const {
    std::vector<CockpitReadout> result;std::set<std::string> shared;for(const auto& route:routes_){if(route.shared_cdu){const auto selected=shared_cdu_owner_.find(route.definition->id);if(selected!=shared_cdu_owner_.end() && &routes_[selected->second.route]!=&route)continue;if(!shared.insert(route.definition->id).second)continue;}auto view=route.view;
        if(!route.definition->encoder)for(const auto& m:mapper.mappings)if(m.binding.id==route.mapping){view.available=m.state.available;view.desired=m.state.desired;view.reason=m.state.reason;break;}
        if(route.definition->encoder){
            if(!mapper.ready){view.available=false;view.reason="PSX disconnected";}
            else if(const auto reason=button_availability(proxy(*route.definition),mapper);!reason.empty()){view.available=false;view.reason=reason;}
            else if(view.available)view.reason="Ready";
        }
        if(route.definition->encoder && override_encoders_.count(route.definition->id))view.reason="Blocked by input override";
        result.push_back(std::move(view));}return result;
}
std::vector<RecordKey> CockpitRouter::interests() const {std::vector<RecordKey> result;for(const auto& route:routes_){result.push_back(route.definition->target.record);for(const auto& p:route.definition->applicability)result.push_back(p.first);}return result;}
std::vector<std::uint64_t> CockpitRouter::command_ids() const {std::vector<std::uint64_t> result;for(const auto& r:routes_)result.push_back(r.mapping);return result;}
} // namespace hwio
