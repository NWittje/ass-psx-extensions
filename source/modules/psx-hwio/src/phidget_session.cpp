#include "hwio/phidget_session.hpp"
#include "hwio/vendor_library.hpp"
#include <phidget22.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <cwctype>
#include <cctype>
#include <cstring>
#include <atomic>
#include <condition_variable>

namespace hwio {
std::string phidget_error_text(int code) {
    // Names come from the installed native C ABI; preserve unknown numeric codes.
    const char* name="EPHIDGET_UNKNOWN";
    switch(code){
    #define ERROR_NAME(value) case value:name=#value;break
    ERROR_NAME(EPHIDGET_OK);ERROR_NAME(EPHIDGET_PERM);ERROR_NAME(EPHIDGET_NOENT);
    ERROR_NAME(EPHIDGET_TIMEOUT);ERROR_NAME(EPHIDGET_KEEPALIVE);ERROR_NAME(EPHIDGET_INTERRUPTED);
    ERROR_NAME(EPHIDGET_IO);ERROR_NAME(EPHIDGET_NOMEMORY);ERROR_NAME(EPHIDGET_ACCESS);
    ERROR_NAME(EPHIDGET_FAULT);ERROR_NAME(EPHIDGET_BUSY);ERROR_NAME(EPHIDGET_EXIST);
    ERROR_NAME(EPHIDGET_NOTDIR);ERROR_NAME(EPHIDGET_ISDIR);ERROR_NAME(EPHIDGET_INVALID);
    ERROR_NAME(EPHIDGET_NFILE);ERROR_NAME(EPHIDGET_MFILE);ERROR_NAME(EPHIDGET_NOSPC);
    ERROR_NAME(EPHIDGET_FBIG);ERROR_NAME(EPHIDGET_ROFS);ERROR_NAME(EPHIDGET_RO);
    ERROR_NAME(EPHIDGET_UNSUPPORTED);ERROR_NAME(EPHIDGET_INVALIDARG);ERROR_NAME(EPHIDGET_AGAIN);
    ERROR_NAME(EPHIDGET_NOTEMPTY);ERROR_NAME(EPHIDGET_UNEXPECTED);ERROR_NAME(EPHIDGET_DUPLICATE);
    ERROR_NAME(EPHIDGET_BADPASSWORD);ERROR_NAME(EPHIDGET_NETUNAVAIL);ERROR_NAME(EPHIDGET_CONNREF);
    ERROR_NAME(EPHIDGET_CONNRESET);ERROR_NAME(EPHIDGET_HOSTUNREACH);ERROR_NAME(EPHIDGET_NODEV);
    ERROR_NAME(EPHIDGET_WRONGDEVICE);ERROR_NAME(EPHIDGET_PIPE);ERROR_NAME(EPHIDGET_RESOLV);
    ERROR_NAME(EPHIDGET_UNKNOWNVAL);ERROR_NAME(EPHIDGET_NOTATTACHED);ERROR_NAME(EPHIDGET_INVALIDPACKET);
    ERROR_NAME(EPHIDGET_2BIG);ERROR_NAME(EPHIDGET_BADVERSION);ERROR_NAME(EPHIDGET_CLOSED);
    ERROR_NAME(EPHIDGET_NOTCONFIGURED);ERROR_NAME(EPHIDGET_EOF);ERROR_NAME(EPHIDGET_FAILSAFE);
    ERROR_NAME(EPHIDGET_UNKNOWNVALHIGH);ERROR_NAME(EPHIDGET_UNKNOWNVALLOW);ERROR_NAME(EPHIDGET_BADPOWER);
    ERROR_NAME(EPHIDGET_POWERCYCLE);ERROR_NAME(EPHIDGET_HALLSENSOR);ERROR_NAME(EPHIDGET_BADCURRENT);
    ERROR_NAME(EPHIDGET_BADCONNECTION);ERROR_NAME(EPHIDGET_NACK);ERROR_NAME(EPHIDGET_REJECTED);
    #undef ERROR_NAME
    }
    return std::string(name)+" ("+std::to_string(code)+")";
}
namespace {
std::mutex sessions_mutex, resources_mutex;
std::wstring default_selection;
// Allocate the release action before reserving a board, server or discovery
// reference. Memory pressure must not leave an owner behind or run cleanup
// while the same nonrecursive ownership mutex is held.
template<class F> struct LeaseCleanup {
    explicit LeaseCleanup(F action):release(std::move(action)){}
    F release;
    bool armed{};
    ~LeaseCleanup(){if(armed)release();}
};
template<class F> auto lease_cleanup(F action){return std::make_shared<LeaseCleanup<F>>(std::move(action));}
// An expired weak pointer does not imply that the SDK's central thread has
// finished. Keep the slot live until finalization and module release complete.
struct SessionSlot {
    std::weak_ptr<PhidgetSession> session;
    std::condition_variable ready;
    bool live{},failed{};
    int finalize_code{};
    DWORD unload_error{};
    HMODULE retained_module{};
};
std::map<std::wstring,std::shared_ptr<SessionSlot>> sessions;
std::map<PhidgetMotorResource,std::string> resources;
struct ServoSupply {int voltage{};unsigned users{};};
std::map<std::pair<int,int>,ServoSupply> servo_supplies;
std::atomic<unsigned> next_server{};
template<class T> T api(const PhidgetSession& s,const char* name) {
    const auto proc=s.symbol(name);T value{};
    static_assert(sizeof(value)==sizeof(proc));memcpy(&value,&proc,sizeof(value));return value;
}
}
struct PhidgetSession::Impl {
    HMODULE dll{};
    decltype(&Phidget_finalize) finalize{};
    std::mutex mutex;
    struct ServerEntry {std::string name,password; unsigned users{};};
    std::map<std::pair<std::string,unsigned>,ServerEntry> servers;
    unsigned discovery_users{};
};
PhidgetSession::PhidgetSession(const std::wstring& path):impl_(std::make_unique<Impl>()) {
    impl_->dll=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!impl_->dll)throw std::runtime_error("Cannot load native Phidget22 library");
    const auto proc=GetProcAddress(impl_->dll,"Phidget_finalize");
    static_assert(sizeof(proc)==sizeof(impl_->finalize));memcpy(&impl_->finalize,&proc,sizeof(proc));
    if(!impl_->finalize){
        // No SDK entry point or discovery worker has started for this module.
        FreeLibrary(impl_->dll);impl_->dll=nullptr;
        throw std::runtime_error("Native Phidget22 library lacks required Phidget_finalize");
    }
}
PhidgetSession::~PhidgetSession()=default;
void PhidgetSession::default_library(std::wstring selection){std::lock_guard lock(sessions_mutex);default_selection=std::move(selection);}
std::shared_ptr<PhidgetSession> PhidgetSession::acquire(std::wstring path) {
    if(path.empty()){std::wstring selection;{std::lock_guard lock(sessions_mutex);selection=default_selection;}path=vendor_library(VendorLibrary::phidgets,selection).wstring();}
    auto key=std::filesystem::weakly_canonical(path).wstring();
    for(auto& c:key)c=static_cast<wchar_t>(std::towlower(c));
std::unique_lock lock(sessions_mutex);
    auto& stored=sessions[key];if(!stored)stored=std::make_shared<SessionSlot>();
    const auto slot=stored;
    for(;;){
        if(auto existing=slot->session.lock())return existing;
        if(slot->live){slot->ready.wait(lock);continue;}
        if(slot->failed)throw std::runtime_error("Phidget22 runtime teardown failed; module retained (finalize "+
            std::to_string(slot->finalize_code)+", unload "+std::to_string(slot->unload_error)+"). Restart HWIO before reopening.");
        slot->live=true;break;
    }
    lock.unlock();
    PhidgetSession* raw{};
    try{raw=new PhidgetSession(path);}catch(...){
        {std::lock_guard guard(sessions_mutex);slot->live=false;}slot->ready.notify_all();throw;
    }
    // The last consumer has already joined its worker and closed/deleted its
    // channels, callbacks and server leases. Finalization is outside the cache
    // lock so another DLL and surviving motors are not blocked by SDK cleanup.
    auto result=std::shared_ptr<PhidgetSession>(raw,[slot](PhidgetSession* session) noexcept {
        auto module=session->impl_->dll;
        const int code=static_cast<int>(session->impl_->finalize(0));
        DWORD unload_error{};
        if(!code){if(FreeLibrary(module))module=nullptr;else unload_error=GetLastError();}
        session->impl_->dll=nullptr;delete session;
        {
            std::lock_guard guard(sessions_mutex);
            // Break the slot/weak-control-block/deleter capture cycle while the
            // deleter still has its implicit control-block weak reference.
            slot->session.reset();
            slot->finalize_code=code;slot->unload_error=unload_error;
            slot->retained_module=module;slot->failed=module!=nullptr;slot->live=false;
        }
        // A failed finalize never unloads potentially executing SDK code or
        // permits a new consumer to use that uncertain runtime.
        slot->ready.notify_all();
    });
    {std::lock_guard guard(sessions_mutex);slot->session=result;}
    return result;
}
FARPROC PhidgetSession::symbol(const char* name,bool required) const {
    const auto result=GetProcAddress(impl_->dll,name);
    if(!result && required)throw std::runtime_error(std::string("Missing Phidget22 export: ")+name);
    return result;
}
HMODULE PhidgetSession::module() const noexcept{return impl_->dll;}
PhidgetLease PhidgetSession::server(const PhidgetServer& server,bool discovery) {
    auto self=shared_from_this();
    if(discovery){
        const auto disable=api<decltype(&PhidgetNet_disableServerDiscovery)>(*this,"PhidgetNet_disableServerDiscovery");
        const auto enable=api<decltype(&PhidgetNet_enableServerDiscovery)>(*this,"PhidgetNet_enableServerDiscovery");
        auto cleanup=lease_cleanup([self,disable]() noexcept {std::lock_guard guard(self->impl_->mutex);if(!--self->impl_->discovery_users)disable(PHIDGETSERVER_DEVICEREMOTE);});
        PhidgetLease lease{cleanup,{}};
        std::lock_guard lock(impl_->mutex);
        if(!impl_->discovery_users && enable(PHIDGETSERVER_DEVICEREMOTE))throw std::runtime_error("Phidget server discovery failed");
        ++impl_->discovery_users;
        cleanup->armed=true;return lease;
    }
    if(server.host.empty() || server.host.size()>253 || !server.port || server.port>65535 || server.host.find_first_of("\r\n\t /\\")!=std::string::npos)throw std::runtime_error("Invalid Phidget server endpoint");
    auto host=server.host;for(auto& c:host)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto key=std::make_pair(host,server.port);
    const auto remove=api<decltype(&PhidgetNet_removeServer)>(*this,"PhidgetNet_removeServer");
    const auto add=api<decltype(&PhidgetNet_addServer)>(*this,"PhidgetNet_addServer");
    auto cleanup=lease_cleanup([self,key,remove]() noexcept {std::lock_guard guard(self->impl_->mutex);auto item=self->impl_->servers.find(key);if(item!=self->impl_->servers.end() && !--item->second.users){remove(item->second.name.c_str());self->impl_->servers.erase(item);}});
    PhidgetLease lease{cleanup,{}};
    std::lock_guard lock(impl_->mutex);
    auto found=impl_->servers.find(key);
    if(found==impl_->servers.end()){
        const auto name="HWIO-Phidget-"+std::to_string(++next_server);
        lease.server_name=name;
        found=impl_->servers.emplace(key,Impl::ServerEntry{name,server.password,0}).first;
        if(add(name.c_str(),server.host.c_str(),static_cast<int>(server.port),server.password.c_str(),0)){
            impl_->servers.erase(found);throw std::runtime_error("Phidget server registration failed");
        }
    }else {
        if(found->second.password!=server.password)throw std::runtime_error("Conflicting credentials for an already registered Phidget server");
        lease.server_name=found->second.name;
    }
    ++found->second.users;cleanup->armed=true;return lease;
}
PhidgetLease PhidgetSession::claim(PhidgetMotorResource key,const std::string& owner) {
    if(key.serial<=0 || key.hub_port<-1 || key.channel<0)throw std::runtime_error("A motor requires an exact physical Phidget address");
    auto cleanup=lease_cleanup([self=shared_from_this(),key]() noexcept {std::lock_guard guard(resources_mutex);resources.erase(key);});
    PhidgetLease lease{cleanup,{}};
    std::lock_guard lock(resources_mutex);
    for(const auto& [resource,consumer]:resources)if(resource.serial==key.serial && ((resource.hub_port==-1)!=(key.hub_port==-1)))
        throw std::runtime_error("Physical Phidget motor root already owned by "+consumer+"; direct and VINT addressing cannot overlap");
    if(const auto found=resources.find(key);found!=resources.end())throw std::runtime_error("Physical Phidget motor already owned by "+found->second);
    resources.emplace(key,owner);
    cleanup->armed=true;return lease;
}
std::optional<PhidgetServer> PhidgetSession::server_address(const std::string& name) const {
    std::lock_guard lock(impl_->mutex);
    for(const auto& [endpoint,value]:impl_->servers)if(value.name==name)return PhidgetServer{endpoint.first,value.password,endpoint.second};
    return {};
}
PhidgetLease PhidgetSession::servo_supply(PhidgetMotorResource key,int voltage){
    const auto root=std::make_pair(key.serial,key.hub_port);
    auto cleanup=lease_cleanup([self=shared_from_this(),root]() noexcept {std::lock_guard guard(resources_mutex);const auto found=servo_supplies.find(root);if(found!=servo_supplies.end() && !--found->second.users)servo_supplies.erase(found);});
    PhidgetLease lease{cleanup,{}};
    std::lock_guard lock(resources_mutex);if(key.function!=0 || !resources.count(key))throw std::runtime_error("Servo supply requires its actuator claim");
    auto [item,inserted]=servo_supplies.try_emplace(root,ServoSupply{voltage,0});
    if(!inserted && item->second.voltage!=voltage)throw std::runtime_error("Servo board supply changed outside its current owners");
    ++item->second.users;cleanup->armed=true;return lease;
}
void PhidgetSession::servo_supply(PhidgetMotorResource key,int voltage,const std::function<void()>& apply){
    std::lock_guard lock(resources_mutex);const auto root=std::make_pair(key.serial,key.hub_port);const auto supply=servo_supplies.find(root);
    if(supply==servo_supplies.end() || !resources.count(key))throw std::runtime_error("Servo board supply is not inspected/owned");
    if(supply->second.voltage==voltage)return; // No redundant global register write.
    for(const auto& [other,owner]:resources)if(other.serial==key.serial && other.hub_port==key.hub_port && other.function==0 && other!=key)
        throw std::runtime_error("Servo supply is board-wide; close the other actuator channels before changing it ("+owner+")");
    apply();supply->second.voltage=voltage;
}
}
