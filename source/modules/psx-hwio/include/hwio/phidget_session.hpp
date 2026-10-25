#pragma once
#include <Windows.h>
#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <functional>

namespace hwio {
std::string phidget_error_text(int code);
struct PhidgetServer {
    std::string host, password;
    unsigned port{5661};
    bool operator==(const PhidgetServer&) const=default;
};
struct PhidgetLease {
    std::shared_ptr<void> ownership;
    std::string server_name;
    explicit operator bool() const noexcept { return bool(ownership); }
};
// One claim covers every control API for the physical motor. A VINT serial is
// the hub serial, not a port's enumeration order or its current IP address.
struct PhidgetMotorResource {
    int serial{}, hub_port{-1}, channel{};
    int function{}; // 0 aliases all motor control classes; typed inputs use 1000 + class.
    auto operator<=>(const PhidgetMotorResource&) const = default;
};
class PhidgetSession : public std::enable_shared_from_this<PhidgetSession> {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PhidgetSession(const std::wstring&);
public:
    ~PhidgetSession();
    PhidgetSession(const PhidgetSession&) = delete;
    PhidgetSession& operator=(const PhidgetSession&) = delete;
    static std::shared_ptr<PhidgetSession> acquire(std::wstring library = {});
    static void default_library(std::wstring selection); // Set before provider startup; UI changes apply after restart.
    FARPROC symbol(const char*, bool required = true) const;
    HMODULE module() const noexcept;
    PhidgetLease server(const PhidgetServer&, bool discovery = false);
    std::optional<PhidgetServer> server_address(const std::string& sdk_name) const;
    PhidgetLease claim(PhidgetMotorResource, const std::string& owner);
    // RCServo supply voltage is board-wide, including channels owned by FDSTQ.
    PhidgetLease servo_supply(PhidgetMotorResource,int current_voltage);
    void servo_supply(PhidgetMotorResource,int voltage,const std::function<void()>& apply);
};
}
