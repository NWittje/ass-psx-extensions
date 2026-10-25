#include "hwio/flightillusion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace hwio::flightillusion {
namespace {
std::uint64_t now_ms(){return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
bool same_identity(const Assignment& a,const Assignment& b){return a.bus==b.bus&&a.address==b.address&&a.type==b.type&&a.model==b.model&&a.firmware==b.firmware&&a.profile==b.profile;}
bool clock_profile(Profile p){return p==Profile::clock_captain||p==Profile::clock_fo;}
std::optional<std::int64_t> calendar(const Reply& r){using namespace std::chrono;const year_month_day date{year(2000+r[6]),month(r[7]),day(r[8])};if(!date.ok()||r[10]>23||r[11]>59||r[12]>59)return {};return duration_cast<milliseconds>((sys_days(date)+hours(r[10])+minutes(r[11])+seconds(r[12])).time_since_epoch()).count();}
}
struct Provider::Impl {
    std::shared_ptr<Store> store;Driver driver;std::unique_ptr<Link> link;mutable std::mutex mutex;std::jthread worker;
    View view;bool configuration_ok{},want_open{},want_close{};Framer framer;Sources sources;
    std::uint64_t now{},next_send{},pending_deadline{},write_started{},input_sequence{},next_poll{};
    unsigned scan_address{},pending_address{},poll_index{};bool pending_scan{};
    struct Packet {Command bytes{};unsigned address{},needle{},command{};int value{};bool query{},manual{},scan{};std::uint64_t generation{};std::optional<std::int64_t> clock_epoch;};
    std::deque<Packet> urgent;std::map<std::pair<unsigned,unsigned>,Packet> outputs;
    std::optional<Packet> writing;std::size_t write_offset{};std::pair<unsigned,unsigned> last_output{};
    std::map<unsigned,unsigned> misses;std::map<unsigned,Reply> baselines;
    std::map<unsigned,std::uint64_t> baseline_ms;
    std::map<unsigned,std::uint64_t> resume_after;
    std::map<std::pair<unsigned,unsigned>,int> last_sent;
    std::map<unsigned,std::pair<std::int64_t,std::uint64_t>> clock_echo;
    std::map<unsigned,ClockSync> clock_modes;
    std::deque<InputEvent> events;
    std::optional<Assignment> edit_identity;
    std::map<std::pair<std::string,unsigned>,Assignment> retained_drafts;
    std::uint64_t edit_generation{};
    bool edit_ready(){if(!view.calibration||!edit_identity||!view.connected)return false;const auto i=instrument(edit_identity->address);const auto a=assignment(edit_identity->address);return i&&a&&i->connected&&i->compatible&&same_identity(*a,*edit_identity)&&i->type==edit_identity->type&&i->model==edit_identity->model&&i->firmware==edit_identity->firmware;}
    void pause_edit(){if(!view.calibration)return;++edit_generation;view.calibration->available=false;view.calibration->commanded.reset();view.calibration->requested.reset();for(auto it=outputs.begin();it!=outputs.end();)if(it->second.manual&&it->second.address==view.calibration->address)it=outputs.erase(it);else ++it;}
    bool checkpoint(std::string& e){if(!view.calibration||!edit_identity)return false;auto draft=*edit_identity;draft.calibration=view.calibration->calibration;retained_drafts[{draft.bus,draft.address}]=draft;auto c=view.configuration;auto it=std::find_if(c.drafts.begin(),c.drafts.end(),[&](const auto& a){return a.bus==draft.bus&&a.address==draft.address;});if(it==c.drafts.end())c.drafts.push_back(draft);else *it=draft;return commit(std::move(c),e);}

    explicit Impl(std::shared_ptr<Store> s,Driver d):store(std::move(s)),driver(std::move(d)){
        if(!driver.enumerate||!driver.create)driver=native_driver();if(!store)throw std::invalid_argument("A Flight Illusion configuration store is required");
        std::string error;configuration_ok=store->load(view.configuration,error);view.status=configuration_ok?"Select a COM port and Connect":error;
    }
    Assignment* assignment(unsigned a){for(auto& x:view.configuration.assignments)if(x.bus==view.configuration.port.instance&&x.address==a)return &x;return nullptr;}
    Instrument* instrument(unsigned a){for(auto& x:view.instruments)if(x.address==a)return &x;return nullptr;}
    bool commit(Configuration c,std::string& e){if(!configuration_ok){e=view.status;return false;}if(!store->save(c,e))return false;view.configuration=std::move(c);++view.revision;return true;}
    void discard(){framer.reset();urgent.clear();outputs.clear();writing.reset();pending_address=0;write_offset=0;pause_edit();baselines.clear();baseline_ms.clear();last_sent.clear();clock_echo.clear();clock_modes.clear();events.clear();misses.clear();resume_after.clear();sources={};}
    bool source_current()const{return sources.session&&sources.sequence&&sources.sample_ms<=now&&now-sources.sample_ms<=2000;}
    const Clock& clock_source(const Assignment& a)const{return a.clock_source==ClockSource::local_utc?sources.pc_utc:a.clock_source==ClockSource::local_pc?sources.pc_local:sources.clocks[a.profile==Profile::clock_fo?1:0];}
    bool clock_controls(const Assignment& a)const{if(!clock_profile(a.profile)||a.clock_source!=ClockSource::psx_utc||!source_current()||!sources.controls_ready)return false;const auto& c=clock_source(a);return c.valid&&c.sync==ClockSync::normal;}
    void closed(std::string message){if(link)link->close();link.reset();view.connected=view.connecting=view.searching=false;want_open=want_close=false;discard();for(auto& x:view.instruments){x.connected=false;x.calibrating=view.calibration&&view.calibration->address==x.address;x.commanded.clear();x.status="Disconnected";}view.status=std::move(message);++view.revision;}
    void query(unsigned address,bool scan){urgent.push_back({scan?identify(address):*encode(address,7,0),address,0,7,0,true,false,scan,view.session});}
    void queue(unsigned address,unsigned needle,unsigned command,int value,bool manual=false,std::optional<std::int64_t> epoch={}){auto frame=encode(address,command,value);if(!frame)return;const auto key=std::pair(address,command);if(!manual){auto prev=last_sent.find(key);if(prev!=last_sent.end()&&prev->second==value)return;}outputs[key]={*frame,address,needle,command,value,false,manual,false,manual?edit_generation:view.session,epoch};}
    void reset_inputs(unsigned address){baselines.erase(address);baseline_ms.erase(address);clock_echo.erase(address);events.erase(std::remove_if(events.begin(),events.end(),[&](const auto& e){return e.address==address;}),events.end());}
    void cancel_edit(){if(!view.calibration)return;if(edit_identity){auto draft=*edit_identity;draft.calibration=view.calibration->calibration;retained_drafts[{draft.bus,draft.address}]=std::move(draft);}pause_edit();const auto address=view.calibration->address;for(auto i=outputs.begin();i!=outputs.end();)if(i->second.address==address)i=outputs.erase(i);else ++i;
        if(auto i=instrument(address))i->calibrating=false;resume_after[address]=sources.sequence;baselines.erase(address);events.erase(std::remove_if(events.begin(),events.end(),[&](const auto& e){return e.address==address;}),events.end());view.calibration.reset();edit_identity.reset();++view.revision;}
    void event(unsigned address,Profile profile,InputKind kind,double value=0,std::int64_t epoch=0){if(events.size()==256){events.clear();baselines.clear();baseline_ms.clear();++view.input_overflows;view.status="Input overflow: establishing fresh instrument baselines";return;}const auto i=instrument(address);events.push_back({view.session,++input_sequence,i?i->generation:0,address,profile,kind,value,epoch,now});}
    bool capture(double value,std::string& e){auto& edit=view.calibration;if(!edit||!edit->commanded){e="Move the needle and wait for transmission before capturing";return false;}if(!edit_ready()){e="Waiting for the same instrument to reconnect";return false;}const auto cs=channels(edit->profile);const auto& c=cs[edit->needle];if(c.firmware){e="Altimeter calibration is owned by its firmware";return false;}if(!std::isfinite(value)||value<c.minimum||value>c.maximum){e="Scale value exceeds the profile range";return false;}if(c.cyclic&&value!=0&&value!=90&&value!=180&&value!=270){e="Capture North/East/South/West";return false;}
        auto& ps=edit->calibration[edit->needle].points;auto it=std::find_if(ps.begin(),ps.end(),[&](const auto& p){return p.value==value;});if(it!=ps.end())*it={value,*edit->commanded,true};else {if(ps.size()==32){e="At most 32 points are supported";return false;}if(!c.cyclic&&ps.size()>=2&&(value<=ps.front().value||value>=ps.back().value)){e="New intermediate points must lie between the minimum and maximum";return false;}ps.push_back({value,*edit->commanded,true});}std::sort(ps.begin(),ps.end(),[](auto a,auto b){return a.value<b.value;});return checkpoint(e);}
    void received(const Reply& r){
        if(!pending_address||r[1]!=pending_address){++view.malformed;return;}const bool discovery=pending_scan;pending_address=0;const unsigned address=r[1];auto i=instrument(address);
        if(!i){view.instruments.push_back({});i=&view.instruments.back();i->address=address;i->generation=1;}
        const bool changed=i->type!=r[2]||i->model!=r[3]||i->firmware!=r[4]||!i->connected;
        if(changed){++i->generation;i->commanded.clear();reset_inputs(address);last_sent.erase({address,4});last_sent.erase({address,5});last_sent.erase({address,6});for(auto it=outputs.begin();it!=outputs.end();)if(it->second.address==address)it=outputs.erase(it);else ++it;if(view.calibration&&view.calibration->address==address)pause_edit();}
        i->type=r[2];i->model=r[3];i->firmware=r[4];i->name=hardware_name(r[2]);i->seen_ms=now;i->connected=true;misses[address]=0;
        const auto a=assignment(address);i->assigned=a?a->profile:Profile::unassigned;i->compatible=!a||(a->type==i->type&&a->model==i->model&&a->firmware==i->firmware&&compatible(a->profile,i->type));
        i->status=i->compatible?(a&&a->profile!=Profile::unassigned?"Assigned; waiting for source/calibration":"Unassigned"):"Hardware identity changed: review assignment";
        if(view.calibration&&view.calibration->address==address){i->calibrating=true;view.calibration->available=edit_ready();}
        i->input_status=i->type==101?"Barometric setting / knob available":i->type==104?(a&&a->clock_source==ClockSource::psx_utc?"PSX GMT/date setting; CHR / DATE / ET controls local":"Clock controls local; PC time is not changed"):"Physical controls unavailable for this type";
        // Invalid barometric samples must not become a baseline for a later edge.
        if(i->type==101&&(r[5]==255||r[8]==255)){reset_inputs(address);++view.revision;return;}
        if(i->type==104&&(!a||!clock_controls(*a)))reset_inputs(address);
        if(!discovery&&a&&i->compatible&&!i->calibrating&&sources.controls_ready&&(i->type!=104||clock_controls(*a))){auto old=baselines.find(address);if(old!=baselines.end()){
            if(i->type==101&&r[5]!=255&&r[8]!=255){if(r[9]==1){const int delta=int(r[11])-128;if(delta)event(address,a->profile,InputKind::barometer_delta_inhg,delta*.01);}
                else if(r[6]!=old->second[6]||r[7]!=old->second[7]){const unsigned raw=unsigned(r[6])+(unsigned(r[7])<<8);const double value=i->model==1?raw/100.:raw/33.86;if(value>=20&&value<=35)event(address,a->profile,InputKind::barometer_inhg,value);}}
            if(i->type==104&&clock_profile(a->profile)){const auto t=calendar(r),previous=calendar(old->second);if(t&&previous&&*t!=*previous){const auto echo=clock_echo.find(address);bool own=false;if(echo!=clock_echo.end()){const auto predicted=echo->second.first+std::int64_t(now-echo->second.second);own=std::llabs(*t-predicted)<=2000;}const auto elapsed=baseline_ms.contains(address)?std::int64_t(now-baseline_ms[address]):0;if(!own&&std::llabs(*t-(*previous+elapsed))>2000)event(address,a->profile,InputKind::clock_epoch_ms,0,*t);}}
        }baselines[address]=r;baseline_ms[address]=now;}
        ++view.revision;
    }
    void prepare_outputs(){
        for(auto& i:view.instruments){i.calibration_status.clear();const auto a=assignment(i.address);if(!a||!i.compatible)continue;const auto cs=channels(a->profile);if(cs.empty())continue;
            auto valid=[&](const Assignment& candidate){std::string error;for(std::size_t n=0;n<cs.size();++n)if(!valid_calibration(cs[n],candidate.calibration[n],error))return false;return true;};
            const bool applied=valid(*a);const Assignment* draft=nullptr;const auto retained=retained_drafts.find({a->bus,a->address});if(retained!=retained_drafts.end()&&same_identity(retained->second,*a))draft=&retained->second;else for(const auto& candidate:view.configuration.drafts)if(same_identity(candidate,*a)){draft=&candidate;break;}
            const bool changed=draft&&draft->calibration!=a->calibration;
            i.calibration_status=applied?"Calibration applied":"Calibration required";
            if(changed)i.calibration_status+=valid(*draft)?"; saved draft not applied":"; draft needs correction";
        }
        if(!source_current()){for(auto it=outputs.begin();it!=outputs.end();)if(!it->second.manual)it=outputs.erase(it);else ++it;for(auto& i:view.instruments)if(i.connected&&i.compatible&&!i.calibrating&&i.assigned!=Profile::unassigned){i.status="Source unavailable; last commanded position retained";if(clock_profile(i.assigned)){clock_modes.erase(i.address);reset_inputs(i.address);}}return;}
        for(auto& i:view.instruments){if(!i.connected||!i.compatible||i.calibrating)continue;const auto a=assignment(i.address);if(!a||a->profile==Profile::unassigned)continue;
            if(resume_after.contains(i.address)&&sources.sequence<=resume_after[i.address]){i.status="Waiting for a fresh source after calibration";continue;}
            if(clock_profile(a->profile)){
                outputs.erase({i.address,4});outputs.erase({i.address,5});
                if(i.type!=104){i.status="Cessna clock output protocol is not qualified";continue;}const auto& clock=clock_source(*a);
                const auto mode=clock.valid?clock.sync:ClockSync::unavailable;
                const auto previous=clock_modes.find(i.address);
                if(previous==clock_modes.end()||previous->second!=mode){clock_modes[i.address]=mode;last_sent.erase({i.address,4});last_sent.erase({i.address,5});reset_inputs(i.address);}
                if(mode==ClockSync::unavailable||clock.epoch_ms==788918400000LL){i.status="GMT/date unavailable; hardware timers remain local";continue;}
                if(mode!=ClockSync::normal){i.status=mode==ClockSync::display_off?"PSX clock display unpowered; hardware blanking unavailable":mode==ClockSync::paused?"PSX paused; hardware clock hold unavailable":mode==ClockSync::rate?"PSX time rate differs; hardware rate control unavailable":"PSX clock setting/hold active; hardware mode control unavailable";continue;}
                using namespace std::chrono;const auto time=sys_time<milliseconds>(milliseconds(clock.epoch_ms));const auto day=floor<days>(time);const year_month_day date{day};const int year=int(date.year())-2000;if(!date.ok()||year<0||year>63){i.status="Clock date outside 2000..2063";continue;}
                const auto seconds_of_day=int(duration_cast<seconds>(time-day).count());const int packed_time=seconds_of_day>65535?65535-seconds_of_day:seconds_of_day;
                queue(i.address,0,4,(year<<10)|(int(unsigned(date.month()))<<6)|int(unsigned(date.day())));queue(i.address,0,5,packed_time,false,clock.epoch_ms);i.status=clock_source_name(a->clock_source)+"; timers and display controls remain local";continue;
            }
            const auto cs=channels(a->profile);i.commanded.resize(cs.size());std::string status;
            for(std::size_t n=0;n<cs.size();++n){Signal signal=Signal::altitude;switch(a->profile){case Profile::attitude:signal=n?Signal::pitch:Signal::bank;break;case Profile::airspeed:signal=n?Signal::ias:Signal::limit_speed;break;case Profile::brake:signal=Signal::brake;break;case Profile::compass:signal=Signal::compass;break;default:break;}
                const auto& value=sources.values[unsigned(signal)];std::string error;const bool calibrated=valid_calibration(cs[n],a->calibration[n],error);
                const auto deadline=sources.valid_until_ms[unsigned(signal)];
                const bool available=value&&std::isfinite(*value)&&(!deadline||now<deadline);std::string state=available?"source available":"source unavailable";
                if(!available&&!sources.unavailable[unsigned(signal)].empty())state+=" ("+sources.unavailable[unsigned(signal)]+")";
                else if(value&&deadline&&now>=deadline)state+=" (source expired)";
                if(!calibrated)state+=", calibration required";
                else if(available){if(cs[n].firmware&&(*value<cs[n].minimum||*value>cs[n].maximum))state="source outside supported range";
                    else if(const auto p=position(cs[n],a->calibration[n],*value)){queue(i.address,unsigned(n),cs[n].command,*p);state="ready";}else state="source cannot be mapped";}
                if(state!="ready")outputs.erase({i.address,cs[n].command});if(!status.empty())status+="; ";status+=cs[n].name+": "+state;}
            i.status=std::move(status);
        }
    }
    void advance(std::uint64_t time){now=time;if(want_close){closed("Disconnected");return;}
        if(want_open){want_open=false;view.connecting=true;const auto list=driver.enumerate();view.ports=list;auto found=std::find_if(list.begin(),list.end(),[&](const auto& p){return p.instance==view.configuration.port.instance;});if(found==list.end()){closed("Saved COM adapter is unavailable; select the correct port");return;}
            link=driver.create();std::string error;if(!link||!link->open(*found,error)){closed("COM port unavailable/busy: "+error);return;}discard();++view.session;view.connected=true;view.connecting=false;view.status="Connected to "+found->name+"; Search instruments";next_send=now;
            for(const auto& a:view.configuration.assignments)if(a.bus==view.configuration.port.instance&&!instrument(a.address)){Instrument i;i.address=a.address;i.type=a.type;i.model=a.model;i.firmware=a.firmware;i.assigned=a.profile;i.name=hardware_name(a.type);i.status="Saved assignment; instrument not yet identified";view.instruments.push_back(std::move(i));}}
        if(!view.connected||!link)return;
        std::array<std::uint8_t,4096> bytes{};std::string error;const int read=link->read(bytes,error);if(read<0||read>int(bytes.size())){closed("Serial read failed: "+error);return;}if(read>0)for(const auto& r:framer.feed(std::span(bytes).first(std::size_t(read))))received(r);
        if(pending_address&&now>=pending_deadline){if(!pending_scan){const auto address=pending_address;if(++misses[address]>=3){if(auto i=instrument(address)){i->connected=false;i->status="Instrument not responding";}reset_inputs(address);for(auto it=outputs.begin();it!=outputs.end();)if(it->second.address==address)it=outputs.erase(it);else ++it;if(view.calibration&&view.calibration->address==address)pause_edit();}}pending_address=0;}
        if(!writing&&!pending_address&&urgent.empty()){
            if(view.searching){if(scan_address<=255){query(scan_address++,true);view.searched=scan_address-1;}else {view.searching=false;view.status="Search complete";next_poll=now;}}
            else if(now>=next_poll){std::vector<unsigned> ids;for(const auto& i:view.instruments)ids.push_back(i.address);if(!ids.empty()){query(ids[poll_index++%ids.size()],false);next_poll=now+100;}}
        }
        if(!view.searching)prepare_outputs();
        if(!writing&&now>=next_send){if(!urgent.empty()){writing=urgent.front();urgent.pop_front();}
            else if(!outputs.empty()){auto it=outputs.upper_bound(last_output);if(it==outputs.end())it=outputs.begin();last_output=it->first;writing=it->second;outputs.erase(it);}if(writing){write_offset=0;write_started=now;}}
        if(writing&&writing->manual&&writing->generation!=edit_generation){if(write_offset){closed("Calibration movement interrupted during transmission");return;}writing.reset();}
        if(writing){if(now-write_started>1000){closed("Serial write timeout");return;}const auto n=link->write(std::span(writing->bytes).subspan(write_offset),error);if(n<0||std::size_t(n)>6-write_offset){closed("Serial write failed: "+error);return;}write_offset+=std::size_t(n);
            if(write_offset==6){const auto p=*writing;writing.reset();++view.sent;next_send=now+10;if(p.query){pending_address=p.address;pending_scan=p.scan;pending_deadline=now+100;}
                else {last_sent[{p.address,p.command}]=p.value;if(auto i=instrument(p.address)){const auto cs=channels(i->assigned);if(p.needle<cs.size()){i->commanded.resize(cs.size());i->commanded[p.needle]=p.value;}}
                    if(p.manual&&view.calibration&&view.calibration->address==p.address&&view.calibration->needle==p.needle&&view.calibration->requested==p.value)view.calibration->commanded=p.value;
                    if(p.clock_epoch)clock_echo[p.address]={*p.clock_epoch,now};}}
        }
    }
};
Provider::Provider(std::shared_ptr<Store> store,Driver driver,bool start):impl_(std::make_unique<Impl>(std::move(store),std::move(driver))){if(start)impl_->worker=std::jthread([this](std::stop_token stop){while(!stop.stop_requested()){try{pump(now_ms());}catch(const std::exception& e){std::lock_guard lock(impl_->mutex);impl_->closed(e.what());}std::this_thread::sleep_for(std::chrono::milliseconds(5));}});}
Provider::~Provider(){stop();}
void Provider::stop(){impl_->worker.request_stop();if(impl_->worker.joinable())impl_->worker.join();std::lock_guard lock(impl_->mutex);impl_->cancel_edit();impl_->closed("Stopped");}
void Provider::pump(std::uint64_t t){std::lock_guard lock(impl_->mutex);impl_->advance(t);}
void Provider::refresh_ports(){auto ports=impl_->driver.enumerate();std::lock_guard lock(impl_->mutex);impl_->view.ports=std::move(ports);++impl_->view.revision;}
bool Provider::select_port(const Port& p,std::string& e){std::lock_guard lock(impl_->mutex);if(impl_->view.connected||impl_->want_open||impl_->view.calibration){e="Leave calibration and disconnect before selecting a different COM port";return false;}if(p.instance.empty()||p.path.empty()){e="Select an enumerated COM port";return false;}auto c=impl_->view.configuration;c.port=p;if(!impl_->commit(std::move(c),e))return false;impl_->view.instruments.clear();impl_->view.status="Selected "+p.name+"; Connect when ready";return true;}
bool Provider::connect(std::string& e){std::lock_guard lock(impl_->mutex);if(!impl_->configuration_ok||impl_->view.configuration.port.instance.empty()){e="Select the correct COM port first";return false;}if(impl_->view.connected||impl_->want_open){e="Already connected or connecting";return false;}impl_->want_open=true;impl_->view.connecting=true;return true;}
void Provider::disconnect(){std::lock_guard lock(impl_->mutex);impl_->want_close=true;impl_->want_open=false;}
bool Provider::search(std::string& e){std::lock_guard lock(impl_->mutex);if(!impl_->view.connected){e="Connect the selected COM port first";return false;}if(impl_->view.calibration){e="Finish calibration before searching";return false;}if(impl_->view.searching)return true;impl_->outputs.clear();impl_->urgent.clear();impl_->baselines.clear();impl_->baseline_ms.clear();impl_->events.clear();impl_->scan_address=1;impl_->view.searching=true;impl_->view.searched=0;impl_->view.status="Searching instrument IDs 1..255";return true;}
void Provider::cancel_search(){std::lock_guard lock(impl_->mutex);impl_->view.searching=false;impl_->urgent.clear();impl_->view.status="Search cancelled";}
bool Provider::assign(unsigned address,Profile p,std::string& e){std::lock_guard lock(impl_->mutex);const auto i=impl_->instrument(address);if(!i||!i->connected||impl_->view.searching||impl_->view.calibration){e="Select a connected instrument outside search/calibration";return false;}if(!compatible(p,i->type)){e="Instrument type is incompatible with this profile";return false;}
    auto c=impl_->view.configuration;auto a=std::find_if(c.assignments.begin(),c.assignments.end(),[&](const auto& x){return x.bus==c.port.instance&&x.address==address;});Assignment next{c.port.instance,address,i->type,i->model,i->firmware,p,{}};
    if(a!=c.assignments.end()&&a->profile==p&&a->type==i->type&&a->model==i->model&&a->firmware==i->firmware)next.calibration=a->calibration;else for(const auto& channel:channels(p))next.calibration.push_back(initial_calibration(channel));
    if(a!=c.assignments.end()&&clock_profile(p)&&clock_profile(a->profile)&&a->type==i->type&&a->model==i->model&&a->firmware==i->firmware)next.clock_source=a->clock_source;
    c.drafts.erase(std::remove_if(c.drafts.begin(),c.drafts.end(),[&](const auto& d){return d.bus==next.bus&&d.address==next.address&&!same_identity(d,next);}),c.drafts.end());
    if(a==c.assignments.end())c.assignments.push_back(next);else *a=next;if(!impl_->commit(std::move(c),e))return false;i->assigned=p;i->compatible=true;i->status="Assignment saved";impl_->baselines.erase(address);for(auto it=impl_->outputs.begin();it!=impl_->outputs.end();)if(it->second.address==address)it=impl_->outputs.erase(it);else ++it;impl_->last_sent.clear();return true;}
bool Provider::select_clock_source(unsigned address,ClockSource source,std::string& e){
    std::lock_guard lock(impl_->mutex);const auto i=impl_->instrument(address);const auto a=impl_->assignment(address);
    if(!i||!a||!i->compatible||!clock_profile(a->profile)||impl_->view.searching||impl_->view.calibration||unsigned(source)>unsigned(ClockSource::local_pc)){e="Select a clock source outside search/calibration";return false;}
    if(a->clock_source==source)return true;
    auto c=impl_->view.configuration;for(auto& x:c.assignments)if(x.bus==c.port.instance&&x.address==address)x.clock_source=source;
    if(!impl_->commit(std::move(c),e))return false;
    ++i->generation;impl_->reset_inputs(address);impl_->clock_modes.erase(address);impl_->last_sent.erase({address,4});impl_->last_sent.erase({address,5});
    for(auto it=impl_->outputs.begin();it!=impl_->outputs.end();)if(it->second.address==address)it=impl_->outputs.erase(it);else ++it;
    if(impl_->writing&&!impl_->writing->query&&impl_->writing->address==address&&!impl_->write_offset)impl_->writing.reset();
    i->status="Clock source saved";return true;
}
bool Provider::begin_calibration(unsigned address,std::string& e){std::lock_guard lock(impl_->mutex);const auto i=impl_->instrument(address);const auto a=impl_->assignment(address);if(!i||!a||!i->connected||!i->compatible||impl_->view.searching||impl_->view.calibration||channels(a->profile).empty()){e="Select an assigned, connected needle instrument";return false;}
    const Assignment* previous=a;for(const auto& draft:impl_->view.configuration.drafts)if(same_identity(draft,*a))previous=&draft;const auto remembered=impl_->retained_drafts.find({a->bus,a->address});if(remembered!=impl_->retained_drafts.end()&&same_identity(remembered->second,*a))previous=&remembered->second;
    impl_->edit_identity=*a;++impl_->edit_generation;impl_->view.calibration=CalibrationEdit{address,0,a->profile,previous->calibration,{}};i->calibrating=true;for(auto it=impl_->outputs.begin();it!=impl_->outputs.end();)if(it->second.address==address)it=impl_->outputs.erase(it);else ++it;impl_->events.erase(std::remove_if(impl_->events.begin(),impl_->events.end(),[&](const auto& event){return event.address==address;}),impl_->events.end());impl_->baselines.erase(address);++impl_->view.revision;return true;}
bool Provider::select_needle(unsigned n,std::string& e){std::lock_guard lock(impl_->mutex);auto& edit=impl_->view.calibration;if(!edit||n>=edit->calibration.size()){e="Invalid calibration needle";return false;}for(auto it=impl_->outputs.begin();it!=impl_->outputs.end();)if(it->second.manual&&it->second.address==edit->address)it=impl_->outputs.erase(it);else ++it;impl_->pause_edit();edit->needle=n;edit->available=impl_->edit_ready();return true;}
bool Provider::move(int value,std::string& e){std::lock_guard lock(impl_->mutex);auto& edit=impl_->view.calibration;if(!edit){e="Open calibration first";return false;}const auto cs=channels(edit->profile);const auto& ch=cs[edit->needle];if(value<ch.native_min||value>ch.native_max){e="Position exceeds the vendor profile command range";return false;}const auto i=impl_->instrument(edit->address);if(!i||!impl_->edit_ready()){e="Waiting for the same instrument to reconnect";return false;}edit->requested=value;edit->commanded.reset();impl_->queue(edit->address,edit->needle,ch.command,value,true);return true;}
bool Provider::capture(double value,std::string& e){std::lock_guard lock(impl_->mutex);return impl_->capture(value,e);}
bool Provider::capture_endpoint(bool maximum,double value,std::string& e){std::lock_guard lock(impl_->mutex);auto& edit=impl_->view.calibration;if(!edit||!edit->commanded){e="Move the needle and wait for transmission before capturing";return false;}if(!impl_->edit_ready()){e="Waiting for the same instrument to reconnect";return false;}const auto c=channels(edit->profile)[edit->needle];auto& ps=edit->calibration[edit->needle].points;if(c.firmware||c.cyclic||ps.size()<2){e="This profile has fixed firmware/cardinal endpoints";return false;}if(!std::isfinite(value)||value<c.minimum||value>c.maximum){e="Scale value exceeds the profile range";return false;}if((maximum&&value<=ps[ps.size()-2].value)||(!maximum&&value>=ps[1].value)){e="An endpoint must remain outside all other scale points";return false;}auto& endpoint=maximum?ps.back():ps.front();endpoint={value,*edit->commanded,true};return impl_->checkpoint(e);}
bool Provider::remove_point(double value,std::string& e){std::lock_guard lock(impl_->mutex);auto& edit=impl_->view.calibration;if(!edit){e="Open calibration first";return false;}const auto c=channels(edit->profile)[edit->needle];if(c.firmware||c.cyclic){e="These profile points cannot be removed";return false;}auto& ps=edit->calibration[edit->needle].points;auto it=std::find_if(ps.begin(),ps.end(),[&](const auto& p){return p.value==value;});if(it==ps.end()){e="Scale point not found";return false;}if(it==ps.begin()||it+1==ps.end()){e="Edit minimum/maximum values instead of removing endpoints";return false;}ps.erase(it);return impl_->checkpoint(e);}
bool Provider::save_calibration(std::string& e){std::lock_guard lock(impl_->mutex);const auto edit=impl_->view.calibration;if(!edit){e="Open calibration first";return false;}const auto cs=channels(edit->profile);for(std::size_t n=0;n<cs.size();++n)if(!valid_calibration(cs[n],edit->calibration[n],e)){e=cs[n].name+": "+e;return false;}auto c=impl_->view.configuration;auto a=std::find_if(c.assignments.begin(),c.assignments.end(),[&](const auto& x){return x.bus==c.port.instance&&x.address==edit->address;});if(a==c.assignments.end()||!impl_->edit_identity||!same_identity(*a,*impl_->edit_identity)||!impl_->edit_ready()){e="Waiting for the same assigned instrument to reconnect";return false;}a->calibration=edit->calibration;c.drafts.erase(std::remove_if(c.drafts.begin(),c.drafts.end(),[&](const auto& draft){return draft.bus==a->bus&&draft.address==a->address;}),c.drafts.end());const auto key=std::pair(a->bus,a->address);if(!impl_->commit(std::move(c),e))return false;impl_->cancel_edit();impl_->retained_drafts.erase(key);return true;}
void Provider::cancel_calibration(){std::lock_guard lock(impl_->mutex);impl_->cancel_edit();}
void Provider::publish(const Sources& s){std::lock_guard lock(impl_->mutex);if(!s.session||!s.sequence)return;if(s.session==impl_->sources.session&&s.sequence<=impl_->sources.sequence)return;if(s.session!=impl_->sources.session){impl_->last_sent.clear();impl_->resume_after.clear();impl_->baselines.clear();impl_->baseline_ms.clear();impl_->events.clear();impl_->clock_echo.clear();impl_->clock_modes.clear();for(auto it=impl_->outputs.begin();it!=impl_->outputs.end();)if(!it->second.manual)it=impl_->outputs.erase(it);else ++it;}if(!s.controls_ready){impl_->baselines.clear();impl_->baseline_ms.clear();impl_->events.clear();}impl_->sources=s;}
std::vector<InputEvent> Provider::drain_inputs(){std::lock_guard lock(impl_->mutex);std::vector<InputEvent> out(impl_->events.begin(),impl_->events.end());impl_->events.clear();return out;}
View Provider::snapshot()const {std::lock_guard lock(impl_->mutex);return impl_->view;}
} // namespace hwio::flightillusion
