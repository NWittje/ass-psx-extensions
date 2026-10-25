#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace hwio::fds {
// Legacy 3302 uses two independent 32-contact packets. The second contains
// two three-bit encoded keys; its bytes must never be routed as raw edges.
class Cdu3302Bitmap {
    std::array<bool,70> values_{};
    unsigned banks_{};
public:
    struct Edge {unsigned channel{};bool on{};};
    struct Result {
        enum class Kind {ignored, rejected, waiting, baseline, changed} kind{};
        std::array<Edge,70> edges{};
        unsigned count{};
    };
    Result accept(std::span<const std::uint8_t>) noexcept;
    void reset() noexcept {values_.fill(false);banks_=0;}
    bool ready() const noexcept {return banks_==3;}
    const auto& values() const noexcept {return values_;}
};
// This narrow I/O boundary permits transport regression tests without opening
// a device. Production uses the Windows HID collection matched to SDK serial.
class Cdu3302Port {
public:
    virtual ~Cdu3302Port()=default;
    virtual bool start(std::string_view serial,std::string& error)=0;
    // 1 = one exact packet, 0 = pending, -1 = loss/error. No allocation.
    virtual int read(std::array<std::uint8_t,8>& packet) noexcept=0;
    virtual void stop() noexcept=0;
};
using Cdu3302Factory=std::unique_ptr<Cdu3302Port>(*)();
std::unique_ptr<Cdu3302Port> native_cdu3302_port();
bool cdu3302_collection(unsigned vendor,unsigned product,unsigned input_bytes,
                        unsigned output_bytes,std::wstring_view hid_serial,
                        std::string_view sdk_serial) noexcept;
} // namespace hwio::fds
