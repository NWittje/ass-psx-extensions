#include "hwio/tray_status.hpp"
#include <algorithm>
#include <sstream>

namespace hwio {
std::vector<ControllerRequirement> controller_requirements(const std::vector<Profile> &profiles,
                                                           const AxisConfiguration &axes,
                                                           const ButtonConfiguration &buttons,
                                                           const DisplayAssignments &display,const CockpitConfiguration& cockpit) {
    std::map<std::string, ControllerRequirement> requirements;
    auto add = [&](const Profile &p, ControlKey key) {
        if (key.kind != ControlKind::axis && key.kind != ControlKind::button)
            return;
        Profile identity = p;
        identity.bindings.clear();
        identity.id = "controller";
        identity.name.clear();
        if (identity.identity.instance_path.empty())
            identity.explicit_path = false;
        const auto signature = profile_encode(identity);
        auto &r = requirements[signature];
        r.profile = std::move(identity);
        if (std::none_of(r.controls.begin(), r.controls.end(), [&](const auto &k) { return same(k, key); }))
            r.controls.push_back(key);
    };
    auto binding = [&](const Profile &p, const Binding &b) {
        auto source = [&](const Source &s) {
            if (s.profile.empty() || s.profile == p.id) {
                add(p, s.key);
                return;
            }
            const auto other =
                std::find_if(profiles.begin(), profiles.end(), [&](const auto &v) { return v.id == s.profile; });
            if (other != profiles.end())
                add(*other, s.key);
            else {
                Profile missing;
                missing.identity.name = "Missing profile: " + s.profile;
                add(missing, s.key);
            }
        };
        for (const auto &s : b.sources)
            source(s);
        if (b.throttle_layout == ThrottleLayout::separate_forward_reverse &&
            (b.reverse_source.key.usage_page || b.reverse_source.key.backend!=BackendKind::usb_hid))
            source(b.reverse_source);
    };
    for (const auto &reference : display)
        if (reference.choice == DisplayChoice::binding) {
            const auto p = std::find_if(profiles.begin(), profiles.end(),
                                        [&](const auto &v) { return v.id == reference.profile; });
            if (p == profiles.end() || std::none_of(p->bindings.begin(), p->bindings.end(),
                                                    [&](const auto &b) { return b.id == reference.binding; })) {
                ControllerRequirement missing;
                missing.profile.identity.name = "Unresolved display source: " + reference.profile;
                requirements["unresolved:" + reference.profile + ":" + std::to_string(reference.binding)] =
                    std::move(missing);
            }
        }
    for (const auto &p : profiles)
        for (const auto &b : p.bindings) {
            const bool shown = std::any_of(display.begin(), display.end(), [&](const auto &r) {
                return r.choice == DisplayChoice::binding && r.profile == p.id && r.binding == b.id;
            });
            if ((b.enabled && !axis_replaces(axes, p.id, b.id) && !button_replaces(buttons, p.id, b.id)) || shown)
                binding(p, b);
        }
    for (const auto &row : axes.rows)
        if (row.active && !row.applied.source.bindings.empty()) {
            const auto &s = row.applied;
            const auto &b = s.source.bindings[0];
            for (const auto &source : b.sources)
                add(s.source, source.key);
            if (b.throttle_layout == ThrottleLayout::separate_forward_reverse && !s.reverse.id.empty())
                add(s.reverse, b.reverse_source.key);
        }
    if (axes.override_assigned)
        for (const auto &b : axes.override_source.bindings)
            for (const auto &s : b.sources)
                add(axes.override_source, s.key);
    for (const auto &[id, row] : buttons.rows)
        for (const auto &b : row.applied.bindings)
            for (const auto &s : b.sources)
                add(row.applied, s.key);
    std::vector<ControllerRequirement> out;
    for(const auto& [id,row]:cockpit.rows)if(row.applied.enabled){const auto definition=active_catalogue().cockpit.find(id);if(definition==active_catalogue().cockpit.end())continue;
        Profile p;p.identity.backend=BackendKind::pokeys_ethernet;p.identity.hardware_key=row.applied.device;
        ControlKey key;key.backend=BackendKind::pokeys_ethernet;key.kind=definition->second->encoder?ControlKind::button:ControlKind::axis;
        key.channel_kind=static_cast<std::uint16_t>(definition->second->encoder?row.applied.software?pokeys::Channel::digital:pokeys::Channel::encoder_count:pokeys::Channel::analog);
        key.channel=static_cast<std::uint16_t>(row.applied.a);add(p,key);if(definition->second->encoder && row.applied.software){key.channel=static_cast<std::uint16_t>(row.applied.b);add(p,key);}
    }
    out.reserve(requirements.size());
    for (auto &[id, r] : requirements)
        out.push_back(std::move(r));
    return out;
}
TraySummary TrayHealth::update(const std::vector<ControllerRequirement> &required, const RuntimeView &view,
                               std::uint64_t now) {
    TraySummary out;
    out.assigned = static_cast<unsigned>(required.size());
    std::vector<DeviceIdentity> identities;
    std::vector<const DeviceInfo *> devices;
    for (const auto &d : view.devices)
        if (d.connected) {
            identities.push_back(d.identity);
            devices.push_back(&d);
        }
    for (const auto &r : required) {
        const int index = match_profile(r.profile, identities);
        bool available = index >= 0;
        if (available)
            for (const auto &key : r.controls)
                if (std::none_of(devices[static_cast<std::size_t>(index)]->controls.begin(),
                                 devices[static_cast<std::size_t>(index)]->controls.end(),
                                 [&](const auto &c) { return same(key, c.key); }))
                    available = false;
        if (available)
            ++out.found;
        else
            out.detail += wide(r.profile.identity.name) +
                          (index == -2 ? L": ambiguous identity\n" : L": assigned controller/control unavailable\n");
    }
    out.controllers_state = out.found == out.assigned ? 3u : 1u;
    out.controllers = L"Controllers";
    if (initialized_ && (view.network.overflows > network_overflow_ || view.input_overflow > input_overflow_))
        warning_until_ = now + 5000;
    initialized_ = true;
    network_overflow_ = view.network.overflows;
    input_overflow_ = view.input_overflow;
    if (!view.network.connected) {
        out.psx = L"PSX: Disconnected";
        out.psx_state = 2;
    } else if (!view.network.ready) {
        out.psx = L"PSX: Synchronizing";
        out.psx_state = 1;
    } else if (view.network.error || view.network.write_stall_ms >= 250 || now < warning_until_) {
        out.psx = L"PSX: Communication issue";
        out.psx_state = 1;
        out.detail += view.network.write_stall_ms >= 250 ? L"Socket output blocked for at least 250 ms\n"
                                                         : L"Communication/input overflow or transport fault\n";
    } else {
        out.psx = L"PSX: Connected";
        out.psx_state = 3;
    }
    out.detail += wide(view.network.message);
    return out;
}
} // namespace hwio
