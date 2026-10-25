// Application composition and commands only. ASS Graphics owns all widgets,
// fonts, rendering, tray behavior, dialogs and the UI message loop.
#include "status_text.hpp"
#include "coordinator/observation_session.hpp"
#include "coordinator/automatic_follow.hpp"
#include "coordinator/scenery_view_control.hpp"
#include "coordinator/runtime_rebind.hpp"
#include <psx/sync/runtime.hpp>
#include <ass_graphics/graphics.hpp>
#include <application-icon.hpp>
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string_view>
#include <vector>

namespace {
namespace gfx=ass::graphics;
using namespace coordinator::ui;
std::wstring widen(std::string_view text) {
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(count,0);MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);return result;
}
std::string narrow(std::wstring_view text) {
    if(text.empty())return {};
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,0);WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);return result;
}
std::wstring timestamp() {
    SYSTEMTIME time{};GetLocalTime(&time);wchar_t text[48]{};
    swprintf_s(text,L"%04u%02u%02u-%02u%02u%02u-%03u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);return text;
}
std::filesystem::path executable_directory() {
    std::wstring path(32768,0);const auto count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!count||count==path.size())throw std::runtime_error("Executable path unavailable");path.resize(count);return std::filesystem::path(path).parent_path();
}

enum Id : unsigned {
    title=1,address_label,ipv4,main_label,main_port,boost_label,boost_port,connect,disconnect,capture,capture_stop,
    follow,stop_motion,mode_label,reconstruction_mode,delay_label,reconstruction_delay,source_preference,mute_audio,
    start_pipe,stop,exit_button,help,tabs,summary,motion,events,timing,support,audio,filter,
    marker_text,marker,log_folder,select_files,export_support,setup_dumps,terrain,aircraft,environment,readiness,traffic,
    xplane_label,xplane_transport,xplane_host_label,xplane_host,enforce_view,view_status,count_ids
};

class CoordinatorUi {
    // Window creation can emit resize before constructor body/children exist.
    bool ready_{},exiting_{},primary_{};
    gfx::Context context_;
    gfx::Window window_;
    psx::windows::WindowIcons window_icons_{static_cast<HWND>(window_.hwnd())};
    float stop_caption_available_{},stop_caption_width_{};unsigned stop_caption_dpi_{};
    std::unique_ptr<coordinator::Runtime> runtime_;
    std::unique_ptr<gfx::Tray> tray_;
    gfx::Control tabs_;
    gfx::Control stop_caption_measure_;
    std::array<gfx::Control,11> pages_;
    std::array<gfx::Control,count_ids> controls_;
    std::vector<std::wstring> selected_files_;
    HANDLE setup_process_{};
    std::int64_t started_=coordinator::qpc_now();
    int last_callback_freshness_=-1,smoke_step_{},exit_code_{};
    std::wstring smoke_path_;
    std::string smoke_log_;
    psx::sync::Settings network_settings_;
    psx::sync::Settings pending_network_settings_;
    coordinator::DeferredRuntimeRebind rebind_;
    coordinator::AutomaticFollow automatic_follow_;
    coordinator::SceneryViewControl scenery_view_;
    std::int64_t automatic_attempt_{};

    static void __cdecl callback(void* user,uint32_t id,uint32_t event,double) noexcept {
        auto& app=*static_cast<CoordinatorUi*>(user);
        try { app.event(static_cast<Id>(id),event); }
        catch(const std::exception& error) {
            const auto graphics=dynamic_cast<const gfx::Error*>(&error);
            const auto message=graphics?narrow(graphics->detail):std::string(error.what());
            if(app.runtime_)app.runtime_->log.add(coordinator::Level::error,message);
            if(!app.smoke_path_.empty()) { app.smoke_log_+="FAIL "+message+"\n";app.exit_code_=1;app.request_exit(); }
        }
    }
    void add(Id id,unsigned kind,const wchar_t* text,void* parent=nullptr,double min=0,double max=1,double initial=0) {
        ag_control_desc d{};d.id=id;d.kind=kind;d.text=text;d.bounds={0,0,100,30};d.minimum=min;d.maximum=max;d.step=1;d.value=initial;
        controls_[id]=gfx::Control(context_,parent?parent:window_.hwnd(),d,callback,this);
    }
    void choice(Id id,std::initializer_list<const wchar_t*> items,int initial,void* parent=nullptr) {
        add(id,AG_CHOICE,L"",parent);for(auto item:items)controls_[id].add(item);controls_[id].value(initial);
    }
    void build() {
        add(title,AG_LABEL,L"PSX / X-Plane coordinator");
        add(address_label,AG_LABEL,L"PSX IPv4");add(ipv4,AG_EDIT,L"127.0.0.1");
        add(main_label,AG_LABEL,L"Main");add(main_port,AG_NUMERIC,L"",nullptr,1,65535,10747);
        add(boost_label,AG_LABEL,L"Boost");add(boost_port,AG_NUMERIC,L"",nullptr,1,65535,10749);
        add(connect,AG_BUTTON,L"Connect");add(disconnect,AG_BUTTON,L"Disconnect");
        add(xplane_label,AG_LABEL,L"X-Plane connection");choice(xplane_transport,{L"Local pipe",L"Remote"},network_settings_.enabled?1:0);
        add(xplane_host_label,AG_LABEL,L"X-Plane host IP");add(xplane_host,AG_EDIT,widen(network_settings_.host.empty()?"127.0.0.1":network_settings_.host).c_str());
        add(capture,AG_BUTTON,L"Start capture...");add(capture_stop,AG_BUTTON,L"Stop capture");
        add(follow,AG_BUTTON,L"Align and Follow PSX");add(stop_motion,AG_BUTTON,L"Stop synchronization");
        // ASS1.1.8 measures labels only; share its native font without displaying a second caption.
        ag_control_desc measure{};measure.kind=AG_LABEL;measure.text=L"Stop synchronization";measure.bounds={-10000,-10000,-9000,-9972};
        stop_caption_measure_=gfx::Control(context_,window_.hwnd(),measure);
        add(mode_label,AG_LABEL,L"Next Follow");choice(reconstruction_mode,{L"Direct",L"Interpolated"},1);
        add(delay_label,AG_LABEL,L"Delay ms");add(reconstruction_delay,AG_NUMERIC,L"",nullptr,10,50,25);
        choice(source_preference,{L"Auto source",L"Public Main/Boost"},0);add(mute_audio,AG_CHECKBOX,L"Mute Aircraft Audio");
        add(enforce_view,AG_CHECKBOX,L"enforce always view without flightdeck");add(view_status,AG_LABEL,L"X-Plane view control unavailable");
        add(start_pipe,AG_BUTTON,L"Start diagnostic pipe");add(stop,AG_BUTTON,L"Stop transports");add(exit_button,AG_BUTTON,L"Exit");
        add(help,AG_LABEL,L"Follow selects motion, audio, terrain, environment and aircraft state. Aircraft animation failed loaded use and is deferred. PSX retains physics; Stop holds its last terrain plane.\r\nConnect observes and keeps X-Plane unpaused. Capture is opt-in.");
        ag_control_desc tab_desc{};tab_desc.kind=AG_TABS;tab_desc.id=tabs;tab_desc.bounds={0,0,100,100};tabs_=gfx::Control(context_,window_.hwnd(),tab_desc,callback,this);
        const wchar_t* names[]={L"Overview",L"Motion",L"Events",L"Timing",L"Recording / Support",L"Audio",L"Terrain",L"Aircraft",L"Environment",L"Simulator states",L"Traffic"};
        constexpr Id outputs[]={summary,motion,events,timing,support,audio,terrain,aircraft,environment,readiness,traffic};
        for(size_t i=0;i<pages_.size();++i) { pages_[i]=tabs_.tab(names[i]);add(outputs[i],AG_STATUS,L"",pages_[i].hwnd()); }
        choice(filter,{L"All severities",L"Info and above",L"Warnings and errors",L"Errors and fatal"},1,pages_[2].hwnd());
        const auto support_parent=pages_[4].hwnd();
        add(marker_text,AG_EDIT,L"Operator marker",support_parent);add(marker,AG_BUTTON,L"Add marker",support_parent);
        add(log_folder,AG_BUTTON,L"Open log folder",support_parent);add(select_files,AG_BUTTON,L"Select captures / logs / dumps...",support_parent);
        add(export_support,AG_BUTTON,L"Export support folder...",support_parent);add(setup_dumps,AG_BUTTON,L"Apply WER policy (admin)",support_parent);
    }
    void layout() {
        if(!ready_)return;
        const auto r=window_.client_bounds();if(r.right<1||r.bottom<1)return;const float right=r.right-16;
        controls_[title].bounds({16,12,right,38});
        float x=16,y=48;
        const auto place=[&](Id id,float width) { if(x+width>right&&x>16){x=16;y+=38;}controls_[id].bounds({x,y,x+width,y+28});x+=width+8; };
        place(address_label,70);place(ipv4,125);place(main_label,36);place(main_port,70);place(boost_label,40);place(boost_port,70);
        place(connect,90);place(disconnect,95);place(capture,130);place(capture_stop,110);
        x=16;y+=40;place(xplane_label,145);place(xplane_transport,135);place(xplane_host_label,135);place(xplane_host,150);
        x=16;y+=40;
        const auto stop_dpi=GetDpiForWindow(static_cast<HWND>(window_.hwnd()));const auto stop_available=std::max(1.f,right-16);
        if(stop_caption_available_!=stop_available||stop_caption_dpi_!=stop_dpi){stop_caption_width_=stop_caption_measure_.text_extent(stop_available).x+24;stop_caption_available_=stop_available;stop_caption_dpi_=stop_dpi;}
        place(follow,175);place(stop_motion,std::max(110.f,stop_caption_width_));place(mode_label,92);place(reconstruction_mode,125);place(delay_label,64);place(reconstruction_delay,48);place(source_preference,172);
        x=16;y+=40;place(mute_audio,172);
        x=16;y+=32;place(enforce_view,350);place(view_status,440);
        x=16;y+=40;place(start_pipe,175);place(stop,135);place(exit_button,80);
        controls_[help].bounds({16,r.bottom-58,right,r.bottom-8});
        tabs_.bounds({16,y+42,right,std::max(y+152,r.bottom-70)});
        constexpr Id outputs[]={summary,motion,events,timing,support,audio,terrain,aircraft,environment,readiness,traffic};
        for(size_t i=0;i<pages_.size();++i) {
            const auto page=pages_[i].client_bounds();float top=8;
            if(i==2){controls_[filter].bounds({8,8,300,36});top=44;}
            if(i==4) {
                float sx=8,sy=8;
                const auto place_support=[&](Id id,float width){if(sx+width>page.right-8&&sx>8){sx=8;sy+=38;}controls_[id].bounds({sx,sy,sx+width,sy+28});sx+=width+8;};
                place_support(marker_text,250);place_support(marker,100);place_support(log_folder,140);place_support(select_files,250);
                place_support(export_support,195);place_support(setup_dumps,215);top=sy+40;
            }
            controls_[outputs[i]].bounds({8,top,std::max(16.f,page.right-8),std::max(top+40,page.bottom-8)});
        }
    }
    void request_exit() noexcept {
        if(exiting_||!runtime_)return;exiting_=true;rebind_.cancel();runtime_->request_stop();
        try { window_.title(L"PSX / X-Plane coordinator - stopping");for(auto& c:controls_)if(c.handle())c.enabled(false); } catch(...) {}
    }
    void event(Id id,uint32_t event_kind) {
        if(!ready_)return;
        if(event_kind==AG_RESIZE){layout();return;}
        if(event_kind==AG_CLOSE){request_exit();return;}
        if(event_kind==AG_TIMER){tick();return;}
        if(exiting_)return;
        if(event_kind==AG_VALUE_CHANGED){if(id==tabs)layout();if(id==xplane_transport||id==xplane_host){controls_[xplane_host].enabled(controls_[xplane_transport].value()!=0);if(smoke_path_.empty())try{auto next=network_settings_;next.enabled=controls_[xplane_transport].value()!=0;next.host=narrow(controls_[xplane_host].text());psx::sync::save_connection(next);}catch(const std::exception&){/* Keep incomplete edits private until a valid address or Connect. */}}return;}
        if(event_kind!=AG_CLICK)return;
        switch(id) {
        case connect:automatic_follow_.connect();connect_psx();break;
        case disconnect:rebind_.cancel();automatic_follow_.disconnect();runtime_->observation.disconnect();break;
        case follow:motion_action(psx::motion::Action::follow);break;
        case stop_motion:rebind_.cancel();motion_action(psx::motion::Action::stop);break;
        case mute_audio:runtime_->observation.mute_aircraft_audio(controls_[mute_audio].value()!=0);break;
        case enforce_view:
            if(!scenery_view_.select(controls_[enforce_view].value()!=0,coordinator::qpc_now(),[&](auto action){return runtime_->observation.request_scenery_view(action);}))
                runtime_->log.add(coordinator::Level::warning,"View request unavailable; selection not confirmed");
            break;
        case stop:rebind_.cancel();automatic_follow_.disconnect();runtime_->request_stop();break;
        case exit_button:request_exit();break;
        case start_pipe:start_diagnostic_pipe();break;
        case capture:start_capture();break;
        case capture_stop:runtime_->observation.stop_capture();break;
        case marker:runtime_->marker(narrow(controls_[marker_text].text()).substr(0,159));break;
        case log_folder:if(reinterpret_cast<INT_PTR>(ShellExecuteW(static_cast<HWND>(window_.hwnd()),L"open",runtime_->diagnostics.options().directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)runtime_->log.add(coordinator::Level::warning,"Cannot open log folder");break;
        case select_files:{auto selected=pick(4,L"Select existing captures, logs or dumps",L"Captures, logs and dumps",L"*.psxcap;*.psxobs;*.pxtcap;*.aircraft;*.environment;*.readiness;*.log;*.txt;*.json;*.jsonl;*.rpt;*.dmp");if(!selected.empty())selected_files_=std::move(selected);break;}
        case export_support:export_evidence();break;
        case setup_dumps:setup_wer();break;
        default:break;
        }
        Refresh();
    }
    std::vector<std::wstring> pick(uint32_t mode,const wchar_t* title_text,const wchar_t* filter_name=nullptr,const wchar_t* pattern=nullptr,const wchar_t* default_name=nullptr) {
        struct Selection {std::vector<std::wstring> files;std::exception_ptr error; } selection;
        const auto receive=[](void* user,const wchar_t* path) noexcept {auto& s=*static_cast<Selection*>(user);try{s.files.emplace_back(path);}catch(...){s.error=std::current_exception();}};
        uint32_t accepted{};gfx::check(ag_file_dialog(context_.handle(),window_.hwnd(),mode,title_text,filter_name,pattern,default_name,receive,&selection,&accepted));
        if(selection.error)std::rethrow_exception(selection.error);return accepted?std::move(selection.files):std::vector<std::wstring>{};
    }
    void connect_psx() {
        if(rebind_.pending()){
            if(!rebind_.advance(*runtime_,[&]{
                psx::sync::stop_runtime();network_settings_=std::move(pending_network_settings_);
                const bool started=psx::sync::start_runtime(psx::sync::Role::coordinator,network_settings_);
                runtime_->start_non_normal();automatic_follow_.reset_binding();
                if(!started)throw std::runtime_error("X-Plane network startup failed; retrying remains available");
            }))return;
        }else{
            if(!runtime_->observation.done())return;
            if(smoke_path_.empty()){
                auto selected=network_settings_;selected.enabled=controls_[xplane_transport].value()!=0;selected.host=narrow(controls_[xplane_host].text());psx::sync::save_connection(selected);
                if(rebind_.required(selected.enabled!=network_settings_.enabled||selected.host!=network_settings_.host||(selected.enabled&&!psx::sync::runtime()))){
                    // Stop posts cancellation. A later timer joins only completed
                    // owners before replacing the endpoint; filesystem I/O may still
                    // be pending, so this UI callback must return without waiting.
                    pending_network_settings_=std::move(selected);rebind_.request(*runtime_);return;
                }
            }
        }
        const auto main=controls_[main_port].value(),boost=controls_[boost_port].value();
        if(main!=std::floor(main)||boost!=std::floor(boost))throw std::runtime_error("Ports must be integers 1..65535");
        coordinator::ObservationEndpoint endpoint;endpoint.ipv4=narrow(controls_[ipv4].text());endpoint.main_port=static_cast<uint16_t>(main);endpoint.boost_port=static_cast<uint16_t>(boost);
        runtime_->observation.join();if(!runtime_->observation.connect(endpoint))runtime_->log.add(coordinator::Level::error,"Connect rejected: enter numeric IPv4 and distinct nonzero ports");
    }
    void motion_action(psx::motion::Action action) {
        if(action==psx::motion::Action::stop)automatic_follow_.stop();else automatic_follow_.follow();
        psx::motion::ReconstructionSettings settings;
        if(action==psx::motion::Action::follow) {
            const auto delay=controls_[reconstruction_delay].value();if(delay!=std::floor(delay))throw std::runtime_error("Interpolation delay must be a whole number of milliseconds");
            settings.mode=controls_[reconstruction_mode].value()==0?psx::motion::ReconstructionMode::direct:psx::motion::ReconstructionMode::interpolated;settings.delay_ms=static_cast<uint16_t>(delay);
        }
        const auto id=runtime_->observation.request_motion(action,settings,controls_[source_preference].value()==0?psx::motion::SourcePreference::automatic:psx::motion::SourcePreference::public_only);
        if(action==psx::motion::Action::follow)automatic_follow_.requested(id);
        runtime_->log.add(id?coordinator::Level::info:coordinator::Level::warning,id?("Motion request queued #"+std::to_string(id)):"Motion request unavailable; environment and aircraft-state selection recorded independently when connected");tabs_.value(1);
    }
    void start_diagnostic_pipe() {
        if(!runtime_->pipe.done())return;runtime_->pipe.join();
        if(!runtime_->pipe.start(L"\\\\.\\pipe\\psx-xp-coordinator.diagnostic.v1."+std::to_wstring(GetCurrentProcessId())))runtime_->log.add(coordinator::Level::error,"Diagnostic pipe start rejected");
    }
    void start_capture() {
        const auto name=L"psx-observation-"+timestamp()+L".psxcap";
        const auto files=pick(1,L"Record selected PSX observations",L"PSX capture",L"*.psxcap",name.c_str());
        if(!files.empty()&&!runtime_->observation.start_capture(files.front()))runtime_->log.add(coordinator::Level::warning,"Capture start rejected; connect first and choose a new file");
    }
    void export_evidence() {
        if(!runtime_->exporter.done())return;const auto folder=pick(2,L"Choose parent folder for a new support folder");if(folder.empty())return;
        coordinator::ExportRequest request;request.directory=(std::filesystem::path(folder.front())/(L"psx-support-"+timestamp())).wstring();request.selected_files=selected_files_;request.observation=runtime_->observation.snapshot();
        request.health=runtime_->health.evaluate(request.observation,runtime_->diagnostics.status(),coordinator::qpc_now(),coordinator::qpc_frequency(),runtime_->diagnostics.options().config.delayed_ms);
        if(!runtime_->exporter.start(std::move(request)))runtime_->log.add(coordinator::Level::warning,"Another support export is running");
    }
    void setup_wer() {
        if(setup_process_)return;const auto helper=(executable_directory()/L"psx-diagnostics-setup.exe").wstring();
        const auto args=std::wstring(runtime_->diagnostics.options().config.crash_dumps?L"--enable \"":L"--disable \"")+coordinator::crash_dump_directory()+L"\"";
        SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_NOCLOSEPROCESS;info.lpVerb=L"runas";info.lpFile=helper.c_str();info.lpParameters=args.c_str();info.nShow=SW_HIDE;
        if(ShellExecuteExW(&info)){setup_process_=info.hProcess;runtime_->marker("Operator requested explicit elevated coordinator WER setup");}
        else runtime_->log.add(coordinator::Level::warning,"WER setup cancelled or failed");
    }
    void Refresh() {
        auto s = runtime_->status.snapshot();
        auto observation=runtime_->observation.snapshot();const auto& source=observation.latest.session();
        const auto journal=runtime_->diagnostics.status();const auto health=runtime_->health.evaluate(observation,journal,coordinator::qpc_now(),coordinator::qpc_frequency(),runtime_->diagnostics.options().config.delayed_ms);
        if(static_cast<int>(health.callback)!=last_callback_freshness_){if(health.callback!=psx::diagnostics::Freshness::unavailable)runtime_->diagnostics.producer(0,psx::diagnostics::Component::plugin).emit(health.callback==psx::diagnostics::Freshness::delayed?psx::diagnostics::Code::callback_delayed:psx::diagnostics::Code::callback_resumed,psx::diagnostics::Severity::info);last_callback_freshness_=static_cast<int>(health.callback);}
        std::ostringstream text;text<<"Main "<<coordinator::state_name(observation.main.state)<<"  Boost "<<coordinator::state_name(observation.boost.state)
            <<"  Plugin "<<coordinator::state_name(observation.bridge)<<" / diagnostics "<<(observation.plugin_available?"available":"unavailable")
            <<"\r\nCoordinator PID "<<GetCurrentProcessId()<<" / pipe peer PID "<<observation.bridge_peer_process_id<<" / bridge Windows error "<<observation.bridge_last_error
            <<"\r\nX-Plane: "<<coordinator::xplane_state_name(observation.readiness.xplane)<<"   PSX: "<<coordinator::psx_state_name(observation.readiness.psx)<<" (observations)"
            <<"\r\nFollow: "<<follow_availability(observation)
            <<"\r\nVersion "<<(source.psx_version[0]?source.psx_version.data():"unknown")<<" (informational; evidence "<<((source.flags&psx::bridge::version_qualified)?"audited":"unqualified")<<") family "<<static_cast<unsigned>(source.family)
            <<" session "<<source.session_epoch<<" configuration "<<source.configuration_epoch<<" Boost generation "<<source.boost_generation<<" flags "<<source.flags
            <<"\r\nObservation "<<(((source.flags&psx::bridge::synchronized) && (source.flags&psx::bridge::boost_available) && source.family!=psx::aircraft::EngineFamily::unknown)?"ready":"waiting for Main/bootstrap/family/Boost")
            <<"\r\nMain RX "<<observation.main.bytes_in<<" B / lines "<<observation.main.lines<<" retry "<<observation.main.retry_seconds<<"s; Boost RX "<<observation.boost.bytes_in<<" B / lines "<<observation.boost.lines<<" retry "<<observation.boost.retry_seconds<<"s"
            <<"\r\nMalformed "<<observation.malformed<<" bootstrap errors "<<observation.bootstrap_errors<<" pipe rejects "<<observation.bridge_rejected<<" coalesced "<<observation.bridge_coalesced
            <<"\r\nCapture "<<(observation.capture.active?"RECORDING":observation.capture.finalizing?"finishing files":observation.capture.incomplete?"INCOMPLETE":observation.capture.complete?"complete":"off")<<" bytes "<<observation.capture.bytes<<" queued "<<observation.capture.queued<<" error "<<observation.capture.error<<" reason "<<coordinator::capture_stop_reason(observation.capture.stop_reason)
            <<"\r\nCallback "<<observation.plugin.callback_count<<" aircraft "<<observation.plugin.aircraft.data()<<"\r\n";
        pause_text(text,observation);
        if(auto* link=psx::sync::runtime()){
            const auto network=link->status();
            text<<"NORM unicast: "<<(network.connected?"connected":"reconnecting")<<" / UDP "<<network_settings_.receive_port<<"-"<<(network_settings_.receive_port+7)
                <<" / peer "<<network.remote_address.data()<<":"<<network.remote_receive_port<<" / error "<<network.error<<" "<<network.failure.data()
                <<" / remote declared PID "<<network.peer.pid<<" / clock "<<(network.clock_valid?"mapped":"unavailable")<<" +/- "<<network.clock_uncertainty_ms<<" ms"
                <<" / replaced "<<network.replaced<<" expired "<<network.expired<<" rejected "<<network.rejected
                <<" / automatic "<<(automatic_follow_.inhibited()?"stopped for this run":network_settings_.automatic_start?"connect and follow":"off")<<"\r\n";
        }
        distribution_text(text,"Receive->decode",observation.decode_duration);distribution_text(text,"Receive->callback",observation.plugin.receive_to_callback);distribution_text(text,"Callback duration",observation.plugin.callback_duration);distribution_text(text,"Frame interval",observation.plugin.frame_interval);
        for(std::size_t i=1;i<psx::bridge::source_slots;++i)if(observation.latest.available()[i]) {
            const auto& record=observation.latest.records()[i].observation;
            text<<"Source "<<i<<" seq "<<record.stamp.sequence<<" consumed "<<observation.plugin.consumed_sequence[i]<<" age "<<(coordinator::qpc_now()-record.stamp.received_qpc)*1.0/coordinator::qpc_frequency()<<"s : ";
            for(std::size_t j=0;j<record.count && j<8;++j){const auto& p=record.values[j];text<<psx::aircraft::property_info(p.id)->name<<'['<<unsigned(p.element)<<"]=";if(p.validity==psx::aircraft::ValueValidity::valid)text<<p.value;else text<<"unavailable";text<<' ';}text<<"\r\n";
        }
        text<<"\r\nReport freshness: "<<freshness_name(health.report)<<" | Callback freshness: "<<freshness_name(health.callback)<<"\r\nJournal: "<<(!runtime_->diagnostics.options().config.enabled?"disabled":journal.persistence?"writing":journal.running?"starting/unavailable":"stopped")<<" status="<<(health.storage_degraded?"degraded":"no reported failure")<<" lost="<<journal.lost<<" persistence lost="<<journal.persistence_lost<<" storage Win32="<<journal.persistence_error<<" last event error="<<journal.last_error<<"\r\n";
        text<<describe(s.pipe,"Separate diagnostic pipe");controls_[summary].status(widen(text.str().c_str()).c_str());controls_[timing].status(widen(timing_text(observation).c_str()).c_str());
        controls_[environment].status(widen(environment_text(observation).c_str()).c_str());controls_[readiness].status(widen(readiness_text(observation)).c_str());controls_[traffic].status(widen(traffic_text(observation)).c_str());
        controls_[motion].status(widen(motion_text(observation).c_str()).c_str());controls_[audio].status(widen(audio_text(observation).c_str()).c_str());
        controls_[terrain].status(widen(terrain_text(observation)).c_str());controls_[aircraft].status(widen(aircraft_text(observation)).c_str());
        coordinator::WindowsCrashRegistry registry;const auto policy=coordinator::inspect_crash_policy(registry,runtime_->diagnostics.options().config.crash_dumps,coordinator::crash_dump_directory());const auto exported=runtime_->exporter.status();
        std::ostringstream support_text;support_text<<"Capture ID="<<observation.capture.capture_id<<" cutoff acknowledged="<<observation.capture.cutoff.acknowledged<<"\r\nOwner cutoff QPC="<<observation.capture.cutoff.owner_qpc<<" ingestion sequence="<<observation.capture.cutoff.ingestion_sequence<<" footer QPC="<<observation.capture.finalized_qpc
            <<"\r\nCallback lifetime counters baseline valid="<<health.counter_baseline_valid<<" delta valid="<<health.counter_delta_valid<<"\r\nSuperseded diagnostics since baseline="<<health.diagnostic_dropped<<" structured events lost since baseline="<<health.events_lost<<" persistence lost="<<health.persistence_lost
            <<"\r\nWER desired="<<policy.desired<<" effective matching mini/count5="<<policy.effective<<" owned="<<policy.owned<<" conflict="<<policy.conflict<<" needs explicit setup="<<policy.needs_setup<<" Win32="<<policy.error
            <<"\r\nActual per-exe key="<<policy.key_exists<<" type="<<(policy.type_present?std::to_string(policy.actual_type):"absent")<<" count="<<(policy.count_present?std::to_string(policy.actual_count):"absent")<<"\r\nInherited/global policy is not evaluated or changed.\r\nEdit [crash_dumps] enabled in per-user diagnostics.ini, restart, then explicitly apply. Conflicts are preserved.\r\nExport="<<(exported.running?"running":exported.complete?"complete":exported.partial?"partial":"idle")<<" files="<<exported.files<<" Win32="<<exported.error<<"\r\nSelected optional files="<<selected_files_.size()<<"\r\nExports contain local evidence; existing X-Plane logs/dumps must be selected above.";
        controls_[support].status(widen(support_text.str().c_str()).c_str());controls_[export_support].enabled(!exiting_ && !exported.running);
        if(setup_process_!=nullptr && WaitForSingleObject(setup_process_,0)==WAIT_OBJECT_0){DWORD code=0;GetExitCodeProcess(setup_process_,&code);CloseHandle(setup_process_);setup_process_=nullptr;runtime_->diagnostics.producer(0,psx::diagnostics::Component::crash_dump).emit(psx::diagnostics::Code::crash_policy,code?psx::diagnostics::Severity::warning:psx::diagnostics::Severity::info,{},psx::diagnostics::ErrorDomain::win32,code,"WER setup helper completed");}
        controls_[setup_dumps].enabled(!exiting_ && setup_process_==nullptr);
        controls_[connect].enabled(!exiting_ && !rebind_.pending() && runtime_->observation.done());controls_[disconnect].enabled(!exiting_ && (rebind_.pending()||!runtime_->observation.done()));
        controls_[xplane_transport].enabled(!exiting_&&!rebind_.pending()&&runtime_->observation.done());controls_[xplane_host].enabled(!exiting_&&!rebind_.pending()&&runtime_->observation.done()&&controls_[xplane_transport].value()!=0);
        scenery_view_.update(observation,coordinator::qpc_now(),coordinator::qpc_frequency(),[&](auto action){return exiting_?std::uint64_t{}:runtime_->observation.request_scenery_view(action);});
        controls_[enforce_view].value(scenery_view_.checked()?1:0);controls_[enforce_view].enabled(!exiting_&&scenery_view_.enabled());
        controls_[view_status].text(scenery_view_.status());
        const bool motion_connected=!exiting_ && observation.motion.connected && observation.motion.identity_available && !observation.motion.transport_fault;
        controls_[stop_motion].enabled(motion_connected||(!exiting_&&(observation.environment.following||observation.aircraft.following)));
        controls_[follow].enabled(!exiting_ && !runtime_->observation.done() && !observation.motion.outstanding);
        controls_[capture].enabled(!exiting_ && !runtime_->observation.done() && !observation.capture.active&&!observation.capture.finalizing);controls_[capture_stop].enabled(!exiting_ && observation.capture.active);
        controls_[start_pipe].enabled(!exiting_ && runtime_->pipe.done());
        auto log = runtime_->log.snapshot();
        {
            std::ostringstream out;
            const auto threshold=controls_[filter].value();
            for(const auto& event:runtime_->diagnostics.recent()) {
                if(static_cast<int>(event.severity)<threshold)continue;
                out<<event.context.run_id<<':'<<event.producer<<':'<<event.sequence<<" QPC "<<event.qpc<<' '<<psx::diagnostics::name(event.severity)<<' '<<psx::diagnostics::name(event.component)<<' '<<psx::diagnostics::name(event.code)<<" error("<<unsigned(event.error_domain)<<")="<<event.error<<' '<<event.text.data()<<"\r\n";
            }
            out<<"\r\nCompatibility UI/diagnostic Ping messages:\r\n";
            for (const auto& e : log) {
                const auto severity=e.level==coordinator::Level::error?3:e.level==coordinator::Level::warning?2:1;if(severity<threshold)continue;
                out << e.sequence << "  +" << static_cast<double>(e.qpc - started_) / static_cast<double>(coordinator::qpc_frequency()) << " s  "
                    << (e.level == coordinator::Level::error ? "ERROR " : e.level == coordinator::Level::warning ? "WARN  " : "INFO  ") << e.text.data() << "\r\n";
            }
            out << "[overwritten events: " << runtime_->log.overwritten() << "]\r\n";
            controls_[events].status(widen(out.str().c_str()).c_str());
        }
    }

    void check(bool value,const char* description) {
        if(!value)throw std::runtime_error(description);smoke_log_+="PASS "+std::string(description)+"\n";
    }
    void smoke() {
        const auto hwnd=static_cast<HWND>(window_.hwnd());
        switch(smoke_step_++) {
        case 0: {
            std::array<wchar_t,256> observed_title{};const auto title_count=GetWindowTextW(hwnd,observed_title.data(),static_cast<int>(observed_title.size()));
            check(title_count>0,"native window title readable");
            smoke_log_+="VERSION_TITLE="+narrow(observed_title.data())+"\nVERSION_BUILD="+runtime_->diagnostics.options().build+"\n";
            check(!IsWindowVisible(hwnd),"startup hidden in tray");window_.show();
            check(IsWindowVisible(hwnd)!=0,"native library window visible");
            check(window_icons_.valid()&&SendMessageW(hwnd,WM_GETICON,ICON_SMALL,0)!=0&&SendMessageW(hwnd,WM_GETICON,ICON_BIG,0)!=0,"embedded CO small and large window icons");
            check(runtime_->stopped(),"startup disconnected; no simulator sockets or restored Follow");
            check(IsWindowEnabled(static_cast<HWND>(controls_[connect].hwnd()))&&!IsWindowEnabled(static_cast<HWND>(controls_[follow].hwnd())),"Connect explicit; Follow unavailable without sources");
            check(std::wstring(context_.theme().font_family)==L"B612","library default B612 theme");
            check(controls_[enforce_view].text()==L"enforce always view without flightdeck","exact scenery-only checkbox caption");
            check(controls_[enforce_view].value()==0&&!IsWindowEnabled(static_cast<HWND>(controls_[enforce_view].hwnd())),"view checkbox unchecked and unavailable without plugin");
            RECT mute_rect{},view_rect{};GetWindowRect(static_cast<HWND>(controls_[mute_audio].hwnd()),&mute_rect);GetWindowRect(static_cast<HWND>(controls_[enforce_view].hwnd()),&view_rect);
            check(view_rect.top>=mute_rect.bottom&&view_rect.left==mute_rect.left,"view checkbox directly below mute audio");
            for(auto& c:controls_)if(c.handle()) {
                const auto font=reinterpret_cast<HFONT>(SendMessageW(static_cast<HWND>(c.hwnd()),WM_GETFONT,0,0));LOGFONTW data{};
                check(font&&GetObjectW(font,sizeof(data),&data)&&std::wstring(data.lfFaceName)==L"B612","control uses shared B612 font");
            }
            check(controls_[reconstruction_delay].value()==25&&controls_[reconstruction_mode].value()==1&&controls_[source_preference].value()==0,"existing Auto / Interpolated 25 ms defaults");
            controls_[reconstruction_mode].value(0);controls_[source_preference].value(1);controls_[reconstruction_delay].value(30);
            check(controls_[reconstruction_mode].value()==0&&controls_[source_preference].value()==1&&controls_[reconstruction_delay].value()==30,"choice and numeric values remain independently editable");
            controls_[reconstruction_mode].value(1);controls_[source_preference].value(0);controls_[reconstruction_delay].value(25);
            ShowWindow(hwnd,SW_MINIMIZE);break;
        }
        case 1:check(IsIconic(hwnd)!=0,"taskbar minimize");window_.show();break;
        case 2:check(!IsIconic(hwnd)&&IsWindowVisible(hwnd),"library restore");SendMessageW(hwnd,WM_CLOSE,0,0);break;
        case 3:check(!IsWindowVisible(hwnd)&&!exiting_,"close hides to library tray without stopping workers");window_.show();break;
        case 4:start_diagnostic_pipe();break;
        case 5:if(runtime_->status.snapshot().pipe.state==coordinator::State::starting){--smoke_step_;break;}check(runtime_->status.snapshot().pipe.state==coordinator::State::listening,"diagnostic pipe explicit startup");break;
        default: {
            const int page=smoke_step_-7;
            if(page<static_cast<int>(pages_.size())){tabs_.value(page);layout();Refresh();const auto image=smoke_path_+L"-tab"+std::to_wstring(page)+L".png";gfx::check(ag_window_export_png(window_.handle(),image.c_str()));}
            else {window_.close();check(exiting_&&IsWindow(hwnd),"library deferred close lets transports finish");}
            break;
        }
        }
    }
    void tick() {
        if(!exiting_&&rebind_.pending())connect_psx();
        if(!exiting_&&!rebind_.pending()&&network_settings_.enabled&&network_settings_.automatic_start&&!automatic_follow_.inhibited()){
            const auto now=coordinator::qpc_now();
            if(now-automatic_attempt_>=coordinator::qpc_frequency()){
                automatic_attempt_=now;
                if(runtime_->observation.done())connect_psx();
                else {
                    const auto state=runtime_->observation.snapshot();
                    // Automatic Follow also owns the independent services when
                    // retained motion already follows; do not place again.
                    runtime_->observation.resume_automatic_follow_services();
                    if(automatic_follow_.eligible(state.motion,state.bridge_attachments)&&
                       state.plugin_available&&state.plugin.callback_count&&now>=state.plugin.callback_qpc&&now-state.plugin.callback_qpc<coordinator::qpc_frequency()){

                        psx::motion::ReconstructionSettings selected;
                        selected.mode=controls_[reconstruction_mode].value()==0?psx::motion::ReconstructionMode::direct:psx::motion::ReconstructionMode::interpolated;
                        selected.delay_ms=static_cast<std::uint16_t>(controls_[reconstruction_delay].value());
                        automatic_follow_.requested(runtime_->observation.request_motion(psx::motion::Action::follow,selected,controls_[source_preference].value()==0?psx::motion::SourcePreference::automatic:psx::motion::SourcePreference::public_only));
                    }
                }
            }
        }
        Refresh();if(!smoke_path_.empty()&&!exiting_)smoke();
        if(exiting_&&runtime_->stopped()) {
            runtime_->join();
            if(!smoke_path_.empty()){smoke_log_+="PASS workers stopped and joined\n";std::ofstream(std::filesystem::path(smoke_path_))<<smoke_log_;}
            window_.timer(1,0);ready_=false;window_.defer_close(false);window_.close();
        }
    }
public:
    explicit CoordinatorUi(std::wstring smoke_path):window_(context_,widen("PSX / X-Plane coordinator " PSX_COORDINATOR_VERSION).c_str(),1180,800,callback,this),smoke_path_(std::move(smoke_path)) {
        const auto identity=smoke_path_.empty()?L"PSX-IOS.Coordinator":L"PSX-IOS.Coordinator.Smoke."+std::to_wstring(GetCurrentProcessId());
        primary_=window_.single_instance(identity.c_str());if(!primary_)return;
        window_.title((widen("PSX / X-Plane coordinator " PSX_COORDINATOR_VERSION)+L" - PID "+std::to_wstring(GetCurrentProcessId())).c_str());
        auto options=psx::diagnostics::default_options(psx::diagnostics::Component::application);
        if(!smoke_path_.empty()){options.directory=(std::filesystem::path(smoke_path_).parent_path()/(L"ui-diagnostics-"+std::to_wstring(psx::diagnostics::new_identity()))).wstring();options.config_path=(std::filesystem::path(options.directory)/L"unused.ini").wstring();options.config={};}
        options.build="psx-xp-coordinator network-" PSX_COORDINATOR_VERSION;runtime_=std::make_unique<coordinator::Runtime>(options);
        if(smoke_path_.empty()){
            network_settings_=psx::sync::read_settings(psx::sync::Role::coordinator);
            if(!psx::sync::start_runtime(psx::sync::Role::coordinator,network_settings_))runtime_->log.add(coordinator::Level::warning,"X-Plane network startup unavailable; automatic retries remain active");
            runtime_->start_non_normal();
        }
        build();controls_[xplane_host].enabled(network_settings_.enabled);try{tray_=std::make_unique<gfx::Tray>(window_,L"PSX / X-Plane coordinator");}catch(const std::exception&){runtime_->log.add(coordinator::Level::warning,"Tray unavailable; Close exits");}
        window_.defer_close(true);gfx::check(ag_window_minimum(window_.handle(),960,620));ready_=true;layout();Refresh();window_.fit();if(!tray_)window_.show();window_.timer(1,250);
    }
    ~CoordinatorUi(){if(runtime_){runtime_->request_stop();runtime_->join();}if(smoke_path_.empty())psx::sync::stop_runtime();if(setup_process_)CloseHandle(setup_process_);}
    int run(){if(!primary_)return 0;window_.run();return exit_code_;}
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        int count{};auto argv=CommandLineToArgvW(GetCommandLineW(),&count);if(!argv)return 2;
        std::wstring smoke_path;if(count==3&&std::wstring_view(argv[1])==L"--ui-smoke-test")smoke_path=argv[2];else if(count!=1){LocalFree(argv);return 2;}LocalFree(argv);
        CoordinatorUi app(std::move(smoke_path));return app.run();
    } catch(const std::exception& error) {
        const auto graphics=dynamic_cast<const gfx::Error*>(&error);
        const auto message=graphics?narrow(graphics->detail):std::string(error.what());
        coordinator::log_startup_failure(message,psx::diagnostics::ErrorDomain::application,ERROR_UNHANDLED_EXCEPTION);
        // Startup failures may precede creation of a graphics context.
        MessageBoxW(nullptr,widen(message).c_str(),L"Coordinator startup error",MB_OK|MB_ICONERROR);return 1;
    }
}
