#include <psx/instructor/communicator.hpp>
#include "metadata_json.hpp"
#include <psx/instructor/navigation_selection.hpp>
#include <iphlpapi.h>
#include <psapi.h>
#include <algorithm>
#include <charconv>
#include <map>
#include <variant>
#include <stdexcept>
#include <cstring>

namespace psx::instructor {
namespace {
[[noreturn]] void unavailable(const std::string &reason) {
  throw std::runtime_error(std::string("Active PSX navigation database unavailable: ") + reason);
}
std::wstring wide(std::string_view s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (!n) unavailable("invalid UTF-8 folder");
  std::wstring result(static_cast<std::size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
  return result;
}
std::string utf8(std::wstring_view s) {
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  if (!n) unavailable("invalid database name");
  std::string result(static_cast<std::size_t>(n), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
  return result;
}
std::uint32_t listener_pid(const std::string &host, std::uint16_t port) {
  auto target=network::Endpoint::parse(host, 0);
  bool local=host=="::1";
  if (target.address.ss_family==AF_INET)
    local=(ntohl(reinterpret_cast<const sockaddr_in *>(&target.address)->sin_addr.s_addr)&0xff000000U)==0x7f000000U;
  if (!local) for (const auto &a:network::enumerate()) for (const auto &ip:a.addresses)
    if (ip.same(target)) local=true;
  if (!local) unavailable("Main endpoint is not on this PC");
  DWORD size{};
  GetExtendedTcpTable(nullptr,&size,FALSE,target.address.ss_family,TCP_TABLE_OWNER_PID_LISTENER,0);
  std::vector<unsigned char> buffer(size);
  if (buffer.empty() || GetExtendedTcpTable(buffer.data(),&size,FALSE,target.address.ss_family,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)
    unavailable("cannot identify Main listener");
  std::uint32_t pid{};
  auto accept=[&](DWORD p) { if (pid && pid!=p) unavailable("ambiguous Main listener"); pid=p; };
  if (target.address.ss_family==AF_INET) {
    auto address=reinterpret_cast<const sockaddr_in *>(&target.address)->sin_addr.s_addr;
    auto table=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.data());
    for (DWORD i=0;i<table->dwNumEntries;++i) { const auto &r=table->table[i];
      if (ntohs(static_cast<u_short>(r.dwLocalPort))==port && (!r.dwLocalAddr||r.dwLocalAddr==address)) accept(r.dwOwningPid);
    }
  } else {
    auto address=reinterpret_cast<const sockaddr_in6 *>(&target.address);
    auto table=reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID *>(buffer.data());
    const unsigned char zero[16]{};
    for (DWORD i=0;i<table->dwNumEntries;++i) { const auto &r=table->table[i];
      if (ntohs(static_cast<u_short>(r.dwLocalPort))==port &&
          (!std::memcmp(r.ucLocalAddr,zero,16) || (!std::memcmp(r.ucLocalAddr,&address->sin6_addr,16) && r.dwLocalScopeId==address->sin6_scope_id))) accept(r.dwOwningPid);
    }
  }
  if (!pid) unavailable("Main listener not found"); return pid;
}
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b) {
  return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
}
std::filesystem::path runtime_directory(HANDLE process,const std::filesystem::path &ordinary) {
  std::array<HMODULE,512> modules{}; DWORD needed{};
  if (!K32EnumProcessModulesEx(process,modules.data(),sizeof(modules),&needed,LIST_MODULES_ALL) || needed>sizeof(modules))
    return ordinary;
  std::filesystem::path result;
  const auto local=ordinary.parent_path().parent_path().parent_path();
  std::array<wchar_t,32768> name{};
  for (DWORD i=0;i<needed/sizeof(HMODULE);++i) {
    const auto count=K32GetModuleFileNameExW(process,modules[i],name.data(),static_cast<DWORD>(name.size()));
    if (!count || count>=name.size()-1) continue;
    const std::filesystem::path module(std::wstring_view(name.data(),count));
    if (!same_path(module.filename(),L"psx-exporter-native.dll")) continue;
    const auto key=module.parent_path(), cache=key.parent_path(), root=cache.parent_path();
    const auto stamp=key.filename().wstring();
    // Both the original hash and the launcher's immutable hash-suffix edition
    // live below the default cache. A custom --state-dir is not a metadata root.
    if (stamp.size()<64 || !std::all_of(stamp.begin(),stamp.begin()+64,[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')||(c>=L'A'&&c<=L'F');}) ||
        (stamp.size()>64 && (stamp[64]!=L'-'||stamp.size()==65)) || !same_path(cache.filename(),L"cache") ||
        !same_path(root.filename(),L"exporter") || !same_path(root.parent_path().filename(),L"PSX-XPlane")) continue;
    const auto physical_local=root.parent_path().parent_path();
    const auto local_cache=physical_local.parent_path(), package=local_cache.parent_path(), packages=package.parent_path();
    const bool redirected=same_path(physical_local.filename(),L"Local") && same_path(local_cache.filename(),L"LocalCache") &&
      !package.filename().empty() && same_path(packages.filename(),L"Packages") && same_path(packages.parent_path(),local);
    if (!same_path(physical_local,local) && !redirected) continue;
    const auto candidate=root/L"navigation";
    if (!result.empty() && !same_path(result,candidate)) unavailable("ambiguous startup exporter metadata root");
    result=candidate;
  }
  // Resolve only the selected Main process's already loaded startup module.
  // This observes its physical cache path; it loads no DLL and scans no packages.
  return result.empty()?ordinary:result;
}
}
std::filesystem::path NavigationDatabaseSelection::default_directory() {
  std::wstring value(32768,0); auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",value.data(),static_cast<DWORD>(value.size()));
  if (!n||n>=value.size()) unavailable("exporter cache location unavailable"); value.resize(n);
  return std::filesystem::path(value)/L"PSX-XPlane"/L"exporter"/L"navigation";
}
NavigationDatabaseSelection::NavigationDatabaseSelection(std::filesystem::path directory)
  : directory_(directory.empty()?default_directory():directory), discover_runtime_(directory.empty()) {}
NavigationSelection NavigationDatabaseSelection::parse(std::string_view bytes, std::uint32_t pid, std::uint64_t start) {
  if (bytes.empty()||bytes.size()>4096) unavailable("metadata exceeds bounds");
  metadata::Json parser{bytes}; std::map<std::string,std::variant<std::string,std::uint64_t>> fields;
  parser.expect('{');
  if (!parser.take('}')) for (;;) {
    auto key=parser.str(); parser.expect(':'); parser.ws();
    std::variant<std::string,std::uint64_t> value;
    if (parser.at<bytes.size()&&bytes[parser.at]=='"') value=parser.str(); else value=parser.number();
    if (!fields.emplace(key,std::move(value)).second || fields.size()>5) unavailable("unexpected metadata fields");
    if (parser.take('}')) break; parser.expect(',');
  }
  parser.ws(); if (parser.at!=bytes.size() || fields.size()!=5) unavailable("incomplete metadata");
  auto number=[&](const char *key) { auto i=fields.find(key); if(i==fields.end()||!std::holds_alternative<std::uint64_t>(i->second)) unavailable("invalid metadata identity"); return std::get<std::uint64_t>(i->second); };
  auto str=[&](const char *key) { auto i=fields.find(key); if(i==fields.end()||!std::holds_alternative<std::string>(i->second)) unavailable("invalid metadata folder"); return std::get<std::string>(i->second); };
  if (number("schema")!=1||number("pid")!=pid||number("process_start_unix_ms")!=start) unavailable("metadata does not match Main process");
  auto error=str("error"); if (!error.empty()) throw std::runtime_error("Active PSX navigation database unavailable: "+error);
  auto folder=str("folder"); if (folder.empty()||folder.find_first_of("\r\n\t")!=std::string::npos||folder.find('\0')!=std::string::npos) unavailable("invalid database folder");
  auto path=std::filesystem::path(wide(folder)).lexically_normal(); if (!path.is_absolute()) unavailable("database folder is not absolute");
  // Normalize trailing separators before extracting the dataset label.
  if (path.filename().empty()) path=path.parent_path();
  auto name=path.filename()==L"Binary"?path.parent_path().filename():path.filename();
  return {path,utf8(name.wstring()),pid,start};
}
NavigationSelection NavigationDatabaseSelection::read(const std::string &host, std::uint16_t port) const {
  auto pid=listener_pid(host,port);
  HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
  if (!process) process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if (!process) unavailable("cannot read Main process identity");
  FILETIME birth{},exit{},kernel{},user{}; bool ok=GetProcessTimes(process,&birth,&exit,&kernel,&user)!=0;
  if(!ok) { CloseHandle(process); unavailable("cannot read Main process start time"); }
  std::filesystem::path directory;
  try { directory=discover_runtime_?runtime_directory(process,directory_):directory_; }
  catch (...) { CloseHandle(process); throw; }
  CloseHandle(process);
  auto ticks=(static_cast<std::uint64_t>(birth.dwHighDateTime)<<32)|birth.dwLowDateTime;
  const auto start=(ticks-116444736000000000ULL)/10000;
  const auto path=directory/(std::to_wstring(pid)+L".json");
  HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if (file==INVALID_HANDLE_VALUE) { const auto error=GetLastError(); unavailable("cannot open exporter selection metadata: "+utf8(path.native())+" (Windows error "+std::to_string(error)+")"); }
  // Delete sharing lets the exporter atomically replace the directory entry;
  // this handle keeps reading the complete document it originally opened.
  std::array<char,4097> bytes{}; DWORD count{};
  const bool read=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)!=0;
  CloseHandle(file);
  if (!read) unavailable("cannot read exporter selection metadata");
  auto selection=parse(std::string_view(bytes.data(),count),pid,start);
  selection.metadata_directory=std::move(directory);
  return selection;
}
}
