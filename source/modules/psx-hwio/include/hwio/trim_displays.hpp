#pragma once
#include "core.hpp"
#include <psx/hardware/trim_connection.hpp>
#include <functional>
#include <memory>

namespace hwio {
using TrimPort = psx::hardware::SerialPort;
struct TrimBinding {
    std::uint64_t device{};
    TrimPort port;
};
struct TrimDisplayConfiguration {
    bool enabled{};
    std::array<TrimBinding,2> roles;
};
bool same_trim_port(const TrimPort&,const TrimPort&) noexcept;
bool same_trim_configuration(const TrimDisplayConfiguration&,const TrimDisplayConfiguration&) noexcept;
bool valid_trim_configuration(const TrimDisplayConfiguration&,std::string&);
struct TrimSource {
    std::uint16_t angle{},indicators{},validity{};
    std::uint32_t generation{};
};
TrimSource trim_source(const Mapper&);
trim::Snapshot trim_snapshot(const TrimSource&,unsigned role) noexcept;
std::string trim_identity(std::uint64_t);
struct TrimDisplayDevice {
    std::uint64_t identity{};
    unsigned role{};
    TrimPort port;
    psx::hardware::TrimConnectionStatus status;
    std::string text;
    bool ready{},duplicate{},identified{}; // HELLO evidence survives session handover and disablement.
};
struct TrimDisplayView {
    bool enabled{};
    std::vector<TrimPort> ports;
    std::vector<TrimDisplayDevice> devices;
    std::string status{"Support disabled"};
};
// One provider owner; each facade retains its own asynchronous serial worker.
// The injectable boundary lets offline tests exercise ownership without COM I/O.
class TrimDisplayLink {
public:
    virtual ~TrimDisplayLink()=default;
    virtual psx::hardware::OpenResult open(const TrimPort&,std::uint64_t,unsigned)=0;
    virtual psx::hardware::SendResult publish(const trim::Snapshot&) noexcept=0;
    virtual psx::hardware::TrimConnectionStatus status() const=0;
    virtual void close() noexcept=0;
};
struct TrimDisplayDriver {
    std::function<std::vector<TrimPort>()> enumerate;
    std::function<std::unique_ptr<TrimDisplayLink>()> create;
};
class TrimDisplays {
    friend struct TrimDisplaysProbe;
    void service(std::uint64_t);
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit TrimDisplays(TrimDisplayDriver={});
    ~TrimDisplays();
    void start();
    void stop();
    void configure(const TrimDisplayConfiguration&);
    void publish(TrimSource) noexcept;
    bool identify(const TrimPort&,std::string&);
    TrimDisplayView snapshot() const;
};
}
