#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>

namespace psx::xp::environment_time {
constexpr std::int64_t first_utc=-62135596800000LL,last_utc=253402300799999LL;
inline bool valid(std::int64_t utc) noexcept {return utc>=first_utc&&utc<=last_utc;}
// Fresh, bounded source interval only. Never convert an unbounded double to int64.
inline bool project(std::int64_t source,std::int64_t elapsed,std::uint64_t frequency,double rate,std::int64_t& result) noexcept {
    if(!valid(source)||!frequency||frequency>1000000000000ULL||elapsed<0||
       static_cast<std::uint64_t>(elapsed)>=frequency||!std::isfinite(rate)||rate<0||rate>64)return false;
    const auto milliseconds=1000.0*(static_cast<double>(elapsed)/static_cast<double>(frequency))*rate;
    const auto increment=static_cast<std::int64_t>(milliseconds); // Proven [0,64000).
    if(increment>last_utc-source)return false;
    result=source+increment;return valid(result);
}
struct Date {int ordinal{},year{};float seconds{};};
inline bool split(std::int64_t utc,Date& result) noexcept {
    if(!valid(utc))return false;
    using namespace std::chrono;const sys_time<milliseconds> time{milliseconds{utc}};const auto day=floor<days>(time);
    const year_month_day calendar{day};const auto start=sys_days{calendar.year()/January/1};
    result={static_cast<int>((day-start).count()),static_cast<int>(calendar.year()),static_cast<float>(duration<double>(time-day).count())};
    // Last milliseconds can round to 86400 in SDK float precision. Keep their
    // original UTC day with the closest representable in-day value (<8 ms error).
    if(result.seconds>=86400)result.seconds=std::nextafter(86400.0f,0.0f);
    return true;
}
inline bool join(int source_year,float ordinal,float seconds,std::int64_t& result) noexcept {
    if(source_year<1||source_year>9999||!std::isfinite(ordinal)||std::floor(ordinal)!=ordinal||
       !std::isfinite(seconds)||seconds<0||seconds>=86400)return false;
    const auto year=std::chrono::year{source_year};const auto length=year.is_leap()?366:365;
    if(ordinal<0||ordinal>=length)return false;
    const auto day=std::chrono::sys_days{year/std::chrono::January/1}+std::chrono::days{static_cast<int>(ordinal)};
    result=std::chrono::duration_cast<std::chrono::milliseconds>(day.time_since_epoch()).count()+
        static_cast<std::int64_t>(std::llround(static_cast<double>(seconds)*1000.0));
    return valid(result);
}
}
