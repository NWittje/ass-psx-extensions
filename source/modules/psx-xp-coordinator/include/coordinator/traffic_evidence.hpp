#pragma once
#include "coordinator/traffic_visual.hpp"
namespace coordinator {
struct TrafficEvidenceStatus {bool active{},complete{},incomplete{};std::uint64_t records{},bytes{},overflows{};std::uint32_t error{};};
struct TrafficEvidenceLimits {std::size_t queue_records{256};std::uint64_t file_bytes{512ull*1024*1024};std::uint32_t writer_delay_ms{};}; // Native fixture seam, not product settings.
class TrafficEvidence {
public:
 explicit TrafficEvidence(TrafficEvidenceLimits={});~TrafficEvidence();
 bool prepare(const std::wstring&,std::uint64_t,bool visual=false);
 void begin(std::int64_t,const TrafficModel::Checkpoint&);
 void begin(std::int64_t,const TrafficModel::Checkpoint&,const TrafficVisualModel::State&);
 void record(const TrafficEvent&,std::int64_t,TrafficAdmission);
 void assertion(const TrafficView&,std::int64_t);
 void visual_record(const TrafficVisualEvent&,std::int64_t,TrafficVisualAdmission);
 void visual_dispatch(const visual_wire::Frame&,std::int64_t);
 void visual_assertion(const TrafficVisualView&,std::int64_t);
 void finish(std::int64_t) noexcept;void incomplete(std::uint32_t,std::int64_t) noexcept;void join();
 TrafficEvidenceStatus status() const;
 bool done() const noexcept;
private:friend struct EvidenceTestAccess;bool test_open_hook(void (*)(void*) noexcept,void*);struct Impl;std::unique_ptr<Impl> impl_;
};
struct TrafficReplayResult {
 bool valid{},complete{},missing{};
 std::uint64_t qpc_frequency{},begin_qpc{},cutoff_qpc{},records{},assertions{};
 TrafficView view{};std::string error;
 bool visual_retained{};std::uint64_t visual_assertions{};TrafficVisualView visual{};
};
TrafficReplayResult inspect_traffic_capture(const std::wstring&); // Read-only; no IPC/simulator operation.
}
