#pragma once
#include "pokeys.hpp"
#include "core.hpp"

namespace hwio {
struct NetworkDevice {
    std::string key, name; // Address is discovered session state, never configuration.
    bool enabled{true};
    std::uint32_t serial{};
    std::uint8_t model{31};
    double fixed_hz{}; // zero selects measured adaptive scheduling.
    bool use_poext{}; // Host ownership only; never changes vendor bus configuration.
    BackendKind backend{BackendKind::pokeys_ethernet};
    std::string provider_model;
    unsigned provider_revision{};
};
enum class OutputTransform {numeric, boolean, lookup, text};
struct HardwareBinding {
    std::uint64_t id{};
    std::string name, device, observation, unit, format{"{value}"}, failure_text;
    pokeys::Channel resource{pokeys::Channel::digital};
    std::uint16_t channel{};
    std::uint16_t bit{UINT16_MAX}; // PoNET bit address; UINT16_MAX retains legacy whole-bank ownership.
    bool enabled{}, velocity{};
    OutputTransform transform{OutputTransform::numeric};
    double scale{1}, offset{}, minimum{}, maximum{1}, failure{}, threshold{.5};
    std::uint32_t mask{};
    std::vector<std::pair<double,double>> table;
};
struct HardwareConfiguration {
    struct PhidgetPort {std::string device;unsigned channel{};bool voltage{};bool operator==(const PhidgetPort&)const=default;};
    std::vector<NetworkDevice> devices;
    std::vector<HardwareBinding> outputs;
    std::vector<PhidgetPort> phidget_ports;
};
HardwareConfiguration initial_hardware_configuration();
bool hardware_valid(const HardwareConfiguration&,std::string&);
std::string hardware_encode(const HardwareConfiguration&);
bool hardware_decode(std::string_view,HardwareConfiguration&,std::string&);
bool hardware_resource_enabled(const HardwareConfiguration&,std::string_view,pokeys::Channel) noexcept;
} // namespace hwio
