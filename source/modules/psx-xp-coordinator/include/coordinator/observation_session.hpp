#pragma once
#include "coordinator/aircraft_decoder.hpp"
#include <psx/bridge/protocol.hpp>
#include <array>
#include <string_view>

namespace coordinator {
enum class SourceStream : std::uint8_t { main=1, boost=2 };
enum class SessionEvent : std::uint8_t { wanted_on=1, wanted_off=2, main_up=3, main_down=4, boost_up=5, boost_down=6 };
struct RawSource {
    std::array<char,1007> text{};
    std::uint16_t size{};
};
struct LineResult {
    bool selected{}, malformed{}, remote_exit{}, bootstrap_error{}, motion_boundary{};
    std::int64_t decoded_qpc{};psx::aircraft::SourceRecord source{};std::uint8_t rejection_reason{};
    double jump_horizontal_m{},jump_vertical_m{},jump_interval_s{},jump_rate{1};
};
struct MotionDetectorCheckpoint {
    std::uint64_t frequency{};bool available{};psx::motion::GeoPose previous{};std::int64_t received_qpc{};
};
// Read-only Qi129 register observation. Availability never implies scheduler
// completion; ECON receipt age alone does not invalidate an unchanged value.
struct PauseSourceState {
    bool available{},paused_all{},motion_frozen{};
    std::uint8_t raw_value{};
    psx::aircraft::SampleStamp stamp{};
};
PauseSourceState pause_source_state(const psx::bridge::LatestState&) noexcept;
bool pause_source_changed(const PauseSourceState&,const PauseSourceState&) noexcept;
// Pure single-owner lifecycle/decoder. Timestamp arguments are supplied, so live
// ingestion and offline replay execute identical semantic transitions.
class ObservationSession {
public:
    explicit ObservationSession(std::uint64_t frequency=0,bool motion=true);
    void clock_frequency(std::uint64_t frequency) noexcept {detector_.frequency=frequency;detector_.available=false;}
    const MotionDetectorCheckpoint& detector_checkpoint() const noexcept{return detector_;}
    const psx::motion::MotionHistory& history() const noexcept{return history_;}
    bool restore_history(const psx::motion::SampleWindow&,const psx::motion::HistoryCounters&) noexcept;
    bool restore_detector(const MotionDetectorCheckpoint&) noexcept;
    void event(SessionEvent, std::int64_t qpc);
    LineResult line(SourceStream, std::string_view, std::int64_t receive, std::int64_t decoded,
                    std::int64_t (*completion_clock)() noexcept = nullptr);
    const psx::bridge::LatestState& latest() const noexcept { return latest_; }
    const std::array<RawSource,psx::bridge::source_slots>& raw() const noexcept { return raw_; }
    std::uint64_t sequence() const noexcept { return sequence_; }
    std::uint8_t bootstrap() const noexcept { return bootstrap_; }
    bool identified() const noexcept { return identified_; }
    bool restore(const psx::bridge::SessionStatus&, std::uint64_t sequence, std::uint8_t bootstrap, bool identified);
    bool seed(const psx::bridge::ObservationMessage&, std::string_view raw);
    static bool selected(SourceStream, std::string_view) noexcept;
private:
    void publish(std::int64_t);
    void new_session(psx::motion::Boundary=psx::motion::Boundary::attachment);
    void boundary(psx::motion::Boundary,std::uint64_t floor,std::int64_t time);
    void detect_jump(const psx::aircraft::Observation&,LineResult&,std::int64_t);
    PsxAircraftDecoder decoder_;
    psx::bridge::LatestState latest_;
    psx::bridge::SessionStatus state_;
    std::array<RawSource,psx::bridge::source_slots> raw_{};
    std::uint64_t sequence_{};
    std::uint8_t bootstrap_{};
    bool identified_{};
    bool motion_enabled_{true};
    MotionDetectorCheckpoint detector_;
    psx::motion::MotionHistory history_;
};
}
