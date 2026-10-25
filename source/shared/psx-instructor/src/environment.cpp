#include <psx/instructor/environment.hpp>
#include <charconv>
#include <cmath>
#include <algorithm>
#include <numbers>

namespace psx::instructor {
std::vector<WeatherEdit> windy_edits(double heading,int offset,int speed,int addition,int spread) {
  if(!std::isfinite(heading)||offset<-60||offset>60||offset%10||speed<10||speed>25||addition<0||addition>20||spread<20||spread>60||spread%10)
    throw std::invalid_argument("Invalid Windy preset inputs");
  using F=WeatherField;
  return {{F::wind_direction,wrap_degrees(std::round(heading*180/std::numbers::pi+offset))},
          {F::wind_variation,double(spread)},{F::wind_speed,double(speed)},{F::gust,double(speed+addition)}};
}
double wrap_degrees(double d) { return d - 360 * std::floor(d / 360); }
WindArc wind_arc(double first, double second) {
  if (!std::isfinite(first) || !std::isfinite(second) || first < 0 || first > 360 || second < 0 || second > 360)
    throw std::invalid_argument("Wind direction must be 0–360 degrees");
  double delta = wrap_degrees(second - first + 180) - 180;
  return {wrap_degrees(first + delta / 2), std::abs(delta)};
}
double isa_temperature(double h) { return std::round(15 - .0019812 * h); }
std::vector<WeatherEdit> minimum_cloud_spacing(const WeatherSettings &s) {
  using F=WeatherField;
  std::vector<WeatherEdit> edits;
  double highest=0;
  for(auto pair:{std::pair{F::lower_base,F::lower_top},std::pair{F::cb_base,F::cb_top}}) {
    const double top=std::max(s.value(pair.second),s.value(pair.first)+500);
    if(top>45000) throw std::invalid_argument("Startup cloud spacing exceeds PSX's cloud ceiling");
    if(top!=s.value(pair.second)) edits.push_back({pair.second,top});
    highest=std::max(highest,top);
  }
  const double base=s.value(F::upper_base), top=std::max(s.value(F::upper_top),base+500);
  const double lift=std::max(0.,highest+500-base);
  if(top+lift>45100) throw std::invalid_argument("Startup cloud spacing exceeds PSX's cloud ceiling");
  if(lift>0) edits.push_back({F::upper_base,base+lift});
  if(top+lift!=s.value(F::upper_top)) edits.push_back({F::upper_top,top+lift});
  if(!edits.empty()) s.with(edits); // Check the complete packed record before sending any edit.
  return edits;
}
std::vector<WeatherEdit> weather_preset(std::string_view name, const WeatherSettings &s, double h) {
  using F=WeatherField;
  if (name == "cavok") return {{F::lower_coverage,0},{F::upper_coverage,0},{F::cb_coverage,0},{F::visibility,9999},
    {F::precipitation,0},{F::qnh,1013},{F::surface_temperature,isa_temperature(h)},
    {F::inversion_enabled,0},{F::wind_speed,0},{F::gust,0},{F::wind_variation,0}};
  int base{}, vis{};
  if (name == "cat1") { base=200; vis=550; }
  else if (name == "cat2") { base=100; vis=300; }
  else if (name == "cat3a") { base=50; vis=200; }
  else throw std::invalid_argument("Undefined weather preset");
  int top=base+5000;
  std::vector<WeatherEdit> edits{{F::lower_base,double(base)},{F::lower_top,double(top)},{F::lower_coverage,8},
    {F::upper_coverage,0},{F::cb_coverage,0},{F::visibility,double(vis)}};
  if (s.value(F::upper_base) < top+500) {
    edits.push_back({F::upper_base,double(top+500)});
    if (s.value(F::upper_top) < top+1000) edits.push_back({F::upper_top,double(top+1000)});
  }
  return edits;
}
void Environment::record(std::string_view line) {
  flight_.record(line);
  weather_.record(line);
  events_.record(line);
  if (line == "load1" || line == "exit") { disconnected(); return; }
  if (line.starts_with("Qi129=")) { auto n=integer(line.substr(6)); if(n && *n>=0 && *n<=3) {
    if(busy() && phase_!=Phase::restoring) {
      if(!mask_echoes_.empty() && *n==mask_echoes_.front()) mask_echoes_.pop_front();
      else external_bits_|=mask_^*n;
    }
    mask_=*n;
  } }
  for (unsigned i=0;i<3;++i) {
    if (!line.starts_with("Qs"+std::to_string(123+i)+"=")) continue;
    auto text=line.substr(6); std::int64_t n{}; auto r=std::from_chars(text.data(),text.data()+text.size(),n);
    clocks_[i].reset();
    if(r.ec==std::errc{} && r.ptr==text.data()+text.size() && n>=-62135596800000LL && n<=253402300799999LL) {
      clocks_[i]=n; if(phase_==Phase::reading_time) clocks_read_[i]=true;
    } else if(phase_==Phase::reading_time) finish("indeterminate","Malformed clock readback; no retry");
  }
  if(line.starts_with("Qs121=")) {
    auto text=line.substr(6); std::array<double,7> v{}; bool good=true;
    for(unsigned i=0;i<7;++i) { auto p=text.find(';'); auto token=text.substr(0,p);
      auto r=std::from_chars(token.data(),token.data()+token.size(),v[i]);
      good=good && r.ec==std::errc{} && r.ptr==token.data()+token.size() && std::isfinite(v[i]);
      if(i<6 && p==text.npos) good=false; if(p!=text.npos) text.remove_prefix(p+1); }
    if(good && std::abs(v[5])<=std::numbers::pi/2 && std::abs(v[6])<=std::numbers::pi) position_=Point{v[5],v[6]};
    else position_.reset();
  }
  if(line.starts_with("Qs483=")) { auto n=integer(line.substr(6),5); variation_.reset(); if(n && *n>=-1800 && *n<=1800) variation_=*n/10.; }
}
void Environment::start(const EnvironmentRequest &request,int mask,std::shared_ptr<const WeatherStations> stations) {
  if(busy()) throw std::logic_error("Environment sender busy");
  // Validate before changing pause. The same WeatherSettings encoder is used by the writer.
  auto edits=request.edits;
  std::string event_packet;
  if(request.kind==EnvironmentRequest::Kind::events) event_packet=events_.prepare(request.events);
  if(request.kind==EnvironmentRequest::Kind::weather) {
    auto &base=weather_.settings(request.target);
    if(!base) throw std::invalid_argument("Weather settings unavailable");
    if(!request.preset.empty()) {
      if(request.target==WeatherTarget::zone1 && !weather_.zone()) throw std::invalid_argument("Airport reference elevation unavailable");
      edits=weather_preset(request.preset,*base,request.target!=WeatherTarget::zone1 ? 0 : weather_.zone()->elevation);
    }
    if(edits.empty()) throw std::invalid_argument("No weather edits");
    auto next=base->with(edits);
    using F=WeatherField;
    auto gap=[&](F top,F bottom){return next.value(top)-next.value(bottom)>=500;};
    for(const auto &edit:edits) {
      bool valid=true;
      switch(edit.field) {
      case F::lower_base: valid=gap(F::lower_top,F::lower_base);break;
      case F::lower_top: valid=gap(F::lower_top,F::lower_base)&&gap(F::upper_base,F::lower_top);break;
      case F::upper_base: valid=gap(F::upper_top,F::upper_base)&&gap(F::upper_base,F::lower_top)&&(next.value(F::cb_coverage)==0||gap(F::upper_base,F::cb_top));break;
      case F::upper_top: valid=gap(F::upper_top,F::upper_base);break;
      case F::cb_base: valid=gap(F::cb_top,F::cb_base);break;
      case F::cb_top: valid=gap(F::cb_top,F::cb_base)&&(next.value(F::cb_coverage)==0||gap(F::upper_base,F::cb_top));break;
      default:break;
      }
      if(!valid) throw std::invalid_argument("Cloud boundaries require at least 500 ft separation");
    }
  }
  if(request.kind==EnvironmentRequest::Kind::nearest && (!stations || !weather_.zone()))
    throw std::invalid_argument("Nearest weather station unavailable");
  if(request.kind==EnvironmentRequest::Kind::time && (request.utc_ms < -62135596800000LL || request.utc_ms>253402300799999LL))
    throw std::invalid_argument("Time outside supported calendar");
  entry_mask_=mask_=mask; temporary_mask_=2; external_bits_=0; mask_echoes_.clear();
  temporary_pause_=mask!=0 || request.kind==EnvironmentRequest::Kind::time;
  state_="sent"; detail_=temporary_pause_ ? "Sending with motion frozen" : "Sending while PSX runs";
  stations_=std::move(stations); phase_=Phase::weather;
  if(temporary_pause_) {
    mask_echoes_.push_back(mask|2); if((mask|2)!=2) mask_echoes_.push_back(2);
    auto prefix="Qi129="+std::to_string(mask|2)+"\n";
    if((mask|2)!=2) prefix+="Qi129=2\n";
    if(!writer_.send(prefix)) { finish("indeterminate","Pause/freeze transmission failed"); return; }
  }
  try {
    if(request.kind==EnvironmentRequest::Kind::time) {
      expected_time_=request.utc_ms; clocks_read_={}; clock_sent_=now(); phase_=Phase::sending_time;
      std::string packet="Qi139=0\n";
      for(unsigned i=0;i<3;++i) packet+="Qs"+std::to_string(123+i)+"="+std::to_string(expected_time_)+"\n";
      if(!writer_.send(packet)) finish("indeterminate","Time transmission failed; no retry");
    } else if(request.kind==EnvironmentRequest::Kind::events) {phase_=Phase::events;events_.start(event_packet);}
    else if(request.kind==EnvironmentRequest::Kind::manual) weather_.select_manual();
    else if(request.kind==EnvironmentRequest::Kind::nearest) {
      if(!writer_.send("Qi139=0\n")) { finish("indeterminate","Manual-mode transmission failed"); return; }
      weather_.set_nearest_airport(*stations_);
    } else weather_.apply(request.target,edits,true);
  } catch(const std::exception &e) { finish("failed",e.what()); }
}
void Environment::finish(std::string state,std::string detail) {
  state_=std::move(state); detail_=std::move(detail);
  if(!temporary_pause_) { phase_=Phase::idle; return; }
  int restore=mask_;
  for(int bit:{1,2}) if(!(external_bits_&bit) && (mask_&bit)==(temporary_mask_&bit)) restore=(restore&~bit)|(entry_mask_&bit);
  if(writer_.state()==network::State::connected) {
    if(!writer_.send("Qi129="+std::to_string(restore)+"\n")) detail_+="; pause/freeze restoration transmission unavailable";
    phase_=Phase::restoring; deadline_=now()+1000;
  } else { detail_+="; disconnected, pause/freeze restoration unavailable"; phase_=Phase::idle; }
}
void Environment::pump(Tick t) {
  if(!busy()) return;
  if(writer_.state()!=network::State::connected || reader_.state()!=network::State::connected) { disconnected(); return; }
  if(phase_==Phase::restoring) { if(!writer_.sending() || t>=deadline_) phase_=Phase::idle; return; }
  if(phase_==Phase::events) {
    events_.pump(t);
    if(!events_.busy()) {const auto &r=events_.result();finish(r.status==SettingsStatus::match?"observed":r.status==SettingsStatus::different?"failed":"indeterminate",r.detail);}
  } else if(phase_==Phase::weather) {
    weather_.pump(t);
    if(!weather_.busy()) {
      auto &r=weather_.result(); std::string detail=r.detail;
      for(auto &d:r.differences) detail+="; "+d.field+": requested "+d.requested+", returned "+d.returned;
      finish(r.status==WeatherStatus::match ? "observed" : r.status==WeatherStatus::different ? "failed" : "indeterminate",detail);
    }
  } else if(phase_==Phase::sending_time && !writer_.sending()) { phase_=Phase::waiting_time; deadline_=t+1000; clock_sent_=t; }
  else if(phase_==Phase::waiting_time && t>=deadline_) {
    phase_=Phase::reading_time; deadline_=t+5000; clocks_read_={};
    if(!reader_.send("bang\n")) finish("indeterminate","Clock readback unavailable; no retry");
  } else if(phase_==Phase::reading_time) {
    if(std::all_of(clocks_read_.begin(),clocks_read_.end(),[](bool b){return b;})) {
      bool same=true; auto elapsed=static_cast<std::int64_t>(t-clock_sent_);
      for(auto &clock:clocks_) same=same && *clock>=expected_time_-1000 && *clock<=expected_time_+elapsed+1000;
      finish(same ? "observed" : "failed",same ? "Earth and pilot-clock settings match, allowing normal advancement; visible daylight unverified" : "Clock settings differ; no retry");
    } else if(t>=deadline_) finish("indeterminate","Clock readback unavailable within five seconds; no retry");
  }
}
void Environment::abort(std::string_view why) { if(busy() && phase_!=Phase::restoring) { weather_.cancel(); events_.cancel(); finish("indeterminate",std::string(why)); } }
void Environment::disconnected() {
  weather_.disconnected(); events_.disconnected(); flight_.reset(); clocks_={}; position_.reset(); variation_.reset();
  if(busy()) { state_="indeterminate"; detail_="Disconnected; settings may have been sent; restoration unavailable; no replay"; phase_=Phase::idle; }
}
}
