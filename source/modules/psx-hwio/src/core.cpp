#include "hwio/catalogue.hpp"
#include "hwio/core.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace hwio {
bool same(ControlKey a, ControlKey b) noexcept {
    if(a.backend!=b.backend)return false;
    if(a.backend!=BackendKind::usb_hid)return a.kind==b.kind && a.channel_kind==b.channel_kind && a.channel==b.channel;
    return a.kind == b.kind && a.usage_page == b.usage_page && a.usage == b.usage &&
           a.link_collection == b.link_collection && a.data_index == b.data_index && a.report_id == b.report_id;
}
std::string record_name(RecordKey key) {
    return std::string(key.kind == RecordKind::qh   ? "Qh"
                       : key.kind == RecordKind::qi ? "Qi"
                                                    : "Qs") +
           std::to_string(key.index);
}
bool integer(std::string_view text, std::int32_t &value) noexcept {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
bool parse_key(std::string_view text, RecordKey &key) noexcept {
    if (text.size() < 3 || text[0] != 'Q' || (text[1] != 'h' && text[1] != 'i' && text[1] != 's'))
        return false;
    std::int32_t i{};
    if (!integer(text.substr(2), i) || i < 0 || i > 65535)
        return false;
    key = {text[1] == 'h'   ? RecordKind::qh
           : text[1] == 'i' ? RecordKind::qi
                            : RecordKind::qs,
           static_cast<std::uint16_t>(i)};
    return true;
}
static std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    for (;;) {
        auto end = text.find(';', begin);
        out.push_back(text.substr(begin, end == text.npos ? text.size() - begin : end - begin));
        if (end == text.npos)
            break;
        begin = end + 1;
    }
    return out;
}
bool descriptor_valid(const TargetDescriptor &d, std::string &error) {
    if (d.id.empty() || d.minimum > d.maximum || d.shift > 31 || d.field_count > 256 || d.throttle_engine < -1 ||
        d.throttle_engine > 3) {
        error = "Invalid target identity/range/field count";
        return false;
    }
    if (d.layout == ValueLayout::semicolon_field &&
        (d.record.kind != RecordKind::qs || d.field_count == 0 || d.field >= d.field_count)) {
        error = "Structured target requires Qs, field and exact field count";
        return false;
    }
    if (d.layout == ValueLayout::fixed_decimal &&
        (d.record.kind != RecordKind::qs || !d.field_count || d.field_count > 9 ||
         d.field > 4096 - d.field_count || d.minimum < 0 ||
         std::to_string(d.maximum).size() > d.field_count)) {
        error = "Decimal slice requires bounded Qs offset, width and nonnegative range";
        return false;
    }
    if (d.layout != ValueLayout::semicolon_field && d.layout != ValueLayout::fixed_decimal && d.record.kind == RecordKind::qs) {
        error = "Qs requires a structured field descriptor";
        return false;
    }
    if (d.layout == ValueLayout::bit_field &&
        (d.owned_mask == 0 || d.minimum < 0 ||
         ((static_cast<std::uint64_t>(d.maximum) << d.shift) & ~static_cast<std::uint64_t>(d.owned_mask)))) {
        error = "Bit target range does not fit its owned mask";
        return false;
    }
    if (d.released < d.minimum || d.released > d.maximum || d.pressed < d.minimum || d.pressed > d.maximum) {
        error = "Pressed/released values outside target range";
        return false;
    }
    if (static_cast<unsigned>(d.layout) > 3 || static_cast<unsigned>(d.semantics) > 4 ||
        static_cast<unsigned>(d.record.kind) > 2) {
        error = "Unknown target type";
        return false;
    }
    if (d.throttle_engine >= 0 &&
        (d.layout != ValueLayout::semicolon_field || d.field_count > 8)) {
        error = "Throttle policy requires a bounded structured engine field";
        return false;
    }
    return true;
}
bool overlaps(const TargetDescriptor &a, const TargetDescriptor &b) noexcept {
    if (!(a.record == b.record))
        return false;
    if (a.layout == ValueLayout::semicolon_field && b.layout == a.layout)
        return a.field == b.field;
    if (a.layout == ValueLayout::fixed_decimal && b.layout == a.layout)
        return a.field < b.field + b.field_count && b.field < a.field + a.field_count;
    if (a.layout == ValueLayout::bit_field && b.layout == a.layout)
        return (a.owned_mask & b.owned_mask) != 0;
    return true;
}
std::optional<std::int32_t> extract(const TargetDescriptor &d, std::string_view payload) {
    std::int32_t v{};
    if (d.layout == ValueLayout::fixed_decimal) {
        if (!d.field_count || d.field > payload.size() || d.field_count > payload.size() - d.field)
            return {};
        const auto selected=payload.substr(d.field,d.field_count);
        if(selected.find_first_not_of("0123456789")!=selected.npos || !integer(selected,v))return {};
    } else if (d.layout == ValueLayout::semicolon_field) {
        auto f = fields(payload);
        if (f.size() != d.field_count || !integer(f[d.field], v))
            return {};
    } else {
        if (!integer(payload, v))
            return {};
        if (d.layout == ValueLayout::bit_field)
            v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v) & d.owned_mask) >> d.shift);
    }
    if (v < d.minimum || v > d.maximum)
        return {};
    return v;
}
bool aileron_trim_command(const TargetDescriptor& d) noexcept {
    return d.record==RecordKey{RecordKind::qh,418} && d.layout==ValueLayout::bit_field &&
           d.semantics==Semantics::held && d.writable && !d.minimum &&
           !d.released && !d.shift && !d.transient_mask &&
           (d.owned_mask==5 || d.owned_mask==10 || d.owned_mask==15) &&
           d.maximum==static_cast<int>(d.owned_mask);
}
std::optional<std::string> compose(const TargetDescriptor &d, std::string_view payload, std::int32_t v) {
    if (!d.writable || d.semantics == Semantics::read_only || v < d.minimum || v > d.maximum)
        return {};
    std::string result;
    if (d.layout == ValueLayout::fixed_decimal) {
        if (!d.field_count || d.field > payload.size() || d.field_count > payload.size() - d.field ||
            payload.substr(d.field,d.field_count).find_first_not_of("0123456789")!=payload.npos)
            return {};
        const auto digits=std::to_string(v);if(digits.size()>d.field_count)return {};
        result=payload;result.replace(d.field,d.field_count,std::string(d.field_count-digits.size(),'0')+digits);
    } else if (d.layout == ValueLayout::semicolon_field) {
        auto f = fields(payload);
        if (f.size() != d.field_count)
            return {};
        if (!catalogue_record_valid(d, f)) return {};
        for (std::size_t i = 0; i < f.size(); ++i) {
            if (i)
                result += ';';
            result += i == d.field ? std::to_string(v) : std::string(f[i]);
        }
    } else if (d.layout == ValueLayout::bit_field) {
        std::int32_t previous{};
        if (!integer(payload, previous)) {
            if(!payload.empty() || !aileron_trim_command(d))return {};
        } else if(aileron_trim_command(d) && (previous<0 || previous>15))return {};
        const auto merged = merge_owned_bits(static_cast<std::uint32_t>(previous),
                                             static_cast<std::uint32_t>(v) << d.shift, d.owned_mask, d.transient_mask);
        result = std::to_string(static_cast<std::int32_t>(merged));
    } else
        result = std::to_string(v);
    return record_name(d.record) + "=" + result;
}
const std::vector<TargetDescriptor> &catalogue() { return active_catalogue().targets; }
bool calibration_valid(const AxisCalibration &c) noexcept {
    const double v[] = {c.minimum,          c.center,       c.maximum,      c.dead_center_low,
                        c.dead_center_high, c.dead_minimum, c.dead_maximum, c.lowpass_ms};
    for (double x : v)
        if (!std::isfinite(x))
            return false;
    if (c.minimum >= c.maximum || c.dead_center_low < 0 || c.dead_center_high < 0 || c.dead_minimum < 0 ||
        c.dead_maximum < 0 || c.lowpass_ms < 0 || c.lowpass_ms > 100 || (c.lowpass_ms > 0 && c.lowpass_ms < 1))
        return false;
    if (c.centered)
        return c.minimum + c.dead_minimum < c.center - c.dead_center_low &&
               c.center + c.dead_center_high < c.maximum - c.dead_maximum;
    return c.minimum + c.dead_minimum < c.maximum - c.dead_maximum;
}
double normalize(double v, const AxisCalibration &c) noexcept {
    double result = 0;
    if (c.centered) {
        if (v < c.center - c.dead_center_low)
            result = (v - c.center + c.dead_center_low) / (c.center - c.dead_center_low - c.minimum - c.dead_minimum);
        else if (v > c.center + c.dead_center_high)
            result = (v - c.center - c.dead_center_high) / (c.maximum - c.dead_maximum - c.center - c.dead_center_high);
        result = std::clamp(result, -1.0, 1.0);
        if (c.inverted)
            result = -result;
    } else {
        result = std::clamp(
            (v - c.minimum - c.dead_minimum) / (c.maximum - c.dead_maximum - c.minimum - c.dead_minimum), 0.0, 1.0);
        if (c.inverted)
            result = 1 - result;
    }
    return result;
}
double physical_fraction(double value, const AxisCalibration &c) noexcept {
    auto physical = c;
    physical.dead_minimum = physical.dead_maximum = physical.dead_center_low = physical.dead_center_high = 0;
    return normalize(value, physical);
}
double control_response(double value, double gain) noexcept {
    return gain * value / (1 + (gain - 1) * std::abs(value));
}
double Filter::sample(double raw, const AxisCalibration &c, double ms) noexcept {
    if (c.median_three) {
        if (count_ == 0) {
            history_.fill(raw);
            count_ = 3;
        }
        history_[pos_] = raw;
        pos_ = (pos_ + 1) % 3;
        count_ = std::min(count_ + 1, std::size_t{3});
        if (count_ == 3) {
            auto sorted = history_;
            std::sort(sorted.begin(), sorted.end());
            raw = sorted[1];
        }
    }
    settle(c, ms);
    median_raw_ = raw;
    target_ = normalize(raw, c);
    if (!initialized_ || c.lowpass_ms == 0) {
        initialized_ = true;
        value_ = target_;
        last_ = ms;
    }
    return value_;
}
double Filter::settle(const AxisCalibration &c, double ms) noexcept {
    if (initialized_) {
        const double dt = std::max(0.0, ms - last_);
        value_ = c.lowpass_ms == 0 ? target_ : target_ + (value_ - target_) * std::exp(-dt / c.lowpass_ms);
        last_ = ms;
    }
    return value_;
}
bool validate(const Profile &p, std::string &error) {
    if (p.id.empty() || p.id.size() > 100 || p.bindings.size() > max_bindings ||
        p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos) {
        error = "Invalid stable profile ID or binding count";
        return false;
    }
    std::set<std::uint64_t> ids;
    for (const auto &b : p.bindings) {
        if (b.id == 0 || !ids.insert(b.id).second || b.sources.empty() || b.sources.size() > 8 ||
            !calibration_valid(b.calibration) || static_cast<unsigned>(b.mode) > 5 ||
            static_cast<unsigned>(b.throttle) > 1 || !std::isfinite(b.control_stiffness) ||
            std::abs(b.control_stiffness) > 1 || !std::isfinite(b.response_gain) || b.response_gain <= 0) {
            error = "Invalid/duplicate binding, source list, calibration or mode";
            return false;
        }
        if (!descriptor_valid(b.target, error))
            return false;
        if (b.enabled && (!b.target.writable || b.target.semantics == Semantics::read_only)) {
            error = "Read-only target cannot be bound for input";
            return false;
        }
        if (b.mode == MappingMode::axis && b.target.semantics != Semantics::absolute) {
            error = "Axis needs an absolute target";
            return false;
        }
        if (b.throttle_layout == ThrottleLayout::separate_forward_reverse && b.target.throttle_engine >= 0 &&
            (!calibration_valid(b.reverse_calibration) || b.calibration.centered || b.reverse_calibration.centered)) {
            error = "Separate throttle axes require independent valid unipolar calibrations";
            return false;
        }
        if (b.mode == MappingMode::relative && (b.target.semantics != Semantics::relative || b.step == 0)) {
            error = "Relative mode needs a relative target and nonzero step";
            return false;
        }
        if ((b.target.semantics == Semantics::pulse || b.target.semantics == Semantics::relative) &&
            b.mode != MappingMode::momentary && b.mode != MappingMode::relative) {
            error = "Event target requires momentary or relative mode";
            return false;
        }
        for (const auto &s : b.sources)
            if (static_cast<unsigned>(s.key.kind) > 2) {
                error = "Invalid source kind";
                return false;
            }
        if (b.mode == MappingMode::selector && b.selector.empty()) {
            error = "Selector needs an explicit pattern:value table";
            return false;
        }
        for (const auto &v : b.selector)
            if (v.second < b.target.minimum || v.second > b.target.maximum) {
                error = "Selector result outside target range";
                return false;
            }
        if (b.thresholds.size() > 31 || !std::isfinite(b.hysteresis) || b.hysteresis < 0 || b.hysteresis > 1) {
            error = "Invalid selector hysteresis/count";
            return false;
        }
        double previous = -2;
        for (double t : b.thresholds) {
            if (!std::isfinite(t) || t <= previous || t < -1 || t > 1) {
                error = "Thresholds must be sorted normalized positions";
                return false;
            }
            previous = t;
        }
    }
    for (std::size_t i = 0; i < p.bindings.size(); ++i)
        for (std::size_t j = i + 1; j < p.bindings.size(); ++j)
            if (p.bindings[i].enabled && p.bindings[j].enabled &&
                overlaps(p.bindings[i].target, p.bindings[j].target)) {
                error = "Field ownership conflict: " + p.bindings[i].name + " / " + p.bindings[j].name;
                return false;
            }
    return true;
}
int identity_score(const DeviceIdentity &a, const DeviceIdentity &b) noexcept {
    if(a.backend!=b.backend)return 0;
    if(a.backend!=BackendKind::usb_hid)return !a.hardware_key.empty() && a.hardware_key==b.hardware_key ? 3 : 0;
    if (a.vendor != b.vendor || a.product != b.product || a.usage_page != b.usage_page || a.usage != b.usage ||
        a.collection_identity != b.collection_identity)
        return 0;
    if (!a.serial.empty())
        return a.serial == b.serial ? 3 : 0;
    if (!a.container_id.empty() && a.container_id == b.container_id)
        return 2;
    return a.name == b.name && !a.descriptor_signature.empty() && a.descriptor_signature == b.descriptor_signature ? 1
                                                                                                                   : 0;
}
std::set<std::pair<std::string, std::uint64_t>> ownership_conflicts(const std::vector<Profile> &profiles) {
    std::set<std::pair<std::string, std::uint64_t>> conflicts;
    struct Owner {
        const Profile *profile;
        const Binding *binding;
    };
    std::vector<Owner> owners;
    for (const auto &p : profiles)
        for (const auto &b : p.bindings)
            if (b.enabled)
                owners.push_back({&p, &b});
    if (owners.size() > max_bindings) {
        for (const auto &owner : owners)
            conflicts.insert({owner.profile->id, owner.binding->id});
        return conflicts;
    }
    for (std::size_t i = 0; i < owners.size(); ++i)
        for (std::size_t j = i + 1; j < owners.size(); ++j)
            if (overlaps(owners[i].binding->target, owners[j].binding->target)) {
                conflicts.insert({owners[i].profile->id, owners[i].binding->id});
                conflicts.insert({owners[j].profile->id, owners[j].binding->id});
            }
    return conflicts;
}
int match_identity(const DeviceIdentity &expected, const std::vector<DeviceIdentity> &devices) noexcept {
    int best = 0, index = -1;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        int score = identity_score(expected, devices[i]);
        if (score > best) {
            best = score;
            index = static_cast<int>(i);
        } else if (score != 0 && score == best)
            index = -2;
    }
    return index;
}
int match_profile(const Profile &profile, const std::vector<DeviceIdentity> &devices) noexcept {
    const int match = match_identity(profile.identity, devices);
    if (match != -2 || !profile.explicit_path || profile.identity.instance_path.empty())
        return match;
    int assigned = -2;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (identity_score(profile.identity, devices[i]) &&
            devices[i].instance_path == profile.identity.instance_path) {
            if (assigned >= 0)
                return -2;
            assigned = static_cast<int>(i);
        }
    }
    return assigned;
}
void Mapper::boundary(std::uint64_t new_epoch, bool same_connection_load) {
    epoch = new_epoch;
    ready = false;
    if (same_connection_load) {
        // PSX ECON/XECON records are change-only across situation loads. Their
        // established values remain current on this connection; pending local
        // intent does not. A socket loss still discards every baseline below.
        for (auto &[name, record] : records)
            record.effective = record.committed;
    } else {
        at = AtAuthority::unavailable;
        records.clear();
    }
    decisions_.clear();
    last_record_event_.clear();
    overflow = false;
    failed_axes=0;runaway_trim=0;trim_owned_mask=0;trim_fo_owned=false;
    for (auto &m : mappings)
        m.state = MappingState{};
}
int Mapper::output_value(const TargetDescriptor& d,int value) const noexcept {
    if(d.record==RecordKey{RecordKind::qs,120} && d.field<3 && (failed_axes&(1u<<d.field)))return 0;
    if(runaway_trim && d.record.kind==RecordKind::qh){
        if(d.record.index==398)return runaway_trim;
        if(d.record.index==399)return 0;
        if(d.record.index==396)return d.layout==ValueLayout::bit_field && (d.owned_mask&trim_owned_mask)?0:static_cast<int>(static_cast<unsigned>(value)&~trim_owned_mask);
    }
    return value;
}
void Mapper::constrain_record(RecordKey key,std::string& payload) const {
    if(failed_axes && key==RecordKey{RecordKind::qs,120})for(std::size_t slot=0;slot<3;++slot){
        const auto* t=flight_target(slot);if(!t || t->descriptor.record!=key)continue;
        const auto& d=t->descriptor;if(!(failed_axes&(1u<<d.field)))continue;
        if(const auto line=compose(d,payload,0))payload=line->substr(line->find('=')+1);
    }
    if(runaway_trim && key.kind==RecordKind::qh){
        if(key.index==398)payload=std::to_string(runaway_trim);
        else if(key.index==399 && trim_fo_owned)payload="0";
        else if(key.index==396){int value{};if(integer(payload,value))payload=std::to_string(static_cast<unsigned>(value)&~trim_owned_mask);}
    }
}
void Mapper::decision(Mapped &m, int value) {
    const auto &d = m.binding.target;
    if (d.layout != ValueLayout::integer) {
        auto record = records.find(record_name(d.record));
        if ((record == records.end() && !aileron_trim_command(d)) ||
            (record!=records.end() && record->second.sequence && record->second.effective.empty()) ||
            !compose(d, record==records.end()?"":record->second.effective, value)) {
            m.state.reason = "Event withheld: current packed target baseline unavailable";
            return;
        }
    }
    if (decisions_.size() >= 1024) {
        overflow = true;
        decisions_.clear();
        ready = false;
        return;
    }
    decisions_.push_back({m.binding.id, value, m.state.input_qpc});
}
static int axis_scale(double value, const Binding &b) {
    if (b.target.record == RecordKey{RecordKind::qs,120} && b.target.field < 3 && b.calibration.centered)
        value = control_response(value, b.response_gain);
    if (const auto *flaps=flight_target(13); flaps && b.target.id==flaps->descriptor.id) {
        const double unit=b.calibration.centered?(value+1)*0.5:value;
        return flight_detent(static_cast<int>(std::llround(std::clamp(unit,0.0,1.0)*6)));
    }
    if (b.calibration.centered && b.target.minimum < 0 && b.target.maximum > 0)
        return static_cast<int>(std::llround(value < 0 ? -value * b.target.minimum : value * b.target.maximum));
    const double unit = b.calibration.centered ? (value + 1) * 0.5 : value;
    return static_cast<int>(
        std::llround(b.target.minimum + unit * (static_cast<double>(b.target.maximum) - b.target.minimum)));
}
void Mapper::receive(std::string_view line, std::int64_t qpc) {
    const auto eq = line.find('=');
    RecordKey key;
    if (eq == line.npos || !parse_key(line.substr(0, eq), key))
        return;
    const bool diagnostic = (key.kind==RecordKind::qs && (key.index==323 || key.index==324)) ||
                            active_catalogue().observed_controls.count(record_name(key));
    if (!diagnostic && !watched_records.count(record_name(key)) &&
        std::none_of(mappings.begin(), mappings.end(), [&](const auto &m) { return m.binding.target.record == key; }))
        return;
    const auto name = record_name(key);
    auto &r = records[name];
    r.observed = std::string(line.substr(eq + 1));
    r.effective = r.observed;
    r.committed = r.observed;
    r.qpc = qpc;
    ++r.sequence;
    if (key == active_catalogue().authority_record) {
        const auto previous = at;
        at = active_catalogue().authority(r.observed);
        if (ready && at == AtAuthority::physical && previous != at)
            ++at_inactive_sequence;
        if (at != previous) {
            for (std::size_t engine=9; engine<13; ++engine) if (const auto *t=flight_target(engine)) {
                auto throttle_record=records.find(record_name(t->descriptor.record));
                if(throttle_record!=records.end()) throttle_record->second.effective=throttle_record->second.observed;
            }
            for (auto &m : mappings)
                if (m.binding.target.throttle_engine >= 0)
                    m.state.dirty = true;
        }
    }
    for (auto &m : mappings) {
        auto &s = m.state;
        const auto &d = m.binding.target;
        if (!(d.record == key))
            continue;
        const auto observed = extract(d, r.observed);
        if (observed && s.pending && *observed == s.desired) {
            s.pending = false;
            s.delivery = DeliveryState::observed;
        }
        if (s.available && d.semantics != Semantics::pulse && d.semantics != Semantics::relative &&
            m.binding.mode != MappingMode::toggle) {
            s.dirty = true;
        }
        // Toggle intent is retained across unrelated electrical updates; no self echo is expected.
        if (s.available && s.pending && m.binding.mode == MappingMode::toggle)
            s.dirty = true;
    }
}
void Mapper::input(Mapped &m, const std::vector<SourceValue> &values, double ms, bool edge, bool baseline,
                   bool relative) {
    auto &s = m.state;
    const auto &b = m.binding;
    if (values.empty() || std::any_of(values.begin(), values.end(), [](auto v) { return !v.available; })) {
        s.available = false;
        s.baseline = false;
        s.filter.reset();
        s.reverse_filter.reset();
        s.reason = "Source unavailable";
        return;
    }
    const bool first = !s.baseline || baseline;
    s.available = true;
    s.input_qpc = values[0].qpc;
    s.reason.clear();
    int value = values[0].value;
    const bool rising = value != 0 && s.previous == 0;
    if (b.mode == MappingMode::axis) {
        if (first || s.forward_filter_qpc != values[0].qpc || s.forward_sequence != values[0].sequence ||
            s.forward_raw != value) {
            s.normalized = s.filter.sample(value, b.calibration, ms);
            s.forward_filter_qpc = values[0].qpc;
            s.forward_sequence = values[0].sequence;
            s.forward_raw = value;
        }
        int next = axis_scale(s.normalized, b);
        if (b.target.throttle_engine >= 0 && b.throttle_layout == ThrottleLayout::separate_forward_reverse &&
            m.reverse_unassigned) {
            s.reverse_normalized = s.physical_reverse = 0;
            s.physical_forward = normalize(value, b.calibration);
            next = static_cast<int>(std::llround(s.normalized * b.target.maximum));
            s.reason = "Forward only: reverser unassigned";
        } else if (b.target.throttle_engine >= 0 && b.throttle_layout == ThrottleLayout::separate_forward_reverse) {
            if (values.size() < 2) {
                s.available = false;
                s.reason = "Reverse source unavailable";
                return;
            }
            if (first || s.reverse_filter_qpc != values.back().qpc || s.reverse_sequence != values.back().sequence ||
                s.reverse_raw != values.back().value) {
                s.reverse_normalized = s.reverse_filter.sample(values.back().value, b.reverse_calibration, ms);
                s.reverse_filter_qpc = values.back().qpc;
                s.reverse_sequence = values.back().sequence;
                s.reverse_raw = values.back().value;
            }
            s.physical_forward = normalize(value, b.calibration);
            s.physical_reverse = normalize(values.back().value, b.reverse_calibration);
            next = static_cast<int>(std::llround(s.physical_forward > 0   ? s.normalized * b.target.maximum
                                                 : s.physical_reverse > 0 ? s.reverse_normalized * b.target.minimum
                                                                          : 0));
            if (s.physical_forward > 0 && normalize(values.back().value, b.reverse_calibration) > 0)
                s.reason = "Reverse blocked: physical forward axis above idle";
        }
        if (next != s.desired || first)
            s.dirty = true;
        s.desired = next;
    } else if (b.mode == MappingMode::selector) {
        int pattern = value;
        if (!b.thresholds.empty()) {
            s.normalized = normalize(value, b.calibration);
            if (first || s.selector_position < 0)
                s.selector_position = static_cast<int>(
                    std::upper_bound(b.thresholds.begin(), b.thresholds.end(), s.normalized) - b.thresholds.begin());
            while (s.selector_position < static_cast<int>(b.thresholds.size()) &&
                   s.normalized > b.thresholds[static_cast<std::size_t>(s.selector_position)] + b.hysteresis)
                ++s.selector_position;
            while (s.selector_position > 0 &&
                   s.normalized < b.thresholds[static_cast<std::size_t>(s.selector_position - 1)] - b.hysteresis)
                --s.selector_position;
            pattern = s.selector_position;
        } else if (values.size() > 1) {
            pattern = 0;
            for (std::size_t i = 0; i < values.size(); ++i)
                if (values[i].value)
                    pattern |= 1 << i;
        }
        auto it = b.selector.find(pattern);
        if (it == b.selector.end()) {
            s.reason = "Invalid selector contacts; retaining last valid destination";
            s.available = false;
        } else {
            s.desired = it->second;
            s.dirty = s.dirty || first || value != s.previous || values.size() > 1;
        }
    } else if (b.mode == MappingMode::maintained) {
        s.desired = value ? b.target.pressed : b.target.released;
        s.dirty = s.dirty || first || value != s.previous;
    } else if (b.mode == MappingMode::toggle) {
        if (first) {
            s.dirty = false;
            s.pending = false;
        }
        if (!first && edge && rising && ready) {
            auto it = records.find(record_name(b.target.record));
            auto current = it == records.end() ? std::optional<int>{} : extract(b.target, it->second.effective);
            if (s.pending)
                current = s.desired;
            if (current) {
                s.desired = *current == b.target.pressed ? b.target.released : b.target.pressed;
                s.dirty = true;
                s.pending = true;
            } else
                s.reason = "Toggle awaiting current destination baseline";
        }
    } else if (b.mode == MappingMode::relative) {
        if (first)
            s.dirty = false;
        if (!first && edge && ready) {
            const long long delta = relative
                                        ? static_cast<long long>(value) * b.step * (b.calibration.inverted ? -1 : 1)
                                        : (rising ? b.step : 0);
            s.relative_pending =
                static_cast<int>(std::clamp(static_cast<long long>(s.relative_pending) + delta, -100000LL, 100000LL));
            s.dirty = s.relative_pending != 0;
        }
    } else if (b.mode == MappingMode::momentary) {
        s.dirty = false;
        if (b.target.semantics == Semantics::pulse || b.target.semantics == Semantics::relative) {
            if (!first && edge && rising && ready)
                decision(m, b.target.pressed);
        } else {
            if (((!first && edge) || first) && ready) {
                s.desired = value ? b.target.pressed : b.target.released;
                decision(m, s.desired);
                s.held = value != 0;
            } else if (first) {
                s.held = false;
            }
        }
    }
    s.previous = value;
    s.baseline = true;
    if (b.target.semantics == Semantics::held && s.available)
        s.held = s.desired != b.target.released;
}
void settle_axis(Mapped &m, double ms) {
    auto &s = m.state;
    const auto &b = m.binding;
    if (m.externally_calculated || !s.available || !s.baseline || b.mode != MappingMode::axis)
        return;
    s.normalized = s.filter.settle(b.calibration, ms);
    int next = axis_scale(s.normalized, b);
    if (b.target.throttle_engine >= 0 && b.throttle_layout == ThrottleLayout::separate_forward_reverse) {
        s.reverse_normalized = m.reverse_unassigned ? 0 : s.reverse_filter.settle(b.reverse_calibration, ms);
        next = static_cast<int>(std::llround(s.physical_forward > 0   ? s.normalized * b.target.maximum
                                             : s.physical_reverse > 0 ? s.reverse_normalized * b.target.minimum
                                                                      : 0));
    }
    if (next != s.desired) {
        s.desired = next;
        s.dirty = true;
    }
}
void Mapper::tick(double ms) {
    for (auto &m : mappings)
        settle_axis(m, ms);
}
std::optional<Command> Mapper::next(double ms, std::int64_t qpc) {
    if (!ready)
        return {};
    std::set<std::uint64_t> blocked;
    for (auto decision_it = decisions_.begin(); decision_it != decisions_.end();) {
        const auto event = *decision_it;
        if (blocked.count(event.binding)) {
            ++decision_it;
            continue;
        }
        auto it = std::find_if(mappings.begin(), mappings.end(),
                               [&](const auto &m) { return m.binding.id == event.binding; });
        if (it == mappings.end()) {
            decision_it = decisions_.erase(decision_it);
            continue;
        }
        auto &m = *it;
        const auto &d = m.binding.target;
        auto &s = m.state;
        const bool paced = d.semantics == Semantics::pulse || d.semantics == Semantics::relative;
        const auto last_record_event = last_record_event_.find(record_name(d.record));
        if (paced && (ms - s.last_event_ms < delta_min_interval_ms ||
                      (last_record_event != last_record_event_.end() &&
                       ms - last_record_event->second < delta_min_interval_ms))) {
            blocked.insert(event.binding);
            ++decision_it;
            continue;
        }
        auto r = records.find(record_name(d.record));
        const bool invalid_received = aileron_trim_command(d) && r != records.end() &&
                                      r->second.sequence && r->second.effective.empty();
        auto line = invalid_received ? std::nullopt :
            compose(d, r == records.end() ? "" : r->second.effective, output_value(d,event.value));
        decision_it = decisions_.erase(decision_it);
        if (!line) {
            s.reason = "Queued event dropped: current target no longer valid";
            continue;
        }
        if (!paced && (r != records.end() || aileron_trim_command(d))) {
            // A predicted command is not a received observation or send receipt.
            records[record_name(d.record)].effective = line->substr(line->find('=') + 1);
            if(aileron_trim_command(d))s.pending=true;
        }
        s.last_event_ms = ms;
        if (paced)
            last_record_event_[record_name(d.record)] = ms;
        s.delivery = DeliveryState::queued;
        s.mapped_qpc = qpc;
        s.desired = event.value;
        Command command{m.binding.id, epoch, event.input_qpc, qpc, *line + "\n", true, 0, event.value};
        command.minimum_interval_ms=paced?50u:0u;
        return command;
    }
    for (auto &m : mappings) {
        auto &s = m.state;
        const auto &b = m.binding;
        const auto &d = b.target;
        if((d.record==RecordKey{RecordKind::qs,436} && (motor_throttle_suppression&(1u<<d.field))) || (d.record==RecordKey{RecordKind::qh,388} && motor_speedbrake_suppression)) {
            s.reason="Motor mapping owns this physical feedback";continue;
        }
        if (!b.enabled || !s.available || !s.dirty || !s.baseline)
            continue;
        if (d.throttle_engine >= 0 && b.throttle == ThrottlePolicy::at_servo_follower && at != AtAuthority::physical) {
            s.reason =
                at == AtAuthority::servo ? "A/T servo follower: input blanked" : "A/T mode unavailable: input withheld";
            continue;
        }
        auto it = records.find(record_name(d.record));
        const bool event = d.semantics == Semantics::pulse || d.semantics == Semantics::relative;
        const auto last_record_event = last_record_event_.find(record_name(d.record));
        if (event &&
            (ms - s.last_event_ms < delta_min_interval_ms ||
             (last_record_event != last_record_event_.end() && ms - last_record_event->second < delta_min_interval_ms)))
            continue;
        if (!event && it == records.end() && !aileron_trim_command(d)) {
            s.reason = "Awaiting current PSX record";
            continue;
        }
        if (b.mode == MappingMode::relative) {
            if (s.relative_pending == 0) {
                s.dirty = false;
                continue;
            }
            s.desired = std::clamp(s.relative_pending, d.minimum, d.maximum);
        }
        const std::string payload = it == records.end() ? "" : it->second.effective;
        if(aileron_trim_command(d) && it != records.end() && it->second.sequence && payload.empty()) {
            s.reason = "Destination record shape/range unavailable";
            continue;
        }
        const auto current = extract(d, payload);
        if (!event && current && *current == output_value(d,s.desired)) {
            s.dirty = false;
            continue;
        }
        auto line = compose(d, payload, output_value(d,s.desired));
        // Seat handovers update the shared record as one selected snapshot.
        // Include every current routed field before exposing any line to TCP.
        if (line && !event)
            for (const auto &sibling : mappings)
                if (sibling.externally_calculated && sibling.state.available && sibling.state.baseline &&
                    sibling.binding.target.record == d.record &&
                    !(d.record==RecordKey{RecordKind::qs,436} && (motor_throttle_suppression&(1u<<sibling.binding.target.field))) &&
                    !(sibling.binding.target.throttle_engine >= 0 &&
                      sibling.binding.throttle == ThrottlePolicy::at_servo_follower && at != AtAuthority::physical)) {
                    auto merged =
                        compose(sibling.binding.target, line->substr(line->find('=') + 1), output_value(sibling.binding.target,sibling.state.desired));
                    if (merged)
                        line = std::move(merged);
                }
        if (!line) {
            s.reason = "Destination record shape/range unavailable";
            continue;
        }
        if (!event && (it != records.end() || aileron_trim_command(d)))
            records[record_name(d.record)].effective = line->substr(line->find('=') + 1);
        s.dirty = false;
        s.pending = !event;
        s.delivery = DeliveryState::queued;
        s.mapped_qpc = qpc;
        s.reason.clear();
        if (event) {
            s.last_event_ms = ms;
            last_record_event_[record_name(d.record)] = ms;
            if (b.mode == MappingMode::relative) {
                s.relative_pending -= s.desired;
                s.dirty = s.relative_pending != 0;
            }
        }
        std::uint8_t followers = 0;
        if (d.throttle_engine >= 0 && at == AtAuthority::physical)
            for (const auto &sibling : mappings)
                if (sibling.binding.target.record == d.record &&
                    sibling.binding.throttle == ThrottlePolicy::at_servo_follower && sibling.state.available)
                    followers |= static_cast<std::uint8_t>(1u << sibling.binding.target.field);
        return Command{b.id, epoch, s.input_qpc, qpc, *line + "\n", event, followers, s.desired};
    }
    return {};
}
void Mapper::sent(std::uint64_t id, std::int64_t qpc, std::optional<int> value) {
    for (auto &m : mappings)
        if (m.binding.id == id) {
            m.state.sent = value ? *value : m.state.desired;
            m.state.sent_qpc = qpc;
            m.state.delivery = DeliveryState::sent;
        }
}
void Mapper::sent_record(std::string_view line, std::int64_t qpc) {
    if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
    const auto equal = line.find('=');
    RecordKey key;
    if (equal == line.npos || !parse_key(line.substr(0, equal), key))
        return;
    auto record = records.find(record_name(key));
    if (record != records.end()) {
        record->second.committed = std::string(line.substr(equal + 1));
        record->second.transmitted = record->second.committed;
        record->second.transmitted_qpc = qpc;
        record->second.effective = record->second.committed;
        // Preserve newer unsent owned intent, while retaining the actually completed
        // fields of bindings which disappeared at a device/profile boundary.
        for (const auto &m : mappings) {
            if (m.binding.target.record == key && m.state.pending && m.state.available) {
                const auto composed = compose(m.binding.target, record->second.effective, output_value(m.binding.target,m.state.desired));
                if (composed)
                    record->second.effective = composed->substr(composed->find('=') + 1);
            }
        }
        constrain_record(key,record->second.effective);
    }
    for (auto &m : mappings)
        if (m.binding.target.record == key) {
            auto value = extract(m.binding.target, line.substr(equal + 1));
            if (value) {
                m.state.sent = *value;
                m.state.sent_qpc = qpc;
                m.state.delivery = output_value(m.binding.target,m.state.desired) == *value ? DeliveryState::sent : DeliveryState::queued;
            }
        }
}
void Mapper::lost(std::uint16_t device) {
    for (auto &m : mappings)
        if (m.device == device ||
            std::find(m.source_devices.begin(), m.source_devices.end(), device) != m.source_devices.end()) {
            auto &s = m.state;
            s.relative_pending = 0;
            s.filter.reset();
            s.reverse_filter.reset();
            s.baseline = false;
            decisions_.erase(std::remove_if(decisions_.begin(), decisions_.end(),
                                            [&](const auto &d) { return d.binding == m.binding.id; }),
                             decisions_.end());
            if (s.held && ready) {
                s.desired = m.binding.target.released;
                decision(m, s.desired);
                s.dirty = false;
                s.held = false;
                s.available = false;
                s.reason = "Lost device: releasing owned held field";
            } else {
                s.available = false;
                s.dirty = false;
                s.reason = "Device disconnected";
            }
        }
}
Indicator fuel_indicator(const Mapper &mapper, unsigned engine) {
    Indicator out;
    if (engine >= 4)
        return out;
    const auto *definition=active_catalogue().target("fuel-run"+std::to_string(engine+1));
    if(!definition)return out;
    const auto contact=mapper.records.find(record_name(definition->record));
    const auto supply = mapper.records.find("Qs323");
    if (contact == mapper.records.end() || supply == mapper.records.end())
        return out;
    int bits{};
    auto f = fields(supply->second.observed);
    int voltage{};
    if (!integer(contact->second.observed, bits) || f.size() < 2 ||
        !integer(f[engine == 0 || engine == 2 ? 0 : 1], voltage))
        return out;
    out.available = true;
    out.contact = (bits & 4) != 0;
    out.failed = (bits & 2) != 0;
    out.supply = voltage;
    out.powered = voltage > 0;
    out.lit = out.contact && !out.failed && out.powered;
    return out;
}
} // namespace hwio
