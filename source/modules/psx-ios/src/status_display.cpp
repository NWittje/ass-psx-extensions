#include <ios/runtime.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace ios {
namespace pi=psx::instructor;
namespace {
bool fresh(Tick stamp,Tick t,bool native){return t>=stamp&&(!native||(stamp>0&&t-stamp<=250));}
template<std::size_t N> std::string values(const std::optional<std::array<double,N>>&a,Tick stamp,Tick t,bool native){
  if(!a||!fresh(stamp,t,native))return "null";std::string s="[";for(auto v:*a){if(s.size()>1)s+=',';s+=number(v);}return s+"]";
}
}
std::string status_controls_json(const pi::StatusDisplay::Values &v,bool ready,Tick t,bool native){
  if(!ready)return "{\"ready\":false}";
  const bool tiller=v.tiller&&fresh(v.tiller_at,t,native);
  return "{\"ready\":true,\"source\":"+quote(native?"PSX native":"PSX TCP")+",\"fast_available\":"+(native?"true":"false")+",\"engine_model\":"+(v.engine?std::to_string(*v.engine):"null")+
    ",\"axes\":"+values(v.axes,v.axes_at,t,native)+",\"tiller\":"+(tiller?number(*v.tiller):"null")+
    ",\"n1\":"+values(v.n1,v.n1_at,t,native)+",\"epr\":"+values(v.epr,v.epr_at,t,native)+
    ",\"flaps\":"+values(v.flaps,v.flaps_at,t,native)+",\"spoilers\":"+values(v.spoilers,v.spoilers_at,t,native)+
    ",\"engine_age_ms\":"+(v.engine?(t>=(*v.engine==0?v.n1_at:v.epr_at)?std::to_string(t-(*v.engine==0?v.n1_at:v.epr_at)):"null"):"null")+"}";
}
std::string Runtime::status_controls()const{std::lock_guard lock(mutex_);return status_controls_json_;}
std::string status_performance_json(const std::optional<pi::FlapSpeeds> &sample,std::uint64_t utc){
  if(!sample||utc<sample->sampled_unix_ms||utc-sample->sampled_unix_ms>3000)return "null";
  return "{\"gross_weight_kg\":"+number(sample->gross_weight_kg)+",\"ref25\":"+std::to_string(sample->ref25_kt)+",\"ref30\":"+std::to_string(sample->ref30_kt)+",\"flaps_up\":"+(sample->up_maneuver_kt?std::to_string(*sample->up_maneuver_kt):"null")+"}";
}
std::string Runtime::status_summary(std::string_view airport,std::string_view runway)const{
  std::lock_guard lock(mutex_);const auto &d=status_display_;const auto t=now();
  if(!engine_.ready())return "{\"ready\":false}";
  auto &v=d.values();std::string s="{\"ready\":true,\"trace_generation\":"+std::to_string(status_trace_generation_)+",\"takeoff\":[";
  for(unsigned i=0;i<3;++i){if(i)s+=',';s+=v.takeoff[i]?std::to_string(*v.takeoff[i]):"null";}s+="],\"performance\":";
  const auto utc=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  s+=status_performance_json(flap_speeds_,utc);
  std::string metar_airport=d.flight().ground()?d.fmc_airport(*d.flight().ground()):std::string{};
  if(metar_airport.empty()&&catalogue_&&d.pose()){double nearest=1e10;for(const auto &[id,a]:catalogue_->airports){auto n=distance_nm(d.pose()->position,a.reference);if(n<nearest){nearest=n;metar_airport=id;}}}
  s+=",\"metar_airport\":"+quote(metar_airport)+",\"metar\":"+quote(d.metar(metar_airport))+",\"runway\":";
  const Airport *a=nullptr;const RunwayEnd *r=nullptr;bool fmc=false;
  auto resolve=[&](std::string_view aid,std::string_view rid){if(!catalogue_)return;auto i=catalogue_->airports.find(std::string(aid));if(i==catalogue_->airports.end())return;auto j=std::find_if(i->second.runways.begin(),i->second.runways.end(),[&](const auto &end){return end.id==rid;});if(j!=i->second.runways.end()){a=&i->second;r=&*j;}};
  auto [fa,fr]=d.flight().fmc();resolve(fa,fr);fmc=r!=nullptr;
  if(!r&&!airport.empty())resolve(airport,runway);
  if(!r&&catalogue_&&d.pose()){std::vector<pi::RunwayOption> options;for(const auto &[id,ap]:catalogue_->airports)for(const auto &end:ap.runways)options.push_back({id,end.id,end.threshold,end.heading});auto ranked=pi::runway_candidates(options,d.pose()->position,d.flight().track(t),"","");if(!ranked.empty())resolve(ranked.front().airport,ranked.front().runway);}
  if(!r)return s+"null,\"trace\":[]}";
  double angle=3,tch=50;std::string source="Nominal 3°";const Glidepath *g=nullptr;bool ambiguous=false;
  for(const auto &path:a->glidepaths)if(path.runway==r->id&&std::isfinite(path.angle)&&path.angle>0&&path.angle<10*std::numbers::pi/180){if(g){ambiguous=true;break;}g=&path;}
  if(g&&!ambiguous){angle=g->angle*180/std::numbers::pi;tch=distance_nm(g->transmitter,r->threshold)*6076.11548556*std::tan(angle*std::numbers::pi/180)+g->elevation-r->elevation;source="Database ILS";}
  s+="{\"id\":"+quote(a->id+r->id)+",\"fmc\":"+(fmc?"true":"false")+",\"angle_deg\":"+number(angle)+",\"threshold_height_ft\":"+number(tch)+",\"reference\":"+quote(source)+",\"database_revision\":"+std::to_string(database_revision_)+"},\"trace\":[";
  // Keep the most recent visit to the approach box, including its touchdown tail.
  std::vector<pi::ApproachPoint> points;bool inside=false;
  for(const auto &p:d.history()){auto q=pi::approach_point(p,*r);bool next=q.distance_nm>=-.5&&q.distance_nm<=15&&std::abs(q.lateral_ft)<=6076.11548556*2;
    if(next&&!inside)points.clear();if(next)points.push_back(q);inside=next;}
  for(std::size_t i=0;i<points.size();++i){if(i)s+=',';const auto &p=points[i];s+="["+number(p.distance_nm)+","+number(p.lateral_ft)+","+number(p.height_ft)+","+std::to_string(p.at)+"]";}
  return s+"]}";
}
}
