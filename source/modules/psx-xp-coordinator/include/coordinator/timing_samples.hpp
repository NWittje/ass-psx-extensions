#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace coordinator {
// Latest 512 observations, in QPC ticks. No allocation or formatting on ingress.
class TimingSamples {
public:
    void add(std::int64_t value) noexcept {if(value<0)return;values_[next_++%values_.size()]=static_cast<std::uint64_t>(value);count_=(std::min)(count_+1,values_.size());}
    std::array<std::uint64_t,6> summary(std::uint64_t frequency) const {
        auto sorted=values_;std::sort(sorted.begin(),sorted.begin()+count_);
        if(!count_)return {0,0,0,0,0,frequency};
        return {count_,sorted[(count_*50+99)/100-1],sorted[(count_*95+99)/100-1],sorted[(count_*99+99)/100-1],sorted[count_-1],frequency};
    }
private:
    std::array<std::uint64_t,512> values_{};std::size_t count_{},next_{};
};
}
