#include "hwio/flightillusion_bridge.hpp"
#include <charconv>
#include <cmath>
#include <numbers>
#include <algorithm>

namespace hwio::flightillusion {
namespace {
ClockSync clock_sync(const Mapper& m,unsigned side){
    auto read=[&](const char* key,int lo,int hi)->std::optional<int>{const auto i=m.records.find(key);int value{};if(i==m.records.end()||!integer(i->second.observed,value)||value<lo||value>hi)return {};return value;};
    const auto pause=read("Qi129",0,3),rate=read("Qi128",25,6400),setting=read(side?"Qh157":"Qh155",0,3),power=read("Qi246",0,2147483647);
    if(!pause||!rate||!setting||!power)return ClockSync::unavailable;
    // ElecDistrib.sendPowerBits bit 0 is clockL.eaC10530.isPowered. Both
    // clocks use the same APU-battery bus, voltage threshold and no local CB.
    if(!(*power&1))return ClockSync::display_off;
    if(*pause&1)return ClockSync::paused; // Motion freeze (bit 1) leaves clock systems running.
    if(*rate!=100)return ClockSync::rate;
    if(*setting)return ClockSync::setting;
    return ClockSync::normal;
}
}
void Connection::update(Provider& provider,bool enabled,std::uint64_t ms){
    const auto view=provider.snapshot();
    if(enabled!=enabled_){enabled_=enabled;next_=0;if(!enabled)provider.disconnect();}
    if(!enabled)return;
    if(!view.connected&&!view.connecting&&ms>=next_){std::string error;provider.connect(error);next_=ms+5000;}
    if(view.calibration)return; // Reconnect polls known instruments without leaving the editor for a full scan.
    if(view.connected&&(searched_!=view.session||(!view.searching&&view.searched==255&&view.instruments.empty()&&ms>=search_after_))){std::string error;if(provider.search(error)){searched_=view.session;search_after_=ms+60000;}}
}
const std::vector<RecordKey>& simulator_interests(){
    static const auto keys=[] {std::vector<RecordKey> out;for(const auto* name:{"Qi274","Qi38","Qs121","Qi273","Qi37","Qi239","Qs124","Qs125","Qi128","Qi129","Qh155","Qh157","Qi246"}){RecordKey k{};if(parse_key(name,k))out.push_back(k);}return out;}();
    return keys;
}
Sources host_sources(const Mapper& m,std::uint64_t sequence,std::uint64_t ms,std::int64_t pc_utc_ms,std::optional<std::int64_t> pc_local_ms){
    Sources s;s.session=m.epoch+1;if(!s.session)s.session=1;s.sequence=sequence;s.sample_ms=ms;s.controls_ready=m.ready;
    auto valid=[](std::int64_t t){return t>=946684800000LL&&t<2966371200000LL;};
    s.pc_utc={pc_utc_ms,valid(pc_utc_ms)};
    if(pc_local_ms)s.pc_local={*pc_local_ms,valid(*pc_local_ms)};
    if(!m.ready)return s;
    auto payload=[&](const char* key)->std::optional<std::string_view>{
        const auto it=m.records.find(key);if(it==m.records.end())return {};
        return it->second.observed;
    };
    auto number=[&](const char* key,int minimum,int maximum)->std::optional<double>{auto p=payload(key);int v{};if(!p||!integer(*p,v)||v<minimum||v>maximum)return {};return double(v);};
    s.values[unsigned(Signal::altitude)]=number("Qi274",-9999,89900);
    s.values[unsigned(Signal::brake)]=number("Qi239",0,4000);
    // Qi37/1000 scales the nonlinear mechanical dial angle. The calibrated IAS
    // profile is valid only at the healthy factor; never scale knots instead.
    if(const auto factor=number("Qi37",-1000,1000);factor&&*factor==1000)
        if(const auto ias=number("Qi273",0,9999))s.values[unsigned(Signal::ias)]=*ias/10.;
    if(auto p=payload("Qs121")){
        auto field=[&]()->std::optional<double>{const auto end=p->find(';');const auto word=p->substr(0,end);double v{};const auto result=std::from_chars(word.data(),word.data()+word.size(),v);if(end==std::string_view::npos)*p={};else p->remove_prefix(end+1);if(result.ec!=std::errc{}||result.ptr!=word.data()+word.size()||!std::isfinite(v))return {};return v*180./(100000.*std::numbers::pi);};
        const auto pitch=field(),bank=field();if(pitch&&bank&&std::abs(*pitch)<=90&&std::abs(*bank)<=180){s.values[unsigned(Signal::pitch)]=pitch;s.values[unsigned(Signal::bank)]=bank;}
    }
    // PSX Clock.advance publishes GMT on six-second boundaries. Paused/held
    // ECON values remain authoritative until Mapper retires the connection/load
    // epoch. Never replace them with PC local time or extrapolate through a hold.
    for(unsigned n=0;n<2;++n){
        auto& clock=s.clocks[n];clock.sync=clock_sync(m,n);
        const auto record=m.records.find(n?"Qs125":"Qs124");
        if(record==m.records.end())continue;
        const auto& text=record->second.observed;std::int64_t value{};const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        if(result.ec==std::errc{}&&result.ptr==text.data()+text.size()&&value>=946684800000LL&&value<2966371200000LL){clock.epoch_ms=value;clock.valid=true;}
    }
    // ExporterSource supplies the separately timestamped indicated barber pole.
    // No qualified magnetic-compass source is available here.
    return s;
}
std::vector<hwio::Command> Inputs::update(const Mapper& m,const View& v,std::span<const InputEvent> events,const std::set<std::string>& blocked,std::uint64_t ms,std::int64_t qpc,std::int64_t frequency){
    constexpr std::uint64_t base=0xF200000000000000ULL;
    std::string ownership;for(const auto& i:v.instruments)ownership+=std::to_string(i.address)+":"+std::to_string(i.generation)+":"+std::to_string(unsigned(i.assigned))+":"+std::to_string(i.connected&&i.compatible&&!i.calibrating)+";";for(const auto& key:blocked)ownership+=key+";";
    for(unsigned side=0;side<2;++side)ownership+=std::to_string(unsigned(clock_sync(m,side)))+";";
    const bool active=m.ready&&v.connected&&!v.searching;
    if(epoch_!=m.epoch||bus_!=v.session||ownership_!=ownership||active_!=active){
        for(unsigned address=1;address<256;++address)cancelled_.push_back(base+address);
        epoch_=m.epoch;bus_=v.session;ownership_=std::move(ownership);active_=active;sequence_=0;baro_.reset();baro_sequence_=0;return {};
    }
    if(!active)return {};
    const auto baro=m.records.find("Qi38");if(baro!=m.records.end()&&baro->second.sequence!=baro_sequence_){int value{};baro_sequence_=baro->second.sequence;if(integer(baro->second.observed,value)&&value>=74500&&value<=110025)baro_=value;else baro_.reset();}else if(baro==m.records.end())baro_.reset();
    std::vector<hwio::Command> out;
    for(const auto& event:events){if(event.session!=v.session||event.sequence<=sequence_||event.sample_ms>ms||ms-event.sample_ms>2000)continue;sequence_=event.sequence;
        const auto i=std::find_if(v.instruments.begin(),v.instruments.end(),[&](const auto& instrument){return instrument.address==event.address&&instrument.generation==event.generation&&instrument.assigned==event.profile&&instrument.connected&&instrument.compatible&&!instrument.calibrating;});if(i==v.instruments.end())continue;
        const auto owners=std::count_if(v.instruments.begin(),v.instruments.end(),[&](const auto& instrument){return instrument.connected&&instrument.compatible&&instrument.assigned==i->assigned;});if(owners!=1)continue;
        std::string key,value;
        if(event.profile==Profile::altimeter&&(event.kind==InputKind::barometer_inhg||event.kind==InputKind::barometer_delta_inhg)){
            if(blocked.contains("Qi38")||blocked.contains("Qh136")||!baro_||!std::isfinite(event.value))continue;
            // PSX Qi38 is Pa (100 * hPa). Its mechanical Qh136 detent is 4 Pa,
            // whereas ISFD has a different step. Convert the observed pressure
            // directly, independent of the aircraft's standby instrument option.
            const double pascal=event.value*3386.3887+(event.kind==InputKind::barometer_delta_inhg?*baro_:0);
            if(pascal<74500||pascal>110025)continue;baro_=pascal;key="Qi38";value=std::to_string(std::lround(pascal));
        }else if(event.kind==InputKind::clock_epoch_ms&&(event.profile==Profile::clock_captain||event.profile==Profile::clock_fo)){
            const auto selected=std::find_if(v.configuration.assignments.begin(),v.configuration.assignments.end(),[&](const auto& a){return a.bus==v.configuration.port.instance&&a.address==event.address&&a.profile==event.profile;});
            if(selected==v.configuration.assignments.end()||selected->clock_source!=ClockSource::psx_utc)continue;
            const unsigned side=event.profile==Profile::clock_fo?1:0;
            key=side?"Qs125":"Qs124";if(blocked.contains(key)||blocked.contains(side?"Qh157":"Qh155")||clock_sync(m,side)!=ClockSync::normal||frequency<=0)continue;
            const auto clock=m.records.find(key);if(clock==m.records.end()||clock->second.qpc>qpc||event.epoch_ms<946684800000LL||event.epoch_ms>=2966371200000LL)continue;
            std::int64_t previous{};const auto& text=clock->second.observed;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),previous);if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||previous<946684800000LL||previous>=2966371200000LL)continue;
            value=std::to_string(event.epoch_ms);
        }else continue;
        hwio::Command command;command.binding=base+event.address;command.epoch=m.epoch;command.input_qpc=command.mapped_qpc=qpc;command.line=key+"="+value+"\n";command.minimum_interval_ms=50;out.push_back(std::move(command));
    }
    return out;
}
}
