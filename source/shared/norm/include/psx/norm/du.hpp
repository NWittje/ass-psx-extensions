#pragma once
#include <psx/norm/norm.hpp>
#include <psx/norm/diagnostics.hpp>
#include <vector>
namespace psx::norm {
using OwnedData=std::shared_ptr<const std::vector<uint8_t>>;
inline constexpr size_t du_max_data=8*1024*1024+4096,du_receive_budget=24*1024*1024;
struct DuPacket:Packet { unsigned stream{};uint64_t completion_qpc{}; };
using DuReceive=void(*)(void*,const DuPacket&);
// Separate payload allocation profile; one native engine services up to six sessions.
class DuEngine {
 struct Impl; std::unique_ptr<Impl> impl_;
public:
 DuEngine();~DuEngine();DuEngine(const DuEngine&)=delete;
 void start(std::span<const Options>,bool sender);void stop()noexcept;
 bool send(unsigned,OwnedData);
 bool send(unsigned,OwnedData,const Correlation&);
 // Callback runs synchronously on the engine owner; pointers must outlive stop.
 void observe(DiagnosticSink,void*,NativeMonitor* = nullptr) noexcept;
 unsigned pump(DuReceive=nullptr,void* user=nullptr,unsigned budget=64);
 HANDLE event()const noexcept;const Stats& stats()const noexcept;
 double tx_rate(unsigned)const;
 double grtt(unsigned)const;
 // Explicit qualification only: never enabled by ordinary applications.
 void debug_log(const char* path);
 // Controlled authored-fixture experiment; ordinary products never call this.
 void qualification_fixed_rate(double bits_per_second);
 void qualification_flow_control(double factor);
 static size_t receive_bytes()noexcept;static size_t receive_peak()noexcept;
 static uint64_t allocation_denied()noexcept;
 // Joint client payload ledger: 24 MiB raw slots + 9 MiB worst-case private
 // ASS1.3.0 scene/cache/replacement leave 31 MiB for RX and native frame models.
 static bool reserve_cpu_payload(size_t)noexcept;
 static void release_cpu_payload(size_t)noexcept;
 static size_t cpu_payload_bytes()noexcept;
};
}
