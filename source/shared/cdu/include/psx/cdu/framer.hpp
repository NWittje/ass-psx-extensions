#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace psx::cdu
{
// Bounded framing of the whole Main feed (Qs122 can be much longer than CDU).
class Framer
{
    std::string pending_;
    bool dropping_{};

  public:
    std::uint64_t oversized{};
    template <class F> void feed(std::string_view bytes, F &&record)
    {
        for (char c : bytes)
        {
            if (c == '\n')
            {
                if (!dropping_)
                {
                    if (!pending_.empty() && pending_.back() == '\r')
                        pending_.pop_back();
                    record(std::string_view(pending_));
                }
                pending_.clear();
                dropping_ = false;
            }
            else if (!dropping_)
            {
                if (pending_.size() == 65536)
                {
                    ++oversized;
                    pending_.clear();
                    dropping_ = true;
                }
                else
                    pending_ += c;
            }
        }
    }
    void reset()
    {
        pending_.clear();
        dropping_ = false;
    }
};
} // namespace psx::cdu
