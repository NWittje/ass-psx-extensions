#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hwio {
// Descriptor limits, not an ADC sampling-rate or accuracy claim.
struct UsbInputInfo {
    bool usb{};
    unsigned speed{}, interval{}, endpoint{}, packet_bytes{};
    double interval_ms{};
    std::string detail;
};
bool joystick_collection(unsigned page, unsigned usage) noexcept;
double usb_descriptor_interval_ms(unsigned speed, unsigned interval) noexcept;
std::uint64_t logical_values(int minimum, int maximum) noexcept;
UsbInputInfo inspect_usb_input(const std::wstring &path);

// One pending native read. The object and its OVERLAPPED never move while in use.
// Two buffers allow rearming before the completed report is parsed.
class AsyncInput {
    HANDLE file_{INVALID_HANDLE_VALUE};
    OVERLAPPED operation_{};
    std::vector<std::uint8_t> buffers_[2];
    unsigned next_{};
    bool pending_{};

  public:
    AsyncInput() = default;
    AsyncInput(const AsyncInput &) = delete;
    AsyncInput &operator=(const AsyncInput &) = delete;
    ~AsyncInput();
    // Takes ownership even on failure; also used with native pipes in lifecycle tests.
    DWORD attach(HANDLE file, unsigned report_bytes);
    DWORD arm() noexcept;
    DWORD complete(const std::uint8_t *&data, DWORD &bytes) noexcept;
    void close() noexcept;
    HANDLE event() const noexcept {
        return operation_.hEvent;
    }
};
} // namespace hwio
