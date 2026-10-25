#include "coordinator/environment_time.hpp"
#include <algorithm>
#include <cmath>
namespace coordinator {
namespace w=environment_wire;
namespace {
bool utc_range(std::int64_t utc) noexcept{return utc>=-62135596800000LL&&utc<=253402300799999LL;}
bool age(std::int64_t source,std::int64_t now,std::int64_t& elapsed) noexcept {
 if(source<=0||now<source)return false;elapsed=now-source;return true;
}
}
EnvironmentUtcCalendar environment_utc_calendar(std::int64_t utc) noexcept {
 EnvironmentUtcCalendar c;if(!utc_range(utc))return c;
 // Nonnegative days since 0001-01-01: 400/100/4/1-year Gregorian decomposition.
 // Bounds precede the epoch shift; maximum shifted milliseconds fit signed64.
 const auto shifted=static_cast<std::uint64_t>(utc+62135596800000LL);
 auto days=shifted/86400000;const auto years400=days/146097;days%=146097;
 const auto years100=(std::min)(days/36524,std::uint64_t{3});days-=years100*36524;
 const auto years4=days/1461;days%=1461;
 const auto years1=(std::min)(days/365,std::uint64_t{3});days-=years1*365;
 c.year=static_cast<unsigned>(years400*400+years100*100+years4*4+years1+1);c.day_index=static_cast<unsigned>(days);
 const bool leap=c.year%4==0&&(c.year%100!=0||c.year%400==0);
 constexpr unsigned lengths[]{31,28,31,30,31,30,31,31,30,31,30,31};
 c.month=1;for(unsigned i=0;i<11;++i){const auto n=lengths[i]+(i==1&&leap?1u:0u);if(days<n)break;days-=n;++c.month;}c.day=static_cast<unsigned>(days+1);
 auto milliseconds=shifted%86400000;c.hour=static_cast<unsigned>(milliseconds/3600000);milliseconds%=3600000;
 c.minute=static_cast<unsigned>(milliseconds/60000);milliseconds%=60000;c.second=static_cast<unsigned>(milliseconds/1000);c.millisecond=static_cast<unsigned>(milliseconds%1000);c.valid=true;return c;
}
bool environment_time_report_qualified(const w::Applied& a) noexcept {
 if(!(a.groups&1u)||a.phase!=w::Phase::owned||(a.reason!=w::Reason::none&&a.reason!=w::Reason::stale)||!(a.source.flags&w::utc_valid)||
  !(a.flags&w::calendar_year_approximate)||a.applied_qpc<=0||a.readback_qpc<a.applied_qpc||
  !utc_range(a.source.utc_millis)||!utc_range(a.accepted_utc_millis)||!utc_range(a.actual_utc_millis))return false;
 // Clock-rate bit0 is not a UTC-readback validity flag. No actual-rate observation
 // is inferred from SDK time/day getters. UTC-only application is legitimate.
 if((a.accepted_mask&1u)&&(!(a.source.valid_mask&1u)||!std::isfinite(a.accepted[0])||a.accepted[0]<0||a.accepted[0]>64||
  a.accepted[0]!=((a.reason==w::Reason::stale||(a.source.flags&w::paused))?0:a.source.values[0])))return false;
 const auto accepted_day=(a.accepted_utc_millis+62135596800000LL)/86400000;
 const auto actual_day=(a.actual_utc_millis+62135596800000LL)/86400000;
 return accepted_day==actual_day&&std::abs(a.actual_utc_millis-a.accepted_utc_millis)<=1000;
}
EnvironmentTimeStatus environment_time_status(const EnvironmentView& v,std::int64_t now,std::uint64_t frequency) noexcept {
 EnvironmentTimeStatus t;const auto& s=v.source;const auto& g=v.groups[0];const auto& a=g.applied;
 t.source_age_available=s.sequence&&age(s.sample_end_qpc,now,t.source_age_qpc);
 t.utc_qualified=s.sequence&&(s.flags&w::utc_valid)&&utc_range(s.utc_millis);
 t.rate_qualified=t.utc_qualified&&(s.valid_mask&1u)&&std::isfinite(s.values[0])&&s.values[0]>=0&&s.values[0]<=64&&(!(s.flags&w::paused)||s.values[0]==0);
 t.paused=(s.flags&w::paused)!=0;t.motion_frozen=(s.flags&w::motion_frozen)!=0;
 t.source_current=v.exporter_connected&&v.source_available&&frequency&&t.source_age_available&&static_cast<std::uint64_t>(t.source_age_qpc)<frequency;
 t.report_retained=g.applied_retained||g.applied_available;
 t.readback_age_available=t.report_retained&&age(a.readback_qpc,now,t.readback_age_qpc);
 std::int64_t source_age{};
 t.report_current=t.source_current&&g.applied_available&&v.plugin_connected&&v.following&&g.requested&&now<g.expires_qpc&&
  a.provider==v.plugin.instance&&a.owner==v.owner&&a.source.producer==s.producer&&a.source.lifecycle==s.lifecycle&&
  age(a.source.sample_end_qpc,now,source_age)&&static_cast<std::uint64_t>(source_age)<frequency&&a.applied_qpc<=now;
 t.readback_current=t.report_current&&g.owned&&a.reason==w::Reason::none&&environment_time_report_qualified(a)&&t.readback_age_available&&static_cast<std::uint64_t>(t.readback_age_qpc)<frequency;
 if(t.readback_current)t.readback_error_millis=a.actual_utc_millis-a.accepted_utc_millis;
 t.phase=g.phase;t.reason=g.reason;
 if(t.report_current&&!t.readback_current&&a.phase==w::Phase::owned)t.reason=w::Reason::binding;
 return t;
}
const char* environment_phase_name(w::Phase v) noexcept {constexpr const char* names[]{"Unavailable","Observing","Owned","Released"};const auto n=static_cast<unsigned>(v);return n<std::size(names)?names[n]:"Unknown";}
const char* environment_reason_name(w::Reason v) noexcept {constexpr const char* names[]{"None","Unsupported","Identity","Lifecycle","Configuration","Stale","Owner","Expired","Stopped","Disconnected","Overflow","Binding/readback","Loading"};const auto n=static_cast<unsigned>(v);return n<std::size(names)?names[n]:"Unknown";}
}
