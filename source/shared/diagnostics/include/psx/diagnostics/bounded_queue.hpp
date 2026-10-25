#pragma once
#include "types.hpp"
#include <atomic>

namespace psx::diagnostics::detail {
// Cache-line separation is intentional: exactly one producer and one consumer.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324)
#endif
struct Queue {
    std::array<Event,queue_capacity> records{};
    alignas(64) std::atomic<std::uint64_t> head{},tail{},attempted{},lost{},lost_errors{};
    bool push(const Event& e) noexcept {
        const auto h=head.load(std::memory_order_relaxed),t=tail.load(std::memory_order_acquire);
        const auto limit=e.severity>=Severity::warning?queue_capacity:queue_capacity-error_reserve;
        if(h-t>=limit)return false;
        records[h%queue_capacity]=e;head.store(h+1,std::memory_order_release);return true;
    }
    bool pop(Event& e) noexcept {
        const auto t=tail.load(std::memory_order_relaxed),h=head.load(std::memory_order_acquire);
        if(t==h)return false;
        e=records[t%queue_capacity];tail.store(t+1,std::memory_order_release);return true;
    }
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"SDK diagnostics require lock-free counters");
}
