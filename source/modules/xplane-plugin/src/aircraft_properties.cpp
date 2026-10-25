#include "psx_plugin/aircraft_properties.hpp"
#include <cmath>
#include <limits>

namespace psx::xp {
using namespace psx::aircraft;
namespace {
constexpr auto basic = mask(Qualification::binding) | mask(Qualification::writer_ownership);
constexpr auto engine = basic | mask(Qualification::consumer_link) | mask(Qualification::sound_bank_parameters);
constexpr std::array<AircraftTargetSpec, static_cast<std::size_t>(AircraftTarget::count)> targets{{
    {AircraftTarget::mango_engine_family, "MangoStudios/742/engine_type", TargetType::integer, Unit::enumeration, 1,
        basic | mask(Qualification::provider_lifetime) | mask(Qualification::sound_bank_parameters)},
    {AircraftTarget::engine_n1, "sim/flightmodel/engine/ENGN_N1_", TargetType::float_array, Unit::percent, 16, engine},
    {AircraftTarget::engine_n2, "sim/flightmodel/engine/ENGN_N2_", TargetType::float_array, Unit::percent, 16, engine},
    {AircraftTarget::gear_deployment, "sim/aircraft/parts/acf_gear_deploy", TargetType::float_array, Unit::ratio, 10,
        basic | mask(Qualification::consumer_link)},
    {AircraftTarget::flap_inboard_left, "sim/flightmodel/controls/wing1l_fla1def", TargetType::scalar_float, Unit::degrees, 1, basic},
    {AircraftTarget::flap_inboard_right, "sim/flightmodel/controls/wing1r_fla1def", TargetType::scalar_float, Unit::degrees, 1, basic},
    {AircraftTarget::flap_outboard_left, "sim/flightmodel/controls/wing3l_fla2def", TargetType::scalar_float, Unit::degrees, 1, basic},
    {AircraftTarget::flap_outboard_right, "sim/flightmodel/controls/wing3r_fla2def", TargetType::scalar_float, Unit::degrees, 1, basic}
}};
void block(AircraftWritePlan& plan, const PropertyValue& value, CapabilityReason reason) noexcept {
    plan.blocked[plan.blocked_count++] = {value.id,value.element,reason};
}
void candidate(AircraftWritePlan& plan, const PropertyValue& source,
               AircraftTarget target, int element, double value) noexcept {
    const auto& spec = *FelisMangoProfile::target(target);
    plan.writes[plan.write_count++] = {source.id,source.element,target,static_cast<std::uint8_t>(element),
        spec.type,value,spec.required};
}
bool valid_physical(AircraftWritePlan& plan, const PropertyValue& v) noexcept {
    if (v.kind != ValueKind::physical) {
        block(plan,v,v.kind == ValueKind::commanded ? CapabilityReason::commanded_not_actual : CapabilityReason::wrong_value_kind);
        return false;
    }
    return true;
}
}
const AircraftTargetSpec* FelisMangoProfile::target(AircraftTarget id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < targets.size() ? &targets[index] : nullptr;
}
int FelisMangoProfile::mango_selector(EngineFamily family) noexcept {
    switch(family) {
    case EngineFamily::ge: return 3;
    case EngineFamily::pw: return 1;
    case EngineFamily::rr: return 2;
    default: return 0;
    }
}
int FelisMangoProfile::gear_index(GearAssembly assembly) noexcept {
    switch(assembly) {
    case GearAssembly::nose: return 0;
    case GearAssembly::wing_left: return 4;
    case GearAssembly::wing_right: return 3;
    case GearAssembly::body_left: return 2;
    case GearAssembly::body_right: return 1;
    default: return -1;
    }
}
AircraftWritePlan AircraftPropertyMapper::plan(const Observation& o, const MappingContext& c) noexcept {
    AircraftWritePlan out{};
    out.stamp = o.stamp;
    if (!well_formed(o)) { out.status=PlanStatus::malformed_observation; return out; }
    if (!c.active || c.session_epoch == 0 || c.configuration_epoch == 0) return out;
    if (!c.target_identity_confirmed) { out.status=PlanStatus::wrong_target; return out; }
    if (o.stamp.session_epoch != c.session_epoch) { out.status=PlanStatus::session_mismatch; return out; }
    if (o.stamp.configuration_epoch != c.configuration_epoch) { out.status=PlanStatus::configuration_mismatch; return out; }
    const auto* family = o.find(PropertyId::engine_family);
    if (family && family->validity == ValueValidity::valid && family->value != static_cast<double>(o.family)) {
        out.status=PlanStatus::inconsistent_family; return out;
    }
    out.status=PlanStatus::ready;
    for (std::size_t i=0; i<o.count; ++i) {
        const auto& v=o.values[i];
        if (v.validity != ValueValidity::valid) {
            auto reason=CapabilityReason::source_unavailable;
            if (v.validity == ValueValidity::invalid) reason=CapabilityReason::source_invalid;
            if (v.validity == ValueValidity::not_applicable) reason=CapabilityReason::source_not_applicable;
            block(out,v,reason); continue;
        }
        switch(v.id) {
        case PropertyId::engine_family: {
            if (v.kind != ValueKind::configuration) { block(out,v,CapabilityReason::wrong_value_kind); break; }
            const int selection=FelisMangoProfile::mango_selector(o.family);
            if (!selection) block(out,v,CapabilityReason::family_unknown);
            else candidate(out,v,AircraftTarget::mango_engine_family,0,selection);
            break;
        }
        case PropertyId::engine_n1_percent:
        case PropertyId::engine_n2_percent:
            if (!valid_physical(out,v)) break;
            if (o.family == EngineFamily::unknown) { block(out,v,CapabilityReason::family_unknown); break; }
            if (v.id == PropertyId::engine_n2_percent && o.family == EngineFamily::rr) {
                block(out,v,CapabilityReason::rr_core_spool_unqualified); break;
            }
            // No invented percent-of-redline cap: the shared/source contract owns
            // physical operating limits. Negative speed and float overflow fail.
            if (v.value < 0 || v.value > std::numeric_limits<float>::max()) {
                block(out,v,CapabilityReason::source_domain); break;
            }
            candidate(out,v,v.id == PropertyId::engine_n1_percent ? AircraftTarget::engine_n1 : AircraftTarget::engine_n2,
                      v.element,v.value);
            break;
        case PropertyId::engine_n3_percent:
            block(out,v,CapabilityReason::rr_core_spool_unqualified); break;
        case PropertyId::gear_deploy_ratio:
            // The coordinator's audited Qs424 extent/20000 normalization is
            // derived telemetry of actual deployment, not a gear command.
            if (!(v.kind == ValueKind::derived && o.source == SourceRecord::qs424) && !valid_physical(out,v)) break;
            if (v.value < 0 || v.value > 1) { block(out,v,CapabilityReason::source_domain); break; }
            candidate(out,v,AircraftTarget::gear_deployment,FelisMangoProfile::gear_index(static_cast<GearAssembly>(v.element)),v.value);
            break;
        case PropertyId::flap_deploy_ratio:
            block(out,v,CapabilityReason::ratio_angle_curve_missing); break;
        case PropertyId::flap_angle_deg:
        case PropertyId::slat_deploy_ratio:
        case PropertyId::leading_edge_deploy_ratio:
        case PropertyId::aileron_angle_deg:
        case PropertyId::elevator_angle_deg:
        case PropertyId::rudder_angle_deg:
        case PropertyId::spoiler_angle_deg:
            block(out,v,CapabilityReason::component_semantics_unqualified); break;
        case PropertyId::engine_reverser_deploy_ratio:
        case PropertyId::engine_combustion:
        case PropertyId::gear_strut_m:
        case PropertyId::gear_contact:
            block(out,v,CapabilityReason::readonly_consumer); break;
        case PropertyId::gear_door_ratio:
        case PropertyId::gear_door_open:
        case PropertyId::hydraulic_pressure_psi:
        case PropertyId::pneumatic_pressure_psi:
            block(out,v,CapabilityReason::custom_provider_unqualified); break;
        case PropertyId::engine_tla_deg:
        case PropertyId::engine_command_epr:
        case PropertyId::engine_command_n1_percent:
        case PropertyId::thrust_limit_n1_percent:
        case PropertyId::thrust_limit_epr:
        case PropertyId::speedbrake_handle_ratio:
        case PropertyId::flap_handle_detent:
            block(out,v,CapabilityReason::commanded_not_actual); break;
        case PropertyId::engine_reverser_lever_locked:
            block(out,v,CapabilityReason::component_semantics_unqualified); break;
        case PropertyId::exterior_light:
        case PropertyId::exterior_light_level:
        case PropertyId::electrical_power_available:
            block(out,v,CapabilityReason::component_semantics_unqualified); break;
        default:
            if (property_info(v.id)->group == PropertyGroup::flight || property_info(v.id)->group == PropertyGroup::configuration)
                block(out,v,CapabilityReason::motion_contract_deferred);
            else block(out,v,CapabilityReason::no_demonstrated_consumer);
            break;
        }
    }
    return out;
}
} // namespace psx::xp
