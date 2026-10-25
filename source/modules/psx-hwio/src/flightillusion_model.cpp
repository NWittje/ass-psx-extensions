#include "hwio/flightillusion.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace hwio::flightillusion {
namespace {
// Cardinal captures may cross the device-coordinate origin. Only a single,
// consistently directed revolution is a valid compass calibration.
int compass_direction(const Channel& c,const Calibration& cal)noexcept {
    if(cal.points.size()!=4)return 0;
    const int period=c.native_max-c.native_min;if(period<=0)return 0;
    int total=0;
    for(std::size_t n=0;n<4;++n){
        int delta=(cal.points[(n+1)%4].position-cal.points[n].position)%period;
        if(delta<0)delta+=period;
        if(!delta)return 0;
        total+=delta;
    }
    return total==period?1:total==3*period?-1:0;
}
}
std::optional<Command> encode(unsigned a,unsigned c,int v)noexcept {
    if(a<1||a>255||c>15||v< -65535||v>65535)return {};
    const auto m=unsigned(v<0?-v:v),lo=m&255,hi=m>>8;
    return Command{0,std::uint8_t(a),std::uint8_t((c<<4)|8|(v<0?4:0)|(lo&1)|(hi&2)),std::uint8_t(lo|1),std::uint8_t(hi|2),255};
}
Command identify(unsigned a){if(a<1||a>255)throw std::invalid_argument("Instrument address must be 1..255");return {0,std::uint8_t(a),0x7c,0x80,0xaa,0xff};}
std::vector<Reply> Framer::feed(std::span<const std::uint8_t> data){
    std::vector<Reply> out;
    for(auto b:data){if(!size_&&b!=0)continue;bytes_[size_++]=b;if(size_!=bytes_.size())continue;
        if(bytes_[24]==255&&bytes_[1]!=0){out.push_back(bytes_);size_=0;}
        else {auto next=std::find(bytes_.begin()+1,bytes_.end(),0);size_=std::size_t(bytes_.end()-next);std::move(next,bytes_.end(),bytes_.begin());}}
    return out;
}
std::string profile_name(Profile p){switch(p){case Profile::altimeter:return "Five-digit altimeter";case Profile::attitude:return "Attitude indicator (aircraft attitude)";case Profile::airspeed:return "Airspeed + barber pole";case Profile::brake:return "Brake pressure";case Profile::compass:return "Wet compass";case Profile::clock_captain:return "Captain clock";case Profile::clock_fo:return "First Officer clock";default:return "Unassigned";}}
std::string clock_source_name(ClockSource s){switch(s){case ClockSource::psx_utc:return "clock shows psx UTC";case ClockSource::local_utc:return "clock shows local UTC";case ClockSource::local_pc:return "clock shows local PC time";}return "Unknown clock source";}
std::string hardware_name(unsigned t){switch(t){case 1:return "Small gauge, single needle";case 2:return "Small gauge, single needle + display";case 3:return "Small gauge, dual needle";case 4:return "Large gauge, single needle";case 5:return "Large gauge, concentric dual needle";case 6:return "Small gauge, concentric dual needle";case 101:return "Digital/analogue altimeter";case 103:return "Attitude indicator";case 104:return "Boeing clock";case 105:return "Heading indicator/directional gyro";case 109:return "Cessna clock";default:return "Unknown hardware type "+std::to_string(t);}}
bool compatible(Profile p,unsigned t)noexcept {switch(p){case Profile::unassigned:return true;case Profile::altimeter:return t==101;case Profile::attitude:return t==103;case Profile::airspeed:return t==3||t==5||t==6;case Profile::brake:return t==1||t==4;case Profile::compass:return t==1||t==4;case Profile::clock_captain:case Profile::clock_fo:return t==104||t==109;}return false;}
Capabilities capabilities(unsigned type)noexcept {return {type==1||type==3||type==4||type==5||type==6||type==103,type==101,type==101,type==104,type==104,false};}
std::vector<Channel> channels(Profile p){switch(p){
case Profile::altimeter:return {{"Altitude (firmware owned)","ft",-9999,50000,-9999,50000,4,false,true}};
case Profile::attitude:return {{"Needle 1: bank","deg",-180,180,0,3840,4},{"Needle 2: pitch","deg",-90,90,0,640,5}};
case Profile::airspeed:return {{"Needle 1: barber pole","kt",0,450,0,945,5},{"Needle 2: IAS","kt",0,450,0,945,6}};
case Profile::brake:return {{"Brake accumulator pressure","psi",0,4000,0,945,5}};
case Profile::compass:return {{"Magnetic heading","deg",0,360,0,1080,5,true}};
default:return {};}}
Calibration initial_calibration(const Channel& c){Calibration out;if(c.cyclic){for(int n=0;n<4;++n)out.points.push_back({n*90.,n*270,false});}else out.points={{c.minimum,c.native_min,c.firmware},{c.maximum,c.native_max,c.firmware}};return out;}
bool valid_calibration(const Channel& c,const Calibration& cal,std::string& e){
    auto fail=[&](std::string why){e=std::move(why);return false;};if(cal.points.size()<2||cal.points.size()>32)return fail("Capture between 2 and 32 scale points");
    if(c.cyclic&&(cal.points.size()!=4||cal.points[0].value!=0||cal.points[1].value!=90||cal.points[2].value!=180||cal.points[3].value!=270))return fail("Compass calibration requires N/E/S/W points");
    auto point=[&](const Point& p){std::ostringstream out;out<<p.value<<' '<<c.unit<<" (position "<<p.position<<')';return out.str();};
    int direction=0;for(std::size_t i=0;i<cal.points.size();++i){const auto& p=cal.points[i];if(!p.captured)return fail("Capture "+point(p)+" before applying calibration");if(!std::isfinite(p.value)||p.value<c.minimum||p.value>c.maximum||p.position<c.native_min||p.position>c.native_max)return fail("Capture each point within the instrument limits");
        if(i){const auto& prev=cal.points[i-1];if(p.value<=prev.value)return fail("Scale values must be distinct and increasing");if(!c.cyclic){
            // The vendor's IAS plate parks 0 and 60 kt at the same stop. Flat
            // segments are valid for forward indication; reversal is not.
            const int d=p.position>prev.position?1:p.position<prev.position?-1:0;if(d){if(direction&&direction!=d)return fail("Needle reverses between "+point(prev)+" and "+point(p));direction=d;}}}}
    if(!c.cyclic&&cal.points.front().value>=cal.points.back().value)return fail("Minimum must be less than maximum");
    if(!c.cyclic&&!direction)return fail("Capture at least two different needle positions");
    if(c.cyclic&&!compass_direction(c,cal))return fail("Compass cardinal positions must progress once around the dial");
    e.clear();return true;
}
std::optional<int> position(const Channel& c,const Calibration& cal,double v)noexcept {
    if(!std::isfinite(v)||cal.points.size()<2)return {};for(const auto& p:cal.points)if(!p.captured)return {};
    const auto& ps=cal.points;double x{},y{};
    if(c.firmware)return int(std::lround(std::clamp(v,c.minimum,c.maximum)));
    if(c.cyclic){
        const int direction=compass_direction(c,cal);if(!direction)return {};
        v=std::fmod(v,360.);if(v<0)v+=360.;
        const auto n=std::size_t(v/90.);const int period=c.native_max-c.native_min;
        int delta=(ps[(n+1)%4].position-ps[n].position)%period;
        if(direction>0&&delta<0)delta+=period;else if(direction<0&&delta>0)delta-=period;
        double result=std::fmod(ps[n].position+(v-ps[n].value)/90.*delta-c.native_min,double(period));
        if(result<0)result+=period;
        return c.native_min+int(std::lround(result))%period;
    }
    v=std::clamp(v,ps.front().value,ps.back().value);
    auto it=std::upper_bound(ps.begin(),ps.end(),v,[](double n,const Point& p){return n<p.value;});
    if(it==ps.begin())return ps.front().position;
    if(it==ps.end())return ps.back().position;
    const auto& a=*(it-1);x=(v-a.value)/(it->value-a.value);y=a.position+x*(it->position-a.position);
    return int(std::lround(y));
}
bool valid_configuration(const Configuration& c,std::string& e){
    if(c.assignments.size()>255||c.port.instance.size()>4096||c.port.path.size()>4096||c.port.name.size()>64){e="Configuration exceeds its bounds";return false;}
    std::set<std::pair<std::string,unsigned>> keys,clock_roles;
    for(const auto& a:c.assignments){if(a.bus.empty()||a.bus.size()>4096||a.address<1||a.address>255||a.type>255||a.model>255||a.firmware>255||unsigned(a.profile)>unsigned(Profile::clock_fo)||!compatible(a.profile,a.type)||!keys.emplace(a.bus,a.address).second){e="Invalid or duplicate instrument assignment";return false;}
        if(unsigned(a.clock_source)>unsigned(ClockSource::local_pc)||((a.profile!=Profile::clock_captain&&a.profile!=Profile::clock_fo)&&a.clock_source!=ClockSource::local_utc)){e="Invalid clock time source";return false;}
        if((a.profile==Profile::clock_captain||a.profile==Profile::clock_fo)&&!clock_roles.emplace(a.bus,unsigned(a.profile)).second){e="Each clock role requires one distinct instrument ID";return false;}
        const auto cs=channels(a.profile);if(a.calibration.size()!=cs.size()){e="Calibration channels do not match the assigned instrument";return false;}
        for(std::size_t i=0;i<cs.size();++i){const auto& cal=a.calibration[i];if(cal.points.size()>32){e="Too many calibration points";return false;}bool complete=std::all_of(cal.points.begin(),cal.points.end(),[](const auto& p){return p.captured;});if(complete&&!valid_calibration(cs[i],cal,e))return false;
            for(const auto& p:cal.points)if(!std::isfinite(p.value)||p.value<cs[i].minimum||p.value>cs[i].maximum||p.position<cs[i].native_min||p.position>cs[i].native_max){e="Calibration draft exceeds instrument bounds";return false;}}}
    if(c.drafts.size()>255){e="Too many calibration drafts";return false;}
    std::set<std::pair<std::string,unsigned>> draft_keys;
    for(const auto& a:c.drafts){
        const auto owner=std::find_if(c.assignments.begin(),c.assignments.end(),[&](const auto& x){return x.bus==a.bus&&x.address==a.address&&x.type==a.type&&x.model==a.model&&x.firmware==a.firmware&&x.profile==a.profile;});
        const auto cs=channels(a.profile);
        if(owner==c.assignments.end()||cs.empty()||a.calibration.size()!=cs.size()||!draft_keys.emplace(a.bus,a.address).second){e="Invalid calibration draft identity";return false;}
        for(std::size_t n=0;n<cs.size();++n){const auto& ps=a.calibration[n].points;if(ps.size()<2||ps.size()>32){e="Invalid draft point count";return false;}
            for(std::size_t k=0;k<ps.size();++k){const auto& p=ps[k];if(!std::isfinite(p.value)||p.value<cs[n].minimum||p.value>cs[n].maximum||p.position<cs[n].native_min||p.position>cs[n].native_max||(k&&p.value<=ps[k-1].value)){e="Calibration draft exceeds instrument bounds";return false;}}}
    }
    e.clear();return true;
}
std::string configuration_text(const Configuration& c){std::string e;if(!valid_configuration(c,e))throw std::invalid_argument(e);std::ostringstream o;o<<"flightillusion 3\n"<<"port "<<std::quoted(c.port.instance)<<' '<<std::quoted(c.port.name)<<' '<<std::quoted(c.port.friendly)<<' '<<std::quoted(c.port.path)<<'\n'<<std::setprecision(17);
    auto entries=[&](const auto& list,const char* kind){for(const auto& a:list){o<<kind<<' '<<std::quoted(a.bus)<<' '<<a.address<<' '<<a.type<<' '<<a.model<<' '<<a.firmware<<' '<<unsigned(a.profile)<<'\n';if(a.profile==Profile::clock_captain||a.profile==Profile::clock_fo)o<<"clock-source "<<unsigned(a.clock_source)<<'\n';for(std::size_t n=0;n<a.calibration.size();++n)for(const auto& p:a.calibration[n].points)o<<"point "<<n<<' '<<p.value<<' '<<p.position<<' '<<p.captured<<'\n';}};
    entries(c.assignments,"instrument");entries(c.drafts,"draft");return o.str();}
bool parse_configuration(std::string_view text,Configuration& out,std::string& e){
    if(text.size()>262144){e="Configuration is too large";return false;}std::istringstream in{std::string(text)};std::string line;Configuration c;bool port=false,source_seen=false;Assignment* active=nullptr;unsigned version{};if(!std::getline(in,line)){e="Empty configuration";return false;}{std::istringstream s(line);std::string magic,extra;if(!(s>>magic>>version)||magic!="flightillusion"||(version<1||version>3)||(s>>extra)){e="Unsupported Flight Illusion configuration";return false;}}
    while(std::getline(in,line)){if(line.empty())continue;std::istringstream s(line);std::string kind,extra;s>>kind;if(kind=="port"){if(port||!(s>>std::quoted(c.port.instance)>>std::quoted(c.port.name)>>std::quoted(c.port.friendly)>>std::quoted(c.port.path))){e="Invalid port entry";return false;}port=true;active=nullptr;}
        else if(kind=="instrument"||(version>=2&&kind=="draft")){Assignment a;unsigned profile{};if(!(s>>std::quoted(a.bus)>>a.address>>a.type>>a.model>>a.firmware>>profile)||profile>unsigned(Profile::clock_fo)){e="Invalid instrument entry";return false;}a.profile=Profile(profile);a.calibration.resize(channels(a.profile).size());auto& list=kind=="draft"?c.drafts:c.assignments;if(list.size()==255){e="Too many instrument entries";return false;}list.push_back(a);active=&list.back();source_seen=false;}
        else if(kind=="clock-source"&&version==3){unsigned n{};if(!active||source_seen||(active->profile!=Profile::clock_captain&&active->profile!=Profile::clock_fo)||!(s>>n)||n>unsigned(ClockSource::local_pc)){e="Invalid clock-source entry";return false;}active->clock_source=ClockSource(n);source_seen=true;}
        else if(kind=="point"){unsigned n{},captured{};Point p;if(!active||!(s>>n>>p.value>>p.position>>captured)||captured>1||n>=active->calibration.size()||active->calibration[n].points.size()==32){e="Invalid calibration point";return false;}p.captured=captured!=0;active->calibration[n].points.push_back(p);}
        else {e="Unknown configuration entry";return false;}if(s>>extra){e="Unexpected configuration fields";return false;}}
    if(!port||!valid_configuration(c,e))return false;out=std::move(c);return true;
}
} // namespace hwio::flightillusion
