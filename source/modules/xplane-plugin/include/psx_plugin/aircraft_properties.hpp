#pragma once

#include <psx/aircraft/state.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::xp {
// These are profile-local destinations, not shared observation or wire types.
enum class AircraftTarget : std::uint8_t {
    mango_engine_family, engine_n1, engine_n2, gear_deployment,
    flap_inboard_left, flap_inboard_right, flap_outboard_left, flap_outboard_right,
    count
};
enum class TargetType : std::uint8_t { integer, scalar_float, float_array };
enum class Qualification : std::uint32_t {
    binding = 1, writer_ownership = 2, consumer_link = 4,
    provider_lifetime = 8, sound_bank_parameters = 16
};
using QualificationMask = std::uint32_t;
constexpr QualificationMask mask(Qualification q) noexcept { return static_cast<QualificationMask>(q); }

struct AircraftTargetSpec {
    AircraftTarget id;
    const char* dataref;
    TargetType type;
    psx::aircraft::Unit unit;
    std::uint8_t catalogue_elements;
    QualificationMask required;
};

class FelisMangoProfile final {
public:
    static constexpr const char* aircraft_file = "B742_Cargo_Felis_XP12.acf";
    static constexpr const char* version = "1.3.0 F";
    static constexpr const char* acf_sha256 = "00E7D19CBD6F053F9DECD2E58FF72F9341C7D4DB970AD68A9603616872EA9B23";
    static constexpr const char* snd_sha256 = "0362C06C150CA8F8684C0EE7FC15552DA6E7783753C3A36D3BC339BC5C30F23D";
    static const AircraftTargetSpec* target(AircraftTarget id) noexcept;
    // 0 indicates unsupported/unknown, never a valid Mango selector.
    static int mango_selector(psx::aircraft::EngineFamily family) noexcept;
    // -1 indicates an invalid enum. ACF order: N, RB, LB, RW, LW.
    static int gear_index(psx::aircraft::GearAssembly assembly) noexcept;
};

enum class PlanStatus : std::uint8_t {
    ready, malformed_observation, inactive_context, wrong_target,
    session_mismatch, configuration_mismatch, inconsistent_family
};
enum class CapabilityReason : std::uint8_t {
    source_unavailable, source_invalid, source_not_applicable, wrong_value_kind,
    source_domain, family_unknown, rr_core_spool_unqualified,
    readonly_consumer, component_semantics_unqualified, ratio_angle_curve_missing,
    custom_provider_unqualified, no_demonstrated_consumer,
    commanded_not_actual, motion_contract_deferred, sound_ownership_deferred
};
struct MappingContext {
    bool active{};
    bool target_identity_confirmed{}; // Offline caller evidence only, never auto-detection.
    std::uint64_t session_epoch{};
    std::uint64_t configuration_epoch{};
};
struct PlannedAircraftWrite {
    psx::aircraft::PropertyId source{};
    std::uint8_t source_element{};
    AircraftTarget target{};
    std::uint8_t target_element{};
    TargetType type{};
    double value{}; // Exact integer or float-representable finite value, discriminated above.
    QualificationMask required{}; // Always unresolved by this pure planner.
};
struct CapabilityBlock {
    psx::aircraft::PropertyId property{};
    std::uint8_t element{};
    CapabilityReason reason{};
};
struct AircraftWritePlan {
    PlanStatus status{PlanStatus::inactive_context};
    psx::aircraft::SampleStamp stamp{};
    std::array<PlannedAircraftWrite, psx::aircraft::max_observation_values> writes{};
    std::array<CapabilityBlock, psx::aircraft::max_observation_values> blocked{};
    std::uint16_t write_count{};
    std::uint16_t blocked_count{};
};

class AircraftPropertyMapper final {
public:
    // Pure/offline: no SDK, reads, writes, file access, clocks, cache or allocation.
    static AircraftWritePlan plan(const psx::aircraft::Observation& observation,
                                 const MappingContext& context) noexcept;
};
} // namespace psx::xp
