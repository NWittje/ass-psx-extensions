#pragma once
#include <psx/non_normal/pipe.hpp>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace ios {
inline std::string hwio_endpoint(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(value=="localhost" || value=="::1" || value=="127.0.0.1")return "127.0.0.1";
    return value;
}
template<std::size_t N> std::string hwio_text(const std::array<char,N>& value) {
    return {value.data(),std::size_t(std::find(value.begin(),value.end(),char{})-value.begin())};
}

// A PID in a pipe name is a lookup hint, not proof of its server process.
// Bind the OS PID, creation time and user before either control lane writes.
inline HANDLE open_hwio_pipe(std::wstring_view prefix,const std::wstring& endpoint,
                             psx::non_normal::ProcessIdentity expected) {
    using Identity=psx::non_normal::ProcessIdentity;
    struct Candidate {std::wstring path;Identity identity;};
    std::array<Candidate,8> candidates;
    std::size_t count{};
    if(!endpoint.empty())candidates[count++]={endpoint,expected};
    else {
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snapshot==INVALID_HANDLE_VALUE)return INVALID_HANDLE_VALUE;
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
        if(Process32FirstW(snapshot,&entry))do {
            if(_wcsicmp(entry.szExeFile,L"psx-hwio.exe"))continue;
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
            if(!process)continue;
            FILETIME born{},exit{},kernel{},user{};
            const bool valid=GetProcessTimes(process,&born,&exit,&kernel,&user)!=FALSE;
            CloseHandle(process);
            if(valid)candidates[count++]={std::wstring(prefix)+std::to_wstring(entry.th32ProcessID),
                {entry.th32ProcessID,(std::uint64_t(born.dwHighDateTime)<<32)|born.dwLowDateTime}};
        }while(count<candidates.size() && Process32NextW(snapshot,&entry));
        CloseHandle(snapshot);
    }
    DWORD error=ERROR_FILE_NOT_FOUND;
    for(std::size_t n=0;n<count;++n) {
        const auto& candidate=candidates[n];
        HANDLE pipe=CreateFileW(candidate.path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(pipe==INVALID_HANDLE_VALUE){error=GetLastError();continue;}
        Identity actual{};
        if(!candidate.identity.pid || !candidate.identity.creation_filetime ||
           !psx::non_normal::authenticate(pipe,false,candidate.identity,actual)) {
            CloseHandle(pipe);error=ERROR_ACCESS_DENIED;continue;
        }
        DWORD mode=PIPE_READMODE_MESSAGE;
        if(SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr))return pipe;
        error=GetLastError();CloseHandle(pipe);
    }
    SetLastError(error);
    return INVALID_HANDLE_VALUE;
}
} // namespace ios
