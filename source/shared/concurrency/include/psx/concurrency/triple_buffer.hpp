#pragma once
#include <array>
#include <atomic>
#include <type_traits>

namespace psx::concurrency {
// One producer and one consumer, each with an exclusively owned sample slot.
// Publishing may replace unread state; use an ordered queue for switch edges,
// commands or acknowledgements. Reset/destruction requires both owners stopped.
// The exchange transfers payload ownership as well as the slot index. No heap,
// lock, retry loop or shared payload read belongs on a simulator callback.
template<class T> class TripleBuffer final {
public:
    T& writing() noexcept { return slots_[back_]; }
    bool publish() noexcept {
        const auto previous = middle_.exchange(back_ | dirty, std::memory_order_acq_rel);
        back_ = previous & index_mask;
        return (previous & dirty) != 0;
    }
    bool consume() noexcept {
        if (!(middle_.load(std::memory_order_acquire) & dirty)) return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & index_mask;
        return true;
    }
    const T& reading() const noexcept { return slots_[front_]; }
    void put(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        writing() = value;
        publish();
    }
    bool take(T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        if (!consume()) return false;
        value = reading();
        return true;
    }
private:
    static constexpr unsigned dirty = 4, index_mask = 3;
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::array<T,3> slots_{};
    unsigned back_{2}, front_{};
    std::atomic<unsigned> middle_{1};
};
} // namespace psx::concurrency
