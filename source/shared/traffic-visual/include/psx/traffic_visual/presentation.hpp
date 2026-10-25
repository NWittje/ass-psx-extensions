#pragma once
#include <psx/traffic_visual/protocol.hpp>
namespace psx::traffic_visual {
enum class SlotState:std::uint32_t {unavailable,inactive,invalid,stale,oldest_hold,interpolated,newest_hold,gap_hold,direct};
struct PoseSample {std::uint64_t sequence{},frame{},generation{};std::int64_t observed{},finished{},received{};double latitude_deg{},longitude_deg{},altitude_msl_m{};motion::Quaternion attitude{};};
struct SlotWindow {std::uint64_t generation{};std::size_t count{};std::array<PoseSample,history_capacity> samples{};SlotState unavailable{SlotState::unavailable};};
struct Window {motion::InstanceId producer{};std::uint64_t connection{},selection{},lifecycle{},frequency{},sequence{},frame{},revision{};std::uint32_t process{};std::uint64_t birth{};std::int64_t source_observed{};std::array<SlotWindow,7> slots{};};
class History {
// Receipt stays an actual local timestamp; uncertainty bounds only admission.
public:bool accept(const traffic_source::Sample&,std::int64_t received,std::int64_t uncertainty=0) noexcept;void clear() noexcept {window_={};}const Window& window() const noexcept {return window_;}
private:Window window_{};
};
struct Placement {bool visible{};SlotState state{SlotState::unavailable};motion::GeoPose geo{};std::uint64_t generation{},left_sequence{},right_sequence{},left_frame{},right_frame{};std::int64_t target_qpc{},presentation_qpc{},source_observed_qpc{},left_observed_qpc{},right_observed_qpc{},left_received_qpc{},right_received_qpc{};double alpha{};};
// Anchor revision1 -> generic OBJ origin, zero anatomical/eye/CG displacement.
inline constexpr motion::Vec3 anchor_from_model_origin_m{0,0,0};
bool reconstruct(const Window&,std::size_t slot,std::int64_t now,std::uint32_t delay_ms,Placement&,std::uint32_t presentation_mode=0,std::int64_t uncertainty=0) noexcept;
bool held(SlotState) noexcept;
}
