#pragma once

#include <XPLMDataAccess.h>
#include <XPLMPlugin.h>
#include <atomic>
#include <cstdint>

namespace psx::xp {
enum class Status {
    ok, wrong_thread, inactive, stale_binding, not_found, unavailable,
    type_mismatch, write_not_authorized, read_only, invalid_argument,
    out_of_range, non_finite, short_read
};
enum class Access { read_only, read_write };

// Lifetime/thread gate and checked SDK adapter. No worker thread may use XPLM.
// The only cross-thread field is the owner ID; reject before touching other state.
class SdkApi final {
public:
    void attach_thread() noexcept;
    bool on_thread() const noexcept;
    Status ready() const noexcept;
    void activate() noexcept;
    void deactivate() noexcept;
    void invalidate() noexcept;
    std::uint64_t generation() const noexcept { return generation_; } // SDK thread only
    Status inspect(XPLMDataRef ref, XPLMDataTypeID type, bool writing) const;
    SdkApi() = default;
    SdkApi(const SdkApi&) = delete;
    SdkApi& operator=(const SdkApi&) = delete;
private:
    std::atomic<unsigned long> owner_{0};
    bool active_{false};
    std::uint64_t generation_{1};
};
} // namespace psx::xp
