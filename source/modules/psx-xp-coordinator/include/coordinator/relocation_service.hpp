#pragma once
#include "coordinator/relocation.hpp"
#include "coordinator/win_handle.hpp"
#include <array>
#include <string>

namespace coordinator::relocation {
// Worker-owned primitive. The issuing worker retains handle/event/OVERLAPPED/buffer
// until receipt is terminal. It is deliberately not a PXR1 service or startup hook.
class Operation {
public:
 explicit Operation(IoKind);
 ~Operation();
 bool begin(HANDLE,std::uint32_t requested) noexcept;
 void issued(BOOL success,DWORD error,DWORD bytes) noexcept;
 void cancel() noexcept;
 void wait_retirement() noexcept; // worker only, never acquisition/native/UI
 template<class Hook> void poll(Hook after_query) noexcept {
  if(!receipt_.issued||!pending())return;
  ResetEvent(event_.get());DWORD bytes{};
  const auto ok=GetOverlappedResult(target_,&overlapped_,&bytes,FALSE);
  observe(ok?ERROR_SUCCESS:GetLastError(),bytes);
  after_query(receipt_); // test completion after pending query must remain signalled
 }
 void poll() noexcept {poll([](const IoReceipt&) noexcept {});}
 bool pending() const noexcept {return receipt_.issued&&!receipt_.terminal;}
 const IoReceipt& receipt() const noexcept {return receipt_;}
 OVERLAPPED* overlapped() noexcept {return &overlapped_;}
 HANDLE event() const noexcept {return event_.get();}
 std::array<std::uint8_t,4112>& buffer() noexcept {return buffer_;}
private:
 void observe(DWORD error,DWORD bytes) noexcept;
 WinHandle event_;OVERLAPPED overlapped_{};HANDLE target_{};
 IoReceipt receipt_{};std::array<std::uint8_t,4112> buffer_{};
};
// OS authentication supplies PID/birth/SID, not claimed native service instance.
// Keep the returned process handle alive while using the authenticated connection.
bool authenticate(HANDLE pipe,bool server_side,const Identity& expected,WinHandle& process) noexcept;
}
