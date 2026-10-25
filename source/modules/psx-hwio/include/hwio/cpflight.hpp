#pragma once
#include "cpflight_protocol.hpp"
#include "fds_modules.hpp"
#include <psx/hardware/connection.hpp>
#include <functional>

namespace hwio {
struct CpflightConfiguration {
    bool enabled{};
    psx::hardware::SerialPort port;
    std::string serial;
};
bool valid_cpflight_configuration(const CpflightConfiguration&,std::string&);
struct CpflightView {
    std::vector<psx::hardware::SerialPort> ports;
    std::string status{"Support disabled"},serial,model;
    std::wstring com_name;
    std::uint64_t generation{},revision{},unknown{},malformed{};
    bool connected{},captain{},first_officer{},vs_ready{};
};
class CpflightPort {
public:
    virtual ~CpflightPort()=default;
    virtual bool open(const psx::hardware::SerialPort&,std::string&)=0;
    // A bounded read returns zero on timeout, negative on transport loss.
    virtual int read(std::span<std::uint8_t>,std::string&)=0;
    virtual bool write(std::string_view,std::string&)=0;
    virtual void close() noexcept=0;
};
struct CpflightDriver {
    std::function<std::vector<psx::hardware::SerialPort>()> enumerate;
    std::function<std::unique_ptr<CpflightPort>()> create;
};
class Cpflight {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Cpflight(CpflightDriver={});
    ~Cpflight();
    HANDLE available_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    Spsc<Sample,8192> edges;
    std::atomic<std::uint64_t> metadata_revision{},reports{},rejected{},overflow{};
    void start();void stop();
    void configure(const CpflightConfiguration&);
    void refresh();
    void publication(bool ready,std::uint64_t epoch,unsigned roles);
    CpflightView snapshot() const;
    std::vector<ModuleDevice> inventory() const;
    std::vector<DeviceInfo> devices() const;
    bool latest(std::size_t,std::size_t,Sample&) const;
    bool submit(const ModuleOutput&);
};
void add_cpflight_profiles(ModuleLibrary&);
}
