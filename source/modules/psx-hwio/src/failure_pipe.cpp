#include "hwio/failures.hpp"
#include <sddl.h>
#include <vector>

namespace hwio {
FailurePipe::FailurePipe(){io_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
FailurePipe::~FailurePipe(){close();if(io_.hEvent)CloseHandle(io_.hEvent);}
void FailurePipe::close(){if(pipe_!=INVALID_HANDLE_VALUE){CancelIoEx(pipe_,nullptr);if(pending_){DWORD n{};GetOverlappedResult(pipe_,&io_,&n,TRUE);}CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}phase_=Phase::closed;pending_=false;}
bool FailurePipe::open(std::wstring_view suffix){
    close();HANDLE token{};PSECURITY_DESCRIPTOR descriptor{};LPWSTR sid_text{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){error_=GetLastError();return false;}
    DWORD bytes{};GetTokenInformation(token,TokenLogonSid,nullptr,0,&bytes);std::vector<std::byte> storage(bytes);
    bool good=bytes && GetTokenInformation(token,TokenLogonSid,storage.data(),bytes,&bytes);
    if(good){const auto* groups=reinterpret_cast<const TOKEN_GROUPS*>(storage.data());good=groups->GroupCount && ConvertSidToStringSidW(groups->Groups[0].Sid,&sid_text);}
    CloseHandle(token);
    if(!good){error_=GetLastError();return false;}
    const std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(sid_text)+L")";LocalFree(sid_text);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)){error_=GetLastError();return false;}
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    const std::wstring name=std::wstring(psx::hwio::failures::pipe_prefix)+(suffix.empty()?std::to_wstring(GetCurrentProcessId()):std::wstring(suffix));
    pipe_=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,512,512,0,&security);
    LocalFree(descriptor);if(pipe_==INVALID_HANDLE_VALUE){error_=GetLastError();return false;}
    error_=0;reset();return true;
}
void FailurePipe::reset(){
    if(pipe_==INVALID_HANDLE_VALUE)return;
    if(pending_){CancelIoEx(pipe_,&io_);DWORD n{};GetOverlappedResult(pipe_,&io_,&n,TRUE);}pending_=false;
    DisconnectNamedPipe(pipe_);ResetEvent(io_.hEvent);phase_=Phase::connect;
    if(ConnectNamedPipe(pipe_,&io_)){SetEvent(io_.hEvent);pending_=false;}
    else{const auto error=GetLastError();if(error==ERROR_IO_PENDING)pending_=true;else if(error==ERROR_PIPE_CONNECTED)SetEvent(io_.hEvent);else{error_=error;close();}}
}
bool FailurePipe::complete(DWORD& bytes){
    if(!pending_)return true;
    if(GetOverlappedResult(pipe_,&io_,&bytes,FALSE)){pending_=false;return true;}
    const auto error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return false;
    pending_=false;error_=error;lost_=true;reset();return false;
}
void FailurePipe::read(){
    ResetEvent(io_.hEvent);phase_=Phase::read;packet_={};DWORD n{};
    if(ReadFile(pipe_,&packet_,sizeof(packet_),&n,&io_)){pending_=false;if(n!=sizeof(packet_)){lost_=true;reset();}}
    else if(GetLastError()==ERROR_IO_PENDING)pending_=true;else{error_=GetLastError();lost_=true;reset();}
}
const psx::hwio::failures::Packet* FailurePipe::poll(){
    if(phase_==Phase::closed || phase_==Phase::reply)return nullptr;
    DWORD bytes{};const bool was_pending=pending_;
    if(!complete(bytes))return nullptr;
    if(phase_==Phase::connect){read();return nullptr;}
    if(phase_==Phase::write){read();return nullptr;}
    if(phase_==Phase::read){
        // Synchronous reads have already been size checked by read().
        if(was_pending && bytes!=sizeof(packet_)){lost_=true;reset();return nullptr;}
        ResetEvent(io_.hEvent);phase_=Phase::reply;return &packet_;
    }
    return nullptr;
}
void FailurePipe::reply(const psx::hwio::failures::Packet& p){
    if(phase_!=Phase::reply)return;packet_=p;ResetEvent(io_.hEvent);phase_=Phase::write;DWORD n{};
    if(WriteFile(pipe_,&packet_,sizeof(packet_),&n,&io_)){pending_=false;if(n!=sizeof(packet_)){lost_=true;reset();}}
    else if(GetLastError()==ERROR_IO_PENDING)pending_=true;else{error_=GetLastError();lost_=true;reset();}
}
} // namespace hwio
