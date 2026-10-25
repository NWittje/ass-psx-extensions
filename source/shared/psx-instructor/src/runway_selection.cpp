#include <psx/instructor/runway_selection.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
namespace psx::instructor {
namespace {
constexpr double pi=std::numbers::pi;
double difference(double a,double b) { return std::remainder(a-b,2*pi); }
double distance(Point a,Point b) {
  double h=std::pow(std::sin((b.lat-a.lat)/2),2)+std::cos(a.lat)*std::cos(b.lat)*std::pow(std::sin((b.lon-a.lon)/2),2);
  return 3440.065*2*std::asin(std::sqrt(std::clamp(h,0.,1.)));
}
double bearing(Point a,Point b) {
  double d=b.lon-a.lon;
  return std::atan2(std::sin(d)*std::cos(b.lat),std::cos(a.lat)*std::sin(b.lat)-std::sin(a.lat)*std::cos(b.lat)*std::cos(d));
}
std::string upper(std::string_view s) { std::string v(s); for(auto &c:v) if(c>='a'&&c<='z') c=char(c-32); return v; }
bool airport(std::string_view s) { return s.size()==4 && std::all_of(s.begin(),s.end(),[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9');}); }
bool runway(std::string_view s) { return (s.size()==2||s.size()==3) && s[0]>='0'&&s[0]<='3'&&s[1]>='0'&&s[1]<='9' && (s[0]!='0'||s[1]!='0') && (s[0]!='3'||s[1]<='6') && (s.size()==2||s[2]=='L'||s[2]=='C'||s[2]=='R'); }
}
void RunwaySelection::reset() { position_.reset();track_.reset();heading_.reset();ground_.reset();position_at_=0;route_=0;origins_={};departure_runways_={};destinations_={};runway_.clear(); }
void RunwaySelection::record(std::string_view line,Tick t) {
  if(line=="load1"||line=="exit") {reset();return;}
  if(line.starts_with("Qi257=")) { auto n=integer(line.substr(6));ground_.reset();if(n&&(*n==0||*n==1))ground_=*n!=0; }
  if(line.starts_with("Qs373=")) {
    auto s=line.substr(6); route_=s.size()==5 && (s[2]=='1'||s[2]=='2') ? unsigned(s[2]-'0') : 0;
  }
  for(unsigned i=0;i<2;++i) if(line.starts_with("Qs"+std::to_string(376+i)+"=")) {
    auto s=line.substr(6); destinations_[i].clear();
    origins_[i].clear();departure_runways_[i].clear();
    if(s.size()>=9 && s[4]==';') {auto a=upper(s.substr(5,4));if(airport(a)) destinations_[i]=std::move(a);auto origin=upper(s.substr(0,4));if(airport(origin))origins_[i]=std::move(origin);}
    if(s.size()>10) {auto end=s.find(';',10);auto r=upper(s.substr(10,end==s.npos?s.npos:end-10));if(runway(r))departure_runways_[i]=std::move(r);}
  }
  if(line.starts_with("Qs493=")) {runway_=upper(line.substr(6));if(!runway(runway_)) runway_.clear();}
  if(!line.starts_with("Qs121=")) return;
  auto s=line.substr(6); std::array<double,7> values{};
  for(unsigned i=0;i<7;++i) {
    auto end=s.find(';');auto v=s.substr(0,end); auto r=std::from_chars(v.data(),v.data()+v.size(),values[i]);
    if(r.ec!=std::errc{}||r.ptr!=v.data()+v.size()||!std::isfinite(values[i])||(i<6&&end==s.npos)) {position_.reset();track_.reset();heading_.reset();return;}
    if(end!=s.npos) s.remove_prefix(end+1);
  }
  Point p{values[5],values[6]}; track_.reset();
  if(std::abs(p.lat)>pi/2||std::abs(p.lon)>pi) {position_.reset();heading_.reset();return;}
  if(position_ && t>position_at_ && t-position_at_<=5000) {
    double metres=distance(*position_,p)*1852;
    if(metres>=1 && metres/(double(t-position_at_)/1000)<400) track_=bearing(*position_,p);
  }
  position_=p;position_at_=t;
  heading_=values[2];
}
std::optional<double> RunwaySelection::track(Tick t) const {return t>=position_at_&&t-position_at_<=5000 ? track_ : std::nullopt;}
std::pair<std::string,std::string> RunwaySelection::fmc(bool departure) const {
  if(!route_) return {};
  if(departure) return {origins_[route_-1],departure_runways_[route_-1]};
  return !runway_.empty() ? std::pair{destinations_[route_-1],runway_} : std::pair<std::string,std::string>{};
}
std::vector<RunwayOption> runway_candidates(std::span<const RunwayOption> catalogue,Point aircraft,std::optional<double> track,std::string_view fmc_airport,std::string_view fmc_runway) {
  std::vector<RunwayOption> result; std::optional<RunwayOption> selected;
  for(auto r:catalogue) {
    r.distance_nm=distance(aircraft,r.threshold);r.fmc=r.airport==fmc_airport&&r.runway==fmc_runway;
    if(r.fmc) selected=r;
    if(track && std::abs(difference(r.heading,*track))<=pi/4 && std::abs(difference(bearing(aircraft,r.threshold),*track))<pi/2) result.push_back(std::move(r));
  }
  std::sort(result.begin(),result.end(),[](const auto &a,const auto &b){if(a.distance_nm!=b.distance_nm)return a.distance_nm<b.distance_nm;return std::pair{a.airport,a.runway}<std::pair{b.airport,b.runway};});
  if(result.size()>3) result.resize(3);
  if(selected && std::none_of(result.begin(),result.end(),[](const auto &r){return r.fmc;})) {
    if(result.size()==3) result.pop_back();result.push_back(*selected);
    std::sort(result.begin(),result.end(),[](const auto &a,const auto &b){return a.distance_nm<b.distance_nm;});
  }
  return result;
}
std::optional<RunwayOption> windy_runway(std::span<const RunwayOption> catalogue,const RunwaySelection &flight,Tick t) {
  if(!flight.ground())return {};
  auto [airport_id,runway_id]=flight.fmc(*flight.ground());
  for(auto r:catalogue) if(r.airport==airport_id&&r.runway==runway_id) {r.fmc=true;if(flight.position())r.distance_nm=distance(*flight.position(),r.threshold);return r;}
  if(!flight.position())return {};
  auto direction=flight.track(t);if(!direction)direction=flight.heading();
  auto options=runway_candidates(catalogue,*flight.position(),direction,"","");
  return options.empty()?std::nullopt:std::optional<RunwayOption>{options.front()};
}
}
