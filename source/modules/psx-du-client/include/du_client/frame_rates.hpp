#pragma once
#include <cstdint>
namespace du_client
{
// UI-owned counters measured against elapsed QPC time, never timer ticks.
class FrameRates
{
    uint64_t started_{}, received_{}, drawn_{};
    bool initialized_{};
  public:
    double received{}, drawn{};
    void reset(uint64_t now, uint64_t received_count, uint64_t drawn_count) noexcept
    {
        started_ = now;
        received_ = received_count;
        drawn_ = drawn_count;
        initialized_ = true;
        received = drawn = 0;
    }
    bool sample(uint64_t now, uint64_t ticks_per_second, uint64_t received_count,
                uint64_t drawn_count) noexcept
    {
        if (!initialized_ || now < started_ || received_count < received_ || drawn_count < drawn_)
        {
            reset(now, received_count, drawn_count);
            return true;
        }
        if (!ticks_per_second || now - started_ < ticks_per_second)
            return false;
        const double seconds = double(now - started_) / double(ticks_per_second);
        received = double(received_count - received_) / seconds;
        drawn = double(drawn_count - drawn_) / seconds;
        started_ = now;
        received_ = received_count;
        drawn_ = drawn_count;
        return true;
    }
};
} // namespace du_client
