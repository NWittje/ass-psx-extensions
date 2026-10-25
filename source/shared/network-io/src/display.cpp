#include <psx/network/io.hpp>
#include <iphlpapi.h>
#include <netioapi.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
namespace psx::network {
namespace {
uint32_t number(const Endpoint&e){return ntohl(reinterpret_cast<const sockaddr_in*>(&e.address)->sin_addr.s_addr);}
uint32_t mask(unsigned prefix){return prefix?0xffffffffu<<(32-prefix):0;}
std::pair<uint32_t,unsigned> parse_subnet(std::string_view key){
    auto slash=key.find('/');if(slash==key.npos)throw std::runtime_error("Invalid saved CDU subnet");
    unsigned prefix{};auto[p,ec]=std::from_chars(key.data()+slash+1,key.data()+key.size(),prefix);
    if(ec!=std::errc{}||p!=key.data()+key.size()||prefix>32)throw std::runtime_error("Invalid saved CDU prefix");
    auto e=Endpoint::parse(std::string(key.substr(0,slash)),0);
    if(e.address.ss_family!=AF_INET)throw std::runtime_error("CDU subnet requires IPv4");
    auto net=number(e);if((net&mask(prefix))!=net)throw std::runtime_error("CDU subnet must be normalized");
    return {net,prefix};
}
}
std::string host_ipv4(std::string_view value){
    while(!value.empty()&&(value.front()==' '||value.front()=='\t'||value.front()=='\r'||value.front()=='\n'))value.remove_prefix(1);
    while(!value.empty()&&(value.back()==' '||value.back()=='\t'||value.back()=='\r'||value.back()=='\n'))value.remove_suffix(1);
    if(value.empty()||value.size()>253)throw std::runtime_error("Enter a host address or computer name");
    std::string host(value);for(auto&c:host)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    if(host=="localhost"||host=="localhost.")return "127.0.0.1";
    if(host.find(':')!=host.npos)throw std::runtime_error("IPv4 is required; IPv6 is not enabled");
    IN_ADDR a{};
    if(InetPtonA(AF_INET,host.c_str(),&a)==1){
        auto n=ntohl(a.s_addr);
        if(!n||n==0xffffffffu||(n>>28)>=14)throw std::runtime_error("Host must be unicast IPv4");
        return host;
    }
    if(host.find_first_not_of("0123456789.")==host.npos)throw std::runtime_error("Invalid IPv4 address");
    for(auto c:host)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'))
        throw std::runtime_error("Invalid computer name");
    return host;
}
std::string subnet_key(const Endpoint&e,unsigned prefix){
    if(e.address.ss_family!=AF_INET||prefix>32)throw std::runtime_error("Invalid IPv4 network prefix");
    auto n=htonl(number(e)&mask(prefix));char text[INET_ADDRSTRLEN]{};
    if(!InetNtopA(AF_INET,&n,text,sizeof text))throw std::runtime_error("Cannot format IPv4 subnet");
    return std::string(text)+"/"+std::to_string(prefix);
}
std::string subnet_label(std::string_view key){
    auto[net,prefix]=parse_subnet(key);(void)net;
    if(prefix>0&&prefix<32&&prefix%8==0){
        auto slash=key.find('/');std::string out(key.substr(0,slash));unsigned dots{};
        for(size_t i=0;i<out.size();++i)if(out[i]=='.'&&++dots==prefix/8){out.resize(i+1);break;}
        for(unsigned i=prefix/8;i<4;++i){if(i>prefix/8)out+='.';out+='x';}
        return out+" (/"+std::to_string(prefix)+")";
    }
    return std::string(key);
}
std::vector<std::string> subnets(const Inventory&i){
    std::vector<std::string> keys;
    for(const auto&a:i.adapters)if(a.up&&a.multicast)for(size_t n=0;n<a.addresses.size();++n)
        if(a.addresses[n].address.ss_family==AF_INET&&n<a.prefixes.size()&&a.prefixes[n]<=32&&
           (number(a.addresses[n])>>24)!=127)keys.push_back(subnet_key(a.addresses[n],a.prefixes[n]));
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
}
std::vector<Adapter> subnet_adapters(const Inventory&i,std::string_view subnet){
    parse_subnet(subnet);std::vector<Adapter> result;
    for(const auto&a:i.adapters)if(a.up&&a.multicast){
        Adapter match=a;match.addresses.clear();match.prefixes.clear();
        for(size_t n=0;n<a.addresses.size();++n)if(a.addresses[n].address.ss_family==AF_INET&&
            n<a.prefixes.size()&&a.prefixes[n]<=32&&subnet_key(a.addresses[n],a.prefixes[n])==subnet){
            match.addresses.push_back(a.addresses[n]);match.prefixes.push_back(a.prefixes[n]);
        }
        if(!match.addresses.empty())result.push_back(std::move(match));
    }return result;
}
Adapter route_ipv4(const Inventory&i,const Endpoint&remote,std::string_view subnet){
    if(remote.address.ss_family!=AF_INET)throw std::runtime_error("IPv4 route required");
    if((number(remote)>>24)==127){
        Adapter a;a.up=true;a.addresses.push_back(Endpoint::parse("127.0.0.1",0));a.prefixes.push_back(8);return a;
    }
    auto candidates=subnet.empty()?i.adapters:subnet_adapters(i,subnet);
    if(!subnet.empty()){
        auto[net,prefix]=parse_subnet(subnet);
        if((number(remote)&mask(prefix))!=net)throw std::runtime_error("CDU host IPv4 is outside the selected subnet");
    }
    DWORD preferred{};GetBestInterfaceEx(const_cast<sockaddr*>(reinterpret_cast<const sockaddr*>(&remote.address)),&preferred);
    Adapter chosen;bool found{},ambiguous{};unsigned best_prefix{};uint64_t best_metric=UINT64_MAX;
    for(auto a:candidates)if(a.up){
        NET_LUID luid{};luid.Value=a.luid;MIB_IPFORWARD_ROW2 route{};SOCKADDR_INET dest{},source{};
        std::memcpy(&dest,&remote.address,sizeof(sockaddr_in));
        SOCKADDR_INET preferred_source{};
        if(!subnet.empty()&&!a.addresses.empty())std::memcpy(&preferred_source,&a.addresses.front().address,sizeof(sockaddr_in));
        if(!a.luid||GetBestRoute2(&luid,0,subnet.empty()?nullptr:&preferred_source,&dest,0,&route,&source)!=NO_ERROR||source.si_family!=AF_INET)continue;
        auto it=std::find_if(a.addresses.begin(),a.addresses.end(),[&](auto&e){return e.address.ss_family==AF_INET&&
            reinterpret_cast<const sockaddr_in*>(&e.address)->sin_addr.s_addr==source.Ipv4.sin_addr.s_addr;});
        // The selected subnet is a source policy: don't accept another alias Windows chose on the NIC.
        if(it==a.addresses.end())continue;
        bool preferred_match=a.index==preferred;uint64_t metric=uint64_t(route.Metric)+a.metric;
        unsigned prefix=route.DestinationPrefix.PrefixLength;
        if(preferred_match){a.addresses={*it};a.prefixes.clear();return a;}
        if(!found||prefix>best_prefix||(prefix==best_prefix&&metric<best_metric)){
            chosen=a;chosen.addresses={*it};chosen.prefixes.clear();found=true;ambiguous=false;best_prefix=prefix;best_metric=metric;
        }else if(prefix==best_prefix&&metric==best_metric)ambiguous=true;
    }
    if(ambiguous)throw std::runtime_error("Multiple adapters match the CDU subnet and host route");
    if(!found)throw std::runtime_error(subnet.empty()?"No usable IPv4 route to host":"Waiting for the saved CDU subnet or its host route");
    return chosen;
}
struct Ipv4Resolver::Impl{
    OVERLAPPED ov{};PADDRINFOEXW result{};HANDLE request{};
    HANDLE done{CreateEventW(nullptr,TRUE,FALSE,nullptr)};bool pending{};int error{};
    std::wstring name;
    Impl(){if(!done)throw std::runtime_error("Resolver event unavailable");WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)){CloseHandle(done);throw std::runtime_error("Resolver Winsock startup");}}
    void cancel()noexcept{
        if(pending){GetAddrInfoExCancel(&request);WaitForSingleObject(done,INFINITE);pending=false;}
        if(result){FreeAddrInfoExW(result);result=nullptr;}
    }
    ~Impl(){cancel();CloseHandle(done);WSACleanup();}
};
Ipv4Resolver::Ipv4Resolver():impl_(std::make_unique<Impl>()){}
Ipv4Resolver::~Ipv4Resolver()=default;
void Ipv4Resolver::cancel()noexcept{impl_->cancel();}
HANDLE Ipv4Resolver::event()const noexcept{return impl_->done;}
void Ipv4Resolver::start(std::string_view target){
    auto&i=*impl_;i.cancel();ResetEvent(i.done);i.ov={};i.ov.hEvent=i.done;i.request=nullptr;
    auto normalized=host_ipv4(target);i.name.assign(normalized.begin(),normalized.end());
    // AF_INET excludes AAAA-only results, including IPv6 fallback by namespace providers.
    ADDRINFOEXW hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;
    timeval timeout{2,0};
    i.error=GetAddrInfoExW(i.name.c_str(),nullptr,NS_ALL,nullptr,&hints,&i.result,&timeout,&i.ov,nullptr,&i.request);
    i.pending=i.error==WSA_IO_PENDING;if(!i.pending)SetEvent(i.done);
}
bool Ipv4Resolver::ready()const noexcept{return WaitForSingleObject(impl_->done,0)==WAIT_OBJECT_0;}
std::vector<Endpoint> Ipv4Resolver::take(){
    auto&i=*impl_;if(!ready())throw std::runtime_error("IPv4 resolution is pending");
    if(i.pending){i.error=GetAddrInfoExOverlappedResult(&i.ov);i.pending=false;}
    std::vector<Endpoint> endpoints;
    if(!i.error)for(auto*p=i.result;p&&endpoints.size()<16;p=p->ai_next)if(p->ai_family==AF_INET&&p->ai_addrlen==sizeof(sockaddr_in)){
        Endpoint e;e.length=sizeof(sockaddr_in);std::memcpy(&e.address,p->ai_addr,sizeof(sockaddr_in));
        auto n=number(e);if(!n||n==0xffffffffu||(n>>28)>=14)continue;
        if(std::none_of(endpoints.begin(),endpoints.end(),[&](auto&v){return v.same(e);}))endpoints.push_back(e);
    }
    if(i.result){FreeAddrInfoExW(i.result);i.result=nullptr;}
    if(i.error||endpoints.empty())throw std::runtime_error("No IPv4 address found for host ("+std::to_string(i.error)+")");
    return endpoints;
}
std::vector<Endpoint> resolve_ipv4(std::string_view target,HANDLE stop){
    Ipv4Resolver resolver;resolver.start(target);HANDLE events[]{resolver.event(),stop};
    auto outcome=WaitForMultipleObjects(stop?2:1,events,FALSE,3000);
    if(outcome!=WAIT_OBJECT_0){resolver.cancel();throw std::runtime_error("IPv4 resolution cancelled or timed out");}
    return resolver.take();
}
}
