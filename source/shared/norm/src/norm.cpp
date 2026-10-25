#include <psx/norm/norm.hpp>
#include <psx/norm/du.hpp>
#include "norm_memory.hpp"
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <filesystem>
#include <iphlpapi.h>
#include <normApi.h>
#include <stdexcept>
#include <vector>
#include <ws2tcpip.h>
namespace psx::norm
{
namespace
{
std::atomic_size_t rx_bytes{}, rx_peak{};
std::atomic_uint64_t denied{};
struct alignas(std::max_align_t) Allocation
{
    std::size_t size;
};
} // namespace
namespace detail
{
char *allocate_payload(std::size_t n)
{
    if (!n || n > max_data)
    {
        ++denied;
        return nullptr;
    }
    auto old = rx_bytes.load();
    do
    {
        if (n > receive_budget - old)
        {
            ++denied;
            return nullptr;
        }
    } while (!rx_bytes.compare_exchange_weak(old, old + n));
    auto p = static_cast<Allocation *>(HeapAlloc(GetProcessHeap(), 0, sizeof(Allocation) + n));
    if (!p)
    {
        rx_bytes -= n;
        ++denied;
        return nullptr;
    }
    p->size = n;
    auto peak = rx_peak.load();
    while (peak < old + n && !rx_peak.compare_exchange_weak(peak, old + n))
    {
    }
    return reinterpret_cast<char *>(p + 1);
}
void free_payload(char *p)
{
    if (p)
    {
        auto a = reinterpret_cast<Allocation *>(p) - 1;
        rx_bytes -= a->size;
        HeapFree(GetProcessHeap(), 0, a);
    }
}
} // namespace detail
namespace
{
std::wstring sibling()
{
    std::wstring p(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
    if (!n || n >= p.size())
        throw std::runtime_error("Cannot locate executable");
    p.resize(n);
    return (std::filesystem::path(p).parent_path() / L"Norm.dll").wstring();
}
// Matching uses current IPv4/interface identity. The immutable vendor DLL only
// accepts its current short ANSI alias at this API boundary; never persist it.
std::string interface_name(const std::string &ip)
{
    if(ip=="127.0.0.1")return "lo";
    IN_ADDR wanted{};
    if(InetPtonA(AF_INET,ip.c_str(),&wanted)!=1)throw std::runtime_error("NORM interface requires IPv4");
    ULONG size=32768,result=ERROR_BUFFER_OVERFLOW;std::vector<uint8_t> memory;
    for(unsigned retry=0;retry<3&&result==ERROR_BUFFER_OVERFLOW;++retry){
        if(size>4*1024*1024)throw std::runtime_error("NORM inventory exceeds bound");
        memory.resize(size);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(memory.data()),&size);
    }
    if(result!=NO_ERROR)throw std::runtime_error("NORM interface enumeration failed");
    for(auto*a=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(memory.data());a;a=a->Next)
        for(auto*u=a->FirstUnicastAddress;u;u=u->Next)if(u->Address.lpSockaddr->sa_family==AF_INET&&
            reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr==wanted.s_addr){
            std::string token;
            for(auto*c=a->FriendlyName;*c;++c){
                if(*c>127||token.size()>=31)throw std::runtime_error("Pinned CDU NORM cannot use this adapter alias (ASCII, maximum 31 characters)");
                token.push_back(char(*c));
            }
            if(token.empty())throw std::runtime_error("NORM interface alias is empty");
            return token;
        }
    throw std::runtime_error("NORM local IPv4 address unavailable");
}
} // namespace
std::uint64_t monotonic_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void random_bytes(std::span<std::uint8_t> p)
{
    if (BCryptGenRandom(nullptr, p.data(), ULONG(p.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("BCryptGenRandom failed");
}
struct Adapter::Impl
{
    HMODULE dll{};
    NormInstanceHandle instance{};
    NormSessionHandle session{};
    bool sender{}, active{};
    std::uint32_t node{};
    Options options;
    Stats stats;
    std::array<int, 3> version{};
    struct Tx
    {
        NormObjectHandle object{};
        std::array<std::uint8_t, max_data> bytes;
    };
    std::vector<Tx> tx;
#define API(name)                                                                                                      \
    decltype(&Norm##name) name                                                                                         \
    {                                                                                                                  \
    }
    API(GetVersion);
    API(CreateInstance);
    API(DestroyInstance);
    API(StopInstance);
    API(CreateSession);
    API(DestroySession);
    API(GetDescriptor);
    API(GetNextEvent);
    API(SetAllocationFunctions);
    API(SetMulticastInterface);
    API(SetMulticastLoopback);
    API(SetTTL);
    API(SetRxPortReuse);
    API(SetTxOnly);
    API(SetTxPort);
    API(StartSender);
    API(StopSender);
    API(StartReceiver);
    API(StopReceiver);
    API(SetTxRate);
    API(SetGrttEstimate);
    API(SetTxCacheBounds);
    API(SetRxCacheLimit);
    API(SetCongestionControl);
    API(SetDefaultNackingMode);
    API(SetSilentReceiver);
    API(SetDefaultSyncPolicy);
    API(DataEnqueue);
    API(DataDetachData);
    API(ObjectGetType);
    API(ObjectGetSize);
    API(DataAccessData);
    API(NodeGetAddress);
    API(ObjectCancel);
    API(SetTxLoss);
    API(SetRxLoss);
#undef API
    template <class T> void load(T &p, const char *name)
    {
        p = reinterpret_cast<T>(GetProcAddress(dll, name));
        if (!p)
            throw std::runtime_error(std::string("NORM missing required export: ") + name);
    }
    void init(const Options &o, bool send, bool inspect_only = false)
    {
        options = o;
        sender = send;
        stats = {};
        auto path = o.dll_path.empty() ? sibling() : o.dll_path;
        if (!std::filesystem::path(path).is_absolute())
            throw std::runtime_error("NORM path must be absolute");
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
            throw std::runtime_error("Cannot load adjacent/explicit Norm.dll: " + std::to_string(GetLastError()));
#define LOAD(name) load(name, "Norm" #name)
        LOAD(GetVersion);
        LOAD(CreateInstance);
        LOAD(DestroyInstance);
        LOAD(StopInstance);
        LOAD(CreateSession);
        LOAD(DestroySession);
        LOAD(GetDescriptor);
        LOAD(GetNextEvent);
        LOAD(SetAllocationFunctions);
        LOAD(SetMulticastInterface);
        LOAD(SetMulticastLoopback);
        LOAD(SetTTL);
        LOAD(SetRxPortReuse);
        LOAD(SetTxOnly);
        LOAD(SetTxPort);
        LOAD(StartSender);
        LOAD(StopSender);
        LOAD(StartReceiver);
        LOAD(StopReceiver);
        LOAD(SetTxRate);
        LOAD(SetGrttEstimate);
        LOAD(SetTxCacheBounds);
        LOAD(SetRxCacheLimit);
        LOAD(SetCongestionControl);
        LOAD(SetDefaultNackingMode);
        LOAD(SetSilentReceiver);
        LOAD(SetDefaultSyncPolicy);
        LOAD(DataEnqueue);
        LOAD(DataDetachData);
        LOAD(ObjectGetType);
        LOAD(ObjectGetSize);
        LOAD(DataAccessData);
        LOAD(NodeGetAddress);
        LOAD(ObjectCancel);
        LOAD(SetTxLoss);
        LOAD(SetRxLoss);
#undef LOAD
        GetVersion(&version[0], &version[1], &version[2]);
        if (inspect_only)
            return;

        do
        {
            random_bytes({reinterpret_cast<std::uint8_t *>(&node), sizeof(node)});
        } while (!node || node == 0xffffffffu);
        instance = CreateInstance(false);
        if (!instance)
            throw std::runtime_error("NormCreateInstance failed");

        SetAllocationFunctions(instance, detail::allocate_payload, detail::free_payload);
        session = CreateSession(instance, o.group.c_str(), o.port, node);
        if (!session)
            throw std::runtime_error("NormCreateSession failed");
        // Windows permits multicast reception bound to the interface address.
        // Keep loopback and Ethernet data/feedback sockets independent.
        SetRxPortReuse(session, true, o.interface_address.c_str(), nullptr, 0);
        auto name = interface_name(o.interface_address);
        if (!SetMulticastInterface(session, name.c_str()) || !SetTTL(session, 1) ||
            !SetMulticastLoopback(session, true))
            throw std::runtime_error("NORM adapter/TTL/loopback binding failed");
        if (sender)
        {
            if(o.tx_objects<4||o.tx_objects>64)throw std::runtime_error("Invalid CDU TX slot count");
            tx.resize(o.tx_objects);
            // Multicast senders join the feedback group. Unicast diagnostics
            // use the connected TX socket, avoiding a competing receive bind.
            const bool unicast = o.group == "127.0.0.1";
            SetTxOnly(session, unicast, unicast);
            if (!SetTxPort(session, 0, false, o.interface_address.c_str()))
                throw std::runtime_error("NORM sender bind failed");
            SetCongestionControl(session, false, false);
            SetTxRate(session, 4e6);
            if (!(o.initial_grtt_seconds >= .001 && o.initial_grtt_seconds <= 10))
                throw std::runtime_error("Invalid initial NORM GRTT estimate");
            SetGrttEstimate(session, o.initial_grtt_seconds);
            if (o.tx_objects < 4 || o.tx_objects > 64 || o.tx_cache_bytes < max_data * o.tx_objects ||
                o.tx_cache_bytes > 256 * 1024)
                throw std::runtime_error("NORM transmit bounds outside qualified limits");
            SetTxCacheBounds(session, o.tx_cache_bytes, 3, o.tx_objects - 1);
            std::uint16_t sid{};
            random_bytes({reinterpret_cast<std::uint8_t *>(&sid), sizeof(sid)});
            if (!StartSender(session, sid, 1024 * 1024, 1200, 16, 0, 0))
                throw std::runtime_error("NormStartSender failed on "+o.interface_address+", socket="+std::to_string(WSAGetLastError())+", Windows="+std::to_string(GetLastError()));
            SetTxLoss(session, o.loss_percent);
        }
        else
        {
            SetDefaultNackingMode(session, NORM_NACK_NORMAL);
            SetSilentReceiver(session, false, -1);
            SetDefaultSyncPolicy(session, NORM_SYNC_CURRENT);
            SetRxCacheLimit(session, 32);
            if (!StartReceiver(session, 1024 * 1024))
                throw std::runtime_error("NormStartReceiver failed, socket error=" + std::to_string(WSAGetLastError()) +
                                         ", Windows error=" + std::to_string(GetLastError()));
            SetRxLoss(session, o.loss_percent);
        }
        active = true;
        if (!GetDescriptor(instance))
            throw std::runtime_error("NORM descriptor unavailable");
    }
    void stop() noexcept
    {
        if (session)
        {
            if (active)
            {
                if (sender)
                    StopSender(session);
                else
                    StopReceiver(session);
            }
            DestroySession(session);
            session = nullptr;
        }
        if (instance)
        {
            DestroyInstance(instance);
            instance = nullptr;
        }
        // TX data is borrowed by NORM; destruction must finish before reuse.
        for (auto &t : tx)
            t.object = nullptr;
        stats.owned = 0;
        active = false;
        if (dll)
        {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }
};
Adapter::Adapter() : impl_(std::make_unique<Impl>())
{
}
std::array<int, 3> Adapter::inspect_runtime(const std::wstring &path)
{
    Adapter adapter;
    Options options;
    options.dll_path = path;
    adapter.impl_->init(options, false, true);
    return adapter.impl_->version;
}
Adapter::~Adapter()
{
    stop();
}
void Adapter::start(const Options &o, bool s)
{
    stop();
    try
    {
        impl_->init(o, s);
    }
    catch (...)
    {
        stop();
        throw;
    }
}
void Adapter::stop() noexcept
{
    impl_->stop();
}
bool Adapter::started() const noexcept
{
    return impl_->active;
}
HANDLE Adapter::event() const noexcept
{
    return impl_->instance ? impl_->GetDescriptor(impl_->instance) : nullptr;
}
std::uint32_t Adapter::node_id() const noexcept
{
    return impl_->node;
}
std::array<int, 3> Adapter::version() const noexcept
{
    return impl_->version;
}
const Stats &Adapter::stats() const noexcept
{
    return impl_->stats;
}
std::size_t Adapter::receive_bytes() noexcept
{
    return rx_bytes.load();
}
std::size_t Adapter::receive_peak() noexcept
{
    return rx_peak.load();
}
std::uint64_t Adapter::allocation_denied() noexcept
{
    return denied.load();
}
bool Adapter::send(std::span<const std::uint8_t> b)
{
    auto &i = *impl_;
    if (!i.active || !i.sender || b.empty() || b.size() > max_data)
        throw std::runtime_error("Invalid NORM send");
    for (auto &t : std::span(i.tx.data(), i.options.tx_objects))
        if (!t.object)
        {
            std::copy(b.begin(), b.end(), t.bytes.begin());
            auto obj =
                i.DataEnqueue(i.session, reinterpret_cast<const char *>(t.bytes.data()), UINT32(b.size()), nullptr, 0);
            if (!obj)
            {
                ++i.stats.busy;
                return false;
            }
            t.object = obj;
            ++i.stats.queued;
            ++i.stats.owned;
            i.stats.owned_peak = std::max(i.stats.owned_peak, i.stats.owned);
            return true;
        }
    ++i.stats.busy;
    return false;
}
unsigned Adapter::pump(Receive callback, void *user, unsigned budget)
{
    auto &i = *impl_;
    if (!i.active)
        return 0;
    NormEvent e{};
    unsigned n = 0;
    while (n < budget && i.GetNextEvent(i.instance, &e, false))
    {
        ++n;
        if (e.type == NORM_TX_OBJECT_PURGED)
        {
            for (auto &t : i.tx)
                if (t.object == e.object)
                {
                    i.DataDetachData(e.object);
                    t.object = nullptr;
                    --i.stats.owned;
                    ++i.stats.purged;
                    break;
                }
        }
        if (e.type == NORM_SEND_ERROR)
            ++i.stats.errors;
        if (e.type == NORM_RX_OBJECT_ABORTED)
            ++i.stats.aborted;
        if (i.sender || !e.sender)
            continue;
        unsigned length = 16;
        std::array<std::uint8_t, 16> addr{};
        std::uint16_t port = 0;
        bool address = i.NodeGetAddress(e.sender, reinterpret_cast<char *>(addr.data()), &length, &port) && length == 4;
        std::string host = address ? std::to_string(addr[0]) + "." + std::to_string(addr[1]) + "." +
                                         std::to_string(addr[2]) + "." + std::to_string(addr[3])
                                   : "";
        if (!address || (!i.options.expected_host.empty() && host != i.options.expected_host))
        {
            if (e.type == NORM_REMOTE_SENDER_NEW || e.type == NORM_REMOTE_SENDER_ADDRESS ||
                e.type == NORM_RX_OBJECT_NEW)
            {
                ++i.stats.wrong_host;
            }
            if (e.object)
                i.ObjectCancel(e.object);
            continue;
        }
        if (e.type == NORM_RX_OBJECT_NEW && e.object)
        {
            auto size = i.ObjectGetSize(e.object);
            if (i.ObjectGetType(e.object) != NORM_OBJECT_DATA || size <= 0 || size > max_data)
            {
                ++i.stats.oversized;
                i.ObjectCancel(e.object);
                continue;
            }
        }
        if (e.type != NORM_RX_OBJECT_COMPLETED || !e.object)
            continue;
        auto size = i.ObjectGetSize(e.object);
        if (i.ObjectGetType(e.object) != NORM_OBJECT_DATA || size <= 0 || size > max_data)
        {
            ++i.stats.oversized;
            continue;
        }
        const auto *data = i.DataAccessData(e.object);
        if (!data)
            continue;
        ++i.stats.completed;
        if (callback)
            callback(user, {{reinterpret_cast<const std::uint8_t *>(data), std::size_t(size)},
                            {addr[0], addr[1], addr[2], addr[3]}});
    }
    return n;
}
// DU profile uses independent callbacks because the pinned ABI has no allocator user data.
namespace {
std::atomic_size_t du_rx{},du_peak{},du_cpu{};std::atomic_uint64_t du_denied{};
bool cpu_reserve(size_t n){auto old=du_cpu.load();do{if(n>31*1024*1024-old)return false;}while(!du_cpu.compare_exchange_weak(old,old+n));return true;}
char* du_allocate(size_t n) {
 if(!n||n>du_max_data){++du_denied;return nullptr;}auto old=du_rx.load();
 do{if(old>du_receive_budget||n>du_receive_budget-old){++du_denied;return nullptr;}}while(!du_rx.compare_exchange_weak(old,old+n));
 if(!cpu_reserve(n)){du_rx-=n;++du_denied;return nullptr;}
 auto a=static_cast<Allocation*>(HeapAlloc(GetProcessHeap(),0,sizeof(Allocation)+n));if(!a){du_rx-=n;du_cpu-=n;++du_denied;return nullptr;}a->size=n;
 auto peak=du_peak.load();while(peak<old+n&&!du_peak.compare_exchange_weak(peak,old+n)){}return reinterpret_cast<char*>(a+1);
}
void du_free(char*p){if(p){auto a=reinterpret_cast<Allocation*>(p)-1;du_rx-=a->size;du_cpu-=a->size;HeapFree(GetProcessHeap(),0,a);}}
}
struct DuEngine::Impl {
 bool winsock{};
 Adapter library; NormInstanceHandle instance{};std::array<NormSessionHandle,6> sessions{};
 std::array<Options,6> options;unsigned count{};bool sender{};Stats stats;
 enum class Ownership{enqueued,sent};
 struct Tx{NormObjectHandle object{};OwnedData data;Correlation token;Ownership state{Ownership::enqueued};};std::array<std::array<Tx,4>,6> tx;
 DiagnosticSink sink{};void*context{};NativeMonitor local_monitor;NativeMonitor*monitor{&local_monitor};DWORD owner{};
 template<class F> decltype(auto) native(NativeCall call,unsigned unit,F&&f) {
  if(owner&&owner!=GetCurrentThreadId())throw std::runtime_error("NORM DU owner thread violation");
  struct Guard{NativeMonitor*m;~Guard(){m->completed.fetch_add(1);m->call=uint32_t(NativeCall::idle);}} guard{monitor};
  monitor->thread=GetCurrentThreadId();monitor->unit=unit;monitor->entered=diagnostic_qpc();monitor->call=uint32_t(call);
  return f();
 }
 void report(Stage stage,unsigned u,const Correlation&token={},uint64_t value=0)noexcept {
  if(sink)sink(context,{token,diagnostic_qpc(),value,0,GetCurrentThreadId(),0,uint16_t(u),uint16_t(stage),0});
 }
 void stop()noexcept {
  auto&i=*library.impl_;
  // DestroySession performs the documented dispatcher suspension, Close and
  // notification purge itself. Do not suspend/stop the dispatcher beforehand,
  // and never cancel cached handles whose purge event may already be queued.
  // Borrowed vectors survive every session and the entire native instance.
  if(instance){for(unsigned u=0;u<sessions.size();++u)if(sessions[u]){native(NativeCall::destroy_session,u,[&]{i.DestroySession(sessions[u]);});sessions[u]=nullptr;}
   native(NativeCall::destroy_instance,0,[&]{i.DestroyInstance(instance);});instance=nullptr;report(Stage::destroyed,0);}
  sessions={};
  for(auto&unit:tx)for(auto&t:unit){t.object=nullptr;t.data.reset();}stats.owned=0;count=0;library.stop();if(winsock){WSACleanup();winsock=false;}owner=0;
 }
};
DuEngine::DuEngine():impl_(std::make_unique<Impl>()){}
DuEngine::~DuEngine(){stop();}
void DuEngine::stop()noexcept{impl_->stop();}
HANDLE DuEngine::event()const noexcept{auto&p=*impl_;return p.instance?p.native(NativeCall::descriptor,0,[&]{return p.library.impl_->GetDescriptor(p.instance);}):nullptr;}
const Stats& DuEngine::stats()const noexcept{return impl_->stats;}
double DuEngine::tx_rate(unsigned u)const{auto&p=*impl_;if(u>=p.count)return 0;decltype(&NormGetTxRate)rate{};p.library.impl_->load(rate,"NormGetTxRate");return p.native(NativeCall::rate,u,[&]{return rate(p.sessions[u]);});}
double DuEngine::grtt(unsigned u)const{auto&p=*impl_;if(u>=p.count)return 0;decltype(&NormGetGrttEstimate)f{};p.library.impl_->load(f,"NormGetGrttEstimate");return p.native(NativeCall::grtt,u,[&]{return f(p.sessions[u]);});}
void DuEngine::observe(DiagnosticSink sink,void*context,NativeMonitor*monitor)noexcept{impl_->sink=sink;impl_->context=context;impl_->monitor=monitor?monitor:&impl_->local_monitor;}
void DuEngine::debug_log(const char* path){auto&p=*impl_;auto&i=*p.library.impl_;decltype(&NormOpenDebugLog)open{};decltype(&NormSetDebugLevel)level{};i.load(open,"NormOpenDebugLog");i.load(level,"NormSetDebugLevel");if(!p.instance||!p.native(NativeCall::configuration,0,[&]{return open(p.instance,path);}))throw std::runtime_error("NORM qualification log");p.native(NativeCall::configuration,0,[&]{level(6);});}
void DuEngine::qualification_fixed_rate(double rate){auto&p=*impl_;if(!p.instance||!p.sender||!(rate>=1e6&&rate<=100e6))throw std::runtime_error("NORM qualification rate");auto&i=*p.library.impl_;for(unsigned u=0;u<p.count;++u){p.native(NativeCall::SetCongestionControl,u,[&]{i.SetCongestionControl(p.sessions[u],false,false);});p.native(NativeCall::SetTxRate,u,[&]{i.SetTxRate(p.sessions[u],rate);});}}
void DuEngine::qualification_flow_control(double factor){auto&p=*impl_;if(!p.instance||!p.sender||factor<0||factor>2)throw std::runtime_error("NORM qualification flow control");decltype(&NormSetFlowControl) f{};p.library.impl_->load(f,"NormSetFlowControl");for(unsigned u=0;u<p.count;++u)p.native(NativeCall::flow_control,u,[&]{f(p.sessions[u],factor);});}
size_t DuEngine::receive_bytes()noexcept{return du_rx.load();}size_t DuEngine::receive_peak()noexcept{return du_peak.load();}uint64_t DuEngine::allocation_denied()noexcept{return du_denied.load();}
bool DuEngine::reserve_cpu_payload(size_t n)noexcept{return cpu_reserve(n);}void DuEngine::release_cpu_payload(size_t n)noexcept{du_cpu-=n;}size_t DuEngine::cpu_payload_bytes()noexcept{return du_cpu.load();}
void DuEngine::start(std::span<const Options> o,bool sender){
 stop();if(o.empty()||o.size()>6)throw std::runtime_error("NORM DU session count");auto&p=*impl_;p.owner=GetCurrentThreadId();p.sender=sender;p.stats={};
 try{
  WSADATA socket_data{};if(WSAStartup(MAKEWORD(2,2),&socket_data))throw std::runtime_error("NORM DU Winsock startup");p.winsock=true;
  auto&i=*p.library.impl_;p.native(NativeCall::library_load_version,0,[&]{i.init(o[0],sender,true);});decltype(&NormSetTxRateBounds) rate{};i.load(rate,"NormSetTxRateBounds");decltype(&NormSetRxSocketBuffer)rx_socket{};i.load(rx_socket,"NormSetRxSocketBuffer");decltype(&NormSetFlowControl)flow{};if(sender)i.load(flow,"NormSetFlowControl");
  p.instance=p.native(NativeCall::create_instance,0,[&]{return i.CreateInstance(false);});if(!p.instance)throw std::runtime_error("NORM DU engine creation");p.native(NativeCall::allocation_functions,0,[&]{i.SetAllocationFunctions(p.instance,du_allocate,du_free);});
  uint32_t node{};do{random_bytes({reinterpret_cast<uint8_t*>(&node),4});}while(!node||node==0xffffffffu);
  for(unsigned u=0;u<o.size();++u){do{random_bytes({reinterpret_cast<uint8_t*>(&node),4});}while(!node||node==0xffffffffu);p.options[u]=o[u];auto&s=p.sessions[u];s=p.native(NativeCall::create_session,u,[&]{return i.CreateSession(p.instance,o[u].group.c_str(),o[u].port,node);});if(!s)throw std::runtime_error("NORM DU session creation");
#define DUCALL(name,...) p.native(NativeCall::name,u,[&]{return i.name(__VA_ARGS__);})
   DUCALL(SetRxPortReuse,s,true,o[u].interface_address.c_str(),nullptr,0);auto name=interface_name(o[u].interface_address);
   if(!DUCALL(SetMulticastInterface,s,name.c_str())||!DUCALL(SetTTL,s,1)||!DUCALL(SetMulticastLoopback,s,true))throw std::runtime_error("NORM DU adapter binding");
   if(sender){DUCALL(SetTxOnly,s,false,false);if(!DUCALL(SetTxPort,s,0,false,o[u].interface_address.c_str()))throw std::runtime_error("NORM DU sender binding");
    // Negative means the protocol's segment/GRTT minimum. Zero means 1 byte/s
    // in the pinned implementation and causes pathological cold pacing.
    DUCALL(SetCongestionControl,s,true,true);DUCALL(SetTxRate,s,20e6);p.native(NativeCall::tx_rate_bounds,u,[&]{rate(s,-1,100e6);});DUCALL(SetGrttEstimate,s,.01);DUCALL(SetTxCacheBounds,s,3*du_max_data,2,3);
    // Latest DU frames are independent. Disable only the documented soft cache
    // timer: default factor2 reproduced an active pinned-native spin; zero
    // completed the payload control. Normal NACK repair and adaptive CC remain.
    p.native(NativeCall::flow_control,u,[&]{flow(s,0.);});
    uint16_t id{};random_bytes({reinterpret_cast<uint8_t*>(&id),2});if(!DUCALL(StartSender,s,id,1024*1024,1200,16,0,0))throw std::runtime_error("NORM DU sender start");DUCALL(SetTxRate,s,20e6);DUCALL(SetTxLoss,s,o[u].loss_percent);
   }else{DUCALL(SetDefaultNackingMode,s,NORM_NACK_NORMAL);DUCALL(SetSilentReceiver,s,false,-1);DUCALL(SetDefaultSyncPolicy,s,NORM_SYNC_CURRENT);DUCALL(SetRxCacheLimit,s,3);
    if(!DUCALL(StartReceiver,s,1024*1024))throw std::runtime_error("NORM DU receiver start");DUCALL(SetRxLoss,s,o[u].loss_percent);}
   if(!p.native(NativeCall::rx_socket_buffer,u,[&]{return rx_socket(s,4*1024*1024);}))throw std::runtime_error("NORM DU receive socket buffer");
#undef DUCALL
   ++p.count;
  }
 }catch(...){stop();throw;}
}
bool DuEngine::send(unsigned u,OwnedData data){return send(u,std::move(data),{});}
bool DuEngine::send(unsigned u,OwnedData data,const Correlation&token){
 auto&p=*impl_;auto&i=*p.library.impl_;if(!p.instance||!p.sender||u>=p.count||!data||data->empty()||data->size()>du_max_data)throw std::runtime_error("NORM DU send");
 for(auto&t:p.tx[u])if(!t.object){p.report(Stage::enqueue_enter,u,token,data->size());auto h=p.native(NativeCall::enqueue,u,[&]{return i.DataEnqueue(p.sessions[u],reinterpret_cast<const char*>(data->data()),UINT32(data->size()),nullptr,0);});p.report(Stage::enqueue_return,u,token,uint64_t(reinterpret_cast<uintptr_t>(h)));if(!h){++p.stats.busy;p.report(Stage::cache_busy,u,token,p.stats.owned);return false;}t.data=std::move(data);t.object=h;t.token=token;t.state=Impl::Ownership::enqueued;++p.stats.queued;++p.stats.owned;p.stats.owned_peak=std::max(p.stats.owned_peak,p.stats.owned);return true;}
 ++p.stats.busy;p.report(Stage::cache_busy,u,token,p.stats.owned);return false;
}
unsigned DuEngine::pump(DuReceive callback,void*user,unsigned budget){
 auto&p=*impl_;if(!p.instance)return 0;auto&i=*p.library.impl_;NormEvent e{};unsigned n=0;
 while(n<budget&&p.native(NativeCall::get_event,0,[&]{return i.GetNextEvent(p.instance,&e,false);})){++n;unsigned u=0;while(u<p.count&&p.sessions[u]!=e.session)++u;if(u==p.count){++p.stats.errors;continue;}
  if(e.type==NORM_TX_OBJECT_SENT){for(auto&t:p.tx[u])if(t.object==e.object){t.state=Impl::Ownership::sent;p.report(Stage::sent,u,t.token,uint64_t(reinterpret_cast<uintptr_t>(e.object)));break;}}
  // TX memory is borrowed from our immutable vector, never allocated by NORM.
  // Purge ends that borrowing; no receive-data detach or further native call
  // on this retiring handle is required.
  if(e.type==NORM_TX_OBJECT_PURGED){for(auto&t:p.tx[u])if(t.object==e.object){p.report(Stage::purged,u,t.token,uint64_t(reinterpret_cast<uintptr_t>(e.object)));t.object=nullptr;t.data.reset();--p.stats.owned;++p.stats.purged;break;}}
  if(e.type==NORM_SEND_ERROR){++p.stats.errors;p.report(Stage::native_error,u,{},e.type);}if(e.type==NORM_RX_OBJECT_ABORTED){++p.stats.aborted;p.report(Stage::native_error,u,{},e.type);}
  if(p.sender||!e.sender)continue;unsigned length=16;std::array<uint8_t,16>a{};uint16_t port{};
  bool address=p.native(NativeCall::node_address,u,[&]{return i.NodeGetAddress(e.sender,reinterpret_cast<char*>(a.data()),&length,&port);})&&length==4;
  std::string host=address?std::to_string(a[0])+"."+std::to_string(a[1])+"."+std::to_string(a[2])+"."+std::to_string(a[3]):"";
  if(!address||(!p.options[u].expected_host.empty()&&host!=p.options[u].expected_host)){if(e.type==NORM_RX_OBJECT_NEW){++p.stats.wrong_host;if(e.object)p.native(NativeCall::cancel,u,[&]{i.ObjectCancel(e.object);});}continue;}
  if(e.type!=NORM_RX_OBJECT_NEW&&e.type!=NORM_RX_OBJECT_COMPLETED)continue;auto size=p.native(NativeCall::object_size,u,[&]{return i.ObjectGetSize(e.object);});
  if(p.native(NativeCall::object_type,u,[&]{return i.ObjectGetType(e.object);})!=NORM_OBJECT_DATA||size<=0||size>du_max_data){++p.stats.oversized;p.native(NativeCall::cancel,u,[&]{i.ObjectCancel(e.object);});continue;}
  if(e.type==NORM_RX_OBJECT_COMPLETED){const auto completed=diagnostic_qpc();auto bytes=p.native(NativeCall::access_data,u,[&]{return i.DataAccessData(e.object);});if(!bytes)continue;++p.stats.completed;p.report(Stage::receive_complete,u,{},uint64_t(size));
   // Borrow only for the synchronous callback, as in the CDU adapter. Native
   // cache eviction/destruction invokes du_free exactly once. Avoid a second
   // ownership transfer and dispatcher suspension for every received frame.
   if(callback)callback(user,{{{reinterpret_cast<const uint8_t*>(bytes),size_t(size)},{a[0],a[1],a[2],a[3]}},u,completed});
  }
 }return n;
}

} // namespace psx::norm
