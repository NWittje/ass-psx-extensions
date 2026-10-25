#pragma once
#include "win_handle.hpp"
#include <psx/sync/worker_io.hpp>
#include <sddl.h>
#include <string>
#include <vector>

namespace coordinator {
inline std::uint64_t service_process_creation(HANDLE process) noexcept {
    FILETIME created{},exit{},kernel{},user{};
    return GetProcessTimes(process,&created,&exit,&kernel,&user)
        ?std::uint64_t(created.dwLowDateTime)|(std::uint64_t(created.dwHighDateTime)<<32):0;
}
inline std::wstring service_user_sid(){
    HANDLE raw{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw_win32_error("Service token");
    WinHandle token(raw);DWORD size{};
    GetTokenInformation(token.get(),TokenUser,nullptr,0,&size);
    std::vector<std::uint8_t> bytes(size);
    if(!GetTokenInformation(token.get(),TokenUser,bytes.data(),size,&size))throw_win32_error("Service TokenUser");
    LPWSTR text{};
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))throw_win32_error("Service SID");
    // LocalFree must also run if allocating the returned string throws.
    struct SidText {LPWSTR value;~SidText(){LocalFree(value);}} owned{text};
    return text;
}
// Keep the operation and buffer alive until either native or network-backed I/O
// has completed. target is the exact transport of this operation; nullptr is the
// existing remote-worker adapter, never a substitute local Windows handle.
struct ServiceOperation final {
    WinHandle event{make_stop_event()};OVERLAPPED value{};bool pending{};
    HANDLE target{};
    ~ServiceOperation(){if(pending)cancel(target);}
    void prepare(){ResetEvent(event.get());value={};value.hEvent=event.get();}
    void cancel(HANDLE pipe){if(pending){psx::sync::io_cancel(pipe,&value);DWORD n{};psx::sync::io_result(pipe,&value,&n,TRUE);pending=false;}}
};
} // namespace coordinator
