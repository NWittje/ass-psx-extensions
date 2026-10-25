#include "psx_plugin/traffic_controller.hpp"
#include <psx/sync/runtime.hpp>
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>
#include <cmath>
#include <algorithm>
#include <Windows.h>
#include <cstring>
namespace psx::xp {
TrafficController::~TrafficController(){worker_stop_.store(true,std::memory_order_release);if(worker_.joinable())worker_.join();}
bool TrafficController::begin(SdkApi& sdk,motion::InstanceId id,std::uint64_t freq,std::int64_t(*clock)(),const std::filesystem::path& root,const std::filesystem::path& cache){
 if(!sdk.on_thread()||!clock||!freq)return false;int xp{},version{},host{};XPLMGetVersions(&xp,&version,&host);if(version<420)return false;retire();sdk_=&sdk;id_=id;frequency_=freq;clock_=clock;enabled_=true;++epoch_;++preparation_epoch_;report_={};report_.plugin=id;report_.phase=tv::Phase::observing;report_.resource_generation=epoch_;load_failed_=false;capture_={};locals_={};next_detail_qpc_=0;detail_ready_=false;
 if(!root.empty()){
  const auto r=root.generic_u8string(),c=cache.generic_u8string();if(r.size()>=1024||c.size()>=1024)return false;
  auto& request=preparations_.writing();request={};std::memcpy(request.root.data(),r.data(),r.size());std::memcpy(request.cache.data(),c.data(),c.size());request.epoch=preparation_epoch_;preparations_.publish();
  if(!worker_.joinable()){worker_stop_.store(false,std::memory_order_release);worker_=std::thread([this]{
   while(!worker_stop_.load(std::memory_order_acquire)){
    if(!worker_done_.load(std::memory_order_acquire)&&preparations_.consume()){
     const auto request=preparations_.reading();const auto root=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(request.root.data())));const auto cache=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(request.cache.data())));
     candidate_=prepare_traffic_model(root,cache);candidate_epoch_=request.epoch;worker_done_.store(true,std::memory_order_release);
    }Sleep(5);
   }
  });}
 }return true;
}
bool TrafficController::prepared(TrafficModel model){if(!enabled_||!sdk_||!sdk_->on_thread()||!model.qualified||!model.sdk_loadable||model.refs.size()>128||model.refs.empty()||model.refs.size()!=model.defaults.size())return false;
 for(const auto& path:model.sdk_objects)if(!traffic_sdk_path(path))return false;
 if(registered_){if(model.refs!=model_.refs)return false;model_=std::move(model);for(std::size_t i=0;i<model_.refs.size();++i)names_[i]=model_.refs[i].c_str();return true;}
 const auto failed=[&]{for(auto& ref:refs_)if(ref){XPLMUnregisterDataAccessor(ref);ref=nullptr;}names_={};registered_=false;return false;};
 model_=std::move(model);for(std::size_t i=0;i<model_.refs.size();++i){names_[i]=model_.refs[i].c_str();if(XPLMFindDataRef(names_[i]))return failed();refs_[i]=XPLMRegisterDataAccessor(names_[i],xplmType_Float,0,nullptr,nullptr,read_ref,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,this,nullptr);if(!refs_[i])return failed();}names_[model_.refs.size()]=nullptr;registered_=true;return true;
}
void TrafficController::submit(std::size_t slot,const motion::LocalPose* p,bool visible)noexcept{
 XPLMDrawInfoDouble_t info{};info.structSize=sizeof(info);if(p){info.x=p->origin.x;info.y=p->origin.y;info.z=p->origin.z;info.pitch=static_cast<float>(p->pitch_deg);info.heading=static_cast<float>(p->heading_deg);info.roll=static_cast<float>(p->bank_deg);}
 for(std::size_t i=0;i<model_.defaults.size();++i)values_[i]=model_.defaults[i]<0?strobe_:model_.defaults[i];
 if(model_.refs.size()>=2){values_[model_.refs.size()-2]=strobe_;values_[model_.refs.size()-1]=visible?1.0F:0.0F;}
 for(auto instance:instances_[slot])if(instance)XPLMInstanceSetPositionDouble(instance,&info,values_.data());
}
void TrafficController::hide()noexcept{if(!sdk_||!sdk_->on_thread())return;for(std::size_t s=0;s<7;++s)submit(s,nullptr,false);report_.hidden_mask=127;report_.submitted_mask=report_.drawable_mask=report_.held_mask=0;placements_={};}
void TrafficController::retire_load()noexcept{if(pending_){++epoch_;release_objects();}}
void TrafficController::release_objects()noexcept{for(auto& slot:instances_)for(auto& instance:slot)if(instance){XPLMDestroyInstance(instance);instance=nullptr;}for(auto& object:objects_)if(object){XPLMUnloadObject(object);object=nullptr;}loaded_=false;}
void TrafficController::retire()noexcept{if(!sdk_||!sdk_->on_thread())return;enabled_=false;++epoch_;++preparation_epoch_;hide();release_objects();claim_={};window_={};renewed_=0;transport_=0;next_detail_qpc_=0;detail_ready_=false;report_.phase=tv::Phase::released;report_.reason=tv::Reason::released;}
void TrafficController::stop()noexcept{retire();worker_stop_.store(true,std::memory_order_release);if(worker_.joinable())worker_.join();worker_done_.store(false,std::memory_order_release);if(!sdk_||!sdk_->on_thread())return;for(auto& ref:refs_)if(ref){XPLMUnregisterDataAccessor(ref);ref=nullptr;}registered_=false;names_={};model_={};}
void TrafficController::loaded(XPLMObjectRef object,void* context)noexcept{
 auto& token=*static_cast<Token*>(context);auto& s=*token.owner;if(!s.sdk_||!s.sdk_->on_thread())return;
 token.pending=false;if(s.pending_)--s.pending_;
 if(!s.enabled_||token.epoch!=s.epoch_||s.load_failed_){if(object)XPLMUnloadObject(object);}else if(!object){s.load_failed_=true;}else s.objects_[token.part]=object;
 if(s.pending_)return;s.loading_=false;
 if(!s.enabled_||token.epoch!=s.epoch_||s.load_failed_){s.release_objects();return;}
 for(std::size_t slot=0;slot<7;++slot)for(std::size_t part=0;part<8;++part){s.instances_[slot][part]=XPLMCreateInstance(s.objects_[part],s.names_.data());if(!s.instances_[slot][part]){s.load_failed_=true;s.release_objects();return;}}
 s.loaded_=true;s.hide();
}
void TrafficController::load()noexcept{if(loading_||pending_||loaded_||load_failed_||!registered_||!model_.qualified||!model_.sdk_loadable)return;++epoch_;loading_=true;pending_=8;for(std::size_t i=0;i<8;++i){tokens_[i]={this,epoch_,i,true};}for(std::size_t i=0;i<8;++i)XPLMLoadObjectAsync(model_.sdk_objects[i].c_str(),loaded,&tokens_[i]);}
std::uint32_t TrafficController::capabilities()const noexcept{return tv::supported|(registered_&&model_.qualified?tv::qualified_model:0u)|(loaded_?tv::loaded_model:0u);}
tv::Ack TrafficController::command(const TrafficCommand& cmd)noexcept{
 tv::Ack ack;auto& a=static_cast<tv::Report&>(ack);const auto now=clock_?clock_():0;const auto& c=cmd.claim;
 a.plugin=id_;a.owner=c.owner;a.connection=c.connection;a.lease=c.lease;a.request=c.request;a.generated_qpc=now;a.source_producer=c.source_producer;a.source_connection=c.source_connection;a.source_selection=c.source_selection;a.lifecycle=c.lifecycle;a.resource_generation=epoch_;
 if(!enabled_||!sdk_||!sdk_->on_thread()||!tv::valid(c)||c.plugin!=id_||c.frequency!=frequency_||cmd.received>now||now-cmd.received>=static_cast<std::int64_t>(frequency_)){a.phase=tv::Phase::rejected;a.reason=tv::Reason::identity;return ack;}
 if(c.action==tv::Action::acquire){if(claim_.lease&&transport_==cmd.transport&&claim_.owner!=c.owner&&now-renewed_<static_cast<std::int64_t>(frequency_)){a.phase=tv::Phase::rejected;a.reason=tv::Reason::contention;return ack;}retire_load();if(registered_&&model_.qualified)load_failed_=false;a.resource_generation=epoch_;hide();claim_=c;window_={};transport_=cmd.transport;renewed_=cmd.received;next_detail_qpc_=0;detail_ready_=false;report_=a;report_.sdk_sequence=sdk_sequence_;report_.phase=tv::Phase::pending;report_.reason=tv::Reason::awaiting_source;}
 else if(c.owner!=claim_.owner||c.connection!=claim_.connection||c.lease!=claim_.lease||cmd.transport!=transport_||c.request<=claim_.request){a.phase=tv::Phase::rejected;a.reason=tv::Reason::identity;return ack;}
 else{claim_.request=c.request;report_.request=c.request;if(c.action==tv::Action::release){retire_load();report_.resource_generation=epoch_;hide();claim_={};window_={};next_detail_qpc_=0;detail_ready_=false;report_.phase=tv::Phase::released;report_.reason=tv::Reason::released;}else renewed_=cmd.received;}
 a=report_;a.generated_qpc=now;return ack;
}
void TrafficController::window(const TrafficWindow& w)noexcept{
 if(!claim_.lease||w.transport!=transport_||w.lease!=claim_.lease||w.window.producer!=claim_.source_producer||w.window.connection!=claim_.source_connection||w.window.selection!=claim_.source_selection||w.window.lifecycle!=claim_.lifecycle||w.window.frequency!=frequency_||w.window.process!=claim_.source_pid||w.window.birth!=claim_.source_birth||w.window.source_observed<=claim_.source_floor_qpc||w.window.sequence<=window_.sequence)return;window_=w.window;
}
void TrafficController::tick(std::uint64_t transport,std::uint64_t scenery,std::uint64_t origin)noexcept{
 detail_ready_=false;
 if(!enabled_||!sdk_||sdk_->ready()!=Status::ok)return;
 if(worker_done_.load(std::memory_order_acquire)){if(candidate_epoch_==preparation_epoch_&&!prepared(std::move(candidate_)))load_failed_=true;worker_done_.store(false,std::memory_order_release);}
 const auto now=clock_();report_.generated_qpc=now;report_.scenery_generation=scenery;report_.origin_generation=origin;report_.resource_generation=epoch_;
 if(!claim_.lease)return;
 if(transport!=transport_||now<renewed_||now-renewed_>=static_cast<std::int64_t>(frequency_)){retire_load();hide();claim_={};window_={};report_.phase=tv::Phase::released;report_.reason=transport!=transport_?tv::Reason::disconnected:tv::Reason::timeout;return;}
 load();report_.resource_generation=epoch_;if(!loaded_){hide();report_.phase=tv::Phase::pending;report_.reason=load_failed_?tv::Reason::model_unavailable:tv::Reason::loading;return;}
 // Fixed cosmetic double flash, 80ms pulses at 0/160ms in each one-second cycle.
 const auto milliseconds=static_cast<std::uint64_t>(now%static_cast<std::int64_t>(frequency_))*1000/frequency_;phase_=static_cast<float>(milliseconds)/1000;strobe_=(milliseconds<80||(milliseconds>=160&&milliseconds<240))?1.0F:0.0F;
 if(!window_.sequence){hide();report_.phase=tv::Phase::pending;report_.reason=tv::Reason::awaiting_source;return;}
 report_.drawable_mask=report_.submitted_mask=report_.held_mask=report_.hidden_mask=0;
 for(std::size_t slot=0;slot<7;++slot){auto& placement=placements_[slot];const auto bit=1u<<slot;motion::LocalPose local{};bool okay=tv::reconstruct(window_,slot,now,claim_.delay_ms,placement,claim_.presentation_mode,psx::sync::freshness_now(now)-now)&&placement.visible;
  if(okay){const auto& g=placement.geo;motion::Vec3 center,north,up;XPLMWorldToLocal(g.latitude_deg,g.longitude_deg,g.altitude_msl_m,&center.x,&center.y,&center.z);XPLMWorldToLocal(g.latitude_deg+0.0001,g.longitude_deg,g.altitude_msl_m,&north.x,&north.y,&north.z);XPLMWorldToLocal(g.latitude_deg,g.longitude_deg,g.altitude_msl_m+10,&up.x,&up.y,&up.z);motion::Rotation tangent;okay=motion::tangent_rotation(north-center,up-center,tangent)&&motion::local_pose(g,center,tangent,{},local);}
  locals_[slot]=local;submit(slot,okay?&local:nullptr,okay);if(okay){report_.drawable_mask|=bit;report_.submitted_mask|=bit;if(tv::held(placement.state))report_.held_mask|=bit;}else report_.hidden_mask|=bit;
 }
 report_.source_sequence=window_.sequence;report_.source_observed_qpc=window_.source_observed;report_.applied_qpc=clock_();report_.sdk_sequence=++sdk_sequence_;const bool stale=psx::sync::freshness_now(report_.applied_qpc)-window_.source_observed>=static_cast<std::int64_t>(frequency_);
 if(report_.applied_qpc-renewed_>=static_cast<std::int64_t>(frequency_)){retire_load();hide();claim_={};report_.phase=tv::Phase::released;report_.reason=tv::Reason::timeout;}
 else{if(stale)hide();report_.phase=stale?tv::Phase::pending:tv::Phase::owned;report_.reason=stale?tv::Reason::stale:tv::Reason::none;}
 report_.generated_qpc=clock_();
 // Select one coherent seven-slot diagnostic observation, at most100Hz, before
 // creating any Capture records. Rendering still executes on every SDK tick.
 // Deliberately unsampled ticks are not transport loss; acquired records keep
 // their actual SDK sequence/clocks and ordinary overflow accounting.
 if((report_.phase==tv::Phase::owned||report_.reason==tv::Reason::stale)&&capture_.enabled&&capture_.plugin==id_&&capture_.connection==report_.connection&&
    report_.generated_qpc>=capture_.sent_qpc&&report_.generated_qpc-capture_.sent_qpc<static_cast<std::int64_t>(frequency_*2)&&report_.generated_qpc>=next_detail_qpc_){
  detail_ready_=true;next_detail_qpc_=report_.generated_qpc+static_cast<std::int64_t>(frequency_/100+(frequency_%100!=0));
 }
}
void TrafficController::capture(const tv::Capture& c)noexcept{
 if(c.plugin!=capture_.plugin||c.connection!=capture_.connection||c.generation!=capture_.generation||c.enabled!=capture_.enabled){next_detail_qpc_=0;detail_ready_=false;}
 capture_=c;
}
bool TrafficController::detail(std::size_t slot,tv::Detail& d)const noexcept{
 if(slot>=7||!detail_ready_||!clock_||(report_.phase!=tv::Phase::owned&&report_.reason!=tv::Reason::stale)||!capture_.enabled||capture_.plugin!=id_||capture_.connection!=report_.connection||clock_()<capture_.sent_qpc||clock_()-capture_.sent_qpc>=static_cast<std::int64_t>(frequency_*2))return false;
 const auto& p=placements_[slot];const auto& local=locals_[slot];d={};d.plugin=id_;d.owner=report_.owner;d.connection=report_.connection;d.lease=report_.lease;d.request=report_.request;d.capture_generation=capture_.generation;d.sdk_sequence=report_.sdk_sequence;d.slot=static_cast<std::uint32_t>(slot);d.flags=(p.visible?tv::detail_pose_valid:0u)|((report_.submitted_mask&(1u<<slot))?tv::detail_submitted|tv::detail_visible:0u);
 d.source_producer=report_.source_producer;d.source_connection=report_.source_connection;d.source_selection=report_.source_selection;d.lifecycle=report_.lifecycle;d.source_sequence=report_.source_sequence;d.source_frame=window_.frame;d.generation=p.generation;d.source_observed_qpc=report_.source_observed_qpc;d.generated_qpc=report_.generated_qpc;d.target_qpc=p.target_qpc;d.presentation_qpc=p.presentation_qpc;d.left_observed_qpc=p.left_observed_qpc;d.right_observed_qpc=p.right_observed_qpc;d.left_received_qpc=p.left_received_qpc;d.right_received_qpc=p.right_received_qpc;d.applied_qpc=report_.applied_qpc;d.left_sequence=p.left_sequence;d.right_sequence=p.right_sequence;d.left_frame=p.left_frame;d.right_frame=p.right_frame;d.scenery_generation=report_.scenery_generation;d.origin_generation=report_.origin_generation;d.resource_generation=epoch_;d.state=static_cast<std::uint32_t>(p.state);d.validity=p.visible?0x3fu:0u;d.delay_ms=claim_.delay_ms;d.presentation_mode=claim_.presentation_mode;d.alpha=p.alpha;d.latitude_deg=p.geo.latitude_deg;d.longitude_deg=p.geo.longitude_deg;d.altitude_msl_m=p.geo.altitude_msl_m;d.heading_deg=p.geo.heading_deg;d.pitch_deg=p.geo.pitch_deg;d.bank_deg=p.geo.bank_deg;d.local_x=local.origin.x;d.local_y=local.origin.y;d.local_z=local.origin.z;d.local_heading_deg=local.heading_deg;d.local_pitch_deg=local.pitch_deg;d.local_bank_deg=local.bank_deg;d.strobe_phase=phase_;d.nav_brightness=1;d.strobe_brightness=strobe_;return true;
}
}
