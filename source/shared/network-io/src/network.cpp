#include <psx/network/io.hpp>
#include <iphlpapi.h>
#include <netioapi.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <combaseapi.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace psx::network {
namespace {
struct Winsock {Winsock(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw std::runtime_error("WSAStartup failed");}~Winsock(){WSACleanup();}};
void startup(){static Winsock instance;}
bool valid_endpoint(const Endpoint& e) noexcept {
    return (e.address.ss_family==AF_INET && e.length==sizeof(sockaddr_in)) ||
           (e.address.ss_family==AF_INET6 && e.length==sizeof(sockaddr_in6));
}
bool bind_interface(SOCKET s,const Adapter& a,const Endpoint& remote,std::uint16_t local_port=0) noexcept {
    // Endpoint storage is public. Reject malformed lengths before copying into
    // Windows' smaller route structures or accepting an interface binding.
    if(!valid_endpoint(remote)){WSASetLastError(WSAEINVAL);return false;}
    const int family=remote.address.ss_family;
    const auto it=std::find_if(a.addresses.begin(),a.addresses.end(),[&](const Endpoint& e){return valid_endpoint(e) && e.address.ss_family==family;});
    if(!a.up || it==a.addresses.end()){WSASetLastError(WSAEADDRNOTAVAIL);return false;}
    auto local=*it;
    // Choose the appropriate source on this interface when it has several addresses.
    NET_LUID luid{};luid.Value=a.luid;MIB_IPFORWARD_ROW2 route{};SOCKADDR_INET destination{},source{};
    std::memcpy(&destination,&remote.address,static_cast<std::size_t>(remote.length));
    if(a.luid && GetBestRoute2(&luid,0,nullptr,&destination,0,&route,&source)==NO_ERROR)
        std::memcpy(&local.address,&source,static_cast<std::size_t>(local.length));
    if(family==AF_INET){
        reinterpret_cast<sockaddr_in*>(&local.address)->sin_port=htons(local_port);
        const DWORD index=htonl(a.index);
        if(setsockopt(s,IPPROTO_IP,IP_UNICAST_IF,reinterpret_cast<const char*>(&index),sizeof(index)))return false;
    }else{
        reinterpret_cast<sockaddr_in6*>(&local.address)->sin6_port=htons(local_port);
        const DWORD index=a.ipv6_index;
        if(setsockopt(s,IPPROTO_IPV6,IPV6_UNICAST_IF,reinterpret_cast<const char*>(&index),sizeof(index)))return false;
    }
    return bind(s,reinterpret_cast<const sockaddr*>(&local.address),local.length)==0;
}
void CALLBACK interface_changed(void* context,PMIB_IPINTERFACE_ROW,MIB_NOTIFICATION_TYPE){SetEvent(static_cast<HANDLE>(context));}
void CALLBACK route_changed(void* context,PMIB_IPFORWARD_ROW2,MIB_NOTIFICATION_TYPE){SetEvent(static_cast<HANDLE>(context));}
void CALLBACK address_changed(void* context,PMIB_UNICASTIPADDRESS_ROW,MIB_NOTIFICATION_TYPE){SetEvent(static_cast<HANDLE>(context));}
}
Endpoint Endpoint::parse(const std::string& host,std::uint16_t port) {
    startup();Endpoint result;auto* v4=reinterpret_cast<sockaddr_in*>(&result.address);
    if(InetPtonA(AF_INET,host.c_str(),&v4->sin_addr)==1){v4->sin_family=AF_INET;v4->sin_port=htons(port);result.length=sizeof(sockaddr_in);return result;}
    auto* v6=reinterpret_cast<sockaddr_in6*>(&result.address);
    if(InetPtonA(AF_INET6,host.c_str(),&v6->sin6_addr)==1){v6->sin6_family=AF_INET6;v6->sin6_port=htons(port);result.length=sizeof(sockaddr_in6);return result;}
    throw std::runtime_error("A numeric IPv4 or IPv6 address is required");
}
std::string Endpoint::host() const {
    if(!valid_endpoint(*this))return {};
    char out[INET6_ADDRSTRLEN]{};const void* address_ptr=address.ss_family==AF_INET?
        static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&address)->sin_addr):
        static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr);
    return InetNtopA(address.ss_family,address_ptr,out,sizeof(out))?out:"";
}
std::uint16_t Endpoint::port() const noexcept {if(!valid_endpoint(*this))return 0;return ntohs(address.ss_family==AF_INET?reinterpret_cast<const sockaddr_in*>(&address)->sin_port:reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);}
bool Endpoint::same(const Endpoint& e) const noexcept {
    if(!valid_endpoint(*this) || !valid_endpoint(e) || address.ss_family!=e.address.ss_family || port()!=e.port())return false;
    if(address.ss_family==AF_INET)return reinterpret_cast<const sockaddr_in*>(&address)->sin_addr.s_addr==reinterpret_cast<const sockaddr_in*>(&e.address)->sin_addr.s_addr;
    const auto& a=*reinterpret_cast<const sockaddr_in6*>(&address);const auto& b=*reinterpret_cast<const sockaddr_in6*>(&e.address);
    return a.sin6_scope_id==b.sin6_scope_id && std::memcmp(&a.sin6_addr,&b.sin6_addr,sizeof(a.sin6_addr))==0;
}
std::vector<Adapter> enumerate(InventoryPolicy policy) {
    startup();ULONG size=15000;std::vector<std::uint8_t> memory(size);ULONG result=ERROR_BUFFER_OVERFLOW;
    for(unsigned attempt=0;attempt<3 && result==ERROR_BUFFER_OVERFLOW;++attempt){
        memory.resize(size);result=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_ALL_INTERFACES|GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(memory.data()),&size);
        if(size>4*1024*1024)throw std::runtime_error("Network adapter inventory exceeds bound");
    }
    if(result==ERROR_NO_DATA)return {};
    if(result!=NO_ERROR)throw std::runtime_error("GetAdaptersAddresses: "+std::to_string(result));
    std::vector<Adapter> list;
    for(auto* p=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(memory.data());p;p=p->Next){
        if(policy==InventoryPolicy::physical && p->IfType!=IF_TYPE_ETHERNET_CSMACD && p->IfType!=IF_TYPE_IEEE80211)continue;
        MIB_IF_ROW2 row{};row.InterfaceLuid=p->Luid;const bool info=GetIfEntry2(&row)==NO_ERROR;
        // NotPresent includes retained hardware records (for example unplugged Apple USB Ethernet).
        // Keep down/disconnected physical adapters selectable; exclude filters and absent hardware.
        if(!info || (policy==InventoryPolicy::physical && !row.InterfaceAndOperStatusFlags.HardwareInterface) || row.InterfaceAndOperStatusFlags.FilterInterface ||
           row.InterfaceAndOperStatusFlags.EndPointInterface || row.OperStatus==IfOperStatusNotPresent)continue;
        GUID guid{};if(ConvertInterfaceLuidToGuid(&p->Luid,&guid)!=NO_ERROR)continue;
        wchar_t guid_text[40]{};StringFromGUID2(guid,guid_text,40);
        Adapter a;a.guid=guid_text;a.name=p->FriendlyName?p->FriendlyName:L"";a.description=p->Description?p->Description:L"";
        a.luid=p->Luid.Value;a.index=p->IfIndex;a.ipv6_index=p->Ipv6IfIndex;a.up=p->OperStatus==IfOperStatusUp;
        a.wired=p->IfType==IF_TYPE_ETHERNET_CSMACD;a.hardware=row.InterfaceAndOperStatusFlags.HardwareInterface!=0;
        a.multicast=!p->NoMulticast;a.metric=p->Ipv4Metric;
        for(auto* u=p->FirstUnicastAddress;u;u=u->Next){
            if(u->DadState!=IpDadStatePreferred || !u->Address.lpSockaddr ||
               u->Address.iSockaddrLength<int(sizeof(sockaddr_in)) || u->Address.iSockaddrLength>int(sizeof(sockaddr_storage)))continue;
            Endpoint e;e.length=u->Address.iSockaddrLength;std::memcpy(&e.address,u->Address.lpSockaddr,static_cast<std::size_t>(e.length));
            if(valid_endpoint(e)){a.addresses.push_back(e);a.prefixes.push_back(u->OnLinkPrefixLength);}
        }
        list.push_back(std::move(a));
    }
    std::sort(list.begin(),list.end(),[](const Adapter& a,const Adapter& b){return a.name<b.name;});return list;
}
std::wstring initial_adapter(const Inventory& i){
    std::wstring selected;
    for(const auto& a:i.adapters)if(a.up && a.wired && !a.addresses.empty()){if(!selected.empty())return {};selected=a.guid;}
    return selected;
}
std::wstring initial_ipv4_adapter(const Inventory& i){
    std::wstring selected;
    for(const auto& a:i.adapters)if(a.up && a.wired && std::any_of(a.addresses.begin(),a.addresses.end(),[](const Endpoint& e){return e.address.ss_family==AF_INET;})){
        if(!selected.empty())return {};
        selected=a.guid;
    }
    return selected;
}
AdapterMonitor::AdapterMonitor(InventoryPolicy policy):policy_(policy){
    if(!changed_ || !stop_ || !available){if(changed_)CloseHandle(changed_);if(stop_)CloseHandle(stop_);if(available)CloseHandle(available);throw std::runtime_error("Cannot create adapter notification events");}
    try{startup();worker_=std::thread([this]{run();});}catch(...){CloseHandle(changed_);CloseHandle(stop_);CloseHandle(available);throw;}
}
AdapterMonitor::~AdapterMonitor(){SetEvent(stop_);if(worker_.joinable())worker_.join();CloseHandle(changed_);CloseHandle(stop_);CloseHandle(available);}
std::shared_ptr<const Inventory> AdapterMonitor::snapshot() const {std::lock_guard lock(mutex_);return inventory_;}
void AdapterMonitor::run(){
    const auto interface_result=NotifyIpInterfaceChange(AF_UNSPEC,interface_changed,changed_,FALSE,&interface_notification_);
    const auto address_result=NotifyUnicastIpAddressChange(AF_UNSPEC,address_changed,changed_,FALSE,&address_notification_);
    const auto route_result=NotifyRouteChange2(AF_UNSPEC,route_changed,changed_,FALSE,&route_notification_);
    const bool fallback=interface_result!=NO_ERROR || address_result!=NO_ERROR || route_result!=NO_ERROR;
    do {
        auto next=std::make_shared<Inventory>();try{next->adapters=enumerate(policy_);}catch(const std::exception& e){next->error=e.what();}
        if(fallback && next->error.empty())next->error="Adapter notifications unavailable ("+std::to_string(interface_result)+", "+std::to_string(address_result)+"); inventory refreshed every second";
        next->revision=revision.load()+1;{std::lock_guard lock(mutex_);inventory_=next;}revision.store(next->revision);SetEvent(available);
        HANDLE handles[]{stop_,changed_};const auto waited=WaitForMultipleObjects(2,handles,FALSE,fallback?1000:INFINITE);if(waited==WAIT_OBJECT_0 || waited==WAIT_FAILED)break;
    }while(true);
    if(interface_notification_)CancelMibChangeNotify2(interface_notification_);
    if(address_notification_)CancelMibChangeNotify2(address_notification_);
    if(route_notification_)CancelMibChangeNotify2(route_notification_);
}
Stream::Stream(){startup();event_=WSACreateEvent();if(event_==WSA_INVALID_EVENT)throw std::runtime_error("Cannot create socket event");}
Stream::~Stream(){close();WSACloseEvent(event_);}
void Stream::close() noexcept {if(socket_!=INVALID_SOCKET){closesocket(socket_);socket_=INVALID_SOCKET;}state_=State::closed;closing_=false;tx_begin_=tx_end_=rx_end_=0;WSAResetEvent(event_);}
void Stream::fail(int e) noexcept {
    // A final reply may arrive in the same event as FIN. Retain received bytes
    // until the protocol owner consumes them or explicitly closes the session.
    if(socket_!=INVALID_SOCKET)closesocket(socket_);
    socket_=INVALID_SOCKET;tx_begin_=tx_end_=0;WSAResetEvent(event_);
    error_=e;state_=State::failed;
}
bool Stream::connect(const Adapter& a,const Endpoint& e) noexcept {
    close();error_=0;if(!valid_endpoint(e)){fail(WSAEINVAL);return false;}
    socket_=socket(e.address.ss_family,SOCK_STREAM,IPPROTO_TCP);if(socket_==INVALID_SOCKET){fail(WSAGetLastError());return false;}
    const BOOL yes=TRUE;
    if(!bind_interface(socket_,a,e) || setsockopt(socket_,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&yes),sizeof(yes)) ||
       WSAEventSelect(socket_,event_,FD_CONNECT|FD_READ|FD_WRITE|FD_CLOSE)){fail(WSAGetLastError());return false;}
    state_=State::connecting;const auto result=::connect(socket_,reinterpret_cast<const sockaddr*>(&e.address),e.length);
    if(result==0)state_=State::connected;
    else if(WSAGetLastError()!=WSAEWOULDBLOCK){fail(WSAGetLastError());return false;}
    return true;
}
void Stream::flush() noexcept {
    while(state_==State::connected && tx_begin_<tx_end_){
        const int n=::send(socket_,reinterpret_cast<const char*>(tx_.data()+tx_begin_),static_cast<int>(tx_end_-tx_begin_),0);
        if(n>0)tx_begin_+=static_cast<std::size_t>(n);
        else {const int e=WSAGetLastError();if(n<0 && e==WSAEWOULDBLOCK)return;fail(n==0?WSAECONNRESET:e);return;}
    }
    if(tx_begin_==tx_end_)tx_begin_=tx_end_=0;
}
bool Stream::send(std::span<const std::uint8_t> data) noexcept {
    if(state_!=State::connected || data.size()>tx_.size()-(tx_end_-tx_begin_))return false;
    // A partial native write releases the front of this bounded queue. Reclaim
    // it only when the tail cannot fit the next message; congestion must not
    // turn usable capacity into a false send failure or reorder pending bytes.
    if(data.size()>tx_.size()-tx_end_){
        std::memmove(tx_.data(),tx_.data()+tx_begin_,tx_end_-tx_begin_);
        tx_end_-=tx_begin_;tx_begin_=0;
    }
    std::copy(data.begin(),data.end(),tx_.begin()+tx_end_);tx_end_+=data.size();flush();return state_==State::connected;
}
void Stream::pump() noexcept {
    if(socket_==INVALID_SOCKET)return;WSANETWORKEVENTS events{};
    if(WSAEnumNetworkEvents(socket_,event_,&events)){fail(WSAGetLastError());return;}
    if(events.lNetworkEvents&FD_CONNECT){if(events.iErrorCode[FD_CONNECT_BIT]){fail(events.iErrorCode[FD_CONNECT_BIT]);return;}state_=State::connected;}
    if(events.lNetworkEvents&FD_CLOSE)closing_=true;
    if((events.lNetworkEvents&(FD_READ|FD_CLOSE)) || closing_){
        if(events.iErrorCode[FD_READ_BIT]){fail(events.iErrorCode[FD_READ_BIT]);return;}
        while(rx_end_<rx_.size()){
            const int n=recv(socket_,reinterpret_cast<char*>(rx_.data()+rx_end_),static_cast<int>(rx_.size()-rx_end_),0);
            if(n>0)rx_end_+=static_cast<std::size_t>(n);
            else {if(n<0 && WSAGetLastError()==WSAEWOULDBLOCK)break;fail(n==0?WSAECONNRESET:WSAGetLastError());return;}
        }
        // A full bounded buffer is back pressure, not evidence of overflow.
        // consume() rearms servicing after the owner has made room.
    }
    // FIN may arrive with more bytes than the bounded receive buffer can hold.
    // Keep the socket until recv observes EOF after the owner consumes data.
    flush();
}
bool same_binding(const Adapter& a,const Adapter& b) noexcept {
    if(a.guid!=b.guid || a.luid!=b.luid || a.index!=b.index || a.ipv6_index!=b.ipv6_index || a.up!=b.up || a.addresses.size()!=b.addresses.size())return false;
    return std::all_of(a.addresses.begin(),a.addresses.end(),[&](const Endpoint& address){
        return std::any_of(b.addresses.begin(),b.addresses.end(),[&](const Endpoint& other){return address.same(other);});
    });
}
void Stream::consume(std::size_t n) noexcept {
    const bool full=rx_end_==rx_.size();n=std::min(n,rx_end_);
    std::memmove(rx_.data(),rx_.data()+n,rx_end_-n);rx_end_-=n;
    if(full && n && socket_!=INVALID_SOCKET){
        const int received=recv(socket_,reinterpret_cast<char*>(rx_.data()+rx_end_),static_cast<int>(rx_.size()-rx_end_),0);
        if(received>0){rx_end_+=static_cast<std::size_t>(received);WSASetEvent(event_);}
        else if(received==0 || WSAGetLastError()!=WSAEWOULDBLOCK)fail(received==0?WSAECONNRESET:WSAGetLastError());
    }
}
Udp::Udp(){startup();event_=WSACreateEvent();if(event_==WSA_INVALID_EVENT)throw std::runtime_error("Cannot create datagram event");}
Udp::~Udp(){close();WSACloseEvent(event_);}
void Udp::close() noexcept {if(socket_!=INVALID_SOCKET)closesocket(socket_);socket_=INVALID_SOCKET;WSAResetEvent(event_);}
bool Udp::open(const Adapter& a,int family,std::uint16_t local_port) noexcept {
    close();if(family!=AF_INET && family!=AF_INET6){WSASetLastError(WSAEAFNOSUPPORT);return false;}
    family_=family;index_=family==AF_INET?a.index:a.ipv6_index;
    socket_=socket(family,SOCK_DGRAM,IPPROTO_UDP);if(socket_==INVALID_SOCKET)return false;
    Endpoint remote;remote.address.ss_family=static_cast<ADDRESS_FAMILY>(family);remote.length=family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6);
    const BOOL yes=TRUE;GUID id=WSAID_WSARECVMSG;DWORD returned{};LPFN_WSARECVMSG fn{};
    if(!bind_interface(socket_,a,remote,local_port) || setsockopt(socket_,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes)) ||
       setsockopt(socket_,family==AF_INET?IPPROTO_IP:IPPROTO_IPV6,family==AF_INET?IP_PKTINFO:IPV6_PKTINFO,reinterpret_cast<const char*>(&yes),sizeof(yes)) ||
       WSAIoctl(socket_,SIO_GET_EXTENSION_FUNCTION_POINTER,&id,sizeof(id),&fn,sizeof(fn),&returned,nullptr,nullptr) ||
       WSAEventSelect(socket_,event_,FD_READ)){close();return false;}
    receive_message_=reinterpret_cast<void*>(fn);return true;
}
bool Udp::send(const Endpoint& e,std::span<const std::uint8_t> data) noexcept {
    if(!valid_endpoint(e) || e.address.ss_family!=family_ || data.size()>std::numeric_limits<int>::max()){WSASetLastError(WSAEINVAL);return false;}
    return socket_!=INVALID_SOCKET && sendto(socket_,reinterpret_cast<const char*>(data.data()),static_cast<int>(data.size()),0,reinterpret_cast<const sockaddr*>(&e.address),e.length)==static_cast<int>(data.size());
}
bool Udp::receive(Datagram& out) noexcept {
    if(socket_==INVALID_SOCKET)return false;WSANETWORKEVENTS events{};WSAEnumNetworkEvents(socket_,event_,&events);
    for(unsigned attempt=0;attempt<32;++attempt){
        out={};WSABUF data{static_cast<ULONG>(out.bytes.size()),reinterpret_cast<char*>(out.bytes.data())};
        alignas(WSACMSGHDR) char control[128]{};WSAMSG message{};message.name=reinterpret_cast<sockaddr*>(&out.source.address);message.namelen=sizeof(sockaddr_storage);
        message.lpBuffers=&data;message.dwBufferCount=1;message.Control.buf=control;message.Control.len=sizeof(control);DWORD count{};
        if(reinterpret_cast<LPFN_WSARECVMSG>(receive_message_)(socket_,&message,&count,nullptr,nullptr))return false;
        bool accepted=false;
        for(auto* h=WSA_CMSG_FIRSTHDR(&message);h;h=WSA_CMSG_NXTHDR(&message,h)){
            if(h->cmsg_level==IPPROTO_IP && h->cmsg_type==IP_PKTINFO)accepted=reinterpret_cast<IN_PKTINFO*>(WSA_CMSG_DATA(h))->ipi_ifindex==index_;
            if(h->cmsg_level==IPPROTO_IPV6 && h->cmsg_type==IPV6_PKTINFO)accepted=reinterpret_cast<IN6_PKTINFO*>(WSA_CMSG_DATA(h))->ipi6_ifindex==index_;
        }
        if(accepted && !(message.dwFlags&MSG_TRUNC)){out.size=count;out.source.length=message.namelen;return true;}
    }return false;
}
} // namespace psx::network
