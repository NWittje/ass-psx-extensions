#pragma once
#include "catalogue.hpp"
#include "ethernet.hpp"
#include "digital_outputs.hpp"

namespace hwio {
class PhidgetIo;
namespace fds {class Provider;}
struct HardwareOutputView {
    std::uint64_t id{};
    bool source_available{}, submitted{},testing{};
    double value{};
    std::string reason;
};
// Configuration and string formatting are compiled outside input processing.
// Normal routing uses received PSX observations. Explicit transient Boolean
// hardware tests never compose PSX commands. Acknowledgement remains separate.
class HardwareRouter {
    struct Route {
        HardwareBinding binding;
        const CatalogueObservation* observation{};
        bool digital{};
        std::string record, prefix, suffix;
        std::uint16_t slot{};
        std::uint64_t generation{}, epoch{}, record_sequence{};
        HardwareOutputIntent previous;
        HardwareOutputView view;
        bool initialized{},embed_value{};
        bool ready{};
        bool testable{};
        std::optional<bool> test;
        AtAuthority authority{AtAuthority::unavailable};
    };
    std::vector<Route> routes_;
    struct Release {std::uint16_t slot{};HardwareOutputIntent intent;double failure{};};
    std::vector<Release> releases_;
    std::uint64_t sequence_{};
    DigitalOutputs digital_;
public:
    void configure(const HardwareConfiguration&,const std::vector<DeviceInfo>&,const Catalogue&,const std::vector<EthernetHealth>& inventory = {});
    void update(const Mapper&,EthernetInput&,fds::Provider* fds=nullptr,PhidgetIo* phidgets=nullptr);
    bool set_test(const std::string&,bool);
    void clear_tests();
    std::vector<HardwareOutputView> snapshot() const;
    std::vector<DigitalOutputState> digital_snapshot() const {return digital_.snapshot();}
    const DigitalOutputs& evaluated_outputs() const noexcept {return digital_;}
};
bool observation_value(const CatalogueObservation&,std::string_view,double&,std::string_view&) noexcept;
} // namespace hwio
