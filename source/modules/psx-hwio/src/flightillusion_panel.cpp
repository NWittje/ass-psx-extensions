#include "hwio/flightillusion_panel.hpp"
#include <ass_graphics/graphics.hpp>
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <sstream>

namespace hwio::flightillusion {
namespace {
namespace gfx=ass::graphics;
std::wstring wide(std::string_view s){if(s.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring out(std::size_t(n),0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n);return out;}
std::string text(const gfx::Control& c){auto w=c.text();const int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string out(std::size_t(n),0);WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),out.data(),n,nullptr,nullptr);return out;}
std::wstring number(double v){std::ostringstream o;o<<v;return wide(o.str());}
std::optional<double> value(const gfx::Control& c){auto s=text(c);double n{};auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size()||!std::isfinite(n))return {};return n;}
void caption(gfx::Control& c,const std::wstring& s){if(c.text()!=s)c.text(s.c_str());}
}
struct Panel::Impl {
    Panel* self{};gfx::Context* context{};gfx::Window* window{};Provider* provider{};
    std::function<bool(bool,std::string&)> connection;
    std::function<bool()> enabled;
    gfx::Control pane,list,editor,status;std::map<unsigned,gfx::Control> controls,labels;
    struct Row {unsigned address{};gfx::Control name,choice,apply,calibrate,state,source;std::vector<Profile> profiles;bool edited{};Profile saved{},draft{};unsigned type{};};
    struct Action {unsigned id{},kind{};double value{};std::uint64_t generation{};};
    std::vector<Row> rows;std::vector<Port> ports;std::deque<Action> events;std::string inventory_key,port_key;std::uint64_t generation{};
    bool binding{},visible{};int width{900},height{660};std::optional<unsigned> editing;unsigned needle{~0u};std::vector<Point> shown_points;std::string message;bool calibration_available{};std::uint64_t calibration_session{};
    static void __cdecl event(void* raw,std::uint32_t id,std::uint32_t kind,double v)noexcept {auto& p=*static_cast<Impl*>(raw);if(p.binding)return;try{
        if(kind==AG_TEXT_CHANGED||kind==AG_DROPDOWN_CHANGED)return;
        if(kind==AG_CLICK||kind==AG_NAVIGATE||kind==AG_CHOICE_ACCEPTED||(id==21&&kind==AG_VALUE_CHANGED)){
            if(kind==AG_NAVIGATE&&v!=double(AG_NAV_TAB_NEXT)&&v!=double(AG_NAV_TAB_PREVIOUS)&&v!=double(AG_NAV_ENTER)&&v!=double(AG_NAV_ESCAPE))return;
            if(p.events.size()==64){p.provider->cancel_calibration();p.events.clear();p.message="Too many pending UI actions; calibration released";return;}
            if(id>=1000&&id<3000&&(id-1000)%4==1){const auto r=(id-1000)/4;if(r<p.rows.size())p.rows[r].edited=true;}
            p.events.push_back({id,kind,v,p.generation});ag_window_post(p.window->handle(),Panel::dispatch_id);
        }}catch(...){p.provider->cancel_calibration();}}
    gfx::Control make(unsigned kind,unsigned id,void* parent,const wchar_t* title){ag_control_desc d{};d.kind=kind;d.id=id;d.text=title;d.bounds={0,0,1,1};d.minimum=0;d.maximum=1;d.step=kind==AG_SLIDER?.0001:1;gfx::Control c(*context,parent,d,event,this);if(kind==AG_EDIT)c.events(AG_CONTROL_NAV_TAB|AG_CONTROL_NOTIFY_TEXT|AG_CONTROL_NAV_ACTIVATE);else if(kind==AG_CHOICE)c.events(AG_CONTROL_NAV_TAB|AG_CONTROL_NOTIFY_DROPDOWN|AG_CONTROL_NOTIFY_CHOICE_DECISION);else if(kind==AG_BUTTON)c.events(AG_CONTROL_NAV_TAB|AG_CONTROL_NAV_ACTIVATE);return c;}
    void add(unsigned kind,unsigned id,const wchar_t* title,void* parent){controls.emplace(id,make(kind,id,parent,title));}
    void field(unsigned id,const wchar_t* title,unsigned kind=AG_EDIT){labels.emplace(id,make(AG_LABEL,0,editor.hwnd(),title));add(kind,id,L"",editor.hwnd());}
    void navigate(unsigned id,bool previous){
        std::vector<HWND> order;
        auto add_control=[&](gfx::Control& c){const auto h=static_cast<HWND>(c.hwnd());if(IsWindowVisible(h)&&IsWindowEnabled(h))order.push_back(h);};
        for(unsigned n:{1u,2u,3u,4u,5u})add_control(controls.at(n));
        if(editing){for(unsigned n:{20u,21u,24u,22u,23u,34u,35u,30u,31u,26u,25u,27u,28u,29u,32u,33u})add_control(controls.at(n));}
        else for(auto& r:rows){add_control(r.choice);add_control(r.apply);add_control(r.calibrate);if(r.source.handle())add_control(r.source);}
        if(order.empty())return;const auto current=static_cast<HWND>(self->native_control(id));const auto found=std::find(order.begin(),order.end(),current);const auto index=found==order.end()?0:std::size_t(found-order.begin());
        SetFocus(order[(index+order.size()+(previous?-1:1))%order.size()]);
    }
    void layout(){if(!provider)return;const float w=float(std::max(width,440));pane.bounds({0,0,w,float(height)});labels.at(1).bounds({16,8,w-16,28});controls.at(1).bounds({16,30,w-160,60});controls.at(2).bounds({w-150,30,w-16,60});controls.at(3).bounds({16,72,160,104});controls.at(4).bounds({172,72,336,104});controls.at(5).bounds({348,72,w-16,104});status.bounds({16,112,w-16,170});
        list.bounds({0,180,w,float(height)});editor.bounds({0,180,w,float(height)});
        float y{};for(auto& r:rows){const bool clock=r.type==104||r.type==109;r.name.bounds({16,y+4,w-16,y+28});r.choice.bounds({16,y+34,w-244,y+66});r.apply.bounds({w-232,y+34,w-128,y+66});r.calibrate.bounds({w-116,y+34,w-16,y+66});if(clock)r.source.bounds({16,y+72,w-16,y+104});r.state.bounds({16,y+(clock?110:72),w-16,y+(clock?152:148)});y+=clock?158:154;}gfx::check(ag_panel_extent(list.handle(),y+12));
        const float half=(w-48)/2;auto f=[&](unsigned id,float x,float y,float fw){labels.at(id).bounds({x,y,x+fw,y+22});controls.at(id).bounds({x,y+24,x+fw,y+56});};
        f(20,16,0,w-32);labels.at(21).bounds({16,70,w-16,94});controls.at(21).bounds({16,100,w-16,132});f(24,16,166,half);controls.at(22).bounds({half+32,190,half+116,222});controls.at(23).bounds({half+128,190,w-16,222});
        f(34,16,240,half);f(35,half+32,240,half);controls.at(30).bounds({16,304,half+16,336});controls.at(31).bounds({half+32,304,w-16,336});
        f(26,16,354,w-32);f(25,16,426,half);controls.at(27).bounds({half+32,450,w-16,482});controls.at(28).bounds({16,494,half+16,526});controls.at(29).bounds({half+32,494,w-16,526});
        controls.at(32).bounds({16,546,half+16,580});controls.at(33).bounds({half+32,546,w-16,580});labels.at(40).bounds({16,596,w-16,684});gfx::check(ag_panel_extent(editor.handle(),700));
    }
    void rebuild(const View& v){std::string key;for(const auto& i:v.instruments)key+=std::to_string(i.address)+":"+std::to_string(i.type)+";";if(key==inventory_key)return;
        for(const auto& row:rows){std::uint32_t open{},source_open{};ag_choice_open(row.choice.handle(),&open);if(row.source.handle())ag_choice_open(row.source.handle(),&source_open);if(open||source_open||row.edited)return;}
        float scroll{};ag_panel_scroll_get(list.handle(),&scroll);auto batch=list.update_scope();++generation;rows.clear();
        for(const auto& i:v.instruments){Row r;r.address=i.address;r.type=i.type;const unsigned id=1000+unsigned(rows.size())*4;r.name=make(AG_LABEL,0,list.hwnd(),L"");r.choice=make(AG_CHOICE,id+1,list.hwnd(),L"");r.apply=make(AG_BUTTON,id+2,list.hwnd(),L"Apply");r.calibrate=make(AG_BUTTON,id+3,list.hwnd(),L"Calibrate...");r.state=make(AG_LABEL,0,list.hwnd(),L"");
            if(i.type==104||i.type==109){r.source=make(AG_CHOICE,3000+unsigned(rows.size()),list.hwnd(),L"");for(unsigned n=0;n<3;++n)r.source.add(wide(clock_source_name(ClockSource(n))).c_str());}
            for(unsigned p=0;p<=unsigned(Profile::clock_fo);++p)if(compatible(Profile(p),i.type)){r.profiles.push_back(Profile(p));r.choice.add(wide(profile_name(Profile(p))).c_str());}r.draft=r.saved=i.assigned;const auto selected=std::find(r.profiles.begin(),r.profiles.end(),i.assigned);r.choice.value(selected==r.profiles.end()?0:double(selected-r.profiles.begin()));rows.push_back(std::move(r));}
        inventory_key=key;layout();ag_panel_scroll_set(list.handle(),scroll);
    }
    void calibration(const View& v){
        const bool active=v.calibration.has_value();ag_visible(list.handle(),active?0:1);ag_visible(editor.handle(),active?1:0);
        if(!active){if(editing)++generation;editing.reset();needle=~0u;return;}const auto& edit=*v.calibration;const auto cs=channels(edit.profile);const auto& c=cs[edit.needle];
        if(calibration_available!=edit.available||calibration_session!=v.session){++generation;events.clear();calibration_available=edit.available;calibration_session=v.session;}
        const bool changed=editing!=edit.address||needle!=edit.needle;
        if(editing!=edit.address){std::vector<std::wstring> names;for(const auto& ch:cs)names.push_back(wide(ch.name));std::vector<const wchar_t*> strings;for(const auto& s:names)strings.push_back(s.c_str());controls.at(20).replace(strings,int(edit.needle));}
        if(changed){++generation;editing=edit.address;needle=edit.needle;controls.at(20).value(needle);const auto& ps=edit.calibration[needle].points;controls.at(34).text(number(ps.empty()?c.minimum:ps.front().value).c_str());controls.at(35).text(number(ps.empty()?c.maximum:ps.back().value).c_str());controls.at(25).text(number(c.minimum).c_str());controls.at(24).text(edit.commanded?number(*edit.commanded).c_str():L"");
            ag_control_desc d{};d.kind=AG_SLIDER;d.id=21;d.text=c.firmware?L"ft":L"device units";d.bounds={16,100,float(width-16),132};d.minimum=c.native_min;d.maximum=c.native_max;d.step=1;d.value=edit.commanded.value_or(c.native_min);controls.at(21)=gfx::Control(*context,editor.hwnd(),d,Impl::event,this);controls.at(21).events(AG_CONTROL_NAV_TAB);}
        if(edit.calibration[needle].points!=shown_points||changed){shown_points=edit.calibration[needle].points;std::vector<std::wstring> names;for(const auto& p:shown_points){const std::wstring cardinal=c.cyclic?(p.value==0?L"North - ":p.value==90?L"East - ":p.value==180?L"South - ":L"West - "):L"";names.push_back(cardinal+number(p.value)+L" "+wide(c.unit)+(p.captured?L" = "+number(p.position):L" (not captured)"));}std::vector<const wchar_t*> strings;for(const auto& s:names)strings.push_back(s.c_str());controls.at(26).replace(strings,0);}
        caption(labels.at(21),L"Commanded position: "+(edit.commanded?number(*edit.commanded):L"not sent")+(c.firmware?L" ft":L" device units"));
        caption(labels.at(20),std::to_wstring(edit.address)+L" - "+wide(profile_name(edit.profile))+L": needle");
        caption(labels.at(34),c.cyclic?L"North (degrees)":L"Minimum scale value");caption(labels.at(35),c.cyclic?L"West (degrees)":L"Maximum scale value");caption(controls.at(30),c.cyclic?L"Set North":L"Set "+controls.at(34).text()+L" "+wide(c.unit));caption(controls.at(31),c.cyclic?L"Set West":L"Set "+controls.at(35).text()+L" "+wide(c.unit));
        std::string validation;const bool valid=valid_calibration(c,edit.calibration[needle],validation);
        caption(labels.at(40),c.firmware?L"Altitude/display calibration belongs to the instrument firmware. This slider previews altitude in feet; it does not change calibration.":(valid?L"Selected needle scale is valid. ":L"Draft: "+wide(validation)+L". ")+std::wstring(L"Save calibration applies all needles; Back keeps a draft. Verify physical travel: commanded position confirms transmission, not measured needle position."));
        for(unsigned id:{25u,27u,28u,30u,31u,34u,35u})controls.at(id).enabled(!c.firmware);controls.at(29).enabled(edit.available&&!shown_points.empty());
        for(unsigned id:{21u,22u,23u,24u,32u})controls.at(id).enabled(edit.available);
        for(unsigned id:{27u,30u,31u})controls.at(id).enabled(edit.available&&!c.firmware);
        if(c.cyclic){controls.at(34).enabled(false);controls.at(35).enabled(false);controls.at(28).enabled(false);}
        if(edit.commanded&&!(controls.at(21).activity()&AG_ACTIVITY_FOCUSED))controls.at(21).value(*edit.commanded);
    }
};
Panel::Panel():impl_(std::make_unique<Impl>()){impl_->self=this;}Panel::~Panel(){hide();}
void Panel::connection_handler(std::function<bool(bool,std::string&)> f,std::function<bool()> enabled){impl_->connection=std::move(f);impl_->enabled=std::move(enabled);}
void Panel::attach(gfx::Context& c,gfx::Window& w,Provider& p,void* parent){auto& s=*impl_;s.context=&c;s.window=&w;s.provider=&p;s.binding=true;s.pane=s.make(AG_PANEL,0,parent,L"");s.list=s.make(AG_PANEL,0,s.pane.hwnd(),L"");s.editor=s.make(AG_PANEL,0,s.pane.hwnd(),L"");s.status=s.make(AG_LABEL,0,s.pane.hwnd(),L"");s.labels.emplace(1,s.make(AG_LABEL,0,s.pane.hwnd(),L"COM port"));s.add(AG_CHOICE,1,L"",s.pane.hwnd());s.add(AG_BUTTON,2,L"Refresh ports",s.pane.hwnd());s.add(AG_BUTTON,3,L"Connect",s.pane.hwnd());s.add(AG_BUTTON,4,L"Search instruments",s.pane.hwnd());s.add(AG_BUTTON,5,L"Cancel search",s.pane.hwnd());
    s.field(20,L"Instrument needle",AG_CHOICE);s.field(21,L"Commanded position",AG_SLIDER);s.field(24,L"Native position (Enter to move)");s.field(25,L"Scale value");s.field(26,L"Saved / draft scale points",AG_CHOICE);s.field(34,L"Minimum scale value");s.field(35,L"Maximum scale value");s.labels.emplace(40,s.make(AG_LABEL,0,s.editor.hwnd(),L""));
    for(const auto& [id,title]:std::initializer_list<std::pair<unsigned,const wchar_t*>>{{22,L"Move -1"},{23,L"Move +1"},{27,L"Set point"},{28,L"Remove point"},{29,L"Move to saved point"},{30,L"Set Min"},{31,L"Set Max"},{32,L"Save calibration"},{33,L"Back"}})s.add(AG_BUTTON,id,title,s.editor.hwnd());s.layout();s.binding=false;p.refresh_ports();refresh();}
void Panel::arrange(int x,int y,int w,int h,bool visible){auto& s=*impl_;s.width=w;s.height=h;s.visible=visible;s.layout();s.pane.bounds({float(x),float(y),float(x+w),float(y+h)});ag_visible(s.pane.handle(),visible?1:0);if(!visible)hide();}
void Panel::refresh(){auto& s=*impl_;if(!s.provider)return;const auto v=s.provider->snapshot();s.binding=true;try{
    std::string key=v.configuration.port.instance;for(const auto& p:v.ports)key+=p.instance+p.name;if(key!=s.port_key){std::uint32_t open{};ag_choice_open(s.controls.at(1).handle(),&open);if(!open){++s.generation;s.ports=v.ports;std::vector<std::wstring> names{L"Select COM port..."};int selected=0;for(const auto& p:s.ports){names.push_back(wide(p.name+" - "+p.friendly+" ["+p.instance+"]"));if(p.instance==v.configuration.port.instance)selected=int(names.size()-1);}if(!v.configuration.port.instance.empty()&&!selected)names[0]=L"Saved adapter unavailable";std::vector<const wchar_t*> strings;for(const auto& n:names)strings.push_back(n.c_str());s.controls.at(1).replace(strings,selected);s.port_key=key;}}
    s.rebuild(v);const bool engaged=s.enabled?s.enabled():v.connected||v.connecting;caption(s.controls.at(3),engaged?L"Disconnect":L"Connect");s.controls.at(1).enabled(!engaged);s.controls.at(4).enabled(v.connected&&!v.searching&&!v.calibration);s.controls.at(5).enabled(v.searching);
    caption(s.status,wide(v.calibration&&!v.calibration->available?"Waiting for the same instrument to reconnect; calibration points retained":s.message.empty()?v.status:s.message)+(v.searching?L" ("+std::to_wstring(v.searched)+L" / 255)":L""));
    for(auto& r:s.rows){const auto i=std::find_if(v.instruments.begin(),v.instruments.end(),[&](const auto& x){return x.address==r.address;});if(i==v.instruments.end())continue;caption(r.name,std::to_wstring(i->address)+L" - "+wide(i->name));std::string state=i->calibration_status.empty()?i->status:i->calibration_status+"\n"+i->status;const auto caps=capabilities(i->type);if(caps.barometer_input||caps.clock_time_setting)state+="\n"+i->input_status;caption(r.state,wide(state));r.apply.enabled(i->connected&&!v.searching&&!v.calibration);r.choice.enabled(!v.calibration&&!v.searching);r.calibrate.enabled(i->connected&&i->compatible&&!channels(i->assigned).empty()&&!v.searching&&!v.calibration);
        if(r.source.handle()){const auto a=std::find_if(v.configuration.assignments.begin(),v.configuration.assignments.end(),[&](const auto& x){return x.bus==v.configuration.port.instance&&x.address==r.address;});const bool clock=a!=v.configuration.assignments.end()&&(a->profile==Profile::clock_captain||a->profile==Profile::clock_fo);ag_visible(r.source.handle(),clock?1:0);r.source.enabled(clock&&i->compatible&&!v.searching&&!v.calibration);std::uint32_t open{};ag_choice_open(r.source.handle(),&open);if(clock&&!open)r.source.value(double(unsigned(a->clock_source)));}
        if(!r.edited&&r.saved!=i->assigned){r.draft=r.saved=i->assigned;const auto it=std::find(r.profiles.begin(),r.profiles.end(),i->assigned);r.choice.value(it==r.profiles.end()?0:double(it-r.profiles.begin()));}}
    s.calibration(v);s.binding=false;
    }catch(...){s.binding=false;throw;}}
void Panel::action(unsigned id,double v){auto& s=*impl_;std::string error;bool ok=true;auto view=s.provider->snapshot();
    if(id==1){const auto n=int(v);if(n>0&&std::size_t(n)<=s.ports.size())ok=s.provider->select_port(s.ports[std::size_t(n-1)],error);}
    else if(id==2)s.provider->refresh_ports();else if(id==3){if(s.connection)ok=s.connection(s.enabled?!s.enabled():!view.connected&&!view.connecting,error);else if(view.connected||view.connecting)s.provider->disconnect();else ok=s.provider->connect(error);}else if(id==4)ok=s.provider->search(error);else if(id==5)s.provider->cancel_search();
    else if(id>=3000&&id<3255){const auto n=id-3000;if(n<s.rows.size()&&v>=0&&v<=2&&std::floor(v)==v)ok=s.provider->select_clock_source(s.rows[n].address,ClockSource(unsigned(v)),error);}
    else if(id>=1000){const auto n=(id-1000)/4;if(n<s.rows.size()){auto& r=s.rows[n];if((id-1000)%4==1){const auto choice=int(v);if(choice>=0&&std::size_t(choice)<r.profiles.size()){r.draft=r.profiles[std::size_t(choice)];r.edited=true;}}else if((id-1000)%4==2){ok=s.provider->assign(r.address,r.draft,error);if(ok)r.edited=false;}else if((id-1000)%4==3)ok=s.provider->begin_calibration(r.address,error);}}
    else if(view.calibration){const auto& edit=*view.calibration;const auto cs=channels(edit.profile);const auto& c=cs[edit.needle];if(id==20)ok=s.provider->select_needle(unsigned(v),error);
        else if(id==21)ok=s.provider->move(int(std::lround(v)),error);
        else if(id==22||id==23){if(edit.commanded)ok=s.provider->move(*edit.commanded+(id==22?-1:1),error);else {ok=false;error="Move the slider first";}}
        else if(id==24){const auto n=value(s.controls.at(24));if(n&&std::floor(*n)==*n&&*n>=c.native_min&&*n<=c.native_max)ok=s.provider->move(int(*n),error);else {ok=false;error="Enter a whole position within the instrument range";}}
        else if(id==27||id==30||id==31){const auto n=value(s.controls.at(id==30?34:id==31?35:25));if(n)ok=id==27||c.cyclic?s.provider->capture(*n,error):s.provider->capture_endpoint(id==31,*n,error);else {ok=false;error="Enter a valid scale value";}}
        else if(id==28){const auto n=value(s.controls.at(25));if(n)ok=s.provider->remove_point(*n,error);else {ok=false;error="Enter the scale value to remove";}}
        else if(id==26){const auto n=int(v);if(n>=0&&std::size_t(n)<s.shown_points.size())s.controls.at(25).text(number(s.shown_points[std::size_t(n)].value).c_str());}
        else if(id==29){const auto n=int(s.controls.at(26).value());if(n>=0&&std::size_t(n)<s.shown_points.size()&&s.shown_points[std::size_t(n)].captured)ok=s.provider->move(s.shown_points[std::size_t(n)].position,error);else {ok=false;error="Capture this point first";}}
        else if(id==32)ok=s.provider->save_calibration(error);else if(id==33)s.provider->cancel_calibration();}
    s.message=ok?std::string{}:error;refresh();}
void Panel::dispatch(){auto& s=*impl_;while(!s.events.empty()){auto e=s.events.front();s.events.pop_front();if(e.generation!=s.generation)continue;if(e.kind==AG_NAVIGATE){if(e.value==double(AG_NAV_TAB_NEXT)||e.value==double(AG_NAV_TAB_PREVIOUS)){s.navigate(e.id,e.value==double(AG_NAV_TAB_PREVIOUS));continue;}if(e.value==double(AG_NAV_ESCAPE)){if(s.editing)action(33);continue;}if(e.id!=24){wchar_t name[32]{};GetClassNameW(static_cast<HWND>(native_control(e.id)),name,32);if(std::wstring_view(name)!=L"Button")continue;}}action(e.id,e.value);}}
void Panel::hide(){auto& s=*impl_;if(s.provider)s.provider->cancel_calibration();s.events.clear();}
void* Panel::native_control(unsigned id)const {auto& s=*impl_;if(id==6)return s.list.hwnd();if(id==7)return s.editor.hwnd();if(s.controls.contains(id))return s.controls.at(id).hwnd();if(id>=2000&&id<2255){const auto n=id-2000;return n<s.rows.size()?s.rows[n].state.hwnd():nullptr;}if(id>=3000&&id<3255){const auto n=id-3000;return n<s.rows.size()?s.rows[n].source.handle()?s.rows[n].source.hwnd():nullptr:nullptr;}if(id>=1000){const auto n=(id-1000)/4;if(n<s.rows.size()){auto& r=s.rows[n];switch((id-1000)%4){case 1:return r.choice.hwnd();case 2:return r.apply.hwnd();case 3:return r.calibrate.hwnd();default:return r.name.hwnd();}}}return nullptr;}
} // namespace hwio::flightillusion
