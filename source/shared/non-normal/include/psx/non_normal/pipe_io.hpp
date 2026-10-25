#pragma once
#include "pipe.hpp"
#include <psx/windows/overlapped.hpp>
#include <algorithm>
#include <array>

namespace psx::non_normal::pipe_io {
// Shared native operation ownership also covers pipe accept/read/write. The
// caller keeps the transport and its buffer alive through cancellation completion.
inline bool transfer(HANDLE pipe, bool writing, void* buffer, DWORD size,
                     ULONGLONG deadline) noexcept {
    psx::windows::Overlapped op(pipe);
    if (!op.ov.hEvent) return false;
    DWORD offset{};
    while (offset < size) {
        auto* bytes = static_cast<std::uint8_t*>(buffer) + offset;
        if (!op.begin(pipe, writing, bytes, size-offset)) return false;
        if (op.pending) {
            const auto now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(op.ov.hEvent,
                    static_cast<DWORD>(deadline-now)) != WAIT_OBJECT_0) {
                op.cancel(pipe);
                SetLastError(ERROR_TIMEOUT);
                return false;
            }
            if (!op.finish(pipe)) {
                const auto error = GetLastError();
                op.cancel(pipe);
                SetLastError(error);
                return false;
            }
        }
        const auto done = op.done;
        if (!done || done > size-offset) { SetLastError(ERROR_BROKEN_PIPE); return false; }
        offset += done;
        if (offset < size && GetTickCount64() >= deadline) {
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
    }
    return true;
}

inline std::uint32_t payload_size(const std::uint8_t* header) noexcept {
    std::uint32_t size{};
    for (unsigned i=0; i<4; ++i) size |= std::uint32_t(header[8+i]) << (8*i);
    return size;
}

template<class ValidSize>
bool read_frame(HANDLE pipe, std::uint8_t* bytes, std::size_t capacity,
                std::size_t& size, std::uint32_t idle_ms, ValidSize valid) noexcept {
    size = 0;
    const auto idle_deadline = GetTickCount64() + idle_ms;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available) {
            // A worker's zero-time poll must leave fragmented input untouched.
            if (!idle_ms) {
                if (available < 16) { SetLastError(ERROR_NO_DATA); return false; }
                std::array<std::uint8_t,16> header{};
                DWORD peeked{};
                if (!PeekNamedPipe(pipe, header.data(), 16, &peeked, &available, nullptr)) return false;
                if (peeked != 16) { SetLastError(ERROR_NO_DATA); return false; }
                const auto payload = payload_size(header.data());
                if (!valid(payload) || capacity < 16 || payload > capacity-16) {
                    SetLastError(ERROR_INVALID_DATA); return false;
                }
                if (available < 16+payload) { SetLastError(ERROR_NO_DATA); return false; }
            }
            break;
        }
        const auto now = GetTickCount64();
        if (now >= idle_deadline) { SetLastError(ERROR_NO_DATA); return false; }
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(2, idle_deadline-now)));
    }
    // Keep the existing one-second assembly allowance, shared by header/body.
    // Positive idle waits still allow streaming frames larger than pipe buffers.
    const auto deadline = GetTickCount64() + 1000;
    if (capacity < 16 || !transfer(pipe, false, bytes, 16, deadline)) return false;
    const auto payload = payload_size(bytes);
    if (!valid(payload) || payload > capacity-16) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (!transfer(pipe, false, bytes+16, payload, deadline)) return false;
    size = 16+payload;
    return true;
}
} // namespace psx::non_normal::pipe_io
