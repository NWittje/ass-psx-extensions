#pragma once
#include "psx/hardware/connection.hpp"
#include <windows.h>

namespace psx::hardware::detail {
enum class IoState { pending, completed, failed };
struct IoResult {IoState state{IoState::pending};std::size_t count{};DWORD error{};};
struct TransportOps {
    void* context{};
    bool (*open)(void*,const SerialPort&,DWORD&) noexcept{};
    IoResult (*read)(void*,std::span<std::uint8_t>) noexcept{};
    IoResult (*write)(void*,Bytes) noexcept{};
    void (*wait)(void*,HANDLE,DWORD) noexcept{};
    void (*cancel_close)(void*) noexcept{};
};
class SerialTransport {
public:
    SerialTransport();
    ~SerialTransport();
    TransportOps operations() noexcept;
private:
    friend struct psx::hardware::testing::ConnectionAccess;
    bool open(const SerialPort&,DWORD&) noexcept;
    IoResult read(std::span<std::uint8_t>) noexcept;
    IoResult write(Bytes) noexcept;
    void wait(HANDLE,DWORD) noexcept;
    void cancel_close() noexcept;
    HANDLE file_{INVALID_HANDLE_VALUE};
    OVERLAPPED reading_{},writing_{};
    std::array<std::uint8_t,512> input_{};
    std::array<std::uint8_t,max_wire> output_{};
    bool read_pending_{},write_pending_{};
};
}
