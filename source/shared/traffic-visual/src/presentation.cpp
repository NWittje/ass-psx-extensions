#include <psx/traffic_visual/presentation.hpp>
#include <algorithm>
namespace psx::traffic_visual {
namespace {
bool domain(const Window& w,const traffic_source::Sample& s) noexcept {const auto& h=s.snapshot.header;return w.producer==s.producer&&w.connection==s.connection_id&&w.selection==s.selection_epoch&&w.lifecycle==h.lifecycle&&w.frequency==h.frequency&&w.process==h.process&&w.birth==h.process_created;}
bool make_pose(const traffic::Header& h,const traffic::Slot& s,std::int64_t received,PoseSample& p) noexcept {
 if((s.valid&63u)!=63u||std::abs(s.values[0])>89*motion::pi/180)return false;
 p.sequence=h.sequence;p.frame=h.frame;p.generation=s.generation;p.observed=static_cast<std::int64_t>(h.observed);p.finished=static_cast<std::int64_t>(h.finished);p.received=received;
 p.latitude_deg=s.values[0]*180/motion::pi;p.longitude_deg=std::remainder(s.values[1]*180/motion::pi,360.0);p.altitude_msl_m=s.values[2]*0.3048;
 // Traffic bank is already positive-right: do not use ownship's sign adapter.
 p.attitude=motion::quaternion(motion::compose(motion::earth_tangent(p.latitude_deg,p.longitude_deg),motion::body_rotation(motion::wrap_heading(s.values[3]*180/motion::pi),s.values[4]*180/motion::pi,std::remainder(s.values[5]*180/motion::pi,360.0))));
 return std::isfinite(p.altitude_msl_m)&&motion::valid(p.attitude);
}
bool geographic(const PoseSample& p,motion::GeoPose& geo) noexcept {
 auto r=motion::compose(motion::transpose(motion::earth_tangent(p.latitude_deg,p.longitude_deg)),motion::rotation(p.attitude));
 geo.latitude_deg=p.latitude_deg;geo.longitude_deg=p.longitude_deg;geo.altitude_msl_m=p.altitude_msl_m;
 geo.pitch_deg=std::asin((std::max)(-1.0,(std::min)(1.0,-r.z.y)))*180/motion::pi;
 if(std::hypot(r.x.y,r.y.y)>1e-10){geo.heading_deg=motion::wrap_heading(std::atan2(-r.z.x,r.z.z)*180/motion::pi);geo.bank_deg=std::atan2(-r.x.y,r.y.y)*180/motion::pi;}
 else {geo.heading_deg=motion::wrap_heading(std::atan2(r.x.z,r.x.x)*180/motion::pi);geo.bank_deg=0;}
 return motion::valid(geo);
}
}
bool History::accept(const traffic_source::Sample& s,std::int64_t received,std::int64_t uncertainty) noexcept {
 if(!traffic_source::valid(s)||received<0||uncertainty<0||uncertainty>INT64_MAX-received)return false;const auto& h=s.snapshot.header;
 const auto latest=static_cast<std::uint64_t>(received+uncertainty);
 if(h.finished>latest&&h.finished-latest>1)return false;
 const bool same=domain(window_,s);if(same&&(h.sequence<=window_.sequence||static_cast<std::int64_t>(h.observed)<window_.source_observed))return false;
 if(!same){const auto revision=window_.revision+1;window_={};window_.revision=revision;window_.producer=s.producer;window_.connection=s.connection_id;window_.selection=s.selection_epoch;window_.lifecycle=h.lifecycle;window_.frequency=h.frequency;window_.process=h.process;window_.birth=h.process_created;}
 window_.sequence=h.sequence;window_.frame=h.frame;window_.source_observed=static_cast<std::int64_t>(h.observed);++window_.revision;
 for(std::size_t i=0;i<7;++i){auto& w=window_.slots[i];const auto& slot=s.snapshot.slots[i];
  if(w.generation!=slot.generation){w={};w.generation=slot.generation;}
  if(!(slot.flags&16u)||h.mode==0||(h.flags&8u)){w.count=0;w.unavailable=SlotState::inactive;continue;}
  PoseSample p{};if(!make_pose(h,slot,received,p)){w.count=0;w.unavailable=SlotState::invalid;continue;}
  if(w.count&&w.samples[w.count-1].observed==p.observed)w.samples[w.count-1]=p;
  else {if(w.count==history_capacity){for(std::size_t k=1;k<history_capacity;++k)w.samples[k-1]=w.samples[k];--w.count;}w.samples[w.count++]=p;}
  w.unavailable=SlotState::unavailable;
 }
 return true;
}
bool held(SlotState s) noexcept {return s==SlotState::oldest_hold||s==SlotState::newest_hold||s==SlotState::gap_hold;}
bool reconstruct(const Window& window,std::size_t slot,std::int64_t now,std::uint32_t delay_ms,Placement& out,std::uint32_t mode,std::int64_t uncertainty) noexcept {
 out={};if(slot>=7||now<0||!window.frequency||window.frequency>1000000000000ULL||mode>1||(mode?delay_ms!=0:(delay_ms<10||delay_ms>50)))return false;
 const auto& w=window.slots[slot];out.generation=w.generation;out.source_observed_qpc=window.source_observed;
 if(w.count>w.samples.size())return false;
 if(!w.count){out.state=w.unavailable;return true;}
 if(uncertainty<0||uncertainty>INT64_MAX-now)return false;const auto age_now=now+uncertainty;
 const auto& newest=w.samples[w.count-1];if(newest.finished>age_now||window.source_observed>age_now)return false;
 if(static_cast<std::uint64_t>(age_now-window.source_observed)>=window.frequency){out.state=SlotState::stale;return true;}
 const auto delay=static_cast<std::int64_t>(window.frequency*delay_ms/1000);const auto target=now>=delay?now-delay:0;out.target_qpc=target;
 std::size_t left{},right{};double alpha{};
 if(mode){left=right=w.count-1;out.state=SlotState::direct;}
 else if(target<w.samples[0].observed){out.state=SlotState::oldest_hold;}
 else {for(std::size_t k=0;k<w.count&&w.samples[k].observed<=target;++k)left=k;right=left;
  if(left+1<w.count){const auto span=w.samples[left+1].observed-w.samples[left].observed;
   if(span<=0)return false;
   if(static_cast<std::uint64_t>(span)>window.frequency/10){out.state=SlotState::gap_hold;}
   else {right=left+1;alpha=static_cast<double>(target-w.samples[left].observed)/static_cast<double>(span);out.state=SlotState::interpolated;}
  }else out.state=SlotState::newest_hold;
 }
 const auto& a=w.samples[left];const auto& b=w.samples[right];auto p=a;
 if(left!=right){p.latitude_deg=a.latitude_deg+(b.latitude_deg-a.latitude_deg)*alpha;p.longitude_deg=std::remainder(a.longitude_deg+std::remainder(b.longitude_deg-a.longitude_deg,360.0)*alpha,360.0);p.altitude_msl_m=a.altitude_msl_m+(b.altitude_msl_m-a.altitude_msl_m)*alpha;p.attitude=motion::slerp(a.attitude,b.attitude,alpha);}
 if(!geographic(p,out.geo))return false;
 out.left_sequence=a.sequence;out.right_sequence=b.sequence;out.left_frame=a.frame;out.right_frame=b.frame;out.left_observed_qpc=a.observed;out.right_observed_qpc=b.observed;out.left_received_qpc=a.received;out.right_received_qpc=b.received;out.presentation_qpc=left==right?a.observed:target;out.alpha=alpha;out.visible=true;return true;
}
}
