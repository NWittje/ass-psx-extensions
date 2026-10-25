#include <psx/instructor/weight_service.hpp>
#include <cmath>
#include <climits>
#include <stdexcept>
namespace psx::instructor {
WeightField weight_field(std::string_view s) {
  if(s=="zfw")return WeightField::zfw;
  if(s=="gw_cg")return WeightField::gw_cg;
  if(s=="ground_power")return WeightField::ground_power;
  if(s=="ground_air")return WeightField::ground_air;
  if(s=="engine_model")return WeightField::engine_model;
  if(s=="fifth_engine")return WeightField::fifth_engine;
  throw std::invalid_argument("Unknown weight/service field");
}
double display_weight(double pounds,bool imperial) { return imperial?pounds:pounds*kg_per_lb; }
int native_weight(double shown,bool imperial) {
  if(!std::isfinite(shown)||shown<0||shown>1000000||std::trunc(shown)!=shown)throw std::invalid_argument("Enter a whole weight");
  return int(std::round(imperial?shown:shown/kg_per_lb));
}
int minimum_zfw(const WeightState &s) {return s.aero_bits&&(*s.aero_bits&4)?366427:352739;}
void validate_weight(WeightField field,double n) {
  if(!std::isfinite(n)||std::trunc(n)!=n)throw std::invalid_argument("Invalid native value");
  const auto low=field==WeightField::zfw?352739:field==WeightField::gw_cg?85:0;
  const auto high=field==WeightField::zfw?639340:field==WeightField::gw_cg?330:field==WeightField::engine_model?2:1;
  if(n<low||n>high)throw std::invalid_argument("Value outside PSX limits");
}
std::string weight_packet(WeightField field,double n,const WeightState &s) {
  validate_weight(field,n);int value=int(n),id{};
  auto bits=[&](const std::optional<int>& observed,int mask) {
    if(!observed)throw std::runtime_error("PSX setting unavailable");
    return value?(*observed|mask):(*observed&~mask);
  };
  switch(field) {
    case WeightField::zfw:if(!s.aero_bits)throw std::runtime_error("Fifth-engine setting unavailable");if(value<minimum_zfw(s))throw std::invalid_argument("ZFW below fifth-engine minimum");id=123;break;
    case WeightField::gw_cg:id=122;break;
    case WeightField::ground_power:id=132;value=bits(s.power_bits,9);break;
    case WeightField::ground_air:id=174;value=bits(s.air_bits,1);break;
    case WeightField::engine_model:id=1;break;
    case WeightField::fifth_engine:if(!s.engine_model)throw std::runtime_error("Engine model unavailable");if(value&&*s.engine_model==0)throw std::invalid_argument("Fifth-engine carriage unavailable with CF6");id=184;value=bits(s.aero_bits,4);break;
  }
  return "Qi"+std::to_string(id)+'='+std::to_string(value)+'\n';
}
std::optional<double> fuel_quantity(std::string_view s) {
  // FuelQty: P/d prefix, nine decipound tanks, whole-pound preselect, temperature.
  if(s.empty()||(s.front()!='P'&&s.front()!='d'))return {};
  s.remove_prefix(1);double total{};
  for(unsigned i=0;i<11;++i) {
    const auto end=s.find(';');if(end==s.npos)return {};
    auto n=integer(s.substr(0,end));if(!n||(i<10&&*n<0)||(i<9&&*n>4000000))return {};
    if(i<9)total+=*n;
    s.remove_prefix(end+1);
  }
  if(!s.empty())return {};return total*.1;
}
void WeightService::set(WeightField field,double value,Tick t) {
  const auto packet=weight_packet(field,value,state_);
  const auto equal=packet.find('=');expected_=*integer(std::string_view(packet).substr(equal+1,packet.size()-equal-2));
  edited_=field;editing_=true;exchange_.begin(packet,std::to_string(expected_),t);
}
void WeightService::record(std::string_view line) {
  if(line=="load1"||line=="exit"){disconnected();return;}
  if(line.starts_with("Qs438=")){state_.fuel_lb=fuel_quantity(line.substr(6));return;}
  std::optional<int>*out=nullptr;WeightField field=WeightField::zfw;int low=0,high=INT_MAX;
  if(line.starts_with("Qi123=")){out=&state_.zfw_lb;field=WeightField::zfw;low=352739;high=639340;}
  else if(line.starts_with("Qi122=")){out=&state_.gw_cg10;field=WeightField::gw_cg;low=85;high=330;}
  else if(line.starts_with("Qi132=")){out=&state_.power_bits;field=WeightField::ground_power;low=INT_MIN;}
  else if(line.starts_with("Qi174=")){out=&state_.air_bits;field=WeightField::ground_air;low=INT_MIN;}
  else if(line.starts_with("Qi1=")){out=&state_.engine_model;field=WeightField::engine_model;high=2;}
  else if(line.starts_with("Qi184=")){out=&state_.aero_bits;field=WeightField::fifth_engine;high=7;}
  if(!out)return;auto n=integer(line.substr(line.find('=')+1));if(n&&(*n<low||*n>high))n.reset();*out=n;
  if(editing_&&exchange_.reading()&&field==edited_) {
    const int mask=field==WeightField::ground_power?9:field==WeightField::ground_air?1:field==WeightField::fifth_engine?4:-1;
    const bool match=n&&(*n&mask)==(expected_&mask);
    exchange_.finish(!n?SettingsStatus::unavailable:match?SettingsStatus::match:SettingsStatus::different,
      !n?"PSX readback unavailable":match?"Setting updated":"PSX returned a different value",n?std::to_string(*n):"");editing_=false;
  }
}
}
