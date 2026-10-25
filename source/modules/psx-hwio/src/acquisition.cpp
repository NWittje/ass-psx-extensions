#include "hwio/acquisition.hpp"

namespace hwio {
bool joystick_collection(unsigned page, unsigned usage) noexcept {
    // Generic Desktop Joystick / Multi-axis Controller, or Simulation Controls.
    // Game Pad (1:5), keyboard/mouse and vendor-defined collections are excluded.
    return (page == 1 && (usage == 4 || usage == 8)) || page == 2;
}
double usb_descriptor_interval_ms(unsigned speed, unsigned interval) noexcept {
    if (!interval || interval > 255)
        return 0;
    if (speed <= 1)
        return static_cast<double>(interval); // USB low/full-speed descriptor frame units.
    if ((speed == 2 || speed == 3) && interval <= 16)
        return static_cast<double>(1u << (interval - 1)) / 8.0;
    return 0;
}
std::uint64_t logical_values(int minimum, int maximum) noexcept {
    return maximum >= minimum ? static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum) - minimum) + 1 : 0;
}
AsyncInput::~AsyncInput() {
    close();
}
DWORD AsyncInput::attach(HANDLE file, unsigned bytes) {
    close();
    file_ = file;
    if (file_ == INVALID_HANDLE_VALUE || !bytes || bytes > 65536) {
        close();
        return ERROR_INVALID_PARAMETER;
    }
    buffers_[0].resize(bytes);
    buffers_[1].resize(bytes);
    operation_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation_.hEvent) {
        const DWORD error = GetLastError();
        close();
        return error;
    }
    return arm();
}
DWORD AsyncInput::arm() noexcept {
    if (pending_)
        return ERROR_IO_PENDING;
    ResetEvent(operation_.hEvent);
    if (!ReadFile(file_, buffers_[next_].data(), static_cast<DWORD>(buffers_[next_].size()), nullptr, &operation_)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING)
            return error;
    }
    pending_ = true;
    return ERROR_SUCCESS;
}
DWORD AsyncInput::complete(const std::uint8_t *&data, DWORD &bytes) noexcept {
    data = nullptr;
    bytes = 0;
    if (!pending_)
        return ERROR_INVALID_FUNCTION;
    if (!GetOverlappedResult(file_, &operation_, &bytes, FALSE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_INCOMPLETE)
            pending_ = false;
        return error;
    }
    pending_ = false;
    data = buffers_[next_].data();
    next_ ^= 1;
    return bytes ? ERROR_SUCCESS : ERROR_HANDLE_EOF;
}
void AsyncInput::close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
        if (pending_) {
            CancelIoEx(file_, &operation_);
            DWORD ignored{};
            // Cancellation is asynchronous: storage must live until completion.
            GetOverlappedResult(file_, &operation_, &ignored, TRUE);
        }
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    if (operation_.hEvent)
        CloseHandle(operation_.hEvent);
    operation_ = {};
    pending_ = false;
    next_ = 0;
}
} // namespace hwio
