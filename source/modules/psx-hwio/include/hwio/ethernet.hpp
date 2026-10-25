#pragma once
#include <psx/network/io.hpp>
#include "hid.hpp"
#include "hardware_config.hpp"
#include "output_identification_port.hpp"

namespace hwio {
struct HardwareResource {
    pokeys::Channel kind{};
    std::uint16_t channel{};
    std::string name, detail;
    bool input{}, output{}, velocity{};
    double minimum{}, maximum{};
};
struct EthernetHealth {
    std::string key, name, address, firmware, status;
    bool connected{}, configured{};
    std::uint32_t serial{};std::uint8_t model{};
    std::vector<HardwareResource> resources; // At most 1024, rebuilt only from card readback.
    std::array<std::uint8_t,55> pin_modes{};
    std::array<std::string,55> pin_functions{}; // Configuration readback only; never per-report work.
    std::vector<std::pair<std::uint64_t,std::string>> inhibited_outputs;
    double requested_hz{}, achieved_hz{}, response_p99_ms{},output_p99_ms{};
    std::uint64_t replies{}, errors{}, overruns{}, revision{},output_acknowledgements{};
};
struct Discovery {std::string address,name,key;std::uint32_t serial{};std::uint8_t model{};};
struct EthernetOutput {std::uint16_t device{};HardwareOutputIntent intent;};
class EthernetInput {
    friend struct RuntimeProbe;
    struct Device;
    struct Bank {
        std::array<std::array<AtomicSample,max_controls>,max_ethernet_devices> values;
        std::array<std::atomic<std::uint64_t>,max_ethernet_devices> reports{};
        std::array<std::atomic<std::uint64_t>,max_ethernet_devices> report_revision{};
        std::array<std::atomic<std::int64_t>,max_ethernet_devices> stamp{};
    };
    std::unique_ptr<Bank> bank_{std::make_unique<Bank>()};
    std::array<std::unique_ptr<Device>,max_ethernet_devices> devices_;
    std::thread worker_;
    HANDLE stop_{CreateEventW(nullptr,TRUE,FALSE,nullptr)},change_{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    mutable std::mutex mutex_;
    HardwareConfiguration pending_;
    std::wstring adapter_;
    std::uint64_t configuration_revision_{};
    std::vector<DeviceInfo> metadata_;
    std::vector<EthernetHealth> health_;
    std::vector<Discovery> discovered_;
    std::atomic<bool> discovery_requested_{};std::atomic<bool> support_enabled_{true};
    struct ControlRequest {std::string device;unsigned peripheral{};std::uint64_t revision{};unsigned operation{};};
    std::vector<ControlRequest> controls_;
    std::uint64_t sequence_{},generation_{};
    IdentificationPort identification_;
    // Constructor-only dependency seam for native socket fixtures. Production
    // uses Windows inventory and the manufacturer's port; no alternate UI mode.
    std::shared_ptr<const psx::network::Inventory> fixed_inventory_;
    std::uint16_t port_{pokeys::port};
    void run();
    void publish();
    void describe(Device&);
    void read_inventory(Device&);
    void inputs(Device&,const pokeys::Packet&,std::int64_t);
public:
    psx::network::AdapterMonitor adapters;
    HANDLE available_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    Spsc<Sample,8192> edges;
    Spsc<EthernetOutput,1024> outputs;
    std::atomic<std::uint64_t> overflow{},reports{},rejected{},metadata_revision{};
    explicit EthernetInput(std::shared_ptr<const psx::network::Inventory> inventory={},std::uint16_t port=pokeys::port);
    ~EthernetInput();
    void start();
    void stop();
    void configure(std::wstring,HardwareConfiguration,bool enabled=true);
    std::vector<DeviceInfo> devices();
    std::vector<EthernetHealth> health();
    std::vector<Discovery> discovered();
    void discover();
    bool refresh_card(const std::string& device);
    bool motor_control(const std::string& device,unsigned operation,unsigned axis); // 1 home, 2 stop, 3 start
    bool latest(std::size_t,std::size_t,Sample&) const noexcept;
    std::uint64_t report_revision(std::size_t slot) const noexcept {return slot>=max_usb_devices && slot<first_fds_device?bank_->report_revision[slot-max_usb_devices].load(std::memory_order_acquire):1;}
    InputActivity activity(std::size_t) const noexcept;
    bool submit(const EthernetOutput&) noexcept;
    bool identify_outputs(IdentificationRequest r,const IdentificationCard& c){const bool ok=identification_.submit(r,c);if(ok)SetEvent(change_);return ok;}
    std::optional<IdentificationCompletion> identification_completion(std::uint64_t session){return identification_.poll(session);}
};
} // namespace hwio
