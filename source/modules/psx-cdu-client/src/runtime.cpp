#include <algorithm>
#include <cdu_client/runtime.hpp>
#include <cstring>
namespace cdu_client
{
psx::cdu::ScreenView presentation_view(const View &v, psx::cdu::CduSnapshot &empty) noexcept
{
    if(v.has_screen && v.blackout.valid &&
       (v.blackout.mask & psx::cdu::blackout::unit_mask(v.screen.unit)))
    {
        empty.unit=v.screen.unit;
        auto black=empty.view();black.blanking_valid=true;black.blanking=1;
        return black;
    }
    return v.has_screen?v.screen.view():empty.view();
}
Runtime::Runtime(Config c, bool allowed, std::string profile, psx::diagnostics::Options o)
    : config_(std::move(c)), allowed_(allowed)
{
    psx::norm::Adapter::inspect_runtime();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    command_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_ || !command_)
    {
        if (stop_) CloseHandle(stop_);
        if (command_) CloseHandle(command_);
        throw std::runtime_error("Client worker events unavailable");
    }
    try
    {
        if (o.directory.empty())
        {
            o = psx::diagnostics::default_options(psx::diagnostics::Component::application);
            o.directory =
                (std::filesystem::path(psx::diagnostics::data_directory()) / L"CduClient" / widen(profile)).wstring();
        }
        o.build = "psx-cdu-client " PSX_CDU_CLIENT_VERSION " / IPv4 subnet / CDU1 + CBL1";
        o.config.crash_dumps = false;
        diagnostics_options_=std::move(o);
        diagnostics_options_.config.enabled=true;
        diagnostics_options_.config.events_enabled=true;
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
    CloseHandle(stop_);
    CloseHandle(command_);
}
void Runtime::request_stop() noexcept
{
    SetEvent(stop_);
}
void Runtime::join()
{
    if (worker_.joinable())
        worker_.join();
}
void Runtime::store(const View &v)
{
    AcquireSRWLockExclusive(&lock_);
    view_ = v;
    ReleaseSRWLockExclusive(&lock_);
}
View Runtime::view() const
{
    AcquireSRWLockShared(&lock_);
    auto v = view_;
    ReleaseSRWLockShared(&lock_);
    return v;
}
uint64_t Runtime::configure(Config c)
{
    std::lock_guard l(commands_);
    pending_ = std::move(c);
    pending_id_ = ++next_;
    SetEvent(command_);
    return pending_id_;
}
void Runtime::reconnect()
{
    std::lock_guard l(commands_);
    reconnect_ = true;
    SetEvent(command_);
}
void Runtime::run() noexcept
{
    using namespace psx::cdu;
    using namespace psx::diagnostics;
    View v;
    Receiver receiver;
    struct Observation
    {
        SelectedRevisionRate rates;
        uint64_t now{};
    } observation;
    observation.rates.reset(config_.unit, psx::norm::monotonic_ns());
    receiver.observe_accepted(
        [](void *user, const CduSnapshot &snapshot) noexcept {
            auto &o = *static_cast<Observation *>(user);
            o.rates.observe(snapshot, o.now);
        },
        &observation);
    uint64_t retry{}, metrics{}, next_status{};
    psx::network::Adapter adapter;
    auto error = [&](std::string_view t) {
        v.error = {};
        memcpy(v.error.data(), t.data(), std::min(t.size(), v.error.size() - 1));
        events_.emit(Code::io_failure, Severity::warning, {}, ErrorDomain::application, 0, t);
    };
    try
    {
        psx::norm::Adapter::inspect_runtime();
        psx::network::AdapterMonitor monitor(psx::network::InventoryPolicy::display);
        psx::network::Ipv4Resolver resolver;bool resolving{};
        std::vector<psx::network::Endpoint> endpoints;std::string resolved;
        uint64_t resolution_due{},candidate_since{};size_t candidate{};
        std::shared_ptr<const psx::network::Inventory> inventory;
        events_.emit(Code::process_start, Severity::info, {}, ErrorDomain::none, 0,
                     "CDU client receive worker started");
        bool diagnostics_active{};
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0)
        {
            if(auto enabled=diagnostics_requested_.load();enabled!=diagnostics_active){
                events_={};journal_.stop();diagnostics_active=enabled;
                if(enabled){journal_.start(diagnostics_options_);events_=journal_.producer(0,Component::application);
                    events_.emit(Code::process_start,Severity::info,{},ErrorDomain::none,0,"CDU client diagnostics manually enabled");}
            }
            auto now = psx::norm::monotonic_ns();
            observation.now = now;
            Config next;
            uint64_t id{};
            bool reconnect{};
            {
                std::lock_guard l(commands_);
                id = pending_id_;
                if (id)
                {
                    next = std::move(pending_);
                    pending_id_ = 0;
                }
                reconnect = reconnect_;
                reconnect_ = false;
            }
            if (id)
            {
                bool selection = next.host != config_.host || next.unit != config_.unit;
                bool restart = !allowed_ || next.host != config_.host || next.group != config_.group ||
                               next.port != config_.port || next.subnet != config_.subnet;
                config_ = std::move(next);
                allowed_ = true;
                v.epoch = id;
                v.ack = id;
                if (selection)
                {
                    observation.rates.reset(config_.unit, now);
                    v.has_screen = false;
                    v.initialized = false;
                }
                if (restart)
                {
                    inventory.reset();resolver.cancel();resolving=false;resolved.clear();endpoints.clear();resolution_due=0;
                    receiver.stop();
                    v.source = {};
                    retry = 0;
                    v.has_screen = false;
                    v.initialized = false;
                }
            }
            if (reconnect)
            {
                receiver.stop();
                retry = 0;
            }
            auto current = monitor.snapshot();
            bool inventory_changed=current&&(!inventory||inventory->revision!=current->revision);
            if(current)inventory=current;
            if(receiver.started()&&!receiver.healthy(now)&&candidate_since&&now-candidate_since>=5000000000ull){
                receiver.stop();++candidate;resolution_due=0;retry=0;v.source={};
            }
            if(allowed_&&!resolving&&(!receiver.started()||!receiver.healthy(now))&&now>=resolution_due){
                try{resolver.start(config_.host);resolving=true;resolution_due=now+5000000000ull;}
                catch(const std::exception&e){error(e.what());resolution_due=now+1000000000ull;}
            }
            if(resolving&&resolver.ready()){
                resolving=false;
                try{
                    endpoints=resolver.take();
                    auto next_host=endpoints[candidate%endpoints.size()].host();
                    if(next_host!=resolved){receiver.stop();v.has_screen=false;v.initialized=false;v.source={};}
                    resolved=next_host;retry=0;
                }catch(const std::exception&e){error(e.what());resolved.clear();resolution_due=now+1000000000ull;}
            }
            if(allowed_&&inventory&&!resolved.empty()&&(inventory_changed||!receiver.started())&&now>=retry){
                try{
                    if(config_.subnet.empty())config_.subnet=configured_subnet(config_,*inventory);
                    auto target=psx::network::Endpoint::parse(resolved,0);
                    if(!resolved.starts_with("127.")&&config_.subnet.empty())throw std::runtime_error("Select the CDU network subnet");
                    auto next_adapter=psx::network::route_ipv4(*inventory,target,config_.subnet);
                    if(!psx::network::same_binding(adapter,next_adapter)){receiver.stop();v.source={};v.has_screen=false;v.initialized=false;}
                    adapter=std::move(next_adapter);
                    if(!receiver.started()){
                        psx::norm::Options o;o.expected_host=resolved;o.group=config_.group;o.port=config_.port;
                        o.interface_address=adapter_ipv4(adapter);receiver.start(o);
                        observation.rates.reset(config_.unit,now);v.error={};v.source={};v.initialized=false;
                        candidate_since=now;
                        events_.emit(Code::connected,Severity::info,{},ErrorDomain::none,0,
                            "CDU IPv4 binding: "+o.interface_address+" -> "+resolved);
                    }
                }catch(const std::exception&e){receiver.stop();error(e.what());retry=now+1000000000ull;}
            }
            if (receiver.started())
            {
                try
                {
                    receiver.pump(now);
                    auto source = receiver.status();
                    if (v.source.pinned &&
                        (source.session != v.source.session || source.generation != v.source.generation))
                    {
                        v.has_screen = false;
                        v.initialized = false;
                    }
                    v.source = source;
                    CduSnapshot screen;
                    if (receiver.mailbox.copy(config_.unit, screen))
                    {
                        if (!v.has_screen || v.screen.session != screen.session ||
                            v.screen.publication_sequence != screen.publication_sequence)
                            v.accepted_ns = psx::norm::monotonic_ns();
                        v.screen = screen;
                        v.has_screen = true;
                        v.initialized =
                            screen.text_valid == 0x3fff && screen.colour_valid == 0x3fff && screen.scalar_valid == 7;
                    }
                }
                catch (const std::exception &e)
                {
                    error(e.what());
                    receiver.stop();
                    retry = now + 1000000000ull;
                    v.initialized = false;
                }
            }
            v.active = receiver.started();
            v.blackout = receiver.blackout_status();
            v.healthy = receiver.started() && receiver.healthy(now);
            v.stats = receiver.stats();
            v.received_updates = observation.rates.count();
            v.received_fps = observation.rates.sample(now);
            v.ownership = receiver.norm_stats();
            if (now >= next_status)
            {
                store(v);
                next_status = now + 16667000ull;
            }
            if (now >= metrics)
            {
                v.diagnostics_enabled=diagnostics_active;v.journal = journal_.status();
                metrics = now + 1000000000ull;
                std::array<uint64_t, 6> values{v.stats.accepted,
                                               v.stats.malformed,
                                               v.stats.conflict,
                                               psx::norm::Adapter::receive_bytes(),
                                               psx::norm::Adapter::receive_peak(),
                                               uint64_t(v.healthy)};
                events_.emit(Code::metrics, Severity::info, {}, ErrorDomain::none, 0, "Client transport metrics",
                             values);
            }
            HANDLE h[4]{stop_, command_, receiver.event(), monitor.available};
            DWORD n = 2;
            if (h[2])
                ++n;
            else
                h[2] = h[3];
            if (h[2] != h[3])
                ++n;
            auto wait = next_status > now ? (next_status - now + 999999) / 1000000 : 0;
            WaitForMultipleObjects(n, h, FALSE, DWORD(std::min<uint64_t>(wait, 1000)));
        }
    }
    catch (const std::exception &e)
    {
        v.fatal = true;
        error(e.what());
    }
    catch (...)
    {
        v.fatal = true;
        error("Unexpected client receive failure");
    }
    receiver.stop();
    v.blackout = {};
    v.ownership = receiver.norm_stats();
    v.active = false;
    events_.emit(Code::process_stop, Severity::info, {}, ErrorDomain::none, 0, "Client transport released");
    journal_.stop();
    v.stopped = true;v.diagnostics_enabled=false;
    store(v);
    stopped_.store(true, std::memory_order_release);
}
} // namespace cdu_client
