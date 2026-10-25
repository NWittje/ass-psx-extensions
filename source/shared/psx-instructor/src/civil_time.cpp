#include <psx/instructor/civil_time.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>
#include <set>
#include <charconv>
#include <array>

namespace psx::instructor {
namespace {
struct Reader {
  std::string bytes; std::size_t p{};
  std::uint32_t u() { if(p+4>bytes.size()) throw std::invalid_argument("Truncated time data"); std::uint32_t n{}; for(int i=0;i<4;++i) n|=std::uint32_t(static_cast<unsigned char>(bytes[p++]))<<(i*8); return n; }
  std::string text() { auto n=u(); if(n>bytes.size()-p) throw std::invalid_argument("Invalid time data length"); auto s=bytes.substr(p,n); p+=n; return s; }
  float f() { return std::bit_cast<float>(u()); }
};
std::int64_t be(std::string_view s,std::size_t p,int size) {
  if(p+size>s.size()) throw std::invalid_argument("Truncated TZif"); std::uint64_t n{};
  for(int k=0;k<size;++k) n=(n<<8)|static_cast<unsigned char>(s[p+k]);
  return size==4 ? static_cast<std::int32_t>(n) : std::bit_cast<std::int64_t>(n);
}
struct Rule { int kind{}, a{},b{},c{},second{7200}; };
struct Future { int standard{}, daylight{}; Rule start,end; bool dst{}; };
Future posix(std::string_view s) {
  std::size_t p{};
  auto number=[&]() { auto begin=p; while(p<s.size() && s[p]>='0' && s[p]<='9') ++p; int n{};
    if(begin==p || std::from_chars(s.data()+begin,s.data()+p,n).ec!=std::errc{}) throw std::invalid_argument("Invalid timezone future rule"); return n; };
  auto name=[&]() { if(p<s.size() && s[p]=='<') { auto end=s.find('>',++p); if(end==s.npos) throw std::invalid_argument("Timezone name"); p=end+1; }
    else { auto first=p; while(p<s.size() && ((s[p]>='A'&&s[p]<='Z')||(s[p]>='a'&&s[p]<='z'))) ++p; if(first==p) throw std::invalid_argument("Timezone name"); } };
  auto offset=[&]() { int sign=1; if(p<s.size() && (s[p]=='-'||s[p]=='+')) { if(s[p]=='-') sign=-1; ++p; }
    int n=number()*3600; if(p<s.size() && s[p]==':') { ++p; n+=number()*60; if(p<s.size() && s[p]==':') { ++p; n+=number(); } } return sign*n; };
  auto rule=[&]() { Rule r; if(p<s.size() && s[p]=='M') { ++p; r.kind=1; r.a=number(); if(p>=s.size()||s[p++]!='.') throw std::invalid_argument("Timezone month rule"); r.b=number(); if(p>=s.size()||s[p++]!='.') throw std::invalid_argument("Timezone weekday rule"); r.c=number(); }
    else { if(p<s.size() && s[p]=='J') { r.kind=2; ++p; } r.a=number(); }
    if(p<s.size() && s[p]=='/') { ++p; r.second=offset(); } return r; };
  Future f; name(); f.standard=-offset();
  if(p==s.size()) return f;
  name(); f.dst=true; f.daylight=f.standard+3600;
  if(p<s.size() && s[p]!=',') f.daylight=-offset();
  if(p>=s.size()||s[p++]!=',') throw std::invalid_argument("Timezone DST rule missing");
  f.start=rule(); if(p>=s.size()||s[p++]!=',') throw std::invalid_argument("Timezone DST end missing"); f.end=rule();
  if(p!=s.size()) throw std::invalid_argument("Timezone trailing rule"); return f;
}
std::int64_t transition(int year,Rule r,int previous) {
  using namespace std::chrono;
  sys_days day=std::chrono::year{year}/January/1;
  if(r.kind==1) {
    if(r.a<1||r.a>12||r.b<1||r.b>5||r.c<0||r.c>6) throw std::invalid_argument("Invalid timezone month rule");
    sys_days first=std::chrono::year{year}/month{static_cast<unsigned>(r.a)}/1;
    int delta=(r.c-int(weekday{first}.c_encoding())+7)%7+7*(r.b-1);
    day=first+days{delta}; if(unsigned(year_month_day{day}.month())!=unsigned(r.a)) day-=days{7};
  } else { int delta=r.a-(r.kind==2 ? 1:0); if(r.kind==2 && std::chrono::year{year}.is_leap() && r.a>=60) ++delta; day+=days{delta}; }
  return duration_cast<seconds>(day.time_since_epoch()).count()+r.second-previous;
}
struct Zone {
  std::string name; std::vector<std::int64_t> transitions; std::vector<unsigned> indices; std::vector<int> offsets; std::optional<Future> future;
  explicit Zone(std::string n,std::string_view bytes) : name(std::move(n)) {
    auto counts=[&](std::size_t p) { std::array<std::size_t,6> c{}; if(bytes.substr(p,4)!="TZif") throw std::invalid_argument("Invalid TZif"); for(unsigned i=0;i<6;++i) c[i]=static_cast<std::size_t>(be(bytes,p+20+i*4,4)); return c; };
    auto c=counts(0); std::size_t p=44; int size=4;
    if(bytes[4]!='\0') { p+=c[3]*5+c[4]*6+c[5]+c[2]*8+c[1]+c[0]; c=counts(p); p+=44; size=8; }
    if(c[3]>100000 || c[4]>256 || !c[4]) throw std::invalid_argument("TZif bounds");
    for(std::size_t i=0;i<c[3];++i) { transitions.push_back(be(bytes,p,size)); p+=size; }
    for(std::size_t i=0;i<c[3];++i) { if(p>=bytes.size()) throw std::invalid_argument("TZif indices"); auto v=static_cast<unsigned char>(bytes[p++]); if(v>=c[4]) throw std::invalid_argument("TZif type"); indices.push_back(v); }
    for(std::size_t i=0;i<c[4];++i) { offsets.push_back(static_cast<int>(be(bytes,p,4))); p+=6; }
    p+=c[5]+c[2]*(size+4)+c[1]+c[0];
    if(size==8 && p<bytes.size() && bytes[p]=='\n') { auto e=bytes.find('\n',p+1); if(e!=bytes.npos && e>p+1) future=posix(bytes.substr(p+1,e-p-1)); }
  }
  int offset(std::int64_t seconds) const {
    if(future && (transitions.empty() || seconds>transitions.back())) {
      auto &f=*future; if(!f.dst) return f.standard;
      int year=CivilTime::date(seconds*1000).year; auto a=transition(year,f.start,f.standard), b=transition(year,f.end,f.daylight);
      bool dst=a<b ? seconds>=a && seconds<b : seconds>=a || seconds<b;
      return dst ? f.daylight : f.standard;
    }
    auto i=std::upper_bound(transitions.begin(),transitions.end(),seconds);
    return offsets[i==transitions.begin() ? 0 : indices[static_cast<std::size_t>(i-transitions.begin()-1)]];
  }
};
struct XY { float x{},y{}; };
struct Polygon {
  unsigned zone{}; float x0{180},y0{90},x1{-180},y1{-90}; std::vector<std::vector<XY>> rings;
  bool contains(double x,double y) const {
    if(x<x0||x>x1||y<y0||y>y1) return false;
    bool result=false;
    for(auto &ring:rings) { bool in=false; for(std::size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
      auto a=ring[i], b=ring[j]; if((a.y>y)!=(b.y>y) && x<(b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x) in=!in;
    } if(in) result=!result; }
    return result;
  }
};
}
struct CivilTime::Data {
  std::vector<Zone> zones; std::vector<Polygon> polygons; std::array<std::vector<unsigned>,72*36> grid;
  const Zone &zone(Point point) const {
    double x=point.lon*180/std::numbers::pi,y=point.lat*180/std::numbers::pi;
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>180||std::abs(y)>90) throw std::invalid_argument("Invalid timezone position");
    int gx=std::clamp(static_cast<int>((x+180)/5),0,71),gy=std::clamp(static_cast<int>((y+90)/5),0,35);
    for(auto n:grid[gy*72+gx]) if(polygons[n].contains(x,y)) return zones[polygons[n].zone];
    throw std::runtime_error("Civil timezone unavailable at this location");
  }
};
CivilTime CivilTime::load(const std::filesystem::path &path) {
  if(!std::filesystem::exists(path) || std::filesystem::file_size(path)>512*1024*1024) throw std::runtime_error("Local civil-time dataset unavailable");
  std::ifstream f(path,std::ios::binary); Reader r; r.bytes.resize(static_cast<std::size_t>(std::filesystem::file_size(path)));
  if(!f.read(r.bytes.data(),static_cast<std::streamsize>(r.bytes.size()))) throw std::runtime_error("Civil-time dataset read failed");
  if(r.bytes.substr(0,8)!="IOSTZ001") throw std::invalid_argument("Invalid civil-time dataset"); r.p=8; r.text();
  auto data=std::make_shared<Data>(); auto count=r.u(); if(count>1000) throw std::invalid_argument("Too many timezones");
  for(unsigned i=0;i<count;++i) { auto name=r.text(); auto bytes=r.text(); data->zones.emplace_back(name,bytes); }
  count=r.u(); if(count>100000) throw std::invalid_argument("Too many timezone polygons");
  for(unsigned i=0;i<count;++i) {
    Polygon poly; poly.zone=r.u(); if(poly.zone>=data->zones.size()) throw std::invalid_argument("Timezone index"); auto rings=r.u(); if(!rings||rings>100000) throw std::invalid_argument("Timezone rings");
    for(unsigned k=0;k<rings;++k) { auto points=r.u(); if(points<4||points>10000000) throw std::invalid_argument("Timezone points"); std::vector<XY> ring; ring.reserve(points);
      for(unsigned n=0;n<points;++n) { XY p{r.f(),r.f()}; if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>180.001||std::abs(p.y)>90.001) throw std::invalid_argument("Timezone geometry"); ring.push_back(p); poly.x0=std::min(poly.x0,p.x); poly.x1=std::max(poly.x1,p.x); poly.y0=std::min(poly.y0,p.y); poly.y1=std::max(poly.y1,p.y); }
      poly.rings.push_back(std::move(ring)); }
    for(int y=std::clamp(int((poly.y0+90)/5),0,35);y<=std::clamp(int((poly.y1+90)/5),0,35);++y)
      for(int x=std::clamp(int((poly.x0+180)/5),0,71);x<=std::clamp(int((poly.x1+180)/5),0,71);++x) data->grid[y*72+x].push_back(i);
    data->polygons.push_back(std::move(poly));
  }
  if(r.p!=r.bytes.size()) throw std::invalid_argument("Civil-time trailing data"); CivilTime t; t.data_=std::move(data); return t;
}
CivilDate CivilTime::date(std::int64_t ms) {
  using namespace std::chrono; sys_time<std::chrono::milliseconds> t{std::chrono::milliseconds{ms}}; auto day=floor<days>(t); year_month_day d{day}; auto minute=duration_cast<minutes>(t-day).count();
  return {int(d.year()),int(unsigned(d.month())),int(unsigned(d.day())),int(minute/60),int(minute%60)};
}
std::int64_t CivilTime::milliseconds(CivilDate d) {
  using namespace std::chrono; year_month_day y{std::chrono::year{d.year},month{static_cast<unsigned>(d.month)},day{static_cast<unsigned>(d.day)}};
  if(!y.ok()||d.year<1||d.year>9999||d.hour<0||d.hour>23||d.minute<0||d.minute>59) throw std::invalid_argument("Invalid civil date/time");
  return duration_cast<std::chrono::milliseconds>(sys_days{y}.time_since_epoch()+hours{d.hour}+minutes{d.minute}).count();
}
CivilClock CivilTime::local(Point p,std::int64_t utc) const {
  if(!data_) throw std::runtime_error("Civil-time dataset unavailable"); auto &z=data_->zone(p);
  // TZif transitions start on a whole second. Floor pre-epoch milliseconds so
  // the final fractional second before a transition keeps its preceding offset.
  const auto seconds=std::chrono::floor<std::chrono::seconds>(std::chrono::milliseconds{utc}).count();
  int offset=z.offset(seconds); return {date(utc+offset*1000LL),z.name,offset};
}
std::vector<std::int64_t> CivilTime::utc(Point p,CivilDate local) const {
  if(!data_) throw std::runtime_error("Civil-time dataset unavailable"); auto &z=data_->zone(p); auto ms=milliseconds(local);
  std::set<int> offsets(z.offsets.begin(),z.offsets.end()); if(z.future) { offsets.insert(z.future->standard); if(z.future->dst) offsets.insert(z.future->daylight); }
  std::vector<std::int64_t> out; for(int offset:offsets) { auto t=ms-offset*1000LL; if(z.offset(t/1000)==offset) out.push_back(t); } std::sort(out.begin(),out.end()); return out;
}
std::optional<std::int64_t> CivilTime::solar(Point p,CivilDate day,bool sunset) const {
  // NOAA fractional-year equations, 90.833-degree apparent zenith.
  using namespace std::chrono; day.hour=12; day.minute=0; auto civil=milliseconds(day);
  auto d=floor<days>(sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{civil}});
  auto n=(d-sys_days{std::chrono::year{day.year}/January/1}).count()+1;
  double g=2*std::numbers::pi/(std::chrono::year{day.year}.is_leap()?366:365)*(n-1);
  double eq=229.18*(.000075+.001868*std::cos(g)-.032077*std::sin(g)-.014615*std::cos(2*g)-.040849*std::sin(2*g));
  double dec=.006918-.399912*std::cos(g)+.070257*std::sin(g)-.006758*std::cos(2*g)+.000907*std::sin(2*g)-.002697*std::cos(3*g)+.00148*std::sin(3*g);
  double c=(std::cos(90.833*std::numbers::pi/180)/(std::cos(p.lat)*std::cos(dec))-std::tan(p.lat)*std::tan(dec));
  if(!std::isfinite(c)||c>1||c< -1) return {};
  double hour=std::acos(c)*180/std::numbers::pi;
  double minutes=720-4*(p.lon*180/std::numbers::pi+(sunset?-hour:hour))-eq;
  auto midnight=duration_cast<std::chrono::milliseconds>(d.time_since_epoch()).count();
  auto event=midnight+static_cast<std::int64_t>(std::round(minutes*60000));
  // UTC day can differ from local date; select the adjacent event with this civil date.
  for(int shift:{0,-1,1}) { auto candidate=event+shift*86400000LL; auto localDate=local(p,candidate).date;
    if(localDate.year==day.year&&localDate.month==day.month&&localDate.day==day.day) return candidate; }
  return {};
}
}
