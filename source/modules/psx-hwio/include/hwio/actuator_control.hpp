#pragma once
#include <psx/hwio/actuators.hpp>
#include <array>
namespace hwio {
class Runtime;
class ActuatorControl {
    Runtime& runtime_;std::uint64_t session_,highwater_{};unsigned cursor_{};
    // Deduplication needs a mutation receipt, not thirty-two 16 KiB status snapshots.
    struct Receipt {
        std::uint64_t request_id{},session{},fault_revision{},revision{},operation_id{};
        psx::hwio::actuators::Kind kind{};std::uint32_t motor{};
        psx::hwio::actuators::Operation operation{};psx::hwio::actuators::Result result{};std::array<char,128> message{};
        Receipt()=default;
        Receipt(const psx::hwio::actuators::Packet& p):request_id(p.request_id),session(p.session),fault_revision(p.fault_revision),revision(p.revision),operation_id(p.operation_id),kind(p.kind),motor(p.motor),operation(p.operation),result(p.result),message(p.message){}
    };
    static_assert(sizeof(Receipt)<=192);
    std::array<Receipt,32> replies_{};
    std::array<std::uint64_t,60> operations_{};std::array<bool,60> resetting_{};
public:
    ActuatorControl(Runtime&,std::uint64_t);
    psx::hwio::actuators::Packet status();
    psx::hwio::actuators::Packet dispatch(const psx::hwio::actuators::Packet&);
};
}
