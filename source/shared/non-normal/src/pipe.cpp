#include <psx/non_normal/pipe.hpp>
#include <psx/non_normal/pipe_io.hpp>
#include <sddl.h>
#include <bcrypt.h>
#include <array>
namespace psx::non_normal {namespace {
std::uint64_t birth(HANDLE process)noexcept {FILETIME c{},e{},k{},u{};if(!GetProcessTimes(process,&c,&e,&k,&u))return 0;return (std::uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
bool same_user(HANDLE process)noexcept {HANDLE a{},b{};if(!OpenProcessToken(process,TOKEN_QUERY,&a))return false;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&b)){CloseHandle(a);return false;}std::array<std::uint8_t,256> x{},y{};DWORD xn{},yn{};bool ok=GetTokenInformation(a,TokenUser,x.data(),static_cast<DWORD>(x.size()),&xn)&&GetTokenInformation(b,TokenUser,y.data(),static_cast<DWORD>(y.size()),&yn)&&EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid);CloseHandle(a);CloseHandle(b);return ok;}

std::wstring endpoint(const wchar_t* lane,ProcessIdentity p,unsigned wire=version){return L"\\\\.\\pipe\\psx-ios."+std::wstring(lane)+L".v"+std::to_wstring(wire)+L"."+current_sid()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
}
ProcessIdentity current_process()noexcept{return {GetCurrentProcessId(),birth(GetCurrentProcess())};}
std::int64_t qpc()noexcept{LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
std::uint64_t qpc_frequency()noexcept{LARGE_INTEGER t{};QueryPerformanceFrequency(&t);return static_cast<std::uint64_t>(t.QuadPart);}
std::uint64_t nonce()noexcept{std::uint64_t n{};if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&n),sizeof(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return 0;return n;}
std::wstring current_sid(){HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return {};std::array<std::uint8_t,256> b{};DWORD size{};LPWSTR text{};std::wstring result;if(GetTokenInformation(token,TokenUser,b.data(),static_cast<DWORD>(b.size()),&size)&&ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid,&text)){result=text;LocalFree(text);}CloseHandle(token);return result;}
std::wstring reset_endpoint(ProcessIdentity p){return endpoint(L"reset.exporter",p);}
std::wstring coordinator_endpoint(){return L"\\\\.\\pipe\\psx-ios.non-normal.coordinator.v2."+current_sid();}
std::wstring plugin_endpoint(ProcessIdentity p){return endpoint(L"runway.plugin",p,actor_version);}
HANDLE create_server(const std::wstring& name,std::uint32_t max_instances,bool first_instance)noexcept {try{if(max_instances<1||max_instances>2)return INVALID_HANDLE_VALUE;const auto sid=current_sid();if(sid.empty())return INVALID_HANDLE_VALUE;const auto descriptor=L"D:P(A;;GA;;;"+sid+L")";PSECURITY_DESCRIPTOR security{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),SDDL_REVISION_1,&security,nullptr))return INVALID_HANDLE_VALUE;SECURITY_ATTRIBUTES a{sizeof(a),security,FALSE};HANDLE h=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(first_instance?FILE_FLAG_FIRST_PIPE_INSTANCE:0),PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,max_instances,4096,4096,0,&a);LocalFree(security);return h;}catch(...){return INVALID_HANDLE_VALUE;}}
bool accept(HANDLE h,std::uint32_t timeout)noexcept {psx::windows::Overlapped op(h);if(!op.ov.hEvent)return false;if(ConnectNamedPipe(h,&op.ov))return true;const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;if(error!=ERROR_IO_PENDING)return false;op.pending=true;if(WaitForSingleObject(op.ov.hEvent,timeout)!=WAIT_OBJECT_0){op.cancel(h);SetLastError(ERROR_TIMEOUT);return false;}DWORD count{};const auto ok=GetOverlappedResult(h,&op.ov,&count,FALSE);if(!ok){const auto failure=GetLastError();op.cancel(h);SetLastError(failure);return false;}op.pending=false;return true;}
bool authenticate(HANDLE pipe,bool server_side,ProcessIdentity expected,ProcessIdentity& actual)noexcept {ULONG pid{};if(!(server_side?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid))||!pid)return false;HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return false;ProcessIdentity p{pid,birth(process)};const bool ok=p.creation_filetime&&same_user(process)&&(!expected.pid||p==expected);CloseHandle(process);if(ok)actual=p;return ok;}
bool connected(HANDLE h)noexcept{DWORD available{};return PeekNamedPipe(h,nullptr,0,nullptr,&available,nullptr)!=FALSE;}
bool read_frame(HANDLE h,Frame& f,std::uint32_t timeout)noexcept {
    f={};
    return pipe_io::read_frame(h,f.bytes.data(),f.bytes.size(),f.size,timeout,[](auto n) {
        return n==request_bytes || n==actor_request_bytes || n==result_bytes;
    });
}
bool write_frame(HANDLE h,const Frame& f,std::uint32_t timeout)noexcept {
    if(f.size<16||f.size>max_frame){SetLastError(ERROR_INVALID_DATA);return false;}
    return pipe_io::transfer(h,true,const_cast<std::uint8_t*>(f.bytes.data()),static_cast<DWORD>(f.size),GetTickCount64()+timeout);
}
}
