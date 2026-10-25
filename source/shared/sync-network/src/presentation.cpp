#include <psx/sync/presentation.hpp>
#include <algorithm>
#include <cmath>
namespace psx::sync {
bool ValueHistory::append(const Values& s)noexcept {
 if(!s.sequence||s.qpc<0||!s.valid)return false;
 for(unsigned i=0;i<32;++i)if((s.valid&(1u<<i))&&!std::isfinite(s.value[i]))return false;
 if(count_){const auto& last=values_[count_-1];
  if(s.producer!=last.producer||s.producer_low!=last.producer_low||s.lifecycle!=last.lifecycle||s.configuration!=last.configuration)count_=0;
  else if(s.sequence<=last.sequence||s.qpc<=last.qpc)return false;
 }
 if(count_==values_.size()){for(unsigned i=1;i<count_;++i)values_[i-1]=values_[i];--count_;}
 values_[count_++]=s;return true;
}
bool ValueHistory::evaluate(std::int64_t t,Presented& p)const noexcept {
 if(!count_||t<0)return false;unsigned left=0,right=0;
 for(unsigned i=0;i<count_;++i){if(values_[i].qpc<=t)left=i;if(values_[i].qpc>=t){right=i;break;}right=i;}
 const auto& a=values_[left];const auto& b=values_[right];p={};p.sample=a;p.left=a.sequence;p.right=b.sequence;
 if(right>left&&t>=a.qpc&&t<=b.qpc){p.alpha=double(t-a.qpc)/double(b.qpc-a.qpc);p.interpolated=true;
  const auto mask=a.valid&b.valid&a.continuous&b.continuous;
  p.sample.valid=(p.sample.valid&~p.sample.continuous)|mask;
  for(unsigned i=0;i<32;++i)if(mask&(1u<<i))p.sample.value[i]=static_cast<float>(a.value[i]+p.alpha*(b.value[i]-a.value[i]));
 }
 // Discrete values are not delayed behind continuous animation.
 const auto& latest=values_[count_-1];for(unsigned i=0;i<32;++i)if(!(latest.continuous&(1u<<i))){
  if(latest.valid&(1u<<i)){p.sample.value[i]=latest.value[i];p.sample.valid|=1u<<i;}else p.sample.valid&=~(1u<<i);
 }
 return true;
}
}
