#include "psx_plugin/runway_actor_controller.hpp"
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>
#include <psx/non_normal/pipe.hpp>
#include <cstring>
namespace psx::xp {
RunwayActorController::~RunwayActorController(){worker_stop_.store(true);if(worker_.joinable())worker_.join();}
bool RunwayActorController::begin(SdkApi& sdk,std::int64_t(*clock)(),std::uint64_t frequency,const std::filesystem::path& root,const std::filesystem::path& cache){
 if(!sdk.on_thread()||!clock||!frequency)return false;retire();sdk_=&sdk;clock_=clock;frequency_=frequency;enabled_=true;++preparation_epoch_;model_failed_=false;model_failure_=nn::Reason::model;
 status_.wire_version=nn::actor_version;status_.capabilities=registered_?nn::actor_capability:0;
 status_.server=nn::current_process();status_.phase=nn::Phase::removed;status_.reason=nn::Reason::model;
 int xp{},version{},host{};XPLMGetVersions(&xp,&version,&host);if(version<420){model_failed_=true;model_failure_=nn::Reason::unsupported;status_.capabilities=0;status_.phase=nn::Phase::unavailable;status_.reason=nn::Reason::unsupported;return false;}
 if(!root.empty()){
  const auto r=root.generic_u8string(),c=cache.generic_u8string();if(r.size()>=1024||c.size()>=1024)return false;
  auto& p=preparations_.writing();p={};std::memcpy(p.root.data(),r.data(),r.size());std::memcpy(p.cache.data(),c.data(),c.size());p.epoch=preparation_epoch_;preparations_.publish();
  if(!worker_.joinable()){worker_stop_.store(false);worker_=std::thread([this]{while(!worker_stop_.load(std::memory_order_acquire)){if(!worker_done_.load(std::memory_order_acquire)&&preparations_.consume()){const auto p=preparations_.reading();const auto root=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.root.data())));candidate_=prepare_traffic_model(root,std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.cache.data()))),true);
    candidate_epoch_=p.epoch;worker_done_.store(true,std::memory_order_release);}Sleep(5);}});}
 }return true;
}
bool RunwayActorController::prepared(TrafficModel model){
 if(!enabled_||model_failed_||!sdk_||!sdk_->on_thread()||!model.qualified||!model.sdk_loadable||model.refs.empty()||model.refs.size()>128||model.refs.size()!=model.defaults.size())return false;
 for(const auto& path:model.sdk_objects)if(!traffic_sdk_path(path))return false;
 if(registered_){if(model.refs!=model_.refs)return false;model_=std::move(model);for(std::size_t i=0;i<model_.refs.size();++i)names_[i]=model_.refs[i].c_str();return true;}
 model_=std::move(model);for(std::size_t i=0;i<model_.refs.size();++i){names_[i]=model_.refs[i].c_str();if(XPLMFindDataRef(names_[i]))model_failed_=true;
  if(!model_failed_)refs_[i]=XPLMRegisterDataAccessor(names_[i],xplmType_Float,0,nullptr,nullptr,read_ref,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,this,nullptr);
  if(!refs_[i]){for(auto& ref:refs_)if(ref){XPLMUnregisterDataAccessor(ref);ref=nullptr;}model_failed_=true;model_failure_=nn::Reason::binding;status_.reason=nn::Reason::binding;return false;}}
 names_[model_.refs.size()]=nullptr;registered_=true;status_.capabilities=nn::actor_capability;status_.reason=nn::Reason::none;return true;
}
void RunwayActorController::release_objects()noexcept{for(auto& instance:instances_)if(instance){XPLMDestroyInstance(instance);instance=nullptr;}for(auto& object:objects_)if(object){XPLMUnloadObject(object);object=nullptr;}loaded_=false;}
void RunwayActorController::clear(nn::Reason reason,nn::Phase phase)noexcept{if(!sdk_||!sdk_->on_thread())return;const bool active=status_.phase==nn::Phase::loading||status_.phase==nn::Phase::applied;++epoch_;release_objects();if(active)++status_.revision;finish(phase,reason,active);}
void RunwayActorController::retire()noexcept{if(!sdk_||!sdk_->on_thread())return;enabled_=false;++preparation_epoch_;clear(nn::Reason::disconnected);connection_=0;}
void RunwayActorController::stop()noexcept{retire();worker_stop_.store(true,std::memory_order_release);if(worker_.joinable())worker_.join();worker_done_.store(false);if(!sdk_||!sdk_->on_thread())return;for(auto& ref:refs_)if(ref){XPLMUnregisterDataAccessor(ref);ref=nullptr;}registered_=false;names_={};}
void RunwayActorController::finish(nn::Phase phase,nn::Reason reason,bool publish)noexcept{const bool changed=status_.phase!=phase||status_.reason!=reason;status_.phase=phase;status_.reason=reason;status_.observed_qpc=clock_?clock_():0;status_.resource_generation=epoch_;status_.scenery_generation=scenery_;if(publish&&changed&&status_.request&&phase!=nn::Phase::loading)completions_.push({status_,connection_});}
bool RunwayActorController::result(nn::Result& r,std::uint64_t& c)noexcept{Completion value;if(!completions_.pop(value))return false;r=value.result;c=value.connection;return true;}
void RunwayActorController::selected_peer(nn::ProcessIdentity peer)noexcept{if(!sdk_||!sdk_->on_thread())return;if(peer!=selected_peer_&&(status_.phase==nn::Phase::loading||status_.phase==nn::Phase::applied))clear(nn::Reason::disconnected);selected_peer_=peer;}
nn::Result RunwayActorController::command(const ActorCommand& c)noexcept{
 const auto& r=c.request;auto response=status_;response.request=r.request;response.session=r.session;response.command=r.command;response.observed_qpc=clock_?clock_():0;
 const auto rejected=[&](nn::Reason reason){response.phase=nn::Phase::rejected;response.reason=reason;return response;};
 if(!enabled_||!sdk_||!sdk_->on_thread()||!nn::valid(r)||r.wire_version!=nn::actor_version||r.target!=status_.server||r.frequency!=frequency_||!c.connection)return rejected(nn::Reason::identity);
 if(r.issued_qpc>response.observed_qpc||r.deadline_qpc<=response.observed_qpc)return rejected(nn::Reason::expired);
 if(r.command==nn::Command::status)return response;
 if(r.command!=nn::Command::place_actor&&r.command!=nn::Command::remove_actor)return rejected(nn::Reason::unsupported);
 if(selected_peer_.pid&&r.client!=selected_peer_)return rejected(nn::Reason::identity);
 if(r.command==nn::Command::place_actor&&!selected_peer_.pid)return rejected(nn::Reason::binding);
 if(r.command==nn::Command::remove_actor&&!selected_peer_.pid&&(status_.phase==nn::Phase::loading||status_.phase==nn::Phase::applied)&&(r.client!=request_.client||c.connection!=connection_))return rejected(nn::Reason::identity);
 if(r.expected_revision&&r.expected_revision!=status_.revision)return rejected(nn::Reason::revision);
 if(r.command==nn::Command::place_actor&&model_failed_)return rejected(model_failure_);
 clear(nn::Reason::replaced);connection_=c.connection;request_=r;status_.request=r.request;status_.session=r.session;status_.command=r.command;++status_.revision;dirty_=false;
 if(r.command==nn::Command::remove_actor){finish(nn::Phase::removed,nn::Reason::none);dirty_=false;return status_;}
 geo_={r.latitude_deg,r.longitude_deg,static_cast<double>(r.runway_elevation_ft)*0.3048,r.heading_deg,0,0};
 if(!flat_actor_attitude(geo_.pitch_deg,geo_.bank_deg)){finish(nn::Phase::unavailable,nn::Reason::binding);return status_;}
 status_.latitude_deg=r.latitude_deg;status_.longitude_deg=r.longitude_deg;status_.heading_deg=r.heading_deg;status_.altitude_m=geo_.altitude_msl_m;status_.pitch_deg=geo_.pitch_deg;status_.bank_deg=geo_.bank_deg;
 finish(nn::Phase::loading,nn::Reason::none);dirty_=false;return status_;
}
void RunwayActorController::loaded(XPLMObjectRef object,void* context)noexcept{
 auto& t=*static_cast<Token*>(context);auto& s=*t.owner;if(!s.sdk_||!s.sdk_->on_thread())return;if(s.pending_)--s.pending_;
 if(!s.enabled_||t.epoch!=s.epoch_||s.model_failed_){if(object)XPLMUnloadObject(object);}else if(!object)s.model_failed_=true;else s.objects_[t.part]=object;
 if(s.pending_)return;s.loading_=false;
 if(!s.enabled_||t.epoch!=s.epoch_||s.model_failed_){s.release_objects();return;}
 for(std::size_t i=0;i<8;++i){s.instances_[i]=XPLMCreateInstance(s.objects_[i],s.names_.data());if(!s.instances_[i]){s.model_failed_=true;s.release_objects();return;}XPLMInstanceSetAutoShift(s.instances_[i]);}s.loaded_=true;
}
void RunwayActorController::load()noexcept{if(pending_||loading_||loaded_||!registered_||model_failed_||!model_.sdk_loadable)return;loading_=true;pending_=8;for(std::size_t i=0;i<8;++i)tokens_[i]={this,epoch_,i};for(std::size_t i=0;i<8;++i)XPLMLoadObjectAsync(model_.sdk_objects[i].c_str(),loaded,&tokens_[i]);}
void RunwayActorController::submit_flat()noexcept{
 motion::Vec3 nose,north,up;XPLMWorldToLocal(geo_.latitude_deg,geo_.longitude_deg,geo_.altitude_msl_m,&nose.x,&nose.y,&nose.z);
 XPLMWorldToLocal(geo_.latitude_deg+0.0001,geo_.longitude_deg,geo_.altitude_msl_m,&north.x,&north.y,&north.z);
 XPLMWorldToLocal(geo_.latitude_deg,geo_.longitude_deg,geo_.altitude_msl_m+10,&up.x,&up.y,&up.z);
 if(!motion::tangent_rotation(north-nose,up-nose,tangent_)||!motion::local_pose(geo_,nose,tangent_,actor_contacts[0],local_)){clear(nn::Reason::binding,nn::Phase::unavailable);return;}
 XPLMDrawInfoDouble_t draw{};draw.structSize=sizeof(draw);draw.x=local_.origin.x;draw.y=local_.origin.y;draw.z=local_.origin.z;draw.pitch=local_.pitch_deg;draw.heading=local_.heading_deg;draw.roll=local_.bank_deg;
 for(std::size_t i=0;i<model_.defaults.size();++i)values_[i]=std::max(0.0F,model_.defaults[i]);values_[model_.refs.size()-1]=1;
 const auto now=clock_();if(now<request_.issued_qpc||now>=request_.deadline_qpc){clear(nn::Reason::expired,nn::Phase::rejected);return;}
 for(auto instance:instances_)XPLMInstanceSetPositionDouble(instance,&draw,values_.data());
 status_.latitude_deg=geo_.latitude_deg;status_.longitude_deg=geo_.longitude_deg;status_.altitude_m=geo_.altitude_msl_m;status_.heading_deg=geo_.heading_deg;status_.pitch_deg=geo_.pitch_deg;status_.bank_deg=geo_.bank_deg;finish(nn::Phase::applied,nn::Reason::none);
}
void RunwayActorController::tick(std::uint64_t connection,std::uint64_t scenery,std::uint64_t)noexcept{
 if(!enabled_||!sdk_||!sdk_->on_thread())return;
 if(worker_done_.load(std::memory_order_acquire)){if(candidate_epoch_==preparation_epoch_&&!prepared(std::move(candidate_)))model_failed_=true;worker_done_.store(false,std::memory_order_release);}
 if(connection_&&connection!=connection_){clear(nn::Reason::disconnected);connection_=0;}
 if(scenery_&&scenery_!=scenery)clear(nn::Reason::scenery);scenery_=scenery;status_.scenery_generation=scenery;
 if(status_.phase!=nn::Phase::loading)return;
 const auto now=clock_();if(now<request_.issued_qpc||now>=request_.deadline_qpc){clear(nn::Reason::expired,nn::Phase::rejected);return;}
 if(model_failed_){clear(model_failure_,nn::Phase::unavailable);return;}load();if(loaded_)submit_flat();
}
}
