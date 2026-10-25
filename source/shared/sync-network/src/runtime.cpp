#include <windows.h>
#include <psx/sync/runtime.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <filesystem>
#include <string_view>

namespace psx::sync {
namespace {
std::unique_ptr<Endpoint> instance;std::atomic<Endpoint*> published{};
std::atomic<Role> runtime_role{Role::coordinator};
std::mutex lifetime;
std::string ascii(const wchar_t* b){std::string s;for(;*b;++b){if(*b>127)return {};s.push_back(char(*b));}return s;}
bool unicast_ipv4(std::string_view s)noexcept{unsigned first{};std::size_t p{};for(unsigned part=0;part<4;++part){const auto begin=p;unsigned value{};while(p<s.size()&&s[p]>='0'&&s[p]<='9'){value=value*10+unsigned(s[p++]-'0');if(value>255||p-begin>3)return false;}if(p==begin||(p-begin>1&&s[begin]=='0'))return false;if(!part)first=value;if(part<3){if(p==s.size()||s[p++]!='.')return false;}}return p==s.size()&&first&&first<224;}
}
Settings read_settings(Role role,const std::wstring& path){
 Settings s;s.receive_port=role==Role::coordinator?coordinator_port:plugin_port;s.destination_port=role==Role::coordinator?plugin_port:coordinator_port;
 s.path=path;
 if(s.path.empty()){
  std::array<wchar_t,32768>b{};const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",b.data(),DWORD(b.size()));
  if(!n||n>=b.size())return s;s.path=std::wstring(b.data(),n)+L"\\ASS PSX Addons\\"+(role==Role::coordinator?L"Coordinator":L"XPlanePlugin")+L"\\network.ini";
 }
 if(role==Role::plugin){s.enabled=true;return s;} // Passive network listener; local pipes remain available.
 std::array<wchar_t,1024>b{};GetPrivateProfileStringW(L"Connection",L"XPlaneConnection",L"LocalPipe",b.data(),DWORD(b.size()),s.path.c_str());
 s.enabled=_wcsicmp(b.data(),L"Remote")==0;if(!s.enabled&&_wcsicmp(b.data(),L"LocalPipe")!=0)throw std::runtime_error("X-Plane connection must be LocalPipe or Remote");
 GetPrivateProfileStringW(L"Connection",L"XPlaneHost",L"127.0.0.1",b.data(),DWORD(b.size()),s.path.c_str());s.host=ascii(b.data());
 if(s.enabled&&!unicast_ipv4(s.host))throw std::runtime_error("Enter the X-Plane computer's IPv4 address");
 return s;

}
void save_connection(const Settings& s){
 if(s.path.empty())throw std::runtime_error("Connection settings path unavailable");
 // Numeric IPv4 is the sole additional address field. Do not alter existing
 // unrelated keys or copy configuration from a deployment package.
 if(s.enabled&&!unicast_ipv4(s.host))throw std::runtime_error("Enter the X-Plane computer's IPv4 address");
 std::filesystem::create_directories(std::filesystem::path(s.path).parent_path());
 const std::wstring host(s.host.begin(),s.host.end());
 if(!WritePrivateProfileStringW(L"Connection",L"XPlaneHost",host.c_str(),s.path.c_str())||!WritePrivateProfileStringW(L"Connection",L"XPlaneConnection",s.enabled?L"Remote":L"LocalPipe",s.path.c_str()))throw std::runtime_error("Cannot save X-Plane connection settings");
}
bool start_runtime(Role role,const Settings& settings,const std::wstring& qualified_dll){
 std::lock_guard lock(lifetime);if(!settings.enabled)return !published.load();if(instance)return true;
 auto next=std::make_unique<Endpoint>();Options o;o.role=role;o.host=settings.host;o.receive_port=settings.receive_port;o.destination_port=settings.destination_port;o.dll_path=qualified_dll;
 if(!next->start(o))return false;instance=std::move(next);runtime_role.store(role,std::memory_order_release);published.store(instance.get(),std::memory_order_release);return true;
}
void stop_runtime()noexcept{std::lock_guard lock(lifetime);published.store(nullptr,std::memory_order_release);if(instance)instance->stop();instance.reset();}
Endpoint* runtime()noexcept{return published.load(std::memory_order_acquire);}
bool enabled()noexcept{auto* e=runtime();return e&&(runtime_role.load(std::memory_order_acquire)==Role::coordinator||e->connected_fast());}
bool local_io_selected()noexcept{return runtime_role.load(std::memory_order_acquire)!=Role::plugin||!enabled();}
std::int64_t freshness_now(std::int64_t now)noexcept{if(!enabled())return now;const auto margin=clock_margin_ticks();return now<0||margin>std::uint64_t(INT64_MAX-now)?INT64_MAX:now+static_cast<std::int64_t>(margin);}
LinkStatus Port::status()const{auto* e=runtime();return e?e->status():LinkStatus{};}
bool Port::connected()const{const auto s=status();return s.connected&&s.clock_valid&&s.connection==binding_.connection&&s.service_generations[unsigned(family_)]==binding_.generation&&(s.ready_services&(1u<<unsigned(family_)));}
bool Port::changed()const{const auto s=status();return !s.connected||!s.clock_valid||s.connection!=binding_.connection||s.service_generations[unsigned(family_)]!=binding_.generation;}
void Port::attach(){if(auto* e=runtime())binding_=e->open_service(family_);else binding_={};normalizer_.reset();}
Port::~Port(){detach();}
void Port::detach()noexcept{if(binding_.connection)if(auto* e=runtime())e->close_service(family_,binding_);binding_={};normalizer_.reset();}
bool Port::send(std::span<const std::uint8_t>b){auto* e=runtime();const auto submitted=native_qpc();if(!e||!binding_.connection||!e->send(family_,b,binding_.connection,binding_.generation))return false;normalizer_.sent(family_,b,submitted);return true;}
bool Port::receive(Frame& f){auto* e=runtime();if(!e||!binding_.connection)return false;Received raw;LinkContext context;return e->receive(family_,raw,&context,binding_.connection,binding_.generation)&&normalizer_.received(raw,context,native_qpc(),f);}
}
