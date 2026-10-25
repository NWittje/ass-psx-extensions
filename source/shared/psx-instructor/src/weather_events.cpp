#include <psx/instructor/weather_events.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <locale>
#include <numbers>
#include <sstream>
namespace psx::instructor {
namespace {
std::size_t aloft_tail(std::string_view s) {
  if(s.size()<30||s.size()>80||s.find_first_of("\r\n\0",0,3)!=s.npos) throw std::invalid_argument("Malformed upper weather record");
  const auto a=s.find(';'),b=a==s.npos?s.npos:s.find(';',a+1);
  if(a<16||b==s.npos||s.find(';',b+1)!=s.npos||b+3>=s.size()||
     (s[b+1]!='c'&&s[b+1]!='C')||(s[b+2]!='v'&&s[b+2]!='V')) throw std::invalid_argument("Malformed upper weather record");
  if(!integer(s.substr(a+1,b-a-1))) throw std::invalid_argument("Malformed upper wind peak");
  std::istringstream input{std::string(s.substr(b+3))};input.imbue(std::locale::classic());
  char ns{},ew{};int lat{},lon{};double lm{},om{};
  if(!(input>>ns>>lat>>lm>>ew>>lon>>om)||!std::isfinite(lm)||!std::isfinite(om)||
    (ns!='N'&&ns!='S')||(ew!='E'&&ew!='W')||lat<0||lat>90||lon<0||lon>180||lm<0||lm>=60||om<0||om>=60||
    (lat==90&&lm!=0)||(lon==180&&om!=0)) throw std::invalid_argument("Malformed ash coordinates");
  input>>std::ws;if(!input.eof())throw std::invalid_argument("Malformed ash coordinates");
  return b;
}
}
Point ash_destination(Point p,double heading) {
  if(!std::isfinite(p.lat)||!std::isfinite(p.lon)||!std::isfinite(heading)||std::abs(p.lat)>std::numbers::pi/2||std::abs(p.lon)>std::numbers::pi)
    throw std::invalid_argument("Aircraft position or direction unavailable");
  const double a=210./3440.065;
  const double lat=std::asin(std::clamp(std::sin(p.lat)*std::cos(a)+std::cos(p.lat)*std::sin(a)*std::cos(heading),-1.,1.));
  double lon=p.lon+std::atan2(std::sin(heading)*std::sin(a)*std::cos(p.lat),std::cos(a)-std::sin(p.lat)*std::sin(lat));
  lon=std::remainder(lon,2*std::numbers::pi);
  return {lat,lon};
}
std::string ash_coordinate(Point p) {
  auto parts=[](double radians) {auto tenths=std::llround(std::abs(radians)*180/std::numbers::pi*600);return std::pair{int(tenths/600),int(tenths%600)};};
  auto [lat,lm]=parts(p.lat);auto [lon,om]=parts(p.lon);char b[40]{};
  std::snprintf(b,sizeof(b),"%c%02d %02d.%d %c%03d %02d.%d",p.lat<0?'S':'N',lat,lm/10,lm%10,p.lon<0?'W':'E',lon,om/10,om%10);
  return b;
}
void WeatherEvents::disconnected() {exchange_.cancel();flight_.reset();zones_={};focused_.reset();aloft_.reset();expected_zone_.reset();expected_aloft_.reset();}
std::optional<bool> WeatherEvents::ash_on() const {return aloft_?std::optional<bool>{(*aloft_)[aloft_tail(*aloft_)+2]=='V'}:std::nullopt;}
std::string WeatherEvents::prepare(const WeatherEventRequest &r,Tick t) {
  if(busy())throw std::logic_error("Weather events sender busy");
  if(!r.shear_mode&&!r.ash)throw std::invalid_argument("No weather event selected");
  expected_zone_.reset();expected_aloft_.reset();
  std::string packet;
  if(r.shear_mode) {
    if(*r.shear_mode<0||*r.shear_mode>3||r.shear_top_ft<100||r.shear_top_ft>1300||r.shear_top_ft%100)throw std::invalid_argument("Windshear top must be 100 to 1300 ft, in 100 ft steps");
    if(!focused_||!zones_[*focused_])throw std::invalid_argument("Active PSX weather zone unavailable");
    target_=*focused_;const auto &s=*zones_[target_];
    if(*r.shear_mode && s.value(WeatherField::lower_coverage)==0&&s.value(WeatherField::cb_coverage)==0)
      throw std::invalid_argument("no compatible cloud layer for windshear");
    auto tokens=s.tokens();tokens[12]=std::to_string(*r.shear_mode);tokens[13]="0";tokens[14]=std::to_string(r.shear_top_ft);
    std::string packed;for(const auto &token:tokens){if(!packed.empty())packed+=';';packed+=token;}
    expected_zone_=WeatherSettings::parse(packed);
    packet="Qs"+std::to_string(328+target_)+'='+packed+'\n';
  }
  if(r.ash) {
    if(!aloft_)throw std::invalid_argument("Upper weather settings unavailable");
    auto b=aloft_tail(*aloft_);auto packed=*aloft_;packed[b+2]=*r.ash?'V':'v';
    if(*r.ash) {
      if(!flight_.position())throw std::invalid_argument("Aircraft position unavailable");
      auto direction=flight_.track(t);if(!direction)direction=flight_.heading();
      if(!direction)throw std::invalid_argument("Aircraft direction unavailable");
      packed.resize(b+3);packed+=ash_coordinate(ash_destination(*flight_.position(),*direction));
    }
    aloft_tail(packed);expected_aloft_=packed;packet+="Qs327="+packed+'\n';
  }
  zone_seen_=!expected_zone_;ash_seen_=!expected_aloft_;different_=false;
  return packet;
}
void WeatherEvents::start(std::string_view packet,Tick t) {exchange_.begin(packet,"Edited windshear/ash settings",t);}
void WeatherEvents::compare() {
  if(!zone_seen_||!ash_seen_)return;
  exchange_.finish(different_?SettingsStatus::different:SettingsStatus::match,different_?"Weather event settings differ; no retry":"Configured weather event settings match; encounter application unverified");
}
void WeatherEvents::record(std::string_view line,Tick t) {
  if(line=="load1"||line=="exit"){disconnected();return;}
  flight_.record(line,t);
  if(line.starts_with("Qi240=")){auto n=integer(line.substr(6));focused_.reset();if(n&&*n>=0&&*n<=7)focused_=unsigned(*n);return;}
  if(line.starts_with("Qs327=")) {
    aloft_.reset();try{aloft_tail(line.substr(6));aloft_=line.substr(6);}catch(const std::invalid_argument&){if(exchange_.reading()&&expected_aloft_)exchange_.finish(SettingsStatus::unavailable,"Malformed ash readback; no retry");return;}
    if(exchange_.reading()&&expected_aloft_){ash_seen_=true;different_=different_||*aloft_!=*expected_aloft_;compare();}return;
  }
  if(!line.starts_with("Qs"))return;
  auto p=line.find('=');if(p==line.npos)return;auto id=integer(line.substr(2,p-2));if(!id||*id<328||*id>335)return;
  const unsigned target=unsigned(*id-328);zones_[target].reset();
  try{zones_[target]=WeatherSettings::parse(line.substr(p+1));}
  catch(const std::invalid_argument&){if(exchange_.reading()&&expected_zone_&&target==target_)exchange_.finish(SettingsStatus::unavailable,"Malformed windshear readback; no retry");return;}
  if(exchange_.reading()&&expected_zone_&&target==target_) {
    zone_seen_=true;for(unsigned i:{12u,13u,14u})different_=different_||zones_[target]->tokens()[i]!=expected_zone_->tokens()[i];compare();
  }
}
}
