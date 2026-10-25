#pragma once
#include "runway_actor_geometry.hpp"
#include "runway_actor_service.hpp"
#include "traffic_model.hpp"
#include "sdk_api.hpp"
#include <XPLMInstance.h>
#include <XPLMScenery.h>
namespace psx::xp {
class RunwayActorController final {
public:
 ~RunwayActorController();
 // Human-selected flat placement uses explicit database runway-end elevation.
 bool begin(SdkApi&,std::int64_t(*)(),std::uint64_t,const std::filesystem::path& root={},const std::filesystem::path& cache={});
 void retire()noexcept;void stop()noexcept;
 bool prepared(TrafficModel);
 nn::Result command(const ActorCommand&)noexcept;
 void tick(std::uint64_t connection,std::uint64_t scenery,std::uint64_t origin)noexcept;
 bool result(nn::Result& r,std::uint64_t& c)noexcept;
 void clear(nn::Reason,nn::Phase=nn::Phase::removed)noexcept;
 void scenery(std::uint64_t generation)noexcept{scenery_=generation;clear(nn::Reason::scenery);}
 void selected_peer(nn::ProcessIdentity peer)noexcept;
 const nn::Result& status()const noexcept{return status_;}
 const motion::LocalPose& pose()const noexcept{return local_;}
private:
 struct Token{RunwayActorController* owner{};std::uint64_t epoch{};std::size_t part{};};
 static void loaded(XPLMObjectRef,void*)noexcept;static float read_ref(void*)noexcept{return 0;}
 void release_objects()noexcept;void load()noexcept;void finish(nn::Phase,nn::Reason,bool publish=true)noexcept;
 void submit_flat()noexcept;
 SdkApi* sdk_{};std::int64_t(*clock_)(){};std::uint64_t frequency_{},epoch_{},preparation_epoch_{},connection_{},scenery_{};
 bool enabled_{},registered_{},loading_{},loaded_{},model_failed_{},dirty_{};std::size_t pending_{};
 nn::Reason model_failure_{nn::Reason::model};
 nn::ProcessIdentity selected_peer_{};nn::Request request_{};nn::Result status_{};motion::GeoPose geo_{};motion::LocalPose local_{};motion::Rotation tangent_{};
 struct Completion{nn::Result result{};std::uint64_t connection{};};SpscFifo<Completion,64> completions_;
 std::array<XPLMObjectRef,8> objects_{};std::array<XPLMInstanceRef,8> instances_{};std::array<Token,8> tokens_{};
 std::array<XPLMDataRef,128> refs_{};std::array<const char*,129> names_{};std::array<float,128> values_{};TrafficModel model_,candidate_;
 struct Preparation{std::array<char,1024> root{},cache{};std::uint64_t epoch{};};TripleBuffer<Preparation> preparations_;
 std::uint64_t candidate_epoch_{};std::thread worker_;std::atomic<bool> worker_stop_{},worker_done_{};
};
}
