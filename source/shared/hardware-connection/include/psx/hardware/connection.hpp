#pragma once
#include "psx/hardware/host_session.hpp"
#include <memory>
#include <string>
#include <vector>

namespace psx::hardware {
namespace detail { struct TransportOps; }
namespace testing { struct ConnectionAccess; }
struct SerialPort {
    std::wstring com_name, friendly_name, interface_path, instance_id;
};
std::vector<SerialPort> enumerate_serial_ports(); // Metadata only; never opens a serial port.
struct DeviceBinding {
    SerialPort candidate;
    HostBinding expected;
    PublicationPolicy publication;
};
enum class ConnectionPhase { closed, opening, discovering, starting, ready, closing, failed };
enum class OpenResult { accepted, busy, invalid };
struct ConnectionStatus {
    ConnectionPhase phase{ConnectionPhase::closed};
    HostStatus link{};
    std::uint32_t windows_error{};
    std::array<wchar_t,192> error{};
    bool owns_port{};
};
class HardwareConnection {
public:
    explicit HardwareConnection(HostCallbacks={});
    ~HardwareConnection();
    HardwareConnection(const HardwareConnection&)=delete;
    HardwareConnection& operator=(const HardwareConnection&)=delete;
    OpenResult open(const DeviceBinding&);
    SendResult publish_latest(const ProfileState&) noexcept;
    ConnectionStatus status() const noexcept;
    void close() noexcept;
private:
    friend struct testing::ConnectionAccess;
    friend class TrimDisplayConnection;
    HardwareConnection(HostCallbacks,detail::TransportOps);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
