#pragma once
#include "coordinator/transport.hpp"
#include "coordinator/observation.hpp"
#include "coordinator/support_export.hpp"
#include "coordinator/crash_policy.hpp"
namespace coordinator {
class Runtime {
public:
    Runtime();
    explicit Runtime(const psx::diagnostics::Options&);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    StatusStore status;
    EventLog log;
    psx::diagnostics::DiagnosticsHub diagnostics;
    SupportExporter exporter;
    HealthEvaluator health;
    TcpClient tcp;
    PipeServer pipe;
    ObservationController observation;
    void request_stop() noexcept;
    bool stopped() const noexcept;
    void join();
    void marker(std::string_view);
    bool start_non_normal();
};
}
