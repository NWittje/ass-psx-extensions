#pragma once
#include <windows.h>
#include <psx/sync/runtime.hpp>
namespace psx::sync {
DWORD wait_local_connection(HANDLE stop,HANDLE completion)noexcept;
// Worker-only compatibility seam for the existing framed OVERLAPPED loops.
// A null transport is explicitly the logical NORM channel; real Windows handles
// always take the original Win32 path. It never fabricates an OS pipe or PID.
class WorkerIo {
 Port port_;WorkerIo* previous_{};
 struct Pending {OVERLAPPED* op{};void* data{};DWORD size{};};
 Pending read_,write_;
 bool read_now(void*,DWORD,DWORD&);bool write_now(const void*,DWORD,DWORD&);
 friend BOOL io_read(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED)noexcept;
 friend BOOL io_write(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED)noexcept;
 friend BOOL io_result(HANDLE,LPOVERLAPPED,LPDWORD,BOOL)noexcept;
 friend BOOL io_cancel(HANDLE,LPOVERLAPPED)noexcept;
 friend bool worker_receive(Frame&)noexcept;
 friend bool worker_send(std::span<const std::uint8_t>)noexcept;
public:
 explicit WorkerIo(Family);~WorkerIo();
 bool poll()noexcept;const LinkStatus status()const{return port_.status();}
};
BOOL io_read(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED)noexcept;
BOOL io_write(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED)noexcept;
BOOL io_result(HANDLE,LPOVERLAPPED,LPDWORD,BOOL)noexcept;
BOOL io_cancel(HANDLE,LPOVERLAPPED)noexcept;
bool worker_poll(HANDLE)noexcept;
bool worker_receive(Frame&)noexcept;
bool worker_send(std::span<const std::uint8_t>)noexcept;
}
