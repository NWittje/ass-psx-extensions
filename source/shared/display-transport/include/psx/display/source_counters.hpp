#pragma once
#include <array>
#include <cstdint>
namespace psx::du::transport {
// DPF counter packet order. Lifetime totals include prior leases; queue depth is
// a gauge. Counter observation never renews the age/validity of display pixels.
struct SourceCounters {
    std::array<uint64_t,17> values{};
    uint64_t generation{},revision{},changed_qpc{};
    bool valid{};
    bool observe(const std::array<uint64_t,17>& next,uint64_t lease,uint64_t now) noexcept {
        if(valid && generation==lease && values==next)return false;
        values=next;generation=lease;changed_qpc=now;valid=true;++revision;return true;
    }
};
inline constexpr std::array<const char*,17> source_counter_names{
    "accepted","dropped","invalid_candidates","buffer_retries","observer_failures",
    "worker_transport_lost","paints","queue_depth","native_queue_retries",
    "native_transport_lost","gc_collections","gc_ms","source_allocated_bytes",
    "worker_allocated_bytes","abandoned_candidates","ordinary_primitives","recorded_primitives"};
}
