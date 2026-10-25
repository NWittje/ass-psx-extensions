#pragma once
#include "motion_support.hpp"
#include "sdk_api.hpp"
#include <psx/diagnostics/journal.hpp>
#include <XPLMUtilities.h>
#include <XPLMScenery.h>

namespace psx::xp {
struct MotionReadiness {
    bool bindings{}, reference{}, required{}, reconciling{}, call_active{};
    std::uint64_t attempt{};
    std::int64_t started_qpc{},returned_qpc{};
    motion::PresentationPose target{};
};
class MotionController final {
public:
    void begin(SdkApi&,std::uint64_t load,std::uint64_t enable,diagnostics::DiagnosticProducer,
               std::int64_t (*clock)() noexcept,std::uint64_t frequency,std::uint32_t boost_timeout_ms=motion::default_boost_timeout_ms,
               std::uint32_t callback_delayed_ms=diagnostics::Config{}.delayed_ms) noexcept;
    void prepare_bindings() noexcept; // Enable / SDK lifecycle handler; no per-frame lookup.
    void retire_resources() noexcept;
    void lifecycle(std::uint64_t aircraft,std::uint64_t scenery,bool aircraft_replaced,bool unloaded) noexcept;
    void profile(const MotionProfile&) noexcept;
    void connection(std::uint64_t id) noexcept;
    void boundary(const bridge::SessionStatus&) noexcept;
    void selection(const bridge::SourceSelection&) noexcept;
    void request(const bridge::MotionRequest&,const bridge::LatestState*,bool connected,const bridge::CompletePoseWindow* complete=nullptr) noexcept;
    void pre(const bridge::LatestState*,bool connected,const bridge::CompletePoseWindow* complete=nullptr) noexcept;
    void post(const bridge::LatestState*,bool connected,const bridge::MotionWindow* window=nullptr,const bridge::CompletePoseWindow* complete=nullptr) noexcept;
    bool stop(motion::Reason=motion::Reason::disabled) noexcept; // Release owned override; SDK still active. Never pauses.
    bool pop_ack(bridge::MotionAck& ack) noexcept;
    void transport_overflow() noexcept;
    void callback_duration(bool pre,std::uint64_t ticks) noexcept {(pre?report_.pre:report_.post).add(ticks);}
    const MotionReport& report() const noexcept {return report_;}
    bool placing() const noexcept {return in_place_call_;}
    bool following() const noexcept {return follow_;}
    MotionReadiness readiness() const noexcept;
    bool readiness_bindings() const;
    bool readiness_reference() const;
    bool readiness_readback() const;
    using ReadinessSink=void (*)(void*) noexcept;
    void readiness_sink(ReadinessSink sink,void* context) noexcept {readiness_sink_=sink;readiness_context_=context;}
private:
    SdkApi* sdk_{};
    XPLMDataRef override_{},paused_{},lat_ref_{},lon_ref_{};
    std::array<XPLMDataRef,3> xyz_{},euler_{},head_{}; // Head is optional view telemetry, never geometry authority.
    XPLMDataRef cg_y_{},cg_z_{};
    XPLMCommandRef pause_off_{};
    XPLMProbeRef probe_{};
    diagnostics::DiagnosticProducer events_;
    MotionReport report_{};
    MotionProfile profile_{};
    std::int64_t (*clock_)() noexcept{};
    std::uint64_t frequency_{},last_operation_{},seen_session_{},seen_epoch_{};
    std::uint32_t boost_timeout_ms_{},callback_delayed_ms_{};
    bool bound_{},owned_{},follow_{},have_pose_{},have_written_{},reconcile_{},need_initial_{},in_place_call_{},relocating_{},placement_ready_{},faulted_{};
    bool placement_applied_{};
    bool readiness_destination_{}; // Status-only actual destination work, independent of source holds.
    bool reference_diagnostic_pending_{true},reference_sampled_{},previous_reference_valid_{};
    bool unpause_attempted_{},pause_inputs_sampled_{};
    bool frozen_pose_changed_{},frozen_pose_baseline_valid_{};
    motion::GeoPose frozen_pose_baseline_{};
    unsigned unpause_result_{};
    std::int64_t last_unpause_attempt_{};
    std::uint64_t unpause_attempts_{};
    std::array<std::uint64_t,5> previous_pause_inputs_{};
    motion::PresentationPose placed_{};
    motion::PresentationPose readiness_target_{};
    std::uint64_t placement_attempt_{};
    std::int64_t placement_returned_{};
    ReadinessSink readiness_sink_{};void* readiness_context_{};
    motion::HistoryDomain seen_domain_{};
    bridge::SourceSelection selection_{};
    std::uint64_t exporter_floor_{};
    std::uint32_t exporter_branch_{};
    bool selection_known_{},handoff_pending_{};
    motion::PresentationPose anchor_{};
    motion::ReconstructionTrace anchor_trace_{},applied_trace_{};
    std::int64_t cursor_{},hold_started_{},last_eligible_{};
    motion::ReconstructionState hold_state_{motion::ReconstructionState::unavailable};
    bool reconstruction_ready_{},anchor_valid_{};
    std::int64_t placement_started_{},last_post_{};
    double previous_lat_ref_{},previous_lon_ref_{};
    bridge::MotionRequest operation_{};
    std::array<bridge::MotionAck,64> acks_{}; // SDK-thread local staging; bounded handoff follows.
    std::size_t ack_read_{},ack_write_{};
    void ack(const bridge::MotionRequest&,motion::AckResult,motion::Reason) noexcept;
    void transition(motion::Phase,motion::Reason) noexcept;
    void flags() noexcept;
    void unpause_connected(bool connected) noexcept; // Pre-model only; independent of Follow/profile/source readiness.
    void pause_diagnostics(const bridge::LatestState*,bool connected) noexcept;
    void frozen_pose_diagnostics(const bridge::LatestState*,const motion::PresentationPose&,bool frozen) noexcept;
    bool reference_valid() noexcept;
    int override_value() noexcept;
    motion::Reason source(const bridge::LatestState*,bool,motion::PresentationPose&,bool&,
        const bridge::CompletePoseWindow*,motion::SourcePreference) const noexcept;
    bool complete_matches(const bridge::CompletePoseWindow*) const noexcept;
    void exporter_boundary(const bridge::CompletePoseWindow*) noexcept;
    bool current_pose(const motion::PresentationPose&) const noexcept;
    bool reconstruct_target(const bridge::LatestState&,const bridge::MotionWindow*,const bridge::CompletePoseWindow*,
        const motion::PresentationPose&,std::int64_t,motion::ReconstructionResult&) noexcept;
    bool project(const motion::PresentationPose&,motion::LocalPose&) noexcept;
    bool read(motion::LocalPose&) noexcept;
    bool apply(const motion::PresentationPose&) noexcept;
    bool competing() noexcept;
    void fault(motion::Reason) noexcept;
    void complete(motion::AckResult,motion::Reason) noexcept;
    void place(const motion::PresentationPose&) noexcept;
    void reset_reconstruction() noexcept;
    void suspend_reconstruction() noexcept;
    void close_hold() noexcept;
    void seed_anchor(const motion::PresentationPose&,const motion::ReconstructionTrace* =nullptr) noexcept;
    void reconstruction_metrics(std::int64_t now) noexcept;
};
}
