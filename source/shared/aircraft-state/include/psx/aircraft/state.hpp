#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psx::aircraft {
inline constexpr std::uint32_t contract_revision = 1;
inline constexpr std::size_t engine_count = 4;
inline constexpr std::size_t gear_count = 5;
inline constexpr std::size_t max_observation_values = 96;

// Semantic enum values are NOT the PSX or Mango selector values.
enum class EngineFamily : std::uint8_t { unknown, ge, pw, rr };
enum class GearAssembly : std::uint8_t { nose, wing_left, wing_right, body_left, body_right };
enum class LeadingEdgeGroup : std::uint8_t {
    left_outboard, left_middle, left_inboard, right_inboard, right_middle, right_outboard
};
enum class TrailingEdgeGroup : std::uint8_t { left_outboard, left_inboard, right_inboard, right_outboard };
enum class Aileron : std::uint8_t { outboard_left, inboard_left, inboard_right, outboard_right };
enum class Elevator : std::uint8_t { outboard_left, inboard_left, inboard_right, outboard_right };
enum class Rudder : std::uint8_t { upper, lower };
enum class ExteriorLight : std::uint8_t {
    landing_outboard_left, landing_outboard_right, landing_inboard_left, landing_inboard_right,
    runway_turnoff_left, runway_turnoff_right, taxi, beacon_upper, beacon_lower,
    navigation_left, navigation_right, strobe, wing, logo
};
enum class SpoilerSegment : std::uint8_t {
    one, two, three, four, five, six_and_seven, eight, nine, ten, eleven, twelve
};
enum class ValueValidity : std::uint8_t { unavailable, valid, invalid, not_applicable };
enum class ValueKind : std::uint8_t { physical, indicated, commanded, derived, configuration };
enum class PropertyGroup : std::uint8_t { configuration, flight, engine, gear, surface, presentation };
enum class PositionReference : std::uint8_t { unknown, psx_flight_deck, psx_internal_unqualified };
enum class HeadingReference : std::uint8_t { unknown, true_north, grid_north };
enum class PitchReference : std::uint8_t { unknown, horizon, boost_conditional };
enum class Unit : std::uint8_t {
    none, boolean, enumeration, degrees, degrees_per_second, metres, metres_per_second,
    percent, ratio, celsius, kilograms_per_second, psi, litres
};
enum class SourceRecord : std::uint16_t {
    unknown, qi1, qi128, qi129, qi257, qs121, boost, qs481, qs482, qs483,
    qs424, qs425, qs436, qs479, qs480, qs363, qs443, qi156, qi157, qi158, qi159, synthetic
};

// Stable semantic identifiers. Component index is zero-based; source wire order
// and target dataref order must be mapped explicitly by their owning modules.
enum class PropertyId : std::uint16_t {
    engine_family = 1, simulation_rate_ratio, paused_all, motion_frozen,
    latitude_deg = 100, longitude_deg, altitude_msl_m, pitch_deg, bank_deg, heading_deg,
    indicated_airspeed_mps, true_airspeed_mps, ground_speed_mps,
    vertical_speed_mps, velocity_north_mps, velocity_east_mps, velocity_down_mps,
    roll_rate_deg_s, pitch_rate_deg_s, yaw_rate_deg_s, on_ground,
    oat_c, tat_c, wind_direction_deg, wind_speed_mps, magnetic_variation_deg,
    engine_n1_percent = 200, engine_n2_percent, engine_n3_percent, engine_combustion,
    engine_running, engine_starter, engine_epr, engine_egt_c, engine_fuel_flow_kg_s,
    engine_tla_deg, engine_reverser_deploy_ratio, engine_command_epr,
    engine_command_n1_percent, engine_oil_pressure_psi, engine_oil_temperature_c,
    engine_oil_quantity_percent, engine_vibration, engine_reverser_lever_locked,
    engine_oil_quantity_litres, thrust_limit_n1_percent, thrust_limit_epr,
    gear_deploy_ratio = 300, gear_door_ratio, gear_contact, gear_strut_m, gear_door_open,
    flap_angle_deg = 400, flap_deploy_ratio, slat_deploy_ratio, spoiler_angle_deg,
    aileron_angle_deg, elevator_angle_deg, rudder_angle_deg, speedbrake_handle_ratio,
    flap_handle_detent, leading_edge_deploy_ratio,
    exterior_light = 500, electrical_power_available, pneumatic_pressure_psi,
    hydraulic_pressure_psi, exterior_light_level
};

struct PropertyInfo {
    PropertyId id;
    const char* name;
    PropertyGroup group;
    Unit unit;
    std::uint8_t elements;
};

// Reserved flap/slat/door/light slots are capacity bounds, NOT a claim that a
// source exports that many independent components. The source matrix defines
// each supported slot; unidentified components must not be published as valid.
inline constexpr PropertyInfo property_catalogue[] = {
    {PropertyId::engine_family, "engine_family", PropertyGroup::configuration, Unit::enumeration, 1},
    {PropertyId::simulation_rate_ratio, "simulation_rate_ratio", PropertyGroup::configuration, Unit::ratio, 1},
    {PropertyId::paused_all, "paused_all", PropertyGroup::configuration, Unit::boolean, 1},
    {PropertyId::motion_frozen, "motion_frozen", PropertyGroup::configuration, Unit::boolean, 1},
    {PropertyId::latitude_deg, "latitude_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::longitude_deg, "longitude_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::altitude_msl_m, "altitude_msl_m", PropertyGroup::flight, Unit::metres, 1},
    {PropertyId::pitch_deg, "pitch_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::bank_deg, "bank_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::heading_deg, "heading_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::indicated_airspeed_mps, "indicated_airspeed_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::true_airspeed_mps, "true_airspeed_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::ground_speed_mps, "ground_speed_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::vertical_speed_mps, "vertical_speed_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::velocity_north_mps, "velocity_north_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::velocity_east_mps, "velocity_east_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::velocity_down_mps, "velocity_down_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::roll_rate_deg_s, "roll_rate_deg_s", PropertyGroup::flight, Unit::degrees_per_second, 1},
    {PropertyId::pitch_rate_deg_s, "pitch_rate_deg_s", PropertyGroup::flight, Unit::degrees_per_second, 1},
    {PropertyId::yaw_rate_deg_s, "yaw_rate_deg_s", PropertyGroup::flight, Unit::degrees_per_second, 1},
    {PropertyId::on_ground, "on_ground", PropertyGroup::flight, Unit::boolean, 1},
    {PropertyId::oat_c, "oat_c", PropertyGroup::flight, Unit::celsius, 1},
    {PropertyId::tat_c, "tat_c", PropertyGroup::flight, Unit::celsius, 1},
    {PropertyId::wind_direction_deg, "wind_direction_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::wind_speed_mps, "wind_speed_mps", PropertyGroup::flight, Unit::metres_per_second, 1},
    {PropertyId::magnetic_variation_deg, "magnetic_variation_deg", PropertyGroup::flight, Unit::degrees, 1},
    {PropertyId::engine_n1_percent, "engine_n1_percent", PropertyGroup::engine, Unit::percent, 4},
    {PropertyId::engine_n2_percent, "engine_n2_percent", PropertyGroup::engine, Unit::percent, 4},
    {PropertyId::engine_n3_percent, "engine_n3_percent", PropertyGroup::engine, Unit::percent, 4},
    {PropertyId::engine_combustion, "engine_combustion", PropertyGroup::engine, Unit::boolean, 4},
    {PropertyId::engine_running, "engine_running", PropertyGroup::engine, Unit::boolean, 4},
    {PropertyId::engine_starter, "engine_starter", PropertyGroup::engine, Unit::boolean, 4},
    {PropertyId::engine_epr, "engine_epr", PropertyGroup::engine, Unit::ratio, 4},
    {PropertyId::engine_egt_c, "engine_egt_c", PropertyGroup::engine, Unit::celsius, 4},
    {PropertyId::engine_fuel_flow_kg_s, "engine_fuel_flow_kg_s", PropertyGroup::engine, Unit::kilograms_per_second, 4},
    {PropertyId::engine_tla_deg, "engine_tla_deg", PropertyGroup::engine, Unit::degrees, 4},
    {PropertyId::engine_reverser_deploy_ratio, "engine_reverser_deploy_ratio", PropertyGroup::engine, Unit::ratio, 4},
    {PropertyId::engine_command_epr, "engine_command_epr", PropertyGroup::engine, Unit::ratio, 4},
    {PropertyId::engine_command_n1_percent, "engine_command_n1_percent", PropertyGroup::engine, Unit::percent, 4},
    {PropertyId::engine_oil_pressure_psi, "engine_oil_pressure_psi", PropertyGroup::engine, Unit::psi, 4},
    {PropertyId::engine_oil_temperature_c, "engine_oil_temperature_c", PropertyGroup::engine, Unit::celsius, 4},
    {PropertyId::engine_oil_quantity_percent, "engine_oil_quantity_percent", PropertyGroup::engine, Unit::percent, 4},
    {PropertyId::engine_vibration, "engine_vibration", PropertyGroup::engine, Unit::none, 4},
    {PropertyId::engine_reverser_lever_locked, "engine_reverser_lever_locked", PropertyGroup::engine, Unit::boolean, 4},
    {PropertyId::engine_oil_quantity_litres, "engine_oil_quantity_litres", PropertyGroup::engine, Unit::litres, 4},
    {PropertyId::thrust_limit_n1_percent, "thrust_limit_n1_percent", PropertyGroup::engine, Unit::percent, 1},
    {PropertyId::thrust_limit_epr, "thrust_limit_epr", PropertyGroup::engine, Unit::ratio, 1},
    {PropertyId::gear_deploy_ratio, "gear_deploy_ratio", PropertyGroup::gear, Unit::ratio, 5},
    {PropertyId::gear_door_ratio, "gear_door_ratio", PropertyGroup::gear, Unit::ratio, 16},
    {PropertyId::gear_contact, "gear_contact", PropertyGroup::gear, Unit::boolean, 5},
    {PropertyId::gear_strut_m, "gear_strut_m", PropertyGroup::gear, Unit::metres, 5},
    {PropertyId::gear_door_open, "gear_door_open", PropertyGroup::gear, Unit::boolean, 5},
    {PropertyId::flap_angle_deg, "flap_angle_deg", PropertyGroup::surface, Unit::degrees, 4},
    {PropertyId::flap_deploy_ratio, "flap_deploy_ratio", PropertyGroup::surface, Unit::ratio, 4},
    {PropertyId::slat_deploy_ratio, "slat_deploy_ratio", PropertyGroup::surface, Unit::ratio, 16},
    {PropertyId::spoiler_angle_deg, "spoiler_angle_deg", PropertyGroup::surface, Unit::degrees, 11},
    {PropertyId::aileron_angle_deg, "aileron_angle_deg", PropertyGroup::surface, Unit::degrees, 4},
    {PropertyId::elevator_angle_deg, "elevator_angle_deg", PropertyGroup::surface, Unit::degrees, 4},
    {PropertyId::rudder_angle_deg, "rudder_angle_deg", PropertyGroup::surface, Unit::degrees, 2},
    {PropertyId::speedbrake_handle_ratio, "speedbrake_handle_ratio", PropertyGroup::surface, Unit::ratio, 1},
    {PropertyId::flap_handle_detent, "flap_handle_detent", PropertyGroup::surface, Unit::enumeration, 1},
    {PropertyId::leading_edge_deploy_ratio, "leading_edge_deploy_ratio", PropertyGroup::surface, Unit::ratio, 6},
    {PropertyId::exterior_light, "exterior_light", PropertyGroup::presentation, Unit::boolean, 16},
    {PropertyId::electrical_power_available, "electrical_power_available", PropertyGroup::presentation, Unit::boolean, 16},
    {PropertyId::pneumatic_pressure_psi, "pneumatic_pressure_psi", PropertyGroup::presentation, Unit::psi, 16},
    {PropertyId::hydraulic_pressure_psi, "hydraulic_pressure_psi", PropertyGroup::presentation, Unit::psi, 4},
    {PropertyId::exterior_light_level, "exterior_light_level", PropertyGroup::presentation, Unit::enumeration, 14}
};

constexpr const PropertyInfo* property_info(PropertyId id) noexcept {
    for (const auto& info : property_catalogue) if (info.id == id) return &info;
    return nullptr;
}

struct SampleStamp {
    std::uint64_t session_epoch{};
    std::uint64_t configuration_epoch{};
    std::uint64_t sequence{};
    std::int64_t received_qpc{}; // Receive-complete on this PC; NOT PSX sample time.
};
struct PropertyValue {
    PropertyId id{PropertyId::engine_family};
    std::uint8_t element{};
    ValueValidity validity{ValueValidity::unavailable};
    ValueKind kind{ValueKind::physical};
    double value{};
};

inline bool well_formed(const PropertyValue& v) noexcept {
    const auto* info = property_info(v.id);
    if (!info || v.element >= info->elements ||
        static_cast<unsigned>(v.validity) > static_cast<unsigned>(ValueValidity::not_applicable) ||
        static_cast<unsigned>(v.kind) > static_cast<unsigned>(ValueKind::configuration)) return false;
    if (v.validity != ValueValidity::valid) return true; // Numeric storage has no meaning.
    if (!std::isfinite(v.value)) return false;
    if (info->unit == Unit::boolean && v.value != 0.0 && v.value != 1.0) return false;
    if (info->unit == Unit::enumeration && std::trunc(v.value) != v.value) return false;
    if (v.id == PropertyId::engine_family && (v.value < 0 || v.value > 3)) return false;
    if (v.id == PropertyId::exterior_light_level && (v.value < 0 || v.value > 9)) return false;
    if (v.id == PropertyId::latitude_deg && (v.value < -90 || v.value > 90)) return false;
    if (v.id == PropertyId::longitude_deg && (v.value < -180 || v.value > 180)) return false;
    return true; // Source/profile-specific domains are not invented here.
}

struct Observation {
    SampleStamp stamp{};
    SourceRecord source{SourceRecord::unknown};
    EngineFamily family{EngineFamily::unknown};
    PositionReference position_reference{PositionReference::unknown};
    HeadingReference heading_reference{HeadingReference::unknown};
    PitchReference pitch_reference{PitchReference::unknown};
    // Retained Boost scheduler field; no invented epoch/unwrapping in Step 1.
    std::uint16_t source_millisecond_mod1000{};
    bool has_source_millisecond{};
    std::array<PropertyValue, max_observation_values> values{};
    std::uint16_t count{};

    const PropertyValue* find(PropertyId id, std::uint8_t element = 0) const noexcept {
        if (count > values.size()) return nullptr;
        for (std::size_t i = 0; i < count; ++i)
            if (values[i].id == id && values[i].element == element) return &values[i];
        return nullptr;
    }
    bool add(PropertyId id, std::uint8_t element, double value,
             ValueKind kind = ValueKind::physical,
             ValueValidity validity = ValueValidity::valid) noexcept {
        PropertyValue v{id, element, validity, kind, value};
        if (count >= values.size() || find(id, element) || !well_formed(v)) return false;
        values[count++] = v;
        return true;
    }
};

inline bool well_formed(const Observation& o) noexcept {
    if (o.count > o.values.size() ||
        static_cast<unsigned>(o.family) > static_cast<unsigned>(EngineFamily::rr) ||
        static_cast<unsigned>(o.source) > static_cast<unsigned>(SourceRecord::synthetic) ||
        static_cast<unsigned>(o.position_reference) > static_cast<unsigned>(PositionReference::psx_internal_unqualified) ||
        static_cast<unsigned>(o.heading_reference) > static_cast<unsigned>(HeadingReference::grid_north) ||
        static_cast<unsigned>(o.pitch_reference) > static_cast<unsigned>(PitchReference::boost_conditional) ||
        (o.has_source_millisecond && o.source_millisecond_mod1000 > 999)) return false;
    for (std::size_t i = 0; i < o.count; ++i) {
        if (!well_formed(o.values[i])) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (o.values[i].id == o.values[j].id && o.values[i].element == o.values[j].element) return false;
    }
    return true;
}

inline constexpr double feet_to_metres = 0.3048;
inline constexpr double knots_to_metres_per_second = 1852.0 / 3600.0;
inline constexpr double radians_to_degrees = 180.0 / 3.1415926535897932384626433832795;
inline constexpr double kilograms_per_hour_to_second = 1.0 / 3600.0;

static_assert(std::is_trivially_copyable<Observation>::value, "Bounded value handoff required");
static_assert(std::is_standard_layout<Observation>::value, "No hidden runtime ownership");
} // namespace psx::aircraft
