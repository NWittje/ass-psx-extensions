#pragma once
#include <psx/status/protocol.hpp>
#include <memory>
#include <string>

namespace psx::xp {
std::wstring status_endpoint();
struct StatusTransportReport {
    bool connected{};
    std::uint64_t connection_id{}, sent{}, transitions_dropped{}, rejected{}, reconnects{};
};
// SDK owner publishes fixed facts; this independent client worker has no SDK dependency.
class StatusService final {
public:
    StatusService(); ~StatusService();
    StatusService(const StatusService&)=delete;
    StatusService& operator=(const StatusService&)=delete;
    bool start(motion::InstanceId,const std::wstring& endpoint={}) noexcept;
    void stop() noexcept; // Lifecycle only; never a frame operation.
    void publish(const status::Snapshot&,bool transition) noexcept;
    bool poll(StatusTransportReport&) noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
