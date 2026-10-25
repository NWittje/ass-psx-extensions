#pragma once
#include "coordinator/diagnostics.hpp"
#include <atomic>
#include <thread>
#include <mutex>
namespace coordinator {
struct ExportRequest {
    std::wstring directory;
    ObservationSnapshot observation;
    HealthReport health;
    std::vector<std::wstring> selected_files;
};
struct ExportStatus {bool running{},complete{},partial{};std::uint32_t error{};std::uint64_t files{};std::wstring directory;};
class SupportExporter {
public:
    explicit SupportExporter(psx::diagnostics::DiagnosticsHub& hub):hub_(hub){}
    ~SupportExporter();
    bool start(ExportRequest);
    void cancel() noexcept;
    bool done() const noexcept{return done_;}
    void join();
    ExportStatus status() const;
private:
    void run(const ExportRequest&);
    psx::diagnostics::DiagnosticsHub& hub_;
    mutable std::mutex mutex_;
    ExportStatus status_;
    std::thread worker_;
    std::atomic<bool> done_{true},cancelled_{};
};
std::string support_snapshot(const ObservationSnapshot&,const HealthReport&,const psx::diagnostics::JournalStatus&);
}
