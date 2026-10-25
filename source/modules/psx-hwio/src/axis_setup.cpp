#include "hwio/catalogue.hpp"
#include "hwio/axis_setup.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hwio {
namespace {
constexpr std::uint64_t router_id = 0x7000000000000000ULL;
bool exists(const AxisSetup &s) {
    return !s.source.bindings.empty();
}
Binding &binding(AxisSetup &s) {
    return s.source.bindings.front();
}
const Binding &binding(const AxisSetup &s) {
    return s.source.bindings.front();
}
void init_points(AxisSetup &s, bool reverse) {
    const auto &c = reverse ? binding(s).reverse_calibration : binding(s).calibration;
    const int first = static_cast<int>(c.inverted ? c.maximum : c.minimum);
    const int last = static_cast<int>(c.inverted ? c.minimum : c.maximum);
    if (reverse)
        s.reverse_points = {first, static_cast<int>(c.center), last};
    else {
        s.points[0] = first;
        s.points[1] = static_cast<int>(c.center);
        s.points[2] = last;
    }
}
void import_deadzone(const AxisCalibration &c, double &percent, bool &custom) {
    const double span = c.maximum - c.minimum;
    custom = c.centered ? c.dead_center_low != c.dead_center_high || c.dead_minimum != 0 || c.dead_maximum != 0
                        : c.dead_minimum != c.dead_maximum;
    percent = span > 0 ? 100 * (c.centered ? c.dead_center_low + c.dead_center_high : c.dead_minimum) / span : 2;
}
bool calibrate(AxisCalibration &c, const int *points, double deadzone, bool custom) {
    c.minimum = std::min(points[0], points[2]);
    c.maximum = std::max(points[0], points[2]);
    c.center = points[1];
    c.inverted = points[0] > points[2];
    if (!custom) {
        if (!std::isfinite(deadzone) || deadzone < 0 || deadzone >= 100)
            return false;
        const double width = (c.maximum - c.minimum) * deadzone / 100;
        c.dead_center_low = c.dead_center_high = c.centered ? width / 2 : 0;
        c.dead_minimum = c.dead_maximum = c.centered ? 0 : width;
    }
    return calibration_valid(c);
}
std::pair<int, int> resolve(const Profile &p, ControlKey key, const std::vector<DeviceInfo> &devices) {
    if (p.id.empty())
        return {-1, -1};
    std::vector<DeviceIdentity> identities;
    std::vector<const DeviceInfo *> connected;
    for (const auto &d : devices)
        if (d.connected) {
            identities.push_back(d.identity);
            connected.push_back(&d);
        }
    const int match = match_profile(p, identities);
    if (match < 0)
        return {-1, -1};
    const auto &d = *connected[static_cast<std::size_t>(match)];
    for (std::size_t c = 0; c < d.controls.size(); ++c)
        if (same(d.controls[c].key, key) && !d.controls[c].relative)
            return {d.slot, static_cast<int>(c)};
    return {-1, -1};
}
std::string packed_profile(const Profile &p) {
    return p.id.empty() ? std::string{} : profile_encode(p);
}
void write_setup(std::ostream &out, const AxisSetup &s) {
    out << std::quoted(packed_profile(s.source)) << ' ' << std::quoted(packed_profile(s.reverse)) << ' '
        << std::quoted(packed_profile(s.inactive_device)) << ' ';
    for (auto n : s.points)
        out << n << ' ';
    for (auto n : s.reverse_points)
        out << n << ' ';
    out << s.captured << ' ' << s.reverse_captured << ' ' << s.deadzone << ' ' << s.reverse_deadzone << ' ' << s.custom
        << ' ' << s.reverse_custom << ' ' << s.detents << ' ' << s.control_stiffness << '\n';
}
bool read_profile(std::istream &in, Profile &p, std::string &error) {
    std::string text;
    if (!(in >> std::quoted(text)))
        return false;
    return text.empty() || profile_decode(text, p, error);
}
bool read_setup(std::istream &in, AxisSetup &s, std::string &error, bool stiffness) {
    if (!read_profile(in, s.source, error) || !read_profile(in, s.reverse, error) ||
        !read_profile(in, s.inactive_device, error))
        return false;
    for (auto &n : s.points)
        in >> n;
    for (auto &n : s.reverse_points)
        in >> n;
    in >> s.captured >> s.reverse_captured >> s.deadzone >> s.reverse_deadzone >> s.custom >> s.reverse_custom >>
        s.detents;
    if (stiffness) in >> s.control_stiffness;
    return !!in && s.captured <= 127 && s.reverse_captured <= 7 && std::isfinite(s.deadzone) &&
           std::isfinite(s.reverse_deadzone) && std::isfinite(s.control_stiffness) &&
           std::abs(s.control_stiffness) <= 1 && s.source.bindings.size() <= 1;
}
std::string signature(const AxisSetup &s) {
    std::ostringstream out;
    out << std::setprecision(17);
    auto geometry = s;
    geometry.control_stiffness = 0;
    for (auto &b : geometry.source.bindings) { b.control_stiffness = 0; b.response_gain = 1; }
    write_setup(out, geometry); // Curve-only edits retain established filter/baseline state.
    return out.str();
}
} // namespace
int axis_partner(std::size_t slot) noexcept {
    if (slot < 8)
        return static_cast<int>(slot < 4 ? slot + 4 : slot - 4);
    if (slot >= 14 && slot < 18)
        return static_cast<int>(slot < 16 ? slot + 2 : slot - 2);
    return -1;
}
bool axis_centered(std::size_t slot, ThrottleLayout layout) noexcept {
    return slot < 8 || (slot >= 9 && slot <= 12 && layout == ThrottleLayout::combined);
}
TargetDescriptor axis_target(std::size_t slot) {
    const auto *target=flight_target(slot);
    return target?target->descriptor:TargetDescriptor{};
}
AxisSetup axis_from_binding(const Profile &p, const Binding &b, const std::vector<Profile> &profiles) {
    AxisSetup s;
    s.control_stiffness = b.control_stiffness;
    s.source = p;
    s.source.bindings = {b};
    // Profiles keep their original format; the sidecar stores the actual source identity.
    if (!b.sources.empty() && !b.sources[0].profile.empty())
        for (const auto &other : profiles)
            if (other.id == b.sources[0].profile) {
                s.source.identity = other.identity;
                s.source.explicit_path = other.explicit_path;
            }
    if (!binding(s).sources.empty())
        binding(s).sources[0].profile.clear();
    s.reverse = p;
    s.reverse.bindings.clear();
    if (!b.reverse_source.profile.empty())
        for (const auto &other : profiles)
            if (other.id == b.reverse_source.profile)
                s.reverse = other;
    s.reverse.bindings.clear();
    binding(s).reverse_source.profile.clear();
    init_points(s, false);
    init_points(s, true);
    s.captured = 7;
    s.reverse_captured = 5;
    import_deadzone(b.calibration, s.deadzone, s.custom);
    import_deadzone(b.reverse_calibration, s.reverse_deadzone, s.reverse_custom);
    return s;
}
void axis_source(AxisSetup &s, std::size_t slot, const DeviceInfo &d, const ControlInfo &c, bool inverted,
                 bool reverse) {
    if (!exists(s)) {
        s.source.id = "axis-" + std::to_string(slot);
        s.source.name = utf8(flight_slot_name(slot));
        Binding b;
        b.id = 1;
        b.name = s.source.name;
        b.target = axis_target(slot);
        b.sources = {{c.key, {}}};
        s.source.bindings = {b};
    }
    auto &b = binding(s);
    auto &p = reverse ? s.reverse : s.source;
    p.identity = d.identity;
    p.explicit_path = true; // Explicit UI selection resolves otherwise identical collections by saved instance path.
    if (p.id.empty()) {
        p.id = "reverse-" + std::to_string(slot);
        p.name = d.identity.name;
    }
    auto &cal = reverse ? b.reverse_calibration : b.calibration;
    const auto filter = cal;
    cal = {};
    cal.minimum = c.minimum;
    cal.maximum = c.maximum;
    cal.center = (static_cast<double>(c.minimum) + c.maximum) / 2;
    cal.centered = !reverse && axis_centered(slot, b.throttle_layout);
    cal.inverted = inverted;
    cal.lowpass_ms = filter.lowpass_ms;
    cal.median_three = filter.median_three;
    if (reverse) {
        b.reverse_source = {c.key, {}};
        s.reverse_captured = 0;
        s.reverse_deadzone = 2;
        s.reverse_custom = false;
    } else {
        b.sources = {{c.key, {}}};
        b.mode = MappingMode::axis;
        s.captured = 0;
        s.deadzone = 2;
        s.custom = false;
        s.detents = slot == 13;
    }
    init_points(s, reverse);
    if (slot == 13 && !reverse)
        s.points.fill(0);
}
bool axis_calibrate(AxisSetup &s, std::size_t slot, std::string &error) {
    if(!flight_target(slot)) {error="Catalogue target unavailable";return false;}
    if (!exists(s) || binding(s).sources.empty()) {
        error = "Choose a controller and axis";
        return false;
    }
    auto &b = binding(s);
    b.target=axis_target(slot);
    if (!std::isfinite(s.control_stiffness) || std::abs(s.control_stiffness) > 1) {
        error = "Control stiffness must be between -1 and 1";
        return false;
    }
    b.control_stiffness = slot < 8 && slot % 4 < 3 ? s.control_stiffness : 0;
    b.response_gain = std::pow(4., b.control_stiffness);
    if(s.source.identity.hardware_key.starts_with("fdstq-phidget22:")){
        const unsigned required=slot==13 && s.detents?127u:axis_centered(slot,b.throttle_layout)?7u:5u;
        if((s.captured&required)!=required){error="FDSTQ requires captured physical endpoints/detents before Apply";return false;}
    }
    if (s.detents && slot == 13) {
        if (s.captured != 127) {
            error = "Capture all seven flap positions";
            return false;
        }
        const double span = std::abs(static_cast<double>(s.points[6]) - s.points[0]);
        if (!std::isfinite(s.deadzone) || s.deadzone < 0 || s.deadzone >= 100 || span == 0) {
            error = "Invalid flap travel or deadzone";
            return false;
        }
        const bool reversed = s.points[6] < s.points[0];
        for (std::size_t i = 1; i < 7; ++i)
            if ((reversed ? static_cast<double>(s.points[i - 1]) - s.points[i]
                          : static_cast<double>(s.points[i]) - s.points[i - 1]) <= span * s.deadzone / 100) {
                error = "Flap points must be ordered with non-overlapping bands";
                return false;
            }
        b.calibration.minimum = std::min(s.points[0], s.points[6]);
        b.calibration.maximum = std::max(s.points[0], s.points[6]);
        b.calibration.centered = false;
        b.calibration.inverted = reversed;
        b.calibration.dead_minimum = b.calibration.dead_maximum = 0;
        b.calibration.dead_center_low = b.calibration.dead_center_high = 0;
        b.mode = MappingMode::axis;
    } else if (!calibrate(b.calibration, s.points.data(), s.deadzone, s.custom)) {
        error = "Invalid endpoints, centre or deadzone; last valid calibration remains active";
        return false;
    }
    if (b.throttle_layout == ThrottleLayout::separate_forward_reverse && b.target.throttle_engine >= 0 &&
        !s.reverse.id.empty()) {
        b.reverse_calibration.centered = false;
        if (!calibrate(b.reverse_calibration, s.reverse_points.data(), s.reverse_deadzone, s.reverse_custom)) {
            error = "Invalid reverse endpoints or deadzone";
            return false;
        }
    }
    b.enabled = true;
    return validate(s.source, error);
}
bool axis_apply(AxisRow &row, std::size_t slot) {
    auto candidate = row.draft;
    if (!axis_calibrate(candidate, slot, row.pending))
        return false;
    row.applied = candidate;
    row.draft = std::move(candidate);
    row.active = true;
    row.managed = true;
    row.pending.clear();
    return true;
}
void axis_layout(AxisSetup &s, bool separate) {
    if (!exists(s))
        return;
    auto &b = binding(s);
    const auto wanted = separate ? ThrottleLayout::separate_forward_reverse : ThrottleLayout::combined;
    if (b.throttle_layout == wanted)
        return;
    if (s.inactive_device.id.empty()) {
        s.inactive_device = s.source;
        s.inactive_device.bindings.clear();
    } else {
        std::swap(s.source.identity, s.inactive_device.identity);
        std::swap(s.source.explicit_path, s.inactive_device.explicit_path);
    }
    std::swap(b.calibration, b.inactive_calibration);
    std::swap(b.sources[0], b.inactive_source);
    if (b.sources[0].key.backend == BackendKind::usb_hid && b.sources[0].key.usage_page == 0) {
        b.sources[0] = b.inactive_source;
        b.calibration = b.inactive_calibration;
        b.calibration.centered = !separate;
        const double span = b.calibration.maximum - b.calibration.minimum;
        b.calibration.dead_minimum = b.calibration.dead_maximum = separate ? span * .02 : 0;
        b.calibration.dead_center_low = b.calibration.dead_center_high = separate ? 0 : span * .01;
    }
    b.throttle_layout = wanted;
    b.reverse_calibration.centered = false;
    b.calibration.centered = !separate;
    init_points(s, false);
    import_deadzone(b.calibration, s.deadzone, s.custom);
}
void AxisMovement::input(const DeviceInfo &d, const Sample &s) {
    if (s.device >= max_devices || s.control >= d.controls.size() || s.control >= max_controls)
        return;
    const auto &c = d.controls[s.control];
    if (c.key.kind != ControlKind::axis || c.relative || s.relative)
        return;
    auto &a = anchors_[s.device][s.control];
    if (!a.known || a.generation != s.generation || s.baseline) {
        a = {true, s.value, s.generation};
        return;
    }
    const double delta = static_cast<double>(s.value) - a.value;
    if (std::abs(delta) < std::max(1.0, (static_cast<double>(c.maximum) - c.minimum) * .01))
        return;
    if (!last_.present || last_.sample.device != s.device || last_.sample.control != s.control ||
        last_.sample.generation != s.generation) {
        last_.device = d.identity.name;
        last_.axis = c.name;
    }
    last_.present = last_.available = true;
    last_.reversed = delta < 0;
    last_.sample = s;
    a.value = s.value;
}
LastAxisMovement AxisMovement::snapshot(const std::vector<DeviceInfo> &devices) const {
    auto result = last_;
    result.available = false;
    for (const auto &d : devices)
        if (d.connected && d.slot == result.sample.device && d.generation == result.sample.generation) {
            result.available = result.present;
            result.device = d.identity.name;
            if (result.sample.control < d.controls.size())
                result.axis = d.controls[result.sample.control].name;
        }
    return result;
}
bool axis_replaces(const AxisConfiguration &c, const std::string &p, std::uint64_t b) {
    for (const auto &r : c.rows)
        if (r.managed && r.replaces.choice == DisplayChoice::binding && r.replaces.profile == p &&
            r.replaces.binding == b)
            return true;
    return false;
}
std::string axis_configuration_encode(const AxisConfiguration &c) {
    std::ostringstream out;
    out << std::setprecision(17) << "[axis-controls-v3]\n";
    // Reserved legacy expansion field: UI visibility is session-only.
    out << 0 << ' ' << c.default_fo << ' ' << c.override_assigned << ' ' << c.throttle_choice << ' '
        << std::quoted(packed_profile(c.override_source)) << '\n';
    for (std::size_t i = 0; i < flight_slots; ++i) {
        const auto &r = c.rows[i];
        out << i << ' ' << r.managed << ' ' << r.active << ' ' << static_cast<unsigned>(r.replaces.choice) << ' '
            << std::quoted(r.replaces.profile) << ' ' << r.replaces.binding << ' ' << std::quoted(r.pending) << '\n';
        write_setup(out, r.applied);
        write_setup(out, r.draft);
    }
    return out.str();
}
bool axis_configuration_decode(std::string_view text, AxisConfiguration &result, std::string &error) {
    if (text.size() > 4 * 1024 * 1024) {
        error = "Axis configuration exceeds 4 MiB";
        return false;
    }
    std::istringstream in{std::string(text)};
    std::string header;
    std::getline(in, header);
    if (header != "[axis-controls-v2]" && header != "[axis-controls-v3]") {
        error = "Unsupported axis configuration version";
        return false;
    }
    AxisConfiguration c;
    bool legacy_expanded{};
    in >> legacy_expanded >> c.default_fo >> c.override_assigned >> c.throttle_choice;
    if (!in || c.throttle_choice < -1 || c.throttle_choice > 1) {
        error = "Invalid throttle choice";
        return false;
    }
    if (!read_profile(in, c.override_source, error))
        return false;
    if (c.override_assigned &&
        (c.override_source.bindings.size() != 1 || c.override_source.bindings[0].sources.size() != 1 ||
         c.override_source.bindings[0].sources[0].key.kind != ControlKind::button)) {
        error = "Invalid override button";
        return false;
    }
    for (std::size_t i = 0; i < flight_slots; ++i) {
        auto &r = c.rows[i];
        std::size_t index{};
        unsigned choice{};
        in >> index >> r.managed >> r.active >> choice >> std::quoted(r.replaces.profile) >> r.replaces.binding >>
            std::quoted(r.pending);
        if (!in || index != i || choice > 2 || !read_setup(in, r.applied, error, header == "[axis-controls-v3]") || !read_setup(in, r.draft, error, header == "[axis-controls-v3]")) {
            error = "Invalid axis configuration row " + std::to_string(i);
            return false;
        }
        r.replaces.choice = static_cast<DisplayChoice>(choice);
        if (r.active && (!exists(r.applied) || (flight_target(i) && !catalogue_target_missing(binding(r.applied).target) && !flight_binding_matches(i, binding(r.applied))))) {
            error = "Wrong axis target";
            return false;
        }
        if (r.managed && r.active && flight_target(i)) {
            auto check = r.applied;
            if (!axis_calibrate(check, i, error))
                return false;
        }
    }
    in >> std::ws;
    if (!in.eof()) {
        error = "Unexpected axis configuration data";
        return false;
    }
    result = std::move(c);
    return true;
}
bool load_axis_configuration(const std::wstring &path, AxisConfiguration &c, std::string &error) {
    if (!std::filesystem::exists(path))
        return true;
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024) {
        error = "Axis configuration too large";
        return false;
    }
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    std::ostringstream out;
    out << in.rdbuf();
    return axis_configuration_decode(out.str(), c, error);
}
bool save_axis_configuration(const std::wstring &path, const AxisConfiguration &c, std::string &error) {
    const auto encoded = axis_configuration_encode(c);
    AxisConfiguration checked;
    if (!axis_configuration_decode(encoded, checked, error))
        return false;
    const auto temporary = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(temporary), std::ios::binary | std::ios::trunc};
        out << encoded;
        out.flush();
        if (!out) {
            error = "Cannot write axis configuration";
            return false;
        }
    }
    const bool present = std::filesystem::exists(path);
    if (!(present ? ReplaceFileW(path.c_str(), temporary.c_str(), (path + L".bak").c_str(), 0, nullptr, nullptr)
                  : MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))) {
        error = "Cannot replace axis configuration: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

AxisRouter::AxisRouter() {
    sources_.reserve(2);
}
void AxisRouter::configure(const AxisConfiguration &configuration, const std::vector<DeviceInfo> &devices,
                           Mapper &output) {
    configuration_ = configuration;
    for (std::size_t i = 0; i < flight_slots; ++i) {
        const auto &row = configuration.rows[i];
        auto &c = candidates_[i];
        draft_devices_[i] = draft_controls_[i] = draft_reverse_devices_[i] = draft_reverse_controls_[i] = -1;
        if (exists(row.draft) && !binding(row.draft).sources.empty()) {
            auto location = resolve(row.draft.source, binding(row.draft).sources[0].key, devices);
            draft_devices_[i] = location.first;
            draft_controls_[i] = location.second;
            location = resolve(row.draft.reverse, binding(row.draft).reverse_source.key, devices);
            draft_reverse_devices_[i] = location.first;
            draft_reverse_controls_[i] = location.second;
        }
        if (!row.managed || !row.active || !exists(row.applied) || !flight_target(i) || !active_catalogue().target(binding(row.applied).target.id)) {
            c = {};
            continue;
        }
        const auto &s = row.applied;
        const auto &b = binding(s);
        const auto location = resolve(s.source, b.sources[0].key, devices);
        const auto rev = resolve(s.reverse, b.reverse_source.key, devices);
        std::uint64_t generation = 0, reverse_generation = 0;
        for (const auto &d : devices) {
            if (d.slot == location.first)
                generation = d.generation;
            if (d.slot == rev.first)
                reverse_generation = d.generation;
        }
        const auto key = signature(s);
        if (c.signature != key || c.device != location.first || c.control != location.second ||
            c.generation != generation || c.reverse_device != rev.first || c.reverse_control != rev.second ||
            c.reverse_generation != reverse_generation) {
            c = {};
            c.signature = key;
            c.mapped.binding = b;
            c.mapped.reverse_unassigned = s.reverse.id.empty();
            c.mapped.profile = s.source.id;
            c.mapped.local_id = b.id;
            c.device = location.first;
            c.control = location.second;
            c.reverse_device = rev.first;
            c.reverse_control = rev.second;
            c.generation = generation;
            c.reverse_generation = reverse_generation;
            changed_ = true;
        }
        c.mapped.binding.control_stiffness = b.control_stiffness;
        c.mapped.binding.response_gain = b.response_gain;
        const int other = axis_partner(i);
        const std::size_t owner = other >= 0 ? std::min(i, static_cast<std::size_t>(other)) : i;
        auto found = std::find_if(output.mappings.begin(), output.mappings.end(),
                                  [&](const auto &m) { return m.binding.id == router_id + owner; });
        if (found == output.mappings.end()) {
            Mapped m;
            m.externally_calculated = true;
            m.binding = b;
            m.binding.target = axis_target(owner);
            m.binding.id = router_id + owner;
            m.profile = "axis-router";
            m.local_id = owner + 1;
            m.binding.mode = MappingMode::axis;
            m.binding.enabled = true;
            output.mappings.push_back(std::move(m));
        }
    }
    override_device_ = override_control_ = -1;
    if (configuration.override_assigned && !configuration.override_source.bindings.empty()) {
        const auto location =
            resolve(configuration.override_source, configuration.override_source.bindings[0].sources[0].key, devices);
        override_device_ = location.first;
        override_control_ = location.second;
    }
}
void AxisRouter::update(const SourceBank &values, Mapper &output, double ms) {
    const auto source = [&](int d, int c) {
        return d < 0 || c < 0 ? SourceValue{} : values[static_cast<std::size_t>(d)][static_cast<std::size_t>(c)];
    };
    const auto button = source(override_device_, override_control_);
    const bool held = configuration_.override_assigned && button.available && button.value != 0;
    const bool fo = configuration_.default_fo != held;
    changed_ = changed_ || status_.effective_fo != fo;
    status_.default_fo = configuration_.default_fo;
    status_.effective_fo = fo;
    status_.override_available = button.available;
    status_.override_held = held;
    for (std::size_t i = 0; i < flight_slots; ++i) {
        auto &view = status_.rows[i];
        const auto &row = configuration_.rows[i];
        auto &c = candidates_[i];
        const auto raw = source(draft_devices_[i], draft_controls_[i]);
        const auto reverse = source(draft_reverse_devices_[i], draft_reverse_controls_[i]);
        view.available = raw.available;
        view.raw = raw.value;
        view.reverse_available = reverse.available;
        view.reverse_raw = reverse.value;
        view.active = row.managed && row.active;
        view.descriptor_defaults = row.draft.captured == 0;
        view.status = row.pending.empty()
                          ? (view.available ? (view.descriptor_defaults ? "Descriptor defaults" : "Calibrated")
                                            : "Source unavailable")
                          : row.pending;
        view.selected = false;
        if (!row.managed || !row.active)
            continue;
        if (!flight_target(i) || !active_catalogue().target(binding(row.applied).target.id)) { view.status="Catalogue target unavailable"; c.mapped.state.available=false; c.mapped.state.reason=view.status; continue; }
        const auto input = source(c.device, c.control);
        sources_.clear();
        sources_.push_back(input);
        if (c.mapped.binding.throttle_layout == ThrottleLayout::separate_forward_reverse &&
            c.mapped.binding.target.throttle_engine >= 0 && !c.mapped.reverse_unassigned)
            sources_.push_back(source(c.reverse_device, c.reverse_control));
        evaluator_.input(c.mapped, sources_, ms, false);
        settle_axis(c.mapped, ms);
        if (row.applied.detents && i == 13) {
            auto &s = c.mapped.state;
            if (!input.available)
                c.flap = -1;
            else {
                const double width = std::abs(static_cast<double>(row.applied.points[6]) - row.applied.points[0]) *
                                     row.applied.deadzone / 200;
                for (std::size_t p = 0; p < 7; ++p)
                    if (std::abs(s.filter.median_raw() - row.applied.points[p]) <= width)
                        c.flap = static_cast<int>(p);
            }
            s.available = input.available && c.flap >= 0;
            s.desired = flight_detent(c.flap);
            if (!s.available)
                s.reason = "Awaiting a recognized flap detent";
        }
    }
    for (auto &out : output.mappings) {
        if (!out.externally_calculated)
            continue;
        const auto owner = static_cast<std::size_t>(out.binding.id - router_id);
        if (owner >= flight_slots)
            continue;
        const int partner = axis_partner(owner);
        std::size_t selected = owner;
        const auto assigned = [&](std::size_t i) {
            return configuration_.rows[i].managed && configuration_.rows[i].active;
        };
        if (partner >= 0) {
            const auto other = static_cast<std::size_t>(partner);
            if (assigned(other) && (!assigned(owner) || fo))
                selected = other;
        }
        auto &next = candidates_[selected].mapped.state;
        const bool available = assigned(selected) && next.available;
        const bool different = available != out.state.available || (available && out.state.desired != next.desired);
        const bool dirty = out.state.dirty || different || changed_;
        const auto delivery = out.state.delivery;
        const int sent = out.state.sent;
        const bool pending = out.state.pending;
        out.state = next;
        out.state.available = available;
        out.state.dirty = dirty;
        out.state.delivery = delivery;
        out.state.sent = sent;
        out.state.pending = pending;
        if (assigned(selected)) {
            out.binding.throttle = binding(configuration_.rows[selected].applied).throttle;
            out.binding.calibration = candidates_[selected].mapped.binding.calibration;
            out.binding.reverse_calibration = candidates_[selected].mapped.binding.reverse_calibration;
            out.binding.throttle_layout = binding(configuration_.rows[selected].applied).throttle_layout;
            out.binding.control_stiffness = candidates_[selected].mapped.binding.control_stiffness;
            out.binding.response_gain = candidates_[selected].mapped.binding.response_gain;
        }
        status_.rows[selected].selected = available;
    }
}
void AxisRouter::display(FlightSnapshot &snapshot, const Mapper &output) const {
    for (std::size_t i = 0; i < flight_slots; ++i) {
        const auto &row = configuration_.rows[i];
        if (row.managed && !row.active && !exists(row.draft)) {
            snapshot[i] = {};
            snapshot[i].slot = static_cast<FlightSlot>(i);
            snapshot[i].reason = "Unassigned";
        }
        if (!row.managed || !row.active)
            continue;
        auto &v = snapshot[i];
        const auto &m = candidates_[i].mapped;
        const auto &s = m.state;
        v = {};
        v.slot = static_cast<FlightSlot>(i);
        v.assigned = true;
        v.available = s.available;
        v.value = s.desired;
        v.raw = s.forward_raw;
        v.reverse_raw = s.reverse_raw;
        v.normalized = flight_fraction(i,s.desired);
        flight_physical(v, m.binding, v.raw);
        v.source = row.applied.source.identity.name + " | " + row.applied.source.id + "/" + std::to_string(m.local_id);
        v.reason = s.reason;
        if (!status_.rows[i].selected)
            v.reason += " | Standby: other seat selected";
        if (m.binding.target.throttle_engine >= 0) {
            v.split = m.binding.throttle_layout == ThrottleLayout::separate_forward_reverse;
            v.forward = v.split ? s.normalized : std::max(0.0, s.normalized);
            v.reverse = v.split ? s.reverse_normalized : std::max(0.0, -s.normalized);
            v.reverse_blocked = v.split && s.physical_forward > 0 && s.physical_reverse > 0;
        }
        const int other = axis_partner(i);
        const auto owner = other < 0 ? i : std::min(i, static_cast<std::size_t>(other));
        for (const auto &out : output.mappings)
            if (out.binding.id == router_id + owner && status_.rows[i].selected) {
                v.delivery = out.state.delivery;
                if (!out.state.reason.empty())
                    v.reason = out.state.reason;
            }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto j = i + 4;
        if (configuration_.rows[i].managed && configuration_.rows[i].active && !exists(configuration_.rows[j].draft) &&
            (!snapshot[j].assigned || snapshot[j].shared)) {
            snapshot[j] = snapshot[i];
            snapshot[j].slot = static_cast<FlightSlot>(j);
            snapshot[j].shared = true;
        } else if (configuration_.rows[j].managed && configuration_.rows[j].active &&
                   !exists(configuration_.rows[i].draft) && (!snapshot[i].assigned || snapshot[i].shared)) {
            snapshot[i] = snapshot[j];
            snapshot[i].slot = static_cast<FlightSlot>(i);
            snapshot[i].shared = true;
        }
    }
    for (std::size_t i = 0; i < 8; ++i) flight_transmitted(snapshot[i], i, output);
}
void AxisRouter::filter_interval(int milliseconds) noexcept {
    for (auto &c : candidates_)
        flight_filter(c.mapped.binding, milliseconds);
}
bool AxisRouter::settling() const noexcept {
    for (const auto &c : candidates_)
        if (c.mapped.state.available &&
            (c.mapped.binding.calibration.lowpass_ms > 0 || c.mapped.binding.reverse_calibration.lowpass_ms > 0))
            return true;
    return false;
}
} // namespace hwio
