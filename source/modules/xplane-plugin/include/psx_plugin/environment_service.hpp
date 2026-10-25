#pragma once
#include "environment_types.hpp"
#include <psx/diagnostics/journal.hpp>
#include <memory>
#include <string>
namespace psx::xp {
std::wstring environment_endpoint();
class EnvironmentService final {
public:
    EnvironmentService();~EnvironmentService();
    bool start(motion::InstanceId,const std::wstring& endpoint={},diagnostics::DiagnosticsHub* journal=nullptr);
    void stop() noexcept;
    bool connected() const noexcept;
    std::uint64_t connection() const noexcept;
    std::uint64_t capture_generation() const noexcept;
    bool command(EnvironmentCommand&) noexcept;
    bool sample(EnvironmentSample&) noexcept;
    void ack(const env::Ack&,std::uint64_t connection) noexcept;
    void publish(const std::array<env::Applied,2>&,const std::array<envp::VisibilityPolicy,2>&,const envp::AppliedPresentation& weather={}) noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
