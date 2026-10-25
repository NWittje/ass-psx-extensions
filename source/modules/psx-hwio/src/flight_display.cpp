#include "hwio/catalogue.hpp"
#include "hwio/flight_display.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hwio {
void flight_filter(Binding &b, int milliseconds) noexcept {
    if (b.mode != MappingMode::axis)
        return;
    for (std::size_t slot = 0; slot < flight_slots; ++slot) {
        if (!flight_binding_matches(slot, b))
            continue;
        b.calibration.median_three = b.reverse_calibration.median_three = true;
        b.calibration.lowpass_ms = b.reverse_calibration.lowpass_ms = milliseconds;
        return;
    }
}
void FlightDisplay::filter_interval(int milliseconds) noexcept {
    for (auto &m : preview_.mappings)
        flight_filter(m.binding, milliseconds);
}

const wchar_t *flight_slot_name(std::size_t slot) noexcept {
    return slot<flight_slots?active_catalogue().labels[slot].c_str():L"Unknown";
}
bool flight_binding_matches(std::size_t slot, const Binding &b) noexcept {
    if (slot >= flight_slots ||
        (b.mode != MappingMode::axis && b.mode != MappingMode::selector && b.mode != MappingMode::maintained))
        return false;
    if(catalogue_target_missing(b.target))return false;
    const auto *role=flight_target(slot);
    if (!role) return false;
    const auto &t=b.target, &d=role->descriptor;
    return t.id==d.id || (t.record==d.record && t.layout==d.layout &&
           t.field==d.field && t.field_count==d.field_count && t.owned_mask==d.owned_mask);

}
bool load_display_assignments(const std::wstring &file, DisplayAssignments &assignments, std::string &error) {
    assignments = {};
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(file, filesystem_error);
    if (filesystem_error) {
        error = "Cannot read display assignments: " + filesystem_error.message();
        return false;
    }
    if (!exists)
        return true;
    const auto size = std::filesystem::file_size(file, filesystem_error);
    if (filesystem_error || size > 65536) {
        error = "Unreadable or oversized axis-display.ini";
        return false;
    }
    std::ifstream in{std::filesystem::path(file)};
    std::string header;
    if (!std::getline(in, header) || (header != "[axis-display-v1]" && header != "[axis-display-v2]")) {
        error = "Invalid axis-display.ini version";
        return false;
    }
    DisplayAssignments parsed;
    std::array<bool, flight_slots> seen{};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::size_t slot;
        unsigned choice;
        DisplayReference ref;
        if (!(row >> slot >> choice >> ref.binding >> std::quoted(ref.profile)) || slot >= flight_slots || choice > 2 ||
            seen[slot] || ref.profile.size() > 1024 || ref.profile.find_first_of("\r\n") != std::string::npos ||
            (choice == 2 && (ref.profile.empty() || !ref.binding))) {
            error = "Invalid axis display assignment";
            return false;
        }
        row >> std::ws;
        if (!row.eof()) {
            error = "Unexpected axis display data";
            return false;
        }
        ref.choice = static_cast<DisplayChoice>(choice);
        parsed[slot] = std::move(ref);
        seen[slot] = true;
    }
    if (!std::all_of(seen.begin(), seen.begin() + (header == "[axis-display-v1]" ? 14 : flight_slots),
                     [](bool value) { return value; })) {
        error = "Incomplete axis display assignments";
        return false;
    }
    assignments = std::move(parsed);
    return true;
}
bool save_display_assignments(const std::wstring &file, const DisplayAssignments &assignments, std::string &error) {
    const auto temporary = file + L".tmp";
    {
        std::ofstream out{std::filesystem::path(temporary), std::ios::trunc};
        out << "[axis-display-v2]\n";
        for (std::size_t i = 0; i < assignments.size(); ++i) {
            const auto &a = assignments[i];
            out << i << ' ' << static_cast<unsigned>(a.choice) << ' ' << a.binding << ' ' << std::quoted(a.profile)
                << '\n';
        }
        out.flush();
        if (!out) {
            error = "Could not write display assignments";
            return false;
        }
    }
    DisplayAssignments verify;
    if (!load_display_assignments(temporary, verify, error))
        return false;
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace display assignments: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}
void FlightDisplay::configure(const std::vector<Profile> &profiles, const std::vector<DeviceInfo> &devices,
                              const DisplayAssignments &assignments) {
    entries_ = {};
    preview_.mappings.clear();
    preview_.boundary(1);
    sources_.reserve(max_controls);
    for (std::size_t i = 0; i < flight_slots; ++i) {
        mirrors_[i] = i;
        entries_[i].reference = assignments[i];
        entries_[i].reading.reason = flight_target(i)?"Unassigned":"Catalogue target unavailable";
    }
    auto automatic = [&](std::size_t slot) {
        auto &entry = entries_[slot];
        unsigned matches = 0;
        for (const auto &p : profiles)
            for (const auto &b : p.bindings)
                if (b.enabled && flight_binding_matches(slot, b)) {
                    entry.reference = {DisplayChoice::binding, p.id, b.id};
                    ++matches;
                }
        if (matches > 1) {
            entry.reference = {DisplayChoice::none, {}, 0};
            entry.reading.assigned = true;
            entry.reading.reason = "Ambiguous assignment";
        }
    };
    for (std::size_t axis = 0; axis < 4; ++axis)
        if (assignments[axis].choice == DisplayChoice::automatic)
            automatic(axis);
    for (std::size_t i = 8; i < flight_slots; ++i)
        if (assignments[i].choice == DisplayChoice::automatic)
            automatic(i);
    std::vector<DeviceIdentity> identities;
    std::vector<const DeviceInfo *> connected;
    for (const auto &d : devices)
        if (d.connected && !d.controls.empty()) {
            identities.push_back(d.identity);
            connected.push_back(&d);
        }
    for (std::size_t slot = 0; slot < flight_slots; ++slot) {
        auto &e = entries_[slot];
        if (e.reference.choice != DisplayChoice::binding)
            continue;
        e.reading.assigned = true;
        e.reading.reason = "Assignment no longer exists";
        const auto p =
            std::find_if(profiles.begin(), profiles.end(), [&](const auto &x) { return x.id == e.reference.profile; });
        if (p == profiles.end())
            continue;
        const auto b = std::find_if(p->bindings.begin(), p->bindings.end(),
                                    [&](const auto &x) { return x.id == e.reference.binding; });
        if (b == p->bindings.end())
            continue;
        e.reading.source = p->name + " / " + b->name;
        e.reading.source += "\nProfile ID: " + p->id + " / binding ID: " + std::to_string(b->id);
        e.reading.display_only = !b->enabled;
        e.reading.split = b->throttle_layout == ThrottleLayout::separate_forward_reverse;
        if (!flight_binding_matches(slot, *b)) {
            e.reading.reason = !flight_target(slot) || catalogue_target_missing(b->target)
                ? "Catalogue target unavailable" : "Incompatible display target";
            continue;
        }
        Mapped m;
        m.binding = *b;
        m.profile = p->id;
        m.local_id = b->id;
        auto sources = b->sources;
        if (b->target.throttle_engine >= 0 && e.reading.split)
            sources.push_back(b->reverse_source);
        bool valid = !sources.empty();
        for (const auto &s : sources) {
            const auto source_profile =
                s.profile.empty()
                    ? p
                    : std::find_if(profiles.begin(), profiles.end(), [&](const auto &x) { return x.id == s.profile; });
            if (source_profile == profiles.end()) {
                valid = false;
                break;
            }
            const int match = match_profile(*source_profile, identities);
            if (match < 0) {
                valid = false;
                break;
            }
            const auto &d = *connected[static_cast<std::size_t>(match)];
            e.reading.source += "\n" + d.identity.name +
                                (d.identity.serial.empty() ? " (no serial)" : " / serial: " + d.identity.serial);
            const auto c =
                std::find_if(d.controls.begin(), d.controls.end(), [&](const auto &x) { return same(x.key, s.key); });
            if (c == d.controls.end()) {
                valid = false;
                break;
            }
            m.source_devices.push_back(d.slot);
            m.source_generations.push_back(d.generation);
            m.controls.push_back(static_cast<std::uint16_t>(c - d.controls.begin()));
        }
        if (!valid) {
            e.reading.reason = "Source unavailable or ambiguous";
            continue;
        }
        m.device = m.source_devices.front();
        e.preview = static_cast<int>(preview_.mappings.size());
        preview_.mappings.push_back(std::move(m));
        e.reading.reason = "Awaiting source sample";
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const bool cpt = entries_[axis].reading.assigned, fo = entries_[axis + 4].reading.assigned;
        if (cpt != fo) {
            mirrors_[cpt ? axis + 4 : axis] = cpt ? axis : axis + 4;
            entries_[cpt ? axis : axis + 4].reading.shared = true;
        }
    }
}
void FlightDisplay::update(const Mapper &live, const SourceBank &values, double ms) {
    for (auto &m : preview_.mappings) {
        const auto active = std::find_if(live.mappings.begin(), live.mappings.end(), [&](const auto &x) {
            return x.profile == m.profile && x.local_id == m.local_id;
        });
        if (active != live.mappings.end()) {
            m.state = active->state;
            continue;
        }
        sources_.clear();
        for (std::size_t i = 0; i < m.controls.size(); ++i)
            sources_.push_back(values[m.source_devices[i]][m.controls[i]]);
        preview_.input(m, sources_, ms, false);
        settle_axis(m, ms);
    }
    // Tick only standalone previews: never advance copied live filter state.
    for (auto &e : entries_) {
        if (e.preview < 0)
            continue;
        const auto &m = preview_.mappings[static_cast<std::size_t>(e.preview)];
        const auto &s = m.state;
        auto &r = e.reading;
        r.available = s.available && s.baseline;
        r.value = s.desired;
        r.raw = values[m.source_devices.front()][m.controls.front()].value;
        r.reverse_raw = r.split ? values[m.source_devices.back()][m.controls.back()].value : 0;
        r.delivery = s.delivery;
        r.reason = s.reason;
        if (!r.available && r.reason.empty())
            r.reason = "Awaiting valid source sample";
        r.normalized = flight_fraction(static_cast<std::size_t>(&e-entries_.data()), s.desired);
        flight_physical(r, m.binding, r.raw);
        if (m.binding.target.throttle_engine >= 0) {
            r.forward = r.split ? std::clamp(s.normalized, 0.0, 1.0) : std::clamp(s.desired / static_cast<double>(m.binding.target.maximum), 0.0, 1.0);
            r.reverse =
                r.split ? std::clamp(s.reverse_normalized, 0.0, 1.0) : std::clamp(s.desired / static_cast<double>(m.binding.target.minimum), 0.0, 1.0);
            r.reverse_blocked = r.split && s.physical_forward > 0 && s.physical_reverse > 0;
        }
        if (!m.binding.enabled)
            r.reason = r.available ? "Display-only (mapping disabled)" : "Display-only source unavailable";
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) flight_transmitted(entries_[i].reading, i, live);
}
void flight_physical(FlightReadout &r, const Binding &b, int raw) noexcept {
    const auto &c = b.calibration;
    r.physical = physical_fraction(raw, c);
    r.deadzone_available = c.centered && calibration_valid(c);
    const double a = physical_fraction(c.center - c.dead_center_low, c);
    const double z = physical_fraction(c.center + c.dead_center_high, c);
    r.deadzone_low = std::min(a, z); r.deadzone_high = std::max(a, z);
}
void flight_transmitted(FlightReadout &r, std::size_t slot, const Mapper &live) {
    if (slot >= 8 || slot % 4 >= 3) return;
    r.transmitted_available = false;
    const auto *t = flight_target(slot);
    if (!live.ready || !t) return;
    const auto record = live.records.find(record_name(t->descriptor.record));
    if (record == live.records.end() || !record->second.transmitted_qpc) return;
    if (const auto value = extract(t->descriptor, record->second.transmitted)) {
        r.transmitted = flight_fraction(slot, *value);
        r.transmitted_available = true;
    }
}
FlightSnapshot FlightDisplay::snapshot() const {
    FlightSnapshot out;
    for (std::size_t i = 0; i < flight_slots; ++i) {
        out[i] = entries_[mirrors_[i]].reading;
        out[i].slot = static_cast<FlightSlot>(i);
    }
    return out;
}
bool FlightDisplay::settling() const noexcept {
    return std::any_of(preview_.mappings.begin(), preview_.mappings.end(), [](const auto &m) {
        return m.state.available &&
               (m.binding.calibration.lowpass_ms > 0 || m.binding.reverse_calibration.lowpass_ms > 0);
    });
}
} // namespace hwio
