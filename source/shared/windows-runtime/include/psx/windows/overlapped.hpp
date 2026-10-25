#pragma once
#include <Windows.h>

namespace psx::windows {
// Worker-owned native I/O. The operation, event and caller's buffer must outlive
// completion, including cancellation. A peer deadline cannot bound a driver's
// cancellation time; closing the event early would leave Windows using dead state.
class Overlapped final {
    HANDLE target_{INVALID_HANDLE_VALUE};
public:
    OVERLAPPED ov{};
    bool pending{};
    DWORD done{};
    explicit Overlapped(HANDLE target=INVALID_HANDLE_VALUE) noexcept : target_(target) { ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr); }
    Overlapped(const Overlapped&)=delete;
    Overlapped& operator=(const Overlapped&)=delete;
    ~Overlapped() {
        const auto error=GetLastError();
        cancel(target_);
        if(ov.hEvent)CloseHandle(ov.hEvent);
        SetLastError(error);
    }
    bool begin(HANDLE pipe,bool writing,void* bytes,DWORD size) noexcept {
        if(!ov.hEvent){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return false;}
        if(pending){SetLastError(ERROR_BUSY);return false;}
        target_=pipe;
        const auto event=ov.hEvent;
        ov={};ov.hEvent=event;ResetEvent(event);done=0;
        const auto completed=writing?WriteFile(pipe,bytes,size,&done,&ov):ReadFile(pipe,bytes,size,&done,&ov);
        pending=!completed&&GetLastError()==ERROR_IO_PENDING;
        return completed||pending;
    }
    bool finish(HANDLE pipe) noexcept {
        if(!pending)return true;
        if(!GetOverlappedResult(pipe,&ov,&done,FALSE))return false;
        pending=false;
        return true;
    }
    void cancel(HANDLE pipe) noexcept {
        if(!pending)return;
        CancelIoEx(pipe,&ov);
        GetOverlappedResult(pipe,&ov,&done,TRUE);
        pending=false;
    }
};
} // namespace psx::windows
