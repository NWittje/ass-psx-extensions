#include <psx/sync/clock.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace psx::sync {
namespace {
bool tick(long double v,std::int64_t& n)noexcept {if(!std::isfinite(v)||v<0||v>static_cast<long double>((std::numeric_limits<std::int64_t>::max)())-1)return false;n=static_cast<std::int64_t>(std::llround(v));return true;}
}
void ClockMap::reset() noexcept {count_=next_=0;last_=local_frequency_=remote_frequency_=0;selected_={};++generation_;}
bool ClockMap::accept(const ClockExchange& x) noexcept {
 if(!x.local_frequency||!x.remote_frequency||x.t4<x.t1||x.t3<x.t2||x.t3>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())||x.t4>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))return false;
 const long double lf=x.local_frequency,rf=x.remote_frequency;
 const long double lower=x.t3/rf-x.t4/lf,upper=x.t2/rf-x.t1/lf;
 if(upper<lower)return false;
 if(count_&&(x.t4<=last_||x.local_frequency!=local_frequency_||x.remote_frequency!=remote_frequency_)){
  if(x.t4<=last_)return false;reset();
 }
 Sample s{x,(lower+upper)/2,(upper-lower)/2,upper-lower};
 if(count_&&std::abs(s.offset-selected_.offset)>selected_.radius+s.radius+.001L)reset();
 local_frequency_=x.local_frequency;remote_frequency_=x.remote_frequency;last_=x.t4;
 samples_[next_++%samples_.size()]=s;count_=(std::min)(unsigned(samples_.size()),count_+1);
 // Pin the affine epoch for this timing generation. Later exchanges refine its
 // uncertainty, not the already presented timeline. Discontinuities retire it.
 if(count_==1)selected_=s;
 return true;
}
unsigned ClockMap::checkpoint(std::array<ClockExchange,17>& out)const noexcept {
 if(!count_)return 0;out[0]=selected_.exchange;unsigned n=1;
 for(unsigned i=0;i<count_;++i){const auto& x=samples_[(next_-count_+i)%samples_.size()].exchange;if(x.t4>out[n-1].t4)out[n++]=x;}
 return n;
}
bool ClockMap::current(std::int64_t n)const noexcept{return count_&&n>=0&&std::uint64_t(n)>=last_&&std::uint64_t(n)-last_<local_frequency_;}
double ClockMap::uncertainty_ms(std::int64_t n)const noexcept {
 if(!current(n))return (std::numeric_limits<double>::infinity)();
 // The offset's observed change is included rather than assuming perfect clocks.
 // The first exchange pins the presentation center, not a permanent latency
 // penalty. Once it leaves the recent window, only current bounds and their
 // distance from that center constrain freshness.
 long double radius=0;
 for(unsigned i=0;i<count_;++i)radius=(std::max)(radius,samples_[i].radius+std::abs(samples_[i].offset-selected_.offset));
 return static_cast<double>(radius*1000);
}
Projection ClockMap::to_local(std::int64_t remote,std::int64_t now)const noexcept {
 Projection p;p.generation=generation_;if(remote<0||!current(now))return p;
 const long double center=(static_cast<long double>(remote)/remote_frequency_-selected_.offset)*local_frequency_;
 const long double error=static_cast<long double>(uncertainty_ms(now))*.001L*local_frequency_;
 p.valid=tick(center,p.value)&&tick(std::floor(center-error),p.earliest)&&tick(std::ceil(center+error),p.latest);return p;
}
Projection ClockMap::to_remote(std::int64_t local,std::int64_t now)const noexcept {
 Projection p;p.generation=generation_;if(local<0||!current(now))return p;
 const long double center=(static_cast<long double>(local)/local_frequency_+selected_.offset)*remote_frequency_;
 const long double error=static_cast<long double>(uncertainty_ms(now))*.001L*remote_frequency_;
 p.valid=tick(center,p.value)&&tick(std::floor(center-error),p.earliest)&&tick(std::ceil(center+error),p.latest);return p;
}
bool fresh_before(std::int64_t now,const Projection& p,std::uint64_t f,std::uint32_t ms)noexcept {
 return p.valid&&f&&now>=p.latest&&p.earliest>=0&&static_cast<long double>(now-p.earliest)*1000<static_cast<long double>(f)*ms;
}
}
