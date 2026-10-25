#pragma once
#include "aircraft_sync_types.hpp"
#include <memory>
#include <string>
namespace psx::xp {
std::wstring aircraft_sync_endpoint();
class AircraftService final {
public:
    AircraftService();~AircraftService();
    bool start(motion::InstanceId,const std::wstring& endpoint={});
    void stop() noexcept;
    bool connected() const noexcept;
    std::uint64_t connection() const noexcept;
    bool command(AircraftCommand&) noexcept;
    bool sample(unsigned,AircraftSample&) noexcept;
    void ack(const aircraft::Ack&,std::uint64_t connection) noexcept;
    void publish(const std::array<aircraft::Applied,6>&) noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
