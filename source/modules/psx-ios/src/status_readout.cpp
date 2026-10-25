#include <ios/status_readout.hpp>
namespace ios {
namespace ns=psx::instructor::ios_status_native;
namespace pi=psx::instructor;
namespace ipc=psx::non_normal;
void StatusReadout::disconnected(){client_.close();sample_.reset();identity_={};pending_=false;request_=query_at_=deadline_=0;minimum_qpc_=ipc::qpc();}
bool StatusReadout::accept(const ns::Snapshot&r){
  const auto clock=ipc::qpc();
  if(r.frequency!=ipc::qpc_frequency()||r.copy_finished_qpc>clock)return false;
  if(sample_&&r.lifecycle&&sample_->lifecycle){
    if(r.lifecycle<sample_->lifecycle)return false;
    if(r.lifecycle==sample_->lifecycle){
      if(r.sequence<sample_->sequence)return false;
      for(unsigned g=0;g<ns::group_count;++g)if((r.groups&sample_->groups)&(1u<<g))
        if(r.group_sequence[g]<sample_->group_sequence[g]||r.frame[g]<sample_->frame[g]||r.observed_qpc[g]<sample_->observed_qpc[g])return false;
    }
  }
  sample_=r;return true;
}
void StatusReadout::poll(pi::Tick t,bool ready,ipc::ProcessIdentity id){
  if(!ready||!id.pid){disconnected();return;}
  if(id!=identity_){disconnected();identity_=id;}
  if(!client_.connected()){
    if(t<query_at_)return;query_at_=t+1000;
    if(!client_.connect(id))return;
    request_=0;pending_=false;query_at_=t;
  }
  ns::Snapshot r;
  for(unsigned i=0;i<4&&client_.receive(r);++i){
    if(!pending_||r.request!=request_)continue;
    pending_=false;
    if(!accept(r)){client_.close();sample_.reset();break;}
  }
  if(!client_.connected()){sample_.reset();pending_=false;query_at_=t+1000;return;}
  if(pending_&&t>=deadline_){client_.close();sample_.reset();pending_=false;query_at_=t+1000;return;}
  if(!pending_&&t>=query_at_){query_at_=t+66;pending_=client_.query();if(pending_){++request_;deadline_=t+2000;}else sample_.reset();}
}
bool StatusReadout::available()const{
  if(!client_.connected()||!sample_||sample_->flags||!sample_->groups||sample_->copy_finished_qpc<=minimum_qpc_)return false;
  const auto t=ipc::qpc();return t>=sample_->copy_finished_qpc&&std::uint64_t(t-sample_->copy_finished_qpc)<=sample_->frequency/4;
}
pi::StatusDisplay::Values StatusReadout::values(pi::Tick t)const{
  pi::StatusDisplay::Values result;if(!available())return result;const auto &s=*sample_;const auto clock=ipc::qpc();
  auto group=[&](unsigned g){return (s.groups&(1u<<g))&&s.observed_qpc[g]>minimum_qpc_&&clock>=s.observed_qpc[g]&&std::uint64_t(clock-s.observed_qpc[g])<=s.frequency/4;};
  auto stamp=[&](unsigned g){const auto age=std::uint64_t(clock-s.observed_qpc[g])*1000/s.frequency;return t>age?t-age:0;};
  auto present=[&](unsigned start,unsigned count){const auto mask=((1u<<count)-1)<<start;return (s.values_valid&mask)==mask;};
  if(group(0)){
    if(present(0,3)){result.axes=std::array<double,3>{s.values[0],s.values[1],s.values[2]};result.axes_at=stamp(0);}
    if(present(3,1)){result.tiller=s.values[3];result.tiller_at=stamp(0);}
  }
  if(group(1)){
    result.engine=s.engine_model;
    if(present(4,4)){result.n1=std::array<double,4>{s.values[4],s.values[5],s.values[6],s.values[7]};result.n1_at=stamp(1);}
    if(present(8,4)){result.epr=std::array<double,4>{s.values[8],s.values[9],s.values[10],s.values[11]};result.epr_at=stamp(1);}
  }
  if(group(2)&&present(12,4)){std::array<double,4>a{};for(unsigned i=0;i<4;++i)a[i]=s.values[12+i]/2250.;result.flaps=a;result.flaps_at=stamp(2);}
  if(group(3)&&present(16,11)){std::array<double,11>a{};for(unsigned i=0;i<11;++i)a[i]=s.values[16+i]/60.;result.spoilers=a;result.spoilers_at=stamp(3);}
  return result;
}
}
