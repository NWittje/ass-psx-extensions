#pragma once
#include <windows.h>
#include <psx/sync/worker_io.hpp>
namespace coordinator::readiness_io {
struct Completion {bool pending{},success{};DWORD bytes{},error{};};
struct NoObserver {void operator()(const Completion&) const noexcept {}};
// Reset BEFORE querying. If completion races a pending result, its signal must
// remain available for the next wait. The default observer compiles away; tests
// complete real Windows I/O precisely after the nonblocking query.
template<class Observer=NoObserver>
Completion poll(HANDLE handle,OVERLAPPED& operation,Observer observe={}) noexcept {
 ResetEvent(operation.hEvent);Completion result;
 result.success=psx::sync::io_result(handle,&operation,&result.bytes,FALSE)!=FALSE;
 result.error=result.success?ERROR_SUCCESS:GetLastError();
 result.pending=!result.success&&result.error==ERROR_IO_INCOMPLETE;
 observe(result);return result;
}
}
