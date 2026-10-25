#include "coordinator/aircraft_decoder.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace coordinator {
namespace {
namespace a = psx::aircraft;
using Id = a::PropertyId;
using Kind = a::ValueKind;
using Valid = a::ValueValidity;
using Record = a::SourceRecord;

bool integer(std::string_view token, std::int32_t& value) noexcept {
    if (token.empty()) return false;
    auto r = std::from_chars(token.data(), token.data() + token.size(), value);
    return r.ec == std::errc{} && r.ptr == token.data() + token.size();
}
bool real(std::string_view token, double& value) noexcept {
    if (token.empty()) return false;
    auto r = std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
    return r.ec == std::errc{} && r.ptr == token.data() + token.size() && std::isfinite(value);
}
template<std::size_t N>
bool split(std::string_view text, std::array<std::string_view, N>& fields, bool optional_final) noexcept {
    if (optional_final && !text.empty() && text.back() == ';') text.remove_suffix(1);
    for (std::size_t i = 0; i < N; ++i) {
        auto delimiter = text.find(';');
        if (i + 1 == N) {
            if (delimiter != std::string_view::npos || text.empty()) return false;
            fields[i] = text; return true;
        }
        if (delimiter == std::string_view::npos || delimiter == 0) return false;
        fields[i] = text.substr(0, delimiter); text.remove_prefix(delimiter + 1);
    }
    return false;
}
template<std::size_t N>
bool integers(std::string_view text, std::array<std::int32_t, N>& values, bool optional_final) noexcept {
    std::array<std::string_view, N> fields{};
    if (!split(text, fields, optional_final)) return false;
    for (std::size_t i = 0; i < N; ++i) if (!integer(fields[i], values[i])) return false;
    return true;
}
bool ascii(std::string_view text) noexcept {
    for (unsigned char c : text) if (c < 32 || c > 126) return false;
    return true;
}
bool put(a::Observation& o, Id id, double value, Kind kind = Kind::physical,
         std::size_t element = 0, Valid validity = Valid::valid) noexcept {
    return o.add(id, static_cast<std::uint8_t>(element), value, kind, validity);
}
bool unavailable(a::Observation& o, Id id, std::size_t element = 0) noexcept {
    return put(o, id, 0, Kind::physical, element, Valid::unavailable);
}
double heading(double degrees) noexcept {
    double normalized = std::fmod(degrees, 360.0);
    if (normalized < 0) normalized += 360.0;
    return normalized == 0 ? 0 : normalized;
}
bool main_pose(std::string_view text, a::Observation& o) noexcept {
    std::array<std::string_view, 7> f{};
    std::int32_t pitch{}, bank{}, altitude{}, tas{}; double hdg{}, lat{}, lon{};
    if (!split(text, f, false) || !integer(f[0], pitch) || !integer(f[1], bank) ||
        !real(f[2], hdg) || !integer(f[3], altitude) || !integer(f[4], tas) ||
        !real(f[5], lat) || !real(f[6], lon)) return false;
    constexpr double pi = 3.1415926535897932384626433832795;
    if (std::abs(lat) > pi / 2 || std::abs(lon) > pi) return false;
    // Target attitude/altitude/TAS are internal synchronizing values, not
    // the high-rate physical flight-deck pose. No smoothing is applied here.
    o.position_reference = a::PositionReference::psx_internal_unqualified;
    o.heading_reference = std::abs(lat * a::radians_to_degrees) > 89 ? a::HeadingReference::grid_north : a::HeadingReference::true_north;
    o.pitch_reference = a::PitchReference::horizon;
    return put(o, Id::pitch_deg, pitch * (a::radians_to_degrees / 100000.0), Kind::derived) &&
        put(o, Id::bank_deg, bank * (a::radians_to_degrees / 100000.0), Kind::derived) &&
        put(o, Id::heading_deg, heading(hdg * a::radians_to_degrees), Kind::derived) &&
        put(o, Id::altitude_msl_m, altitude * (a::feet_to_metres / 1000.0), Kind::derived) &&
        put(o, Id::true_airspeed_mps, tas * (a::knots_to_metres_per_second / 1000.0), Kind::derived) &&
        put(o, Id::latitude_deg, lat * a::radians_to_degrees) && put(o, Id::longitude_deg, lon * a::radians_to_degrees);
}
bool engine_flame(std::string_view text, a::Observation& o) noexcept {
    if (text.size() < 4) return false;
    for (std::size_t i = 0; i < 4; ++i) if (text[i] != 'L' && text[i] != 'x') return false;
    std::array<std::int32_t, 4> n1{};
    if (!integers(text.substr(4), n1, true)) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!put(o, Id::engine_combustion, text[i] == 'L' ? 1 : 0, Kind::physical, i) ||
            !put(o, Id::engine_n1_percent, n1[i] / 10.0, Kind::physical, i)) return false;
    }
    return true;
}
bool engine_parameters(std::string_view text, a::Observation& o) noexcept {
    std::array<std::int32_t, 37> f{};
    if (!integers(text, f, true)) return false;
    const bool ge = o.family == a::EngineFamily::ge;
    const bool rr = o.family == a::EngineFamily::rr;
    if (ge) for (std::size_t i = 5; i < 9; ++i) if (f[i] != 0) return false;
    if (!put(o, ge ? Id::thrust_limit_n1_percent : Id::thrust_limit_epr, f[0] / (ge ? 10.0 : 100.0), Kind::commanded)) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!put(o, ge ? Id::engine_command_n1_percent : Id::engine_command_epr, f[1 + i] / (ge ? 10.0 : 100.0), Kind::commanded, i) ||
            !put(o, Id::engine_epr, f[5 + i] / 100.0, Kind::physical, i, ge ? Valid::not_applicable : Valid::valid) ||
            !put(o, Id::engine_egt_c, f[9 + i], Kind::physical, i) ||
            !put(o, rr ? Id::engine_n3_percent : Id::engine_n2_percent, f[13 + i] / 10.0, Kind::physical, i) ||
            !put(o, Id::engine_fuel_flow_kg_s, f[17 + i] * a::kilograms_per_hour_to_second, Kind::physical, i)) return false;
        // These slots were structurally parsed above. Their precise physical
        // units remain explicitly unqualified; never relabel as percent/psi.
        if (!unavailable(o, Id::engine_oil_pressure_psi, i) || !unavailable(o, Id::engine_oil_quantity_litres, i) ||
            !put(o, Id::engine_oil_temperature_c, f[25 + i], Kind::physical, i) ||
            !put(o, Id::engine_vibration, f[33 + i] / 10.0, Kind::physical, i)) return false;
        // RR's unreported intermediate spool is NOT zero and is not its N3.
        if (rr && !unavailable(o, Id::engine_n2_percent, i)) return false;
    }
    return true;
}
bool misc_flight(std::string_view text, a::Observation& o) noexcept {
    std::array<std::int32_t, 6> f{};
    if (!integers(text, f, true)) return false;
    return put(o, Id::indicated_airspeed_mps, f[0] * (a::knots_to_metres_per_second / 10.0)) &&
        put(o, Id::oat_c, f[1] / 10.0) && put(o, Id::tat_c, f[2] / 10.0) &&
        put(o, Id::wind_direction_deg, f[3], Kind::derived) &&
        put(o, Id::wind_speed_mps, f[4] * a::knots_to_metres_per_second, Kind::derived) &&
        put(o, Id::magnetic_variation_deg, f[5] / 10.0, Kind::derived);
}
bool gears(std::string_view text, a::Observation& o) noexcept {
    std::array<std::string_view, 42> f{};
    if (!split(text, f, true) || f[5].size() != 5) return false;
    std::array<std::int32_t, 42> raw{};
    // Validate the ENTIRE record, including unsupported thermal/tyre fields.
    for (std::size_t i = 0; i < f.size(); ++i) if (i != 5 && !integer(f[i], raw[i])) return false;
    for (std::size_t i = 0; i < 5; ++i) {
        if (raw[i] < 0 || raw[i] > 20000 || (f[5][i] != 'O' && f[5][i] != 'c')) return false;
        if (!put(o, Id::gear_deploy_ratio, raw[i] / 20000.0, Kind::derived, i) ||
            !put(o, Id::gear_door_open, f[5][i] == 'O' ? 1 : 0, Kind::physical, i)) return false;
    }
    return true;
}
bool flaps(std::string_view text, a::Observation& o) noexcept {
    std::array<std::int32_t, 10> f{};
    if (!integers(text, f, false)) return false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        const int full = i < 6 ? 500 : 2250;
        if (f[i] < 0 || f[i] > full) return false;
        if (!put(o, i < 6 ? Id::leading_edge_deploy_ratio : Id::flap_deploy_ratio,
                 static_cast<double>(f[i]) / full, Kind::derived, i < 6 ? i : i - 6)) return false;
    }
    return true;
}
bool levers(std::string_view text, a::Observation& o) noexcept {
    std::array<std::int32_t, 4> f{};
    if (!integers(text, f, false)) return false;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] < -8925 || f[i] > 5000 || !put(o, Id::engine_tla_deg, f[i] / 100.0, Kind::commanded, i)) return false;
    }
    return true;
}
bool surfaces(std::string_view text, bool spoilers, a::Observation& o) noexcept {
    if (text.size() != (spoilers ? 22U : 20U)) return false;
    constexpr int neutral[] = {15,20,20,15,15,17,17,15,32,32};
    for (std::size_t i = 0; i < text.size() / 2; ++i) {
        const char hi = text[i * 2], lo = text[i * 2 + 1];
        if (hi < '0' || hi > '9' || lo < '0' || lo > '9') return false;
        const int raw = (hi - '0') * 10 + lo - '0';
        const Id id = spoilers ? Id::spoiler_angle_deg : i < 4 ? Id::aileron_angle_deg : i < 8 ? Id::elevator_angle_deg : Id::rudder_angle_deg;
        const std::size_t index = spoilers || i < 4 ? i : i < 8 ? i - 4 : i - 8;
        if (!put(o, id, spoilers ? raw : raw - neutral[i], Kind::physical, index)) return false;
    }
    return true;
}
bool lights(std::string_view text, a::Observation& o) noexcept {
    if (text.size() != 14) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool valid = i < 4 ? (c == '0' || c == '1' || c == '7' || c == '9') :
            (i == 9 || i == 10) ? (c >= '0' && c <= '9') : (c == '0' || c == '9');
        if (!valid || !put(o, Id::exterior_light_level, c - '0', Kind::physical, i)) return false;
    }
    return true;
}
struct Descriptor { std::string_view code; Record record; std::size_t max_payload; bool needs_family; };
constexpr Descriptor records[] = {
    {"Qi1",Record::qi1,11,false}, {"Qi128",Record::qi128,11,false}, {"Qi129",Record::qi129,11,false}, {"Qi257",Record::qi257,11,false},
    {"Qs121",Record::qs121,200,false}, {"Qs481",Record::qs481,24,true}, {"Qs482",Record::qs482,300,true},
    {"Qs483",Record::qs483,300,false}, {"Qs424",Record::qs424,500,false}, {"Qs425",Record::qs425,1000,false},
    {"Qs436",Record::qs436,23,true}, {"Qs479",Record::qs479,22,false}, {"Qs480",Record::qs480,20,false},
    {"Qs443",Record::qs443,14,false}, {"Qi156",Record::qi156,11,true}, {"Qi157",Record::qi157,11,true},
    {"Qi158",Record::qi158,11,true}, {"Qi159",Record::qi159,11,true}
};
}
const char* decode_result_name(DecodeResult r) noexcept {
    switch (r) {
    case DecodeResult::ok: return "ok"; case DecodeResult::unsupported: return "unsupported";
    case DecodeResult::malformed: return "malformed"; case DecodeResult::missing_session: return "missing_session";
    case DecodeResult::missing_configuration: return "missing_configuration"; case DecodeResult::stale_sequence: return "stale_sequence";
    case DecodeResult::configuration_overflow: return "configuration_overflow";
    } return "unknown";
}
bool PsxAircraftDecoder::begin_session(std::uint64_t epoch) noexcept {
    if (!epoch || epoch <= session_) return false;
    session_ = epoch; configuration_ = 0; sequence_ = 0; family_ = a::EngineFamily::unknown; return true;
}
DecodeResult PsxAircraftDecoder::preflight(std::uint64_t sequence, std::int64_t qpc) const noexcept {
    if (!session_) return DecodeResult::missing_session;
    if (!sequence || sequence <= sequence_) return DecodeResult::stale_sequence;
    return qpc < 0 ? DecodeResult::malformed : DecodeResult::ok;
}
bool PsxAircraftDecoder::restore_context(std::uint64_t session, std::uint64_t configuration,
    a::EngineFamily family, std::uint64_t sequence) noexcept {
    if (session_ || !session || static_cast<unsigned>(family) > static_cast<unsigned>(a::EngineFamily::rr) ||
        (configuration == 0) != (family == a::EngineFamily::unknown)) return false;
    session_ = session; configuration_ = configuration; family_ = family; sequence_ = sequence; return true;
}
DecodeResult PsxAircraftDecoder::decode_main(std::string_view line, std::uint64_t sequence, std::int64_t qpc, a::Observation& output) noexcept {
    const auto pre = preflight(sequence, qpc); if (pre != DecodeResult::ok) return pre;
    // The longest supported key has five characters. Bound even unsupported
    // prefix scans independently of the transport framer's larger line cap.
    const auto equal = line.substr(0, 6).find('=');
    if (equal == std::string_view::npos) return DecodeResult::unsupported;
    const Descriptor* descriptor = nullptr;
    for (const auto& entry : records) if (line.substr(0, equal) == entry.code) { descriptor = &entry; break; }
    if (!descriptor) return DecodeResult::unsupported;
    auto text = line.substr(equal + 1);
    if (text.empty() || text.size() > descriptor->max_payload || !ascii(text)) return DecodeResult::malformed;
    if (descriptor->needs_family && family_ == a::EngineFamily::unknown) return DecodeResult::missing_configuration;
    a::Observation candidate{}; candidate.source = descriptor->record; candidate.family = family_;
    candidate.stamp = {session_, configuration_, sequence, qpc};
    auto next_family = family_; auto next_configuration = configuration_;
    bool ok = false; std::int32_t value{};
    switch (descriptor->record) {
    case Record::qi1:
        if (!integer(text, value) || value < 0 || value > 2) break;
        next_family = value == 0 ? a::EngineFamily::ge : value == 1 ? a::EngineFamily::pw : a::EngineFamily::rr;
        if (next_family != family_) {
            if (configuration_ == (std::numeric_limits<std::uint64_t>::max)()) return DecodeResult::configuration_overflow;
            ++next_configuration;
        }
        candidate.family = next_family; candidate.stamp.configuration_epoch = next_configuration;
        ok = put(candidate, Id::engine_family, static_cast<unsigned>(next_family), Kind::configuration); break;
    case Record::qi128:
        ok = integer(text, value) && value >= 25 && value <= 6400 && put(candidate, Id::simulation_rate_ratio, value / 100.0, Kind::configuration); break;
    case Record::qi129:
        ok = integer(text, value) && value >= 0 && value <= 3 &&
             put(candidate, Id::paused_all, (value & 1) != 0, Kind::configuration) && put(candidate, Id::motion_frozen, (value & 2) != 0, Kind::configuration); break;
    case Record::qi257:
        ok = integer(text, value) && (value == 0 || value == 1) && put(candidate, Id::on_ground, value); break;
    case Record::qs121: ok = main_pose(text, candidate); break;
    case Record::qs481: ok = engine_flame(text, candidate); break;
    case Record::qs482: ok = engine_parameters(text, candidate); break;
    case Record::qs483: ok = misc_flight(text, candidate); break;
    case Record::qs424: ok = gears(text, candidate); break;
    case Record::qs425: ok = flaps(text, candidate); break;
    case Record::qs436: ok = levers(text, candidate); break;
    case Record::qs479: ok = surfaces(text, true, candidate); break;
    case Record::qs480: ok = surfaces(text, false, candidate); break;
    case Record::qs443: ok = lights(text, candidate); break;
    case Record::qi156: case Record::qi157: case Record::qi158: case Record::qi159: {
        if (!integer(text, value) || value < -9999 || value > 9999) break;
        const auto index = static_cast<unsigned>(descriptor->record) - static_cast<unsigned>(Record::qi156);
        const auto magnitude = value < 0 ? -value : value;
        // Valid registry values beyond normal modeled travel are retained as
        // unavailable geometry, not silently clamped into a fabricated ratio.
        ok = put(candidate, Id::engine_reverser_lever_locked, value < 0, Kind::physical, index) &&
             put(candidate, Id::engine_reverser_deploy_ratio, magnitude <= 1 ? 0 : magnitude / 3000.0,
                 Kind::derived, index, magnitude <= 3000 ? Valid::valid : Valid::unavailable);
        break;
    }
    default: return DecodeResult::unsupported;
    }
    if (!ok || !a::well_formed(candidate)) return DecodeResult::malformed;
    output = candidate; family_ = next_family; configuration_ = next_configuration; sequence_ = sequence;
    return DecodeResult::ok;
}
DecodeResult PsxAircraftDecoder::decode_boost(std::string_view line, std::uint64_t sequence, std::int64_t qpc, a::Observation& output) noexcept {
    const auto pre = preflight(sequence, qpc); if (pre != DecodeResult::ok) return pre;
    if (line.size() > 256 || !ascii(line)) return DecodeResult::malformed;
    std::array<std::string_view, 8> f{}; std::array<std::int32_t, 4> scaled{};
    std::int32_t millisecond{}; double lat{}, lon{};
    if (!split(line, f, false) || (f[0] != "G" && f[0] != "F")) return DecodeResult::malformed;
    for (std::size_t i = 0; i < 4; ++i) if (!integer(f[i + 1], scaled[i])) return DecodeResult::malformed;
    if (!real(f[5], lat) || !real(f[6], lon) || !integer(f[7], millisecond) ||
        std::abs(lat) > 90 || std::abs(lon) > 180 || millisecond < 0 || millisecond > 999) return DecodeResult::malformed;
    a::Observation candidate{}; candidate.source = Record::boost; candidate.family = family_;
    candidate.stamp = {session_, configuration_, sequence, qpc};
    candidate.position_reference = a::PositionReference::psx_flight_deck;
    candidate.heading_reference = std::abs(lat) > 89 ? a::HeadingReference::grid_north : a::HeadingReference::true_north;
    candidate.pitch_reference = a::PitchReference::boost_conditional;
    candidate.has_source_millisecond = true; candidate.source_millisecond_mod1000 = static_cast<std::uint16_t>(millisecond);
    const bool ok = put(candidate, Id::on_ground, f[0] == "G" ? 1 : 0) &&
        put(candidate, Id::altitude_msl_m, scaled[0] * (a::feet_to_metres / 100.0)) &&
        put(candidate, Id::heading_deg, heading(scaled[1] / 100.0)) &&
        put(candidate, Id::pitch_deg, scaled[2] / 100.0, Kind::derived) && put(candidate, Id::bank_deg, scaled[3] / 100.0) &&
        put(candidate, Id::latitude_deg, lat) && put(candidate, Id::longitude_deg, lon);
    if (!ok || !a::well_formed(candidate)) return DecodeResult::malformed;
    output = candidate; sequence_ = sequence; return DecodeResult::ok;
}
}
