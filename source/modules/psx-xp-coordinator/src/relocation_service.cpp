#include "coordinator/relocation_service.hpp"
#include <sddl.h>
#include <vector>
#include <exception>
namespace coordinator::relocation {
namespace {
std::uint64_t birth(HANDLE p) noexcept {FILETIME a{},b{},c{},d{};return GetProcessTimes(p,&a,&b,&c,&d)?std::uint64_t(a.dwLowDateTime)|(std::uint64_t(a.dwHighDateTime)<<32):0;}
std::wstring sid(HANDLE p) {
 HANDLE h{};if(!OpenProcessToken(p,TOKEN_QUERY,&h))return {};WinHandle token(h);DWORD n{};
 GetTokenInformation(token.get(),TokenUser,nullptr,0,&n);if(!n)return {};std::vector<std::uint8_t> data(n);
 if(!GetTokenInformation(token.get(),TokenUser,data.data(),n,&n))return {};
 LPWSTR s{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&s))return {};
 std::wstring result(s);LocalFree(s);return result;
}
}
Operation::Operation(IoKind kind):event_(make_stop_event()) {receipt_.kind=kind;}
Operation::~Operation() {if(pending()){wait_retirement();if(pending())std::terminate();}} // Invalid lifetime cannot be certified safe.
bool Operation::begin(HANDLE target,std::uint32_t requested) noexcept {
 if(pending()||!target||target==INVALID_HANDLE_VALUE||requested>buffer_.size()||
 (receipt_.kind==IoKind::connect?requested!=0:requested==0))return false;
 const auto kind=receipt_.kind;receipt_={};receipt_.kind=kind;receipt_.requested=requested;target_=target;
 ResetEvent(event_.get());overlapped_={};overlapped_.hEvent=event_.get();return true;
}
void Operation::issued(BOOL success,DWORD error,DWORD bytes) noexcept {
 receipt_.issued=true;
 if(!success&&error==ERROR_IO_PENDING){receipt_.outcome=IoOutcome::pending;receipt_.error=error;return;}
 if(!success&&error==ERROR_PIPE_CONNECTED&&receipt_.kind==IoKind::connect)error=ERROR_SUCCESS;
 observe(success?ERROR_SUCCESS:error,bytes);
}
void Operation::observe(DWORD error,DWORD bytes) noexcept {
 receipt_.error=error;receipt_.completed=0;receipt_.bytes_valid=false;receipt_.failure_qualified=false;
 receipt_.query_valid=error!=ERROR_INVALID_HANDLE;
 if(error==ERROR_IO_INCOMPLETE||error==ERROR_IO_PENDING||error==ERROR_INVALID_HANDLE){
  receipt_.terminal=false;receipt_.outcome=error==ERROR_INVALID_HANDLE?IoOutcome::unknown:IoOutcome::pending;return;
 }
 receipt_.terminal=true;
 if(!error){receipt_.outcome=IoOutcome::completed;if(receipt_.kind!=IoKind::connect){receipt_.bytes_valid=true;receipt_.completed=bytes;}}
 else if(error==ERROR_OPERATION_ABORTED)receipt_.outcome=IoOutcome::cancelled;
 else {receipt_.outcome=IoOutcome::failed;receipt_.failure_qualified=true;}
}
void Operation::cancel() noexcept {
 if(!pending())return;
 if(CancelIoEx(target_,&overlapped_)){receipt_.cancellation=Cancel::requested;receipt_.cancel_error=0;}
 else {receipt_.cancel_error=GetLastError();receipt_.cancellation=receipt_.cancel_error==ERROR_NOT_FOUND?Cancel::not_found:Cancel::failed;}
}
void Operation::wait_retirement() noexcept {
 if(!pending())return;cancel();DWORD bytes{};
 const auto ok=GetOverlappedResult(target_,&overlapped_,&bytes,TRUE);observe(ok?ERROR_SUCCESS:GetLastError(),bytes);
}
bool authenticate(HANDLE pipe,bool server_side,const Identity& expected,WinHandle& process) noexcept {
 try {
  ULONG pid{};if(!(server_side?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid))||pid!=expected.pid||!pid||!expected.birth)return false;
  WinHandle actual(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
  if(!actual.valid()||WaitForSingleObject(actual.get(),0)!=WAIT_TIMEOUT||birth(actual.get())!=expected.birth)return false;
  const auto own=sid(GetCurrentProcess()),peer=sid(actual.get());if(own.empty()||peer!=own)return false;
  // Transfer native handle ownership locally; no filesystem transfer is involved.
  HANDLE retained{};if(!DuplicateHandle(GetCurrentProcess(),actual.get(),GetCurrentProcess(),&retained,0,FALSE,DUPLICATE_SAME_ACCESS))return false;
  process.reset(retained);return true;
 }catch(...){return false;}
}
}
