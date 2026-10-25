#pragma once
#include <psx/cdu/snapshot.hpp>

namespace cdu_client
{
// Worker/UI-owned one-second windows; no allocations or cross-thread clocks.
class EventRate
{
    uint64_t start_{}, count_{};
    double value_{};

  public:
    void reset(uint64_t now) noexcept
    {
        start_ = now;
        count_ = 0;
        value_ = 0;
    }
    void add() noexcept
    {
        ++count_;
    }
    double sample(uint64_t now) noexcept
    {
        if (now < start_)
            reset(now);
        if (now - start_ >= 1000000000ull)
        {
            value_ = double(count_) * 1e9 / double(now - start_);
            start_ = now;
            count_ = 0;
        }
        return value_;
    }
};
class SelectedRevisionRate
{
    EventRate rate_;
    psx::cdu::Unit unit_{};
    psx::cdu::SessionId session_{};
    uint64_t generation_{}, revision_{}, count_{};
    bool known_{}, selected_known_{};

  public:
    void reset(psx::cdu::Unit unit, uint64_t now) noexcept
    {
        unit_ = unit;
        known_ = false;
        selected_known_ = false;
        count_ = 0;
        rate_.reset(now);
    }
    void observe(const psx::cdu::CduSnapshot &screen, uint64_t now) noexcept
    {
        const bool identity = !known_ || screen.session != session_ || screen.generation != generation_;
        if (identity)
        {
            rate_.reset(now);
            selected_known_ = false;
        }
        session_ = screen.session;
        generation_ = screen.generation;
        known_ = true;
        if (screen.unit != unit_ || (selected_known_ && screen.cache_revision <= revision_))
            return;
        revision_ = screen.cache_revision;
        selected_known_ = true;
        ++count_;
        rate_.add();
    }
    double sample(uint64_t now) noexcept
    {
        return rate_.sample(now);
    }
    uint64_t count() const noexcept
    {
        return count_;
    }
};
// A submission alone is not a draw. Require both source draw and complete render.
class ContentDrawRate
{
    EventRate rate_;
    uint64_t revision_{}, source_{}, frame_{}, device_{}, count_{};

  public:
    void reset(uint64_t now) noexcept
    {
        revision_ = count_ = 0;
        rate_.reset(now);
    }
    void submitted(uint64_t revision, uint64_t source, uint64_t frame, uint64_t device) noexcept
    {
        revision_ = revision;
        source_ = source;
        frame_ = frame;
        device_ = device;
    }
    void clear_pending() noexcept
    {
        revision_ = 0;
    }
    void rendered(uint64_t source, uint64_t frame, uint64_t device, bool available) noexcept
    {
        const bool recreated = device != device_;
        if (revision_ && available && source > (recreated ? 0 : source_) && frame > (recreated ? 0 : frame_))
        {
            ++count_;
            rate_.add();
            revision_ = 0;
        }
    }
    bool pending() const noexcept
    {
        return revision_ != 0;
    }
    double sample(uint64_t now) noexcept
    {
        return rate_.sample(now);
    }
    uint64_t count() const noexcept
    {
        return count_;
    }
};
} // namespace cdu_client
