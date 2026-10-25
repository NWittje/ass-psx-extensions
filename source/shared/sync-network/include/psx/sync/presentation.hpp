#pragma once
#include <array>
#include <cstdint>
namespace psx::sync {
constexpr std::uint32_t audio_continuous_mask(unsigned group)noexcept {
 switch(group){case 0:{std::uint32_t mask{};for(unsigned engine=0;engine<4;++engine)mask|=7u<<(engine*6);return mask;}
 case 1:return 31;case 2:return 1023;case 3:case 7:return 3;case 4:return 56;case 5:return 15;default:return 0;}
}
struct Values {
 std::uint64_t producer{},producer_low{},lifecycle{},configuration{},sequence{};
 std::int64_t qpc{};std::uint32_t valid{},continuous{};
 std::array<float,32> value{};
};
struct Presented {Values sample;std::uint64_t left{},right{};double alpha{};bool interpolated{};};
// Four real observations per group. Callers retain source objects unchanged.
class ValueHistory {
 std::array<Values,4> values_{};unsigned count_{};
public:
 bool append(const Values&) noexcept;
 bool evaluate(std::int64_t target,Presented&) const noexcept;
 void clear() noexcept{count_=0;}
};
}
