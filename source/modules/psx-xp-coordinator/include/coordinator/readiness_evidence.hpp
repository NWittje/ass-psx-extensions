#pragma once
#include "coordinator/readiness.hpp"
namespace coordinator {
struct ReadinessEvidenceStatus {bool active{},complete{},incomplete{};std::uint64_t records{},bytes{},overflows{};std::uint32_t error{};};
class ReadinessEvidence {
public:
 ReadinessEvidence();~ReadinessEvidence();bool prepare(const std::wstring&,std::uint64_t);
 void begin(std::int64_t,const ReadinessModel::Checkpoint&);
 void record(const ReadinessEvent&,std::int64_t,ReadinessAdmission=ReadinessAdmission::accepted);void assertion(const ReadinessView&,std::int64_t);
 void finish(std::int64_t) noexcept;void incomplete(std::uint32_t,std::int64_t) noexcept;void join();
 ReadinessEvidenceStatus status() const;
 bool done() const noexcept;
private:friend struct EvidenceTestAccess;bool test_open_hook(void (*)(void*) noexcept,void*);struct Impl;std::unique_ptr<Impl> impl_;
};
struct ReadinessReplayResult {bool valid{},complete{},missing{};std::uint64_t qpc_frequency{},begin_qpc{},cutoff_qpc{},records{},assertions{};ReadinessView view{};std::string error;};
// Missing historical sidecars explicitly report Unknown. This inspector has no IPC.
ReadinessReplayResult inspect_readiness_capture(const std::wstring&);
}
