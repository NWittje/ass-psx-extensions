#pragma once
#include <cstdint>

namespace psx::motion {
enum class PoseSource : std::uint8_t { public_main_boost=0, exporter=1 };
enum class SourcePreference : std::uint8_t { automatic=0, public_only=1 };
enum class SelectionReason : std::uint8_t { startup, exporter_ready, exporter_timeout, exporter_unavailable, public_unavailable, preference, discontinuity, connection };
// Revision 1 retained PSX's left-positive bank as if it were right-positive.
// Revision 2 corrects that source normalization; raw PXSE/Boost data is unchanged.
inline constexpr std::uint32_t legacy_reference_revision=1;
inline constexpr std::uint32_t instant_reference_revision=2;
inline bool known_reference_revision(std::uint32_t revision) noexcept {
    return revision==legacy_reference_revision||revision==instant_reference_revision;
}
inline constexpr std::uint32_t exporter_timeout_ms=100;
inline constexpr std::uint32_t exporter_stable_ms=500;
struct InstanceId { std::uint64_t high{},low{}; };
inline bool operator==(InstanceId a,InstanceId b) noexcept {return a.high==b.high&&a.low==b.low;}
inline bool operator!=(InstanceId a,InstanceId b) noexcept {return !(a==b);}
inline bool valid(InstanceId a) noexcept {return a.high||a.low;}
struct PoseDomain {
    InstanceId producer{};
    std::uint64_t pose_epoch{};
    std::uint32_t reference_revision{instant_reference_revision};
};
inline bool operator==(const PoseDomain& a,const PoseDomain& b) noexcept {return a.producer==b.producer&&a.pose_epoch==b.pose_epoch&&a.reference_revision==b.reference_revision;}
inline bool operator!=(const PoseDomain& a,const PoseDomain& b) noexcept {return !(a==b);}
// Historical capture readers can validate revision 1. Live consumers must also
// require current_reference() before accepting an exporter source/window.
inline bool valid(const PoseDomain& d) noexcept {return valid(d.producer)&&d.pose_epoch&&known_reference_revision(d.reference_revision);}
inline bool current_reference(const PoseDomain& d) noexcept {return valid(d)&&d.reference_revision==instant_reference_revision;}
// Exporter provenance is separate from the historical public Main/Boost pair.
// Zero/default exporter fields never confer source validity.
struct PoseProvenance {
    PoseSource kind{PoseSource::public_main_boost};
    std::uint64_t selection_epoch{};
    PoseDomain domain{};
    std::uint64_t left_sequence{},right_sequence{},frame_sequence{};
    std::int64_t sample_start_qpc{},sample_end_qpc{},received_qpc{};
};
} // namespace psx::motion
