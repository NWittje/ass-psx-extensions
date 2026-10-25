#include "hwio/trim_displays.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <iomanip>
#include <sstream>
#include <thread>

namespace hwio {
namespace ph=psx::hardware;
namespace {
class NativeTrimLink final:public TrimDisplayLink {
    ph::TrimDisplayConnection link_;
public:
    ph::OpenResult open(const TrimPort& p,std::uint64_t id,unsigned role) override {return link_.open(p,id,static_cast<std::uint8_t>(role));}
    ph::SendResult publish(const trim::Snapshot& s) noexcept override {return link_.publish_latest(s);}
    ph::TrimConnectionStatus status() const override {return link_.status();}
    void close() noexcept override {link_.close();}
};
bool released(const ph::TrimConnectionStatus& s){return !s.connection.owns_port && s.connection.phase==ph::ConnectionPhase::closed;}
std::string describe(const ph::TrimConnectionStatus& s){
    if(s.connection.error[0])return utf8(s.connection.error.data())+(s.connection.windows_error?" (Windows error "+std::to_string(s.connection.windows_error)+")":"");
    switch(s.connection.phase){
    case ph::ConnectionPhase::ready:
        if(s.has_status && s.device.panel_fault)return "Panel fault; USB session ready";
        return std::string(s.source_connected?"PSX source connected":"Waiting for PSX observations")+
            (s.has_status?"; accepted "+(s.device.has_accepted?std::to_string(s.device.accepted):"unknown")+
                " / rendered "+(s.device.has_rendered?std::to_string(s.device.rendered):"unknown"):"; waiting for device status");
    case ph::ConnectionPhase::opening:return "Opening selected port";
    case ph::ConnectionPhase::discovering:return "Identifying selected display";
    case ph::ConnectionPhase::starting:return "Starting verified display session";
    case ph::ConnectionPhase::closing:return "Releasing serial port";
    case ph::ConnectionPhase::failed:return "Display connection failed";
    default:return "Display disconnected";
    }
}
}
bool same_trim_endpoint(const TrimPort& a,const TrimPort& b) noexcept {
    return !a.instance_id.empty() && !b.instance_id.empty()?a.instance_id==b.instance_id:
        !a.interface_path.empty() && a.interface_path==b.interface_path;
}
bool same_trim_port(const TrimPort& a,const TrimPort& b) noexcept {
    return a.com_name==b.com_name && a.instance_id==b.instance_id && a.interface_path==b.interface_path;
}
bool same_trim_configuration(const TrimDisplayConfiguration& a,const TrimDisplayConfiguration& b) noexcept {
    if(a.enabled!=b.enabled)return false;
    for(unsigned n=0;n<2;++n)if(a.roles[n].device!=b.roles[n].device || !same_trim_port(a.roles[n].port,b.roles[n].port))return false;
    return true;
}
bool valid_trim_configuration(const TrimDisplayConfiguration& c,std::string& error){
    for(const auto& b:c.roles)if(b.device && (b.device>>48 || b.port.com_name.empty() ||
        (b.port.instance_id.empty() && b.port.interface_path.empty()) ||
        b.port.com_name.size()>64 || b.port.instance_id.size()>1024 || b.port.interface_path.size()>2048 || b.port.friendly_name.size()>512)){
        error="Trim display requires its verified 48-bit identity and exact Windows port identity";return false;
    }
    if(c.roles[0].device && c.roles[1].device && (c.roles[0].device==c.roles[1].device ||
        same_trim_endpoint(c.roles[0].port,c.roles[1].port) || c.roles[0].port.com_name==c.roles[1].port.com_name)){
        error="Captain and First Officer indicators require separate physical displays";return false;
    }
    return true;
}
std::string trim_identity(std::uint64_t id){std::ostringstream s;s<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(12)<<id;return s.str();}
TrimSource trim_source(const Mapper& m){
    TrimSource s;s.generation=static_cast<std::uint32_t>(m.epoch);
    if(!m.ready)return s;
    s.validity=trim::source_connected;
    auto read=[&](const char* name,int hi,int& out){const auto r=m.records.find(name);return r!=m.records.end() && integer(r->second.observed,out) && out>=0 && out<=hi;};
    int value{};
    if(read("Qi119",15000,value)){s.angle=static_cast<std::uint16_t>(value);s.validity|=trim::trim_known;}
    if(read("Qi226",INT_MAX,value)){s.indicators=static_cast<std::uint16_t>(value&15);s.validity|=trim::indicators_known;}
    return s;
}
trim::Snapshot trim_snapshot(const TrimSource& s,unsigned role) noexcept {
    return {s.angle,s.indicators,s.validity,255,static_cast<trim::Role>(role),s.generation};
}
struct TrimDisplays::Impl {
    struct Slot {
        std::unique_ptr<TrimDisplayLink> link;
        TrimBinding binding;
        ph::TrimConnectionStatus last_status;
        unsigned role{};
        bool attempted{},retiring{},blocked{};
        std::uint64_t retry_at{};
    };
    mutable std::mutex gate;
    std::condition_variable wake;
    std::thread worker;
    bool stopping{};
    TrimDisplayConfiguration requested;
    TrimSource source;
    std::optional<TrimPort> candidate;
    TrimDisplayView view;
    TrimDisplayDriver driver;
    std::array<Slot,3> slots;
    std::vector<TrimDisplayDevice> devices;
    std::vector<TrimPort> ports;
    std::uint64_t enumerate_at{};
    bool enabled_last{};
    std::string problem;
    std::string configuration_error;
    explicit Impl(TrimDisplayDriver d):driver(std::move(d)){
        if(!driver.enumerate)driver.enumerate=ph::enumerate_serial_ports;
        if(!driver.create)driver.create=[] {return std::make_unique<NativeTrimLink>();};
    }
    void shutdown(std::string failure={}){
        for(auto& slot:slots)if(slot.link)slot.link->close();
        for(auto& slot:slots)while(slot.link && !released(slot.link->status()))std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Destruction joins the facade's cancellation completion on this owner,
        // never on the PSX/acquisition/UI callbacks.
        for(auto& slot:slots)slot.link.reset();
        std::lock_guard lock(gate);view.enabled=false;view.status=failure.empty()?"Stopped; serial ports released":std::move(failure);
        for(auto& d:view.devices){d.ready=false;d.status.connection.owns_port=false;d.status.connection.phase=ph::ConnectionPhase::closed;d.text="Stopped; serial port released";}
    }
    void cycle(std::uint64_t now){
        TrimDisplayConfiguration c;TrimSource observation;std::optional<TrimPort> probe;std::string invalid;
        {std::lock_guard lock(gate);c=requested;observation=source;probe=std::move(candidate);candidate.reset();invalid=configuration_error;}
        if(!invalid.empty())c.enabled=false;
        if(c.enabled && !enabled_last)enumerate_at=0;
        enabled_last=c.enabled;
        for(unsigned n=0;n<2;++n)if(c.roles[n].device && devices.size()<8 && std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d.identity==c.roles[n].device;})){
            TrimDisplayDevice d;d.identity=c.roles[n].device;d.role=n+1;d.port=c.roles[n].port;devices.push_back(std::move(d));
        }
        if(c.enabled && now>=enumerate_at){
            try {
                const auto inventory=driver.enumerate();ports.clear();
                for(const auto& p:inventory)if(!p.com_name.empty() &&
                    std::none_of(ports.begin(),ports.end(),[&](const auto& other){return same_trim_endpoint(p,other) || p.com_name==other.com_name;})){
                    ports.push_back(p);if(ports.size()==128)break;
                }
                problem.clear();
            }
            catch(const std::exception& e){ports.clear();problem=std::string("Serial inventory unavailable: ")+e.what();}
            enumerate_at=now+2000;
        }
        const auto present=[&](const TrimPort& p){return std::any_of(ports.begin(),ports.end(),[&](const auto& current){return same_trim_endpoint(p,current);});};
        const auto verified=[&](const TrimDisplayDevice& d){return d.identified && present(d.port);};
        for(auto& device:devices)if(const auto current=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return same_trim_endpoint(device.port,p);});current!=ports.end())device.port=*current;
        if(probe){
            slots[2].binding={0,*probe};slots[2].role=0;slots[2].attempted=false;slots[2].blocked=false;slots[2].retry_at=0;slots[2].last_status={};
            if(slots[2].link){slots[2].retiring=true;slots[2].link->close();}
        }
        for(unsigned n=0;n<slots.size();++n){
            auto& slot=slots[n];
            auto desired=n<2?c.roles[n]:slot.binding;
            if(n<2 && desired.device){
                // A port number is a location, never the display identity. A
                // verified HELLO may relocate a saved board whose old Windows
                // endpoint has disappeared; two present copies remain a conflict.
                if(std::count_if(devices.begin(),devices.end(),[&](const auto& d){return d.identity==desired.device && d.role==n+1 && verified(d);})==1)
                    desired.port=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.identity==desired.device && d.role==n+1 && verified(d);})->port;
                else if(const auto current=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return same_trim_endpoint(desired.port,p);});current!=ports.end())desired.port=*current;
            }
            const bool changed=n<2 && (slot.binding.device!=desired.device || !same_trim_port(slot.binding.port,desired.port));
            if(changed){if(slot.link)slot.link->close();slot.retiring=slot.link!=nullptr;slot.binding=desired;slot.role=n+1;slot.attempted=false;slot.blocked=false;slot.retry_at=0;slot.last_status={};}
            bool assigned=n<2 && slot.binding.device!=0;
            bool permitted=c.enabled && (assigned || (n==2 && !slot.binding.port.com_name.empty()));
            if(assigned && std::count_if(devices.begin(),devices.end(),[&](const auto& d){return d.identity==slot.binding.device && verified(d);})>1)permitted=false;
            // A selected probe must never take a port already owned by a role.
            if(n==2)for(unsigned i=0;i<2;++i)if(c.roles[i].device &&
                (same_trim_endpoint(c.roles[i].port,slot.binding.port) ||
                 same_trim_endpoint(slots[i].binding.port,slot.binding.port)))permitted=false;
            if(slot.link){
                auto status=slot.link->status();
                slot.last_status=status;
                if(status.connection.phase==ph::ConnectionPhase::failed){slot.blocked=status.connection.link.fault==ph::HostFault::identity_mismatch;slot.link->close();status=slot.link->status();}
                if(!permitted || slot.retiring)slot.link->close();
                if(released(status)){
                    slot.link.reset();slot.retiring=false;
                    if(assigned)slot.retry_at=now+1000;
                }
            }
            if(!permitted){if(n==2){slot.binding={};slot.attempted=false;}continue;}
            if(slot.retiring || slot.blocked)continue;
            if(!slot.link && (!slot.attempted || assigned) && now>=slot.retry_at){
                // Reconnect only a saved Windows interface/instance, never a COM
                // number reused by unrelated equipment. HELLO then pins the board.
                const auto& saved=slot.binding.port;
                auto matches=[&](const TrimPort& p){return !saved.instance_id.empty()?p.instance_id==saved.instance_id:
                    !saved.interface_path.empty() && p.interface_path==saved.interface_path;};
                const auto matches_count=std::count_if(ports.begin(),ports.end(),matches);
                if(matches_count!=1)continue;
                const auto port=*std::find_if(ports.begin(),ports.end(),matches);
                bool occupied=false;
                for(unsigned other=0;other<slots.size();++other)if(other!=n && slots[other].link &&
                    same_trim_port(slots[other].binding.port,port))occupied=true;
                if(occupied)continue;
                slot.link=driver.create();slot.binding.port=port;slot.attempted=true;
                if(slot.link->open(port,slot.binding.device,slot.role)!=ph::OpenResult::accepted){slot.link.reset();slot.retry_at=now+1000;continue;}
            }
            if(!slot.link)continue;
            auto status=slot.link->status();
            if(assigned && status.connection.phase==ph::ConnectionPhase::ready){
                (void)slot.link->publish(trim_snapshot(observation,slot.role));status=slot.link->status();
            }
            slot.last_status=status;
            if(status.connection.link.identified && status.connection.phase==ph::ConnectionPhase::ready){
                const auto identity=status.connection.link.identity.device;
                const auto same=[&](const auto& d){return d.identity==identity && same_trim_endpoint(d.port,slot.binding.port);};
                auto found=std::find_if(devices.begin(),devices.end(),same);
                if(found==devices.end())found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.identity==identity && !verified(d);});
                if(found==devices.end() && devices.size()<8){devices.push_back({});found=std::prev(devices.end());}
                if(found!=devices.end()){found->identity=identity;found->role=status.role;found->port=slot.binding.port;found->status=status;found->identified=true;}
            }
        }
        for(auto& device:devices){
            device.ready=false;device.duplicate=false;
            for(const auto& slot:slots)if(same_trim_endpoint(device.port,slot.binding.port) &&
                (slot.binding.device==device.identity || (slot.last_status.connection.link.identified && slot.last_status.connection.link.identity.device==device.identity))){
                device.status=slot.last_status;device.port=slot.binding.port;device.ready=slot.link && !slot.retiring && device.status.connection.phase==ph::ConnectionPhase::ready;
                if(device.status.connection.link.fault==ph::HostFault::identity_mismatch)device.identified=false;
            }
            for(const auto& other:devices)if(&other!=&device && other.identity==device.identity &&
                !same_trim_endpoint(other.port,device.port) && verified(device) && verified(other))device.duplicate=true;
            device.text=device.duplicate?"Duplicate display identity; assignment inhibited":!c.enabled?"Support disabled":
                device.ready?describe(device.status):"Display unavailable / "+describe(device.status);
        }
        TrimDisplayView next;next.enabled=c.enabled;next.ports=ports;next.devices=devices;
        next.status=!invalid.empty()?invalid:!c.enabled?"Support disabled":!problem.empty()?problem:
            "Select a known trim-display COM port, then Identify. Other serial ports are never probed.";
        const auto& selected=slots[2];
        if(c.enabled && selected.attempted)next.status=selected.last_status.connection.phase==ph::ConnectionPhase::ready?
            "Display "+trim_identity(selected.last_status.connection.link.identity.device)+" identified; choose its matching CPT or FO assignment":
            "Selected "+utf8(selected.binding.port.com_name)+": "+describe(selected.last_status);
        {std::lock_guard lock(gate);view=std::move(next);}
    }
    void run(){
        std::string failure;
        try{
            for(;;){
                {std::unique_lock lock(gate);if(stopping)break;}
                cycle(GetTickCount64());
                std::unique_lock lock(gate);wake.wait_for(lock,std::chrono::milliseconds(33));if(stopping)break;
            }
        }catch(const std::exception& e){failure=std::string("Trim provider stopped: ")+e.what();}
        shutdown(std::move(failure));
    }
};
TrimDisplays::TrimDisplays(TrimDisplayDriver d):impl_(std::make_unique<Impl>(std::move(d))){}
TrimDisplays::~TrimDisplays(){stop();}
void TrimDisplays::service(std::uint64_t now){impl_->cycle(now);}
void TrimDisplays::start(){std::lock_guard lock(impl_->gate);if(!impl_->worker.joinable()){impl_->stopping=false;impl_->worker=std::thread([this]{impl_->run();});}}
void TrimDisplays::stop(){
    {std::lock_guard lock(impl_->gate);impl_->stopping=true;}impl_->wake.notify_all();
    if(impl_->worker.joinable())impl_->worker.join();else impl_->shutdown();
}
void TrimDisplays::configure(const TrimDisplayConfiguration& c){std::lock_guard lock(impl_->gate);if(same_trim_configuration(c,impl_->requested))return;impl_->requested=c;impl_->configuration_error.clear();valid_trim_configuration(c,impl_->configuration_error);impl_->wake.notify_all();}
void TrimDisplays::publish(TrimSource s) noexcept {std::lock_guard lock(impl_->gate);impl_->source=s;}
bool TrimDisplays::identify(const TrimPort& p,std::string& error){
    std::lock_guard lock(impl_->gate);
    if(!impl_->requested.enabled){error="Enable Aeronautical Simulator Service support first";return false;}
    if(impl_->candidate){error="An identification request is already pending";return false;}
    if(std::none_of(impl_->view.ports.begin(),impl_->view.ports.end(),[&](const auto& present){return same_trim_port(p,present);})){
        error="Select a currently present serial-port candidate";return false;
    }
    for(const auto& b:impl_->requested.roles)if(b.device && same_trim_endpoint(b.port,p)){error="This port is already assigned to an indicator";return false;}
    for(const auto& d:impl_->view.devices)if(d.ready && same_trim_endpoint(d.port,p) &&
        std::any_of(impl_->requested.roles.begin(),impl_->requested.roles.end(),[&](const auto& b){return b.device==d.identity;})){
        error="This display is already assigned to an indicator";return false;
    }
    impl_->candidate=p;impl_->wake.notify_all();return true;
}
TrimDisplayView TrimDisplays::snapshot() const {std::lock_guard lock(impl_->gate);return impl_->view;}
}
