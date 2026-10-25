#include <psx/instructor/status_display.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
namespace psx::instructor {
namespace {
template<std::size_t N> std::optional<std::array<double,N>> fields(std::string_view s,double low,double high,bool trailing=false,bool whole=true) {
  std::array<double,N> result{};
  for(std::size_t i=0;i<N;++i){const auto end=s.find(';');auto part=s.substr(0,end);double v{};
    auto r=std::from_chars(part.data(),part.data()+part.size(),v);
    if(r.ec!=std::errc{}||r.ptr!=part.data()+part.size()||!std::isfinite(v)||v<low||v>high||(whole&&std::trunc(v)!=v)||(i+1<N&&end==s.npos))return {};
    result[i]=v;if(end!=s.npos)s.remove_prefix(end+1);else s={};
    if(i+1==N && (end!=part.npos&&!trailing))return {};
  } if(!s.empty())return {};return result;
}
void report(std::map<std::string,std::string> &cache,std::string_view s) {
  if(s.starts_with('i'))s.remove_prefix(1);
  if(s.size()<5||s.size()>3000||s[4]!=' '||!std::all_of(s.begin(),s.begin()+4,[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9');})||
    std::any_of(s.begin(),s.end(),[](char c){return c<32||c>126;}))return;
  if(cache.size()==64&&!cache.contains(std::string(s.substr(0,4))))cache.erase(cache.begin());
  cache[std::string(s.substr(0,4))]=s;
}
}
ApproachPoint approach_point(const StatusPose &p,const Runway &r) {
  // Spherical distance/bearing, projected into the runway's true-heading frame.
  const auto dlon=std::remainder(p.position.lon-r.threshold.lon,2*std::numbers::pi);
  const auto h=std::pow(std::sin((p.position.lat-r.threshold.lat)/2),2)+std::cos(p.position.lat)*std::cos(r.threshold.lat)*std::pow(std::sin(dlon/2),2);
  const auto d=3440.065*2*std::asin(std::sqrt(std::clamp(h,0.,1.)));
  const auto bearing=std::atan2(std::sin(dlon)*std::cos(p.position.lat),std::cos(r.threshold.lat)*std::sin(p.position.lat)-std::sin(r.threshold.lat)*std::cos(p.position.lat)*std::cos(dlon));
  const auto angle=std::remainder(bearing-r.heading,2*std::numbers::pi);
  return {-d*std::cos(angle),d*6076.11548556*std::sin(angle),p.altitude_ft-r.elevation,p.at};
}
void StatusDisplay::clear_trace(){history_.clear();next_sample_=0;}
void StatusDisplay::reset(){values_={};flight_.reset();pose_.reset();metars_.clear();route_=0;origins_={};destinations_={};clear_trace();}
void StatusDisplay::record(std::string_view line,Tick t){
  if(line=="load1"||line=="exit"){reset();return;}flight_.record(line,t);
  if(line.starts_with("Qs373=")){auto s=line.substr(6);route_=s.size()==5&&(s[2]=='1'||s[2]=='2')?unsigned(s[2]-'0'):0;}
  for(unsigned i=0;i<2;++i)if(line.starts_with("Qs"+std::to_string(376+i)+"=")){auto s=line.substr(6);origins_[i].clear();destinations_[i].clear();if(s.size()>=9&&s[4]==';'){auto valid=[](std::string_view a){return std::all_of(a.begin(),a.end(),[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9');});};if(valid(s.substr(0,4)))origins_[i]=s.substr(0,4);if(valid(s.substr(5,4)))destinations_[i]=s.substr(5,4);}}
  if(line.starts_with("Qs120=")){values_.axes=fields<3>(line.substr(6),-999,999);values_.axes_at=t;}
  if(line.starts_with("Qh426=")){auto v=integer(line.substr(6));values_.tiller=v&&*v>=-999&&*v<=999?std::optional<double>{double(*v)}:std::nullopt;values_.tiller_at=t;}
  if(line.starts_with("Qi1=")){auto v=integer(line.substr(4));values_.engine=v&&*v>=0&&*v<=2?v:std::nullopt;}
  for(unsigned i=0;i<3;++i)if(line.starts_with("Qi"+std::to_string(231+i)+"=")){auto v=integer(line.substr(6));values_.takeoff[i]=v&&*v>0&&*v<=300?v:std::nullopt;}
  if(line.starts_with("Qs481=")){values_.n1.reset();auto s=line.substr(6);if(s.size()>4&&std::all_of(s.begin(),s.begin()+4,[](char c){return c=='L'||c=='x';})){auto v=fields<4>(s.substr(4),0,1500,true);if(v){for(auto &n:*v)n/=10;values_.n1=v;}}values_.n1_at=t;}
  if(line.starts_with("Qs482=")){values_.epr.reset();auto v=fields<37>(line.substr(6),-100000,100000,true);if(v){std::array<double,4>a{};bool valid=true;for(unsigned i=0;i<4;++i){a[i]=(*v)[5+i]/100.;valid=valid&&a[i]>=0&&a[i]<=3;}if(valid)values_.epr=a;}values_.epr_at=t;}
  if(line.starts_with("Qs425=")){values_.flaps.reset();auto v=fields<10>(line.substr(6),0,2250);if(v&&std::all_of(v->begin(),v->begin()+6,[](double n){return n<=500;})){std::array<double,4>a{};for(unsigned i=0;i<4;++i)a[i]=(*v)[6+i]/2250.;values_.flaps=a;}values_.flaps_at=t;}
  if(line.starts_with("Qs479=")){values_.spoilers.reset();auto s=line.substr(6);std::array<double,11>a{};bool valid=s.size()==22;for(unsigned i=0;valid&&i<11;++i){auto v=integer(s.substr(i*2,2));valid=v&&*v>=0&&*v<=60;if(valid)a[i]=double(*v)/60;}if(valid)values_.spoilers=a;values_.spoilers_at=t;}
  if(line.starts_with("Qs121=")){auto previous=pose_;pose_.reset();auto v=fields<7>(line.substr(6),-1e12,1e12,false,false);if(v&&std::abs((*v)[5])<=std::numbers::pi/2&&std::abs((*v)[6])<=std::numbers::pi){StatusPose p{{(*v)[5],(*v)[6]},(*v)[3]/1000,t};if(p.altitude_ft>=-2000&&p.altitude_ft<=100000){if(previous){Runway reference;reference.threshold=previous->position;auto delta=approach_point(p,reference);if(std::hypot(delta.distance_nm,delta.lateral_ft/6076.11548556)>.5||std::abs(p.altitude_ft-previous->altitude_ft)>2000)clear_trace();}pose_=p;}}}
  for(unsigned i=343;i<=349;++i)if(line.starts_with("Qs"+std::to_string(i)+"="))report(metars_,line.substr(6));
}
void StatusDisplay::sample(Tick t){if(t<next_sample_)return;next_sample_=t+1000;if(!pose_||t<pose_->at||t-pose_->at>2000)return;history_.push_back(*pose_);if(history_.size()>1800)history_.pop_front();}
std::string StatusDisplay::metar(std::string_view a)const{auto i=metars_.find(std::string(a));return i==metars_.end()?std::string{}:i->second;}
}
