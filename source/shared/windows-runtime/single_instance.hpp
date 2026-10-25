#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace psx::windows {
enum class InstanceResult { primary, already_running, unavailable };

// Stable role name, independent of binary version/configuration/path/profile.
// Keep this object alive until the process runtime has stopped. No wait or IO.
class SingleInstance final {
    HANDLE handle_{};
    DWORD error_{};
    InstanceResult result_{InstanceResult::unavailable};
public:
    constexpr SingleInstance() noexcept = default;
    explicit SingleInstance(const wchar_t* role) noexcept { acquire(role); }
    InstanceResult acquire(const wchar_t* role) noexcept {
        if (handle_) return result_;
        if (!role || !*role) {
            error_ = ERROR_INVALID_PARAMETER;
            result_ = InstanceResult::unavailable;
            return result_;
        }
        result_ = InstanceResult::unavailable;
        SetLastError(ERROR_SUCCESS);
        handle_ = CreateMutexW(nullptr, FALSE, role);
        error_ = GetLastError();
        if (handle_) {
            result_ = error_ == ERROR_ALREADY_EXISTS ? InstanceResult::already_running : InstanceResult::primary;
            if (result_ != InstanceResult::primary) {
                CloseHandle(handle_);
                handle_ = nullptr;
            }
        }
        return result_;
    }
    void release() noexcept {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
        error_ = 0;
        result_ = InstanceResult::unavailable;
    }
    ~SingleInstance() { release(); }
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    InstanceResult result() const noexcept { return result_; }
    DWORD error() const noexcept { return error_; }
};
}
