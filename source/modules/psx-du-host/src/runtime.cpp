#include <algorithm>
#include <du_host/runtime.hpp>
#include <du_host/source_diagnostics.hpp>
#include <cstring>
#include <filesystem>
namespace du_host
{
namespace
{
constexpr auto second = 1000000000ull;
bool loopback(const std::string &ip) noexcept
{
    IN_ADDR address{};
    return InetPtonA(AF_INET, ip.c_str(), &address) == 1 && (ntohl(address.s_addr) >> 24) == 127;
}
template <std::size_t N> void copy(std::array<char, N> &out, std::string_view text)
{
    out = {};
    std::memcpy(out.data(), text.data(), std::min(text.size(), N - 1));
}
void wait(HANDLE stop,HANDLE command,HANDLE main,std::span<HANDLE> network,HANDLE adapters,
          HANDLE control,HANDLE resolver,std::uint64_t deadline)
{
    std::array<HANDLE,64> handles{};DWORD count{};
    for(auto h:{stop,command,main,adapters,control,resolver})if(h)handles[count++]=h;
    for(auto h:network)if(h)handles[count++]=h;
    auto now=psx::norm::monotonic_ns();auto ms=deadline>now?(deadline-now+999999)/1000000:0;
    WaitForMultipleObjects(count,handles.data(),FALSE,DWORD(std::min<uint64_t>(ms,1000)));
}
} // namespace
psx::diagnostics::Options diagnostics_options()
{
    auto o = psx::diagnostics::default_options(psx::diagnostics::Component::application);
    auto root = psx::diagnostics::data_directory();
    if (!root.empty())
        o.directory = (std::filesystem::path(root) / L"DuHost").wstring();
    o.build = "psx-du-host " PSX_DU_HOST_VERSION " / automatic IPv4 / CDU1 / DUT2";
    o.config.crash_dumps = false;
    return o;
}
Runtime::Runtime(Config c, psx::diagnostics::Options o, bool allowed,std::filesystem::path trace_output) : config_(std::move(c)), allowed_(allowed),diagnostics_options_(std::move(o))
{
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    command_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_ || !command_)
    {
        if (stop_)
            CloseHandle(stop_);
        if (command_)
            CloseHandle(command_);
        throw std::runtime_error("Host event creation failed");
    }
    try
    {
        if(!trace_output.empty()){if(std::filesystem::exists(trace_output))throw std::runtime_error("Host trace requires a fresh output directory");du_source_trace_=trace_output/L"source";du_trace_=std::make_shared<psx::du::transport::Trace>(std::move(trace_output),9);}
        diagnostics_options_.config.enabled=true;
        diagnostics_options_.config.events_enabled=true;
        stopped_ = false;
        worker_ = std::thread([this] { run(); });
    }
    catch (...)
    {
        journal_.stop();
        CloseHandle(stop_);
        CloseHandle(command_);
        throw;
    }
}
Runtime::~Runtime()
{
    request_stop();
    join();
    CloseHandle(command_);
    CloseHandle(stop_);
}
void Runtime::request_stop() noexcept
{
    if(auto control=blackout_service_.load())control->request_stop();
    if(auto du=du_runtime_.load())du->request_stop();
    SetEvent(stop_);
}
void Runtime::join()
{
    if (worker_.joinable())
        worker_.join();
}
void Runtime::store(const Status &s)
{
    AcquireSRWLockExclusive(&status_lock_);
    status_ = s;
    ReleaseSRWLockExclusive(&status_lock_);
}
Status Runtime::status() const
{
    AcquireSRWLockShared(&status_lock_);
    auto s = status_;
    ReleaseSRWLockShared(&status_lock_);
    return s;
}
std::uint64_t Runtime::configure(Config c)
{
    std::lock_guard lock(command_lock_);
    pending_ = std::move(c);
    pending_id_ = ++next_command_;
    SetEvent(command_);
    return pending_id_;
}
void Runtime::reconnect()
{
    std::lock_guard lock(command_lock_);
    reconnect_ = true;
    SetEvent(command_);
}
void Runtime::run() noexcept
{
    using namespace psx::cdu;
    using namespace psx::diagnostics;
    Status s;s.running=true;s.configuration_required=!allowed_;
    std::unique_ptr<MainSource> source;
    // Four TX slots per binding preserve the aggregate 64-slot/256-KiB cache
    // limits. Vendor protocol/control memory is separate from owned CDU payloads.
    struct Binding{
        Publisher publisher;std::string address;std::wstring guid;
        std::array<uint64_t,3> sent{};uint64_t retry{};
    };
    std::array<Binding,16> bindings;
    std::array<SourceReport,6> source_reports;
    std::shared_ptr<psx::du::transport::TcpHostPublisher> du;
    std::shared_ptr<blackout::Service> control;
    blackout::Context main_context;uint64_t binding_epoch{1};bool context_valid{};
    uint64_t next_status{},next_metrics{},published_base{},coalesced_base{},busy_base{},max_delay{};
    auto failure=[&](std::string_view text){
        copy(s.error,text);events_.emit(Code::io_failure,Severity::warning,{},ErrorDomain::application,0,text);
    };
    auto revoke_control=[&]{
        main_context.binding_epoch=++binding_epoch;
        if(control)control->update(main_context,source?source->model.generation:0,false,psx::norm::monotonic_ns());
    };
    auto stop_one=[&](Binding&b){
        if(b.publisher.started()){
            revoke_control();published_base+=b.publisher.stats().published;coalesced_base+=b.publisher.stats().coalesced;
            busy_base+=b.publisher.norm_stats().busy;max_delay=std::max(max_delay,b.publisher.stats().max_delay_ns);
            b.publisher.stop();
        }b.sent={};b.retry=0;
    };
    auto stop_publishers=[&]{for(auto&b:bindings)stop_one(b);s.publication_active=false;s.loopback_active=false;};
    try{
        try{control=std::make_shared<blackout::Service>();blackout_service_.store(control);if(!control->start())
            events_.emit(Code::io_failure,Severity::warning,{},ErrorDomain::application,control->error(),"CDU blackout control unavailable");}
        catch(const std::exception&e){failure(e.what());}
        s.norm_version=psx::norm::Adapter::inspect_runtime();
        psx::network::AdapterMonitor monitor(psx::network::InventoryPolicy::display);
        psx::network::Ipv4Resolver resolver;bool resolving{};uint64_t resolution_due{};size_t candidate{};
        std::string resolved_psx;psx::network::Adapter main_adapter;
        s.inventory=std::make_shared<psx::network::Inventory>();
        du=std::make_shared<psx::du::transport::TcpHostPublisher>();du_runtime_.store(du);
        if(du_trace_)du->observe(du_trace_,du_source_trace_);
        auto update_bindings=[&](bool reset){
            if(reset)stop_publishers();
            std::vector<std::pair<std::string,std::wstring>> wanted{{"127.0.0.1",L""}};
            for(const auto&a:s.inventory->adapters)if(a.up&&a.multicast)
                for(const auto&e:a.addresses)if(e.address.ss_family==AF_INET&&!e.host().starts_with("127."))
                    if(std::none_of(wanted.begin(),wanted.end(),[&](auto&v){return v.first==e.host();}))
                        wanted.emplace_back(e.host(),a.guid);
            if(wanted.size()>bindings.size())failure("CDU interface capacity exceeded (16 IPv4 bindings including loopback)");
            for(auto&b:bindings)if(!b.address.empty()&&std::none_of(wanted.begin(),wanted.end(),[&](auto&v){return v.first==b.address&&v.second==b.guid;})){
                stop_one(b);b.address.clear();b.guid.clear();
            }
            for(const auto&[address,guid]:wanted){
                if(std::any_of(bindings.begin(),bindings.end(),[&](auto&b){return b.address==address&&b.guid==guid;}))continue;
                auto free=std::find_if(bindings.begin(),bindings.end(),[](auto&b){return b.address.empty();});
                if(free==bindings.end())break;free->address=address;free->guid=guid;free->retry=0;
            }
            // A wildcard listener covers loopback and every local IPv4 address.
            du->configure({"",config_.du_pid,config_.du_created,config_.du_mask,allowed_,config_.du_tcp_port,config_.du_freshness_ms,true});
        };
        auto configure_source=[&](const std::string&target,bool force){
            auto endpoint=psx::network::Endpoint::parse(target,config_.psx_port);
            auto selected=psx::network::route_ipv4(*s.inventory,endpoint);
            bool changed=force||target!=resolved_psx||!psx::network::same_binding(selected,main_adapter);
            if(changed)revoke_control();
            std::array<char,48> guid{};bool ascii=selected.guid.size()<guid.size();
            for(size_t n=0;ascii&&n<selected.guid.size();++n){ascii=selected.guid[n]>0&&selected.guid[n]<=127;if(ascii)guid[n]=char(selected.guid[n]);}
            context_valid=ascii&&blackout::context(main_context,target,config_.psx_port,guid.data(),binding_epoch);
            if(!source)source=std::make_unique<MainSource>(target,config_.psx_port,selected);
            else if(changed)source->configure(target,config_.psx_port,selected,psx::norm::monotonic_ns());
            main_adapter=std::move(selected);resolved_psx=target;copy(s.address,target);
            source->sink=[](void*user,std::string_view,uint64_t,Outcome result){
                auto self=static_cast<Runtime*>(user);if(result.result==Result::accepted)++self->accepted_;
                if(result.result==Result::rejected)self->events_.emit(Code::observation_rejected,Severity::warning,{},ErrorDomain::decoder,unsigned(result.reason),"CDU record rejected");
            };source->sink_user=this;
        };
        if(allowed_)update_bindings(false);
        SourceState old_state=SourceState::disconnected;
        events_.emit(Code::process_start,Severity::info,{},ErrorDomain::none,0,"DU host automatic IPv4 networking started");
        bool diagnostics_active{};
        while(WaitForSingleObject(stop_,0)!=WAIT_OBJECT_0){
            if(auto enabled=diagnostics_requested_.load();enabled!=diagnostics_active){
                events_={};journal_.stop();diagnostics_active=enabled;
                if(enabled){journal_.start(diagnostics_options_);events_=journal_.producer(0,Component::application);source_reports={};
                    events_.emit(Code::process_start,Severity::info,{},ErrorDomain::none,0,"DU host diagnostics manually enabled");}
            }
            auto now=psx::norm::monotonic_ns();Config next;uint64_t id{};bool reconnect{};
            {std::lock_guard lock(command_lock_);id=pending_id_;if(id){next=std::move(pending_);pending_id_=0;}reconnect=reconnect_;reconnect_=false;}
            if(id){
                auto error=validate(next,*s.inventory);s.configuration_applied=error.empty();
                if(error.empty()){
                    bool endpoint=next.psx!=config_.psx||next.psx_port!=config_.psx_port;
                    bool transport=next.group!=config_.group||next.port!=config_.port;
                    config_=std::move(next);allowed_=true;s.configuration_required=false;s.error={};
                    update_bindings(transport);
                    if(endpoint){revoke_control();if(source)source->suspend();context_valid=false;resolved_psx.clear();resolver.cancel();resolving=false;resolution_due=0;}
                }else failure(error);
                s.configuration_ack=id;
            }
            if(reconnect){du->reconnect();revoke_control();if(source)source->reconnect(now);resolution_due=0;}
            bool inventory_changed{};
            if(monitor.revision.load()!=s.inventory->revision)if(auto current=monitor.snapshot()){
                s.inventory=std::move(current);inventory_changed=true;if(allowed_)update_bindings(false);
            }
            bool source_up=source&&(source->source_state==SourceState::initializing||source->source_state==SourceState::connected);
            if(allowed_&&!resolving&&!source_up&&now>=resolution_due){
                try{resolver.start(config_.psx);resolving=true;resolution_due=now+5000000000ull;}
                catch(const std::exception&e){failure(e.what());resolution_due=now+second;}
            }
            if(resolving&&resolver.ready()){
                resolving=false;
                try{auto targets=resolver.take();configure_source(targets[candidate++%targets.size()].host(),false);}
                catch(const std::exception&e){failure(e.what());resolution_due=now+second;}
            }else if(inventory_changed&&!resolved_psx.empty()){
                try{configure_source(resolved_psx,false);}catch(const std::exception&e){failure(e.what());revoke_control();if(source)source->bind({});context_valid=false;resolved_psx.clear();resolution_due=0;}
            }
            if(source){
                try{source->pump(now);}catch(const std::exception&e){failure(e.what());revoke_control();source->reconnect(now+second);}
                if(source->source_state!=old_state){
                    old_state=source->source_state;events_.emit(old_state==SourceState::disconnected?Code::disconnected:Code::connected,Severity::info,{},ErrorDomain::winsock,unsigned(source->socket_error()),"Main source state",{unsigned(old_state),source->model.generation});
                }
            }
            now=psx::norm::monotonic_ns();
            if(control){
                bool ready=context_valid&&source&&source->source_state==SourceState::connected&&!source->model.loading&&cache_initialized(source->model);
                control->update(main_context,source?source->model.generation:0,ready,now);control->pump(now);
                s.blackout=control->status();s.blackout_error=control->error();s.blackout_available=s.blackout_error==ERROR_SUCCESS;
            }
            for(auto&b:bindings){
                if(b.address.empty())continue;
                auto&p=b.publisher;
                if(allowed_&&!p.started()&&now>=b.retry){
                    try{
                        psx::norm::Options o;o.group=config_.group;o.port=config_.port;o.interface_address=b.address;
                        o.initial_grtt_seconds=.01;o.tx_objects=4;o.tx_cache_bytes=16*1024;
                        revoke_control();p.start(o);b.sent={};
                        events_.emit(Code::enable,Severity::info,{},ErrorDomain::none,0,"CDU publication on "+b.address);
                    }catch(const std::exception&e){failure(e.what());b.retry=now+second;}
                }
                if(p.started())try{
                    if(source){
                        if(control){auto state=control->status();p.offer_blackout(state.mask,state.revision,state.generation,state.ready,now);}
                        for(unsigned u=0;u<3;++u)if(b.sent[u]!=source->revisions[u]){
                            p.offer(make_snapshot(source->model.screens[u],source->model.source_version,source->source_state,p.session(),1,source->revisions[u],now,origin_),now);
                            b.sent[u]=source->revisions[u];
                        }
                    }
                    auto before=p.stats().published,errors=p.norm_stats().errors;p.pump(now,32);
                    if(p.norm_stats().errors!=errors)throw std::runtime_error("NORM send error; retrying only affected binding");
                    if(p.stats().published>before)s.last_publication_ns=now;
                }catch(const std::exception&e){failure(e.what());stop_one(b);b.retry=now+second;}
            }
            s.publication_active=false;s.loopback_active=false;s.multicast_bindings=s.multicast_unavailable=0;
            for(auto&b:bindings)if(!b.address.empty()){
                if(b.publisher.started()){++s.multicast_bindings;if(b.address=="127.0.0.1")s.loopback_active=true;else s.publication_active=true;}
                else ++s.multicast_unavailable;
            }
            if(now>=next_status){
                if(source){
                    s.source_state=source->source_state;s.main_socket_error=source->socket_error();s.generation=source->model.generation;
                    s.connections=source->connections;s.records=source->records;s.accepted=accepted_;s.rejections=source->rejections;
                    for(unsigned u=0;u<3;++u){
                        s.screens[u]=make_snapshot(source->model.screens[u],source->model.source_version,source->source_state,bindings[0].publisher.session(),1,source->revisions[u],now,origin_);
                        for(auto&r:s.screens[u].receipts)s.last_record_ns=std::max(s.last_record_ns,r.sequence?origin_+r.elapsed_ns:0);
                    }
                }else s.source_state=SourceState::disconnected;
                s.published=published_base;s.coalesced=coalesced_base;s.busy=busy_base;s.owned=0;s.max_delay_ns=max_delay;
                for(auto&b:bindings)if(b.publisher.started()){
                    s.published+=b.publisher.stats().published;s.coalesced+=b.publisher.stats().coalesced;s.busy+=b.publisher.norm_stats().busy;
                    s.owned+=b.publisher.norm_stats().owned;s.max_delay_ns=std::max(s.max_delay_ns,b.publisher.stats().max_delay_ns);
                }
                s.owned_peak=std::max(s.owned_peak,s.owned);s.du=du->view();s.diagnostics_enabled=diagnostics_active;s.journal=journal_.status();store(s);next_status=now+250000000ull;
            }
            if(now>=next_metrics){
                events_.emit(Code::metrics,Severity::info,{},ErrorDomain::none,0,"CDU IPv4 publication metrics",{s.published,s.busy,s.owned,s.owned_peak,s.multicast_bindings,s.multicast_unavailable});
                auto v=du->view();if(diagnostics_active)for(unsigned u=0;u<6;++u)source_reports[u].publish(u,v.units[u],[&](const auto& e){return events_.emit(e);});events_.emit(Code::metrics,Severity::info,{},ErrorDomain::none,0,"DU publication counters",{v.prepared,v.published,v.expired,v.replaced,v.payload_bytes,v.diagnostic_lost});
                if(v.diagnostics_failed||v.diagnostic_lost)events_.emit(Code::io_failure,Severity::warning,{},ErrorDomain::application,0,"DU diagnostics incomplete; streams continue",{v.diagnostic_lost,uint64_t(v.diagnostics_failed)});
                for(unsigned b=0;b<2;++b){const auto&m=v.bindings[b].native;if(m.call&&psx::du::transport::qpc()-m.entered>psx::du::transport::frequency())
                    events_.emit(Code::io_failure,Severity::warning,{},ErrorDomain::application,0,"DU native call exceeds one second; publication remains enabled",{b,m.call,m.unit,m.thread,m.entered,m.completed});}
                next_metrics=now+second;
            }
            auto deadline=std::min(next_status,next_metrics);std::array<HANDLE,16> events{};unsigned count{};
            for(auto&b:bindings)if(!b.address.empty()){
                if(b.publisher.started()){events[count++]=b.publisher.event();deadline=std::min(deadline,b.publisher.deadline());}
                else if(allowed_)deadline=std::min(deadline,b.retry);
            }
            if(source&&source->deadline()>now)deadline=std::min(deadline,source->deadline());
            if(control)deadline=std::min(deadline,control->deadline());
            wait(stop_,command_,source?source->event():nullptr,{events.data(),count},monitor.available,control?control->event():nullptr,resolving?resolver.event():nullptr,deadline);
        }
    }catch(const std::exception&e){failure(e.what());s.fatal=true;}
    s.stopping=true;store(s);
    if(control){control->request_stop();control->pump(psx::norm::monotonic_ns());control->stop();s.blackout=control->status();s.blackout_available=false;}
    blackout_service_.store({});if(source)source->stop();if(du)du->request_stop();stop_publishers();if(du)du->stop();du_runtime_.store({});
    s.loopback_active=false;s.owned=0;s.source_state=SourceState::disconnected;
    events_.emit(Code::process_stop,Severity::info,{},ErrorDomain::none,0,"DU host stopped");journal_.stop();s.journal=journal_.status();s.running=false;s.diagnostics_enabled=false;store(s);stopped_.store(true,std::memory_order_release);
}
} // namespace du_host
