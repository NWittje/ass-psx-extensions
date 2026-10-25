#include <ios/visibility.hpp>
#include <ios/airport.hpp>
#include <algorithm>
namespace ios {
namespace p=psx::environment::presentation;
namespace ipc=psx::non_normal;
namespace {
const char *reason(p::VisibilityReason r){
 switch(r){case p::VisibilityReason::none:return "";case p::VisibilityReason::unsupported:return "Visibility presentation unsupported";
 case p::VisibilityReason::identity:return "PSX association unavailable";case p::VisibilityReason::expired:return "Request expired";
 case p::VisibilityReason::disconnected:return "Disconnected";case p::VisibilityReason::busy:return "Coordinator busy";
 case p::VisibilityReason::malformed:return "Invalid request";case p::VisibilityReason::source_stale:return "PSX source unavailable";
 case p::VisibilityReason::context_unavailable:return "PSX weather context unavailable";case p::VisibilityReason::inactive_target:return "Target inactive";
 case p::VisibilityReason::configured_edit:return "PSX visibility changed";case p::VisibilityReason::station_changed:return "Zone 1 airport changed";
 case p::VisibilityReason::lifecycle:return "Situation changed";case p::VisibilityReason::not_following:return "Follow stopped";
 case p::VisibilityReason::plugin_unavailable:return "X-Plane unavailable";case p::VisibilityReason::binding:return "X-Plane visibility unavailable";
 case p::VisibilityReason::pending_readback:return "Waiting for PSX configured readback";}return "Unavailable";
}
}
void Visibility::disconnected(){client_.disconnect();psx_={};targets_={};poll_at_=0;}
void Visibility::record(std::string_view line,const psx::instructor::Weather &weather){
 if(line=="load1"||line=="exit"){disconnected();return;}
 for(unsigned i=0;i<2;++i){auto &t=targets_[i];const auto target=i?psx::instructor::WeatherTarget::zone1:psx::instructor::WeatherTarget::planet;
  if(line.starts_with(i?"Qs329=":"Qs328=")){
   const auto &settings=weather.settings(target);t.configured=settings?std::optional<int>{int(settings->value(psx::instructor::WeatherField::visibility))}:std::nullopt;
   t.readback_qpc=settings?ipc::qpc():0;
   if(t.commanded&&t.configured&&*t.configured!=9999){t.commanded.reset();t.result.reset();t.pending=p::VisibilityCommand::clear;t.notice="PSX visibility changed";poll_at_=0;}
  }
 }
 auto &all=targets_[2];
 if(line.size()>=6 && line.starts_with("Qs") && line[5]=='=') {
  const auto n=psx::instructor::integer(line.substr(2,3));
  if(n && *n>=328 && *n<=335) {
   const auto &zones=weather.zone_settings();
   const bool complete=std::all_of(zones.begin(),zones.end(),[](const auto &s){return s.has_value();});
   bool same=complete;
   if(complete)for(const auto &s:zones)same=same&&s->value(psx::instructor::WeatherField::visibility)==zones[0]->value(psx::instructor::WeatherField::visibility);
   all.configured=same?std::optional<int>{int(zones[0]->value(psx::instructor::WeatherField::visibility))}:std::nullopt;
   all.readback_qpc=same?ipc::qpc():0;
   if(all.commanded&&(!same||*all.configured!=9999)) {all.commanded.reset();all.result.reset();all.pending=p::VisibilityCommand::clear;all.notice="PSX visibility changed";poll_at_=0;}
  }
 }
 if(line.starts_with("Qs336=")){
  auto &t=targets_[1];std::array<char,8> station{};
  if(weather.zone())std::copy_n(weather.zone()->identifier.begin(),std::min(station.size(),weather.zone()->identifier.size()),station.begin());
  if(station!=t.station&&t.commanded){t.commanded.reset();t.result.reset();t.pending=p::VisibilityCommand::clear;t.notice="Zone 1 airport changed";poll_at_=0;}
  t.station=station;
 }
}
void Visibility::selected(psx::instructor::WeatherTarget target,int metres){
 auto &t=targets_[target==psx::instructor::WeatherTarget::all?2:target==psx::instructor::WeatherTarget::planet?0:1];
 if(metres>9999){t.commanded=metres;t.pending=p::VisibilityCommand::select;t.notice="Waiting for coordinator";}
 else{t.commanded.reset();t.pending=p::VisibilityCommand::clear;t.notice="Native visibility";}
 t.result.reset();poll_at_=0;
}
void Visibility::poll(Tick tick,bool ready,Identity identity){
 if(!ready){if(psx_.pid||client_.is_connected())disconnected();return;}
 if(identity!=psx_){disconnected();psx_=identity;}
 if(tick<poll_at_)return;poll_at_=tick+1000;
 if(!identity.pid){for(auto &t:targets_)t.notice="Local PSX association unavailable";return;}
 if(!client_.is_connected()&&!client_.connect(endpoint_.empty()?ipc::coordinator_endpoint():endpoint_)){
  for(auto &t:targets_){t.result.reset();t.notice="Coordinator unavailable";}return;
 }
 for(unsigned i=0;i<targets_.size();++i){auto &t=targets_[i];p::VisibilityRequest request;
  request.command=t.pending.value_or(p::VisibilityCommand::query);t.pending.reset();request.psx=identity;
  request.target=i==2?p::Target::all:i?p::Target::zone1:p::Target::planet;request.commanded_visibility_m=request.command==p::VisibilityCommand::select?t.commanded.value_or(0):0;
  request.configured_readback_qpc=t.readback_qpc;request.zone1_station=t.station;
  p::VisibilityResult result;
  if(!client_.transact(request,result,200)){for(auto &item:targets_){item.pending.reset();item.result.reset();item.notice="Coordinator readback unavailable";}return;}
  t.result=result;t.notice=reason(result.reason);
  if(result.reason==p::VisibilityReason::configured_edit||result.reason==p::VisibilityReason::station_changed||result.reason==p::VisibilityReason::lifecycle||result.reason==p::VisibilityReason::disconnected)t.commanded.reset();
 }
}
std::string Visibility::status() const {
 std::string out="{";
 for(unsigned i=0;i<targets_.size();++i){if(i)out+=',';const auto &t=targets_[i];const auto &r=t.result;
  std::string state=t.notice;const bool available=client_.is_connected()&&r&&r->presentation_available;
  if(r&&t.notice.empty())state=r->weather_owned?"Follow owns weather":r->weather_requested?"Waiting for weather ownership":"Follow stopped";
  out+=quote(i==2?"all":i?"zone1":"planet")+":{\"commanded_m\":"+(t.commanded?std::to_string(*t.commanded):t.configured?std::to_string(*t.configured):"null")+
   ",\"psx_m\":"+(t.configured?std::to_string(*t.configured):"null")+",\"extended\":"+(t.commanded?"true":"false")+
   ",\"xplane_available\":"+(available?"true":"false")+",\"xplane_state\":"+quote(state)+
   ",\"xplane_m\":"+(r&&(r->valid_mask&p::accepted_valid)?number(r->accepted_visibility_m):"null")+
   ",\"aircraft_m\":"+(r&&(r->valid_mask&p::actual_valid)?number(r->actual_visibility_m):"null")+"}";
 }return out+"}";
}
}
