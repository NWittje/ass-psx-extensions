#include "coordinator/visibility.hpp"
#include <cmath>

namespace coordinator {
namespace {
bool same(ProcessIdentity a,ProcessIdentity b) noexcept {return a.pid==b.pid&&a.creation_filetime==b.creation_filetime;}
bool domain(const psx::environment::Sample& a,const psx::environment::Sample& b) noexcept {
 return a.producer==b.producer&&a.lifecycle==b.lifecycle;
}
}
void VisibilitySelections::clear() noexcept {selections_={};context_={};++revision_;}
void VisibilitySelections::associate(ProcessIdentity psx) noexcept {if(same(psx,psx_))return;clear();psx_=psx;}
void VisibilitySelections::ios(std::uint64_t epoch,std::uint64_t session,bool connected) noexcept {
 if(epoch<ios_epoch_)return;
 if(epoch==ios_epoch_&&ios_session_&&session!=ios_session_)return;
 if(epoch!=ios_epoch_||session!=ios_session_||!connected)clear();
 ios_epoch_=epoch;ios_session_=session;ios_connected_=connected&&epoch&&session;
}
bool VisibilitySelections::current(std::int64_t now) const noexcept {
 const auto& s=context_.source;
 return frequency_&&psx_.pid&&same(context_.psx,psx_)&&s.sequence&&context_.configured_observation_qpc>0&&now>=s.sample_end_qpc&&
  now>=s.received_qpc&&static_cast<long double>(now-context_.configured_observation_qpc)*1000<static_cast<long double>(frequency_)*psx::environment::source_stale_ms;
}
bool VisibilitySelections::select(VisibilityTarget target,double metres,ProcessIdentity psx,
 std::int64_t readback,const std::array<char,8>& station,std::int64_t now) noexcept {
 const auto index=static_cast<std::size_t>(target);
 if((target!=VisibilityTarget::planet&&target!=VisibilityTarget::zone1&&target!=VisibilityTarget::all)||index>=selections_.size()||!ios_connected_||!psx.pid||!same(psx,psx_)||readback<0||(metres&&readback==0)||readback>now||
  !std::isfinite(metres)||
  (metres&&(metres<10000||metres>40000)))return false;
 if(metres&&target==VisibilityTarget::zone1&&station[0]==0)return false;
 selections_[index]={metres,readback,station};++revision_;return true;
}
bool VisibilitySelections::observe(const VisibilityContext& value,std::int64_t now) noexcept {
 const auto& s=value.source;const auto& old=context_.source;
 if(!psx_.pid||!same(value.psx,psx_)||!s.sequence||!s.lifecycle||s.sample_end_qpc>now||s.received_qpc>now||
  s.sample_end_qpc<=0||value.configured_observation_qpc>s.sample_end_qpc||value.configured_observation_start_qpc>value.configured_observation_qpc||
  (value.configured_mask&&value.configured_observation_start_qpc<s.sample_start_qpc)||(value.configured_mask&~19u)||static_cast<unsigned>(value.active)>2)return false;
 for(std::size_t i=0;i<selections_.size();++i)if((value.configured_mask&(1u<<i))&&value.configured_m[i]>9999)return false;
 if(old.sequence&&old.producer==s.producer&&s.lifecycle<old.lifecycle)return false;
 if(old.sequence&&domain(old,s)&&(s.sequence<=old.sequence||s.sample_end_qpc<old.sample_end_qpc))return false;
 if(old.sequence&&!domain(old,s))clear();
 context_=value;
 for(std::size_t i=0;i<selections_.size();++i){auto& selection=selections_[i];if(!selection.metres||value.configured_observation_start_qpc<=selection.configured_readback_qpc)continue;
  if(((value.configured_mask&(1u<<i))&&value.configured_m[i]!=9999)||
   (i==1&&value.station_available&&value.zone1_station!=selection.station)){selection={};++revision_;}
 }
 return true;
}
bool VisibilitySelections::source_matches(const psx::environment::Sample& s) const noexcept {
 const auto& c=context_.source;
 return domain(s,c)&&s.configuration==c.configuration&&s.sequence==c.sequence&&s.sample_start_qpc==c.sample_start_qpc&&
  s.sample_end_qpc==c.sample_end_qpc&&s.writer_version==c.writer_version&&s.frame_sequence==c.frame_sequence;
}
VisibilityPolicy VisibilitySelections::policy(const psx::environment::Sample& s,std::int64_t now) const noexcept {
 VisibilityPolicy result;result.revision=revision_;
 if((s.valid_mask&psx::environment::bit(psx::environment::Field::visibility_m))&&(s.flags&psx::environment::visibility_unlimited)){result.kind=VisibilityPolicyKind::sentinel_ceiling;result.metres=9999;}
 if(!ios_connected_||!current(now)||!source_matches(s))return result;
 const auto& global=selections_[4];
 if(global.metres&&(context_.configured_mask&16u)&&context_.configured_m[4]==9999&&context_.configured_observation_start_qpc>global.configured_readback_qpc){
  result.target=VisibilityTarget::all;result.kind=VisibilityPolicyKind::session_selection;result.metres=global.metres;return result;
 }
 const auto i=static_cast<std::size_t>(context_.active);
 if(i>=selections_.size())return result;
 result.target=context_.active;const auto& selection=selections_[i];
 if(!selection.metres||!(context_.configured_mask&(1u<<i))||context_.configured_m[i]!=9999||context_.configured_observation_start_qpc<=selection.configured_readback_qpc||
  (i==1&&(!context_.station_available||context_.zone1_station!=selection.station)))return result;
 result.kind=VisibilityPolicyKind::session_selection;result.metres=selection.metres;return result;
}
}
