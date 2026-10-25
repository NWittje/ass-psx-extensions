#pragma once
#include <array>
#include <cstdint>
namespace psx::sync {
struct ClockExchange {std::uint64_t t1{},t2{},t3{},t4{},local_frequency{},remote_frequency{};};
struct Projection {bool valid{};std::int64_t earliest{},value{},latest{};std::uint64_t generation{};};
// Affine conversion only; this class never changes either system clock.
class ClockMap {
 struct Sample {ClockExchange exchange;long double offset{},radius{},rtt{};};
 std::array<Sample,16> samples_{};unsigned next_{},count_{};
 std::uint64_t generation_{1},last_{},local_frequency_{},remote_frequency_{};
 Sample selected_{};
public:
 bool accept(const ClockExchange&) noexcept;
 Projection to_local(std::int64_t remote,std::int64_t local_now)const noexcept;
 Projection to_remote(std::int64_t local,std::int64_t local_now)const noexcept;
 bool current(std::int64_t local_now)const noexcept;
 void reset() noexcept;
 std::uint64_t generation()const noexcept{return generation_;}
 std::uint64_t remote_frequency()const noexcept{return remote_frequency_;}
 double uncertainty_ms(std::int64_t local_now)const noexcept;
 unsigned checkpoint(std::array<ClockExchange,17>&)const noexcept;
};
bool fresh_before(std::int64_t now,const Projection&,std::uint64_t frequency,std::uint32_t limit_ms)noexcept;
}
