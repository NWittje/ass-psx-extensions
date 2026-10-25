#pragma once
#include <windows.h>
#include <stdexcept>
#include <system_error>
namespace coordinator {
[[noreturn]] inline void throw_win32_error(const char* operation){const auto error=GetLastError();throw std::system_error(error,std::system_category(),operation);}
class WinHandle {
public:
    explicit WinHandle(HANDLE h = nullptr) noexcept : value_(h) {}
    ~WinHandle() { if (valid()) CloseHandle(value_); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h=nullptr) noexcept {if(value_==h)return;if(valid())CloseHandle(value_);value_=h;}
private: HANDLE value_;
};
inline HANDLE make_stop_event() {
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!e) throw_win32_error("CreateEvent"); return e;
}
inline HANDLE make_publication_event(){HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw_win32_error("CreateEvent publication");return event;}
}
