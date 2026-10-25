#pragma once
#include "sdk_api.hpp"
#include "traffic_service.hpp"
#include "traffic_model.hpp"
#include "triple_buffer.hpp"
#include <XPLMInstance.h>
#include <XPLMScenery.h>
#include <atomic>
#include <thread>
namespace psx::xp {
class TrafficController final {
public:
 ~TrafficController();
 bool begin(SdkApi&,motion::InstanceId,std::uint64_t,std::int64_t(*)(),const std::filesystem::path& root,const std::filesystem::path& cache);
 void retire() noexcept;void stop() noexcept;
 tv::Ack command(const TrafficCommand&) noexcept;void window(const TrafficWindow&) noexcept;
 void tick(std::uint64_t transport,std::uint64_t scenery,std::uint64_t origin) noexcept;
 const tv::Report& report() const noexcept{return report_;}
 std::uint32_t capabilities()const noexcept;
 // Authored/fake-SDK seam; production qualification exclusively uses the worker.
 bool prepared(TrafficModel model);
 const std::array<tv::Placement,7>& placements()const noexcept{return placements_;}
 float light_phase()const noexcept{return phase_;}
 void capture(const tv::Capture&) noexcept;
 bool detail(std::size_t,tv::Detail&) const noexcept;
private:
 struct Token{TrafficController* owner{};std::uint64_t epoch{};std::size_t part{};bool pending{};};
 static void loaded(XPLMObjectRef,void*) noexcept;static float read_ref(void*) noexcept{return 0;}
 void hide()noexcept;void release_objects()noexcept;void retire_load()noexcept;void load()noexcept;void submit(std::size_t,const motion::LocalPose*,bool)noexcept;
 SdkApi* sdk_{};motion::InstanceId id_{};std::uint64_t frequency_{},epoch_{},preparation_epoch_{},transport_{},sdk_sequence_{};std::int64_t(*clock_)(){};
 bool enabled_{},registered_{},loading_{},load_failed_{},loaded_{};std::size_t pending_{};
 struct Preparation{std::array<char,1024> root{},cache{};std::uint64_t epoch{};};
 TripleBuffer<Preparation> preparations_;
 std::thread worker_;std::atomic<bool> worker_done_{},worker_stop_{};std::uint64_t candidate_epoch_{};TrafficModel candidate_,model_;
 std::array<XPLMDataRef,128> refs_{};std::array<const char*,129> names_{};std::array<float,128> values_{};
 std::array<XPLMObjectRef,8> objects_{};std::array<Token,8> tokens_{};
 std::array<std::array<XPLMInstanceRef,8>,7> instances_{};
 tv::Claim claim_{};std::int64_t renewed_{};tv::Window window_{};tv::Report report_{};std::array<tv::Placement,7> placements_{};float phase_{};
 tv::Capture capture_{};std::array<motion::LocalPose,7> locals_{};float strobe_{};
 std::int64_t next_detail_qpc_{};bool detail_ready_{};
};
}
