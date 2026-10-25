#include "hwio/catalogue.hpp"
#include "hwio/storage.hpp"
#include <Windows.h>
#include <objbase.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace hwio {
namespace { constexpr const char* digital_tests[]{"positive","any","not-all","clear","equal","greater","valid","at-most"};
constexpr const char* digital_encodings[]{"numbers","trailing-numbers","characters","digits","mixed-fields"}; }
namespace {
constexpr std::uintmax_t max_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_entries = 16384, max_text = 8192;
template <class T> struct Com {
    T *p{};
    ~Com() {
        if (p)
            p->Release();
    }
    T **out() {
        return &p;
    }
    T *operator->() const {
        return p;
    }
};
void ok(HRESULT h, const char *operation) {
    if (FAILED(h))
        throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(h) + ")");
}
using Attributes = std::map<std::string, std::string>;
std::string take(Attributes &a, const char *key, const char *fallback = nullptr) {
    auto it = a.find(key);
    if (it == a.end()) {
        if (fallback)
            return fallback;
        throw std::runtime_error(std::string("Missing attribute: ") + key);
    }
    auto value = std::move(it->second);
    a.erase(it);
    return value;
}
std::int64_t number(std::string_view s, std::int64_t lo = INT32_MIN, std::int64_t hi = INT32_MAX) {
    std::int64_t n{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || n < lo || n > hi)
        throw std::runtime_error("Invalid integer: " + std::string(s));
    return n;
}
int num(Attributes &a, const char *key, int lo = INT32_MIN, int hi = INT32_MAX, const char *fallback = nullptr) {
    return static_cast<int>(number(take(a, key, fallback), lo, hi));
}
bool flag(Attributes &a, const char *key, const char *fallback = "false") {
    const auto s = take(a, key, fallback);
    if (s != "true" && s != "false")
        throw std::runtime_error("Expected true/false: " + std::string(key));
    return s == "true";
}
std::vector<int> numbers(std::string_view s) {
    std::vector<int> v;
    while (!s.empty()) {
        const auto comma = s.find(',');
        v.push_back(static_cast<int>(number(s.substr(0, comma))));
        if (v.size() > 256)
            throw std::runtime_error("Too many integer values");
        if (comma == s.npos)
            break;
        s.remove_prefix(comma + 1);
        if (s.empty())
            throw std::runtime_error("Trailing integer separator");
    }
    return v;
}
std::string join(const std::vector<int> &v) {
    std::string s;
    for (const int n : v) {
        if (!s.empty())
            s += ',';
        s += std::to_string(n);
    }
    return s;
}
RecordKey record(std::string_view s) {
    RecordKey r;
    if (!parse_key(s, r))
        throw std::runtime_error("Invalid PSX record: " + std::string(s));
    return r;
}
template <class E, std::size_t N> E enumeration(const std::string &s, const char *const (&values)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (s == values[i])
            return static_cast<E>(i);
    throw std::runtime_error("Unsupported enum value: " + s);
}
constexpr const char *layouts[]{"integer", "bits", "fields", "fixed-decimal"};
constexpr const char *semantics[]{"absolute", "held", "pulse", "relative", "read-only"};
constexpr const char *operations[]{"position", "pulse", "held", "step"};
constexpr const char *encodings[]{"numeric", "character"};
constexpr const char *observation_encodings[]{"number", "text", "character", "decimal-slice"};
TargetDescriptor read_target(Attributes &a, bool action) {
    TargetDescriptor d;
    d.id = take(a, action ? "target-id" : "id");
    d.name = take(a, action ? "target-name" : "name");
    d.unit = take(a, "unit", "");
    d.record = record(take(a, "record"));
    d.layout = enumeration<ValueLayout>(take(a, "layout"), layouts);
    d.semantics = enumeration<Semantics>(take(a, "semantics"), semantics);
    d.minimum = num(a, "min");
    d.maximum = num(a, "max");
    d.pressed = num(a, "pressed");
    d.released = num(a, "released");
    d.owned_mask = static_cast<std::uint32_t>(number(take(a, "mask", "0"), 0, UINT32_MAX));
    d.transient_mask = static_cast<std::uint32_t>(number(take(a, "transient", "0"), 0, UINT32_MAX));
    d.shift = static_cast<std::uint8_t>(num(a, "shift", 0, 31, "0"));
    d.field = static_cast<std::uint16_t>(num(a, "field", 0, 255, "0"));
    d.field_count = static_cast<std::uint16_t>(num(a, "fields", 0, 256, "0"));
    d.throttle_engine = static_cast<std::int8_t>(num(a, "engine", -1, 3, "-1"));
    d.writable = flag(a, "writable", "true");
    return d;
}
void identity(const std::string &s) {
    if (s.empty() || s.size() > 256 ||
        s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:") != s.npos)
        throw std::runtime_error("Invalid stable ID: " + s);
}
void target_valid(const TargetDescriptor &d) {
    identity(d.id);
    std::string e;
    if (!descriptor_valid(d, e))
        throw std::runtime_error(d.id + ": " + e);
}
const std::array<std::string_view, flight_slots> role_names{
    "roll",    "pitch",   "rudder",  "tiller",  "roll",  "pitch",      "rudder",      "tiller",     "spoiler",
    "engine1", "engine2", "engine3", "engine4", "flaps", "brake-left", "brake-right", "brake-left", "brake-right"};
struct Writer {
    Com<IStream> stream;
    Com<IXmlWriter> xml;
    explicit Writer(const std::filesystem::path &p) {
        ok(SHCreateStreamOnFileEx(p.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL,
                                  TRUE, nullptr, stream.out()),
           "Create XML output");
        ok(CreateXmlWriter(__uuidof(IXmlWriter), reinterpret_cast<void **>(xml.out()), nullptr), "Create XML writer");
        ok(xml->SetOutput(stream.p), "Set XML output");
        ok(xml->SetProperty(XmlWriterProperty_Indent, TRUE), "Set XML indentation");
        ok(xml->WriteStartDocument(XmlStandalone_Omit), "Write XML declaration");
    }
    void start(const wchar_t *name) {
        ok(xml->WriteStartElement(nullptr, name, nullptr), "Write XML element");
    }
    void end() {
        ok(xml->WriteEndElement(), "Close XML element");
    }
    void attr(const wchar_t *name, const std::string &v) {
        ok(xml->WriteAttributeString(nullptr, name, nullptr, wide(v).c_str()), "Write XML attribute");
    }
    void attr(const wchar_t *name, const char *v) {
        attr(name, std::string(v));
    }
    void attr(const wchar_t *name, std::int64_t v) {
        attr(name, std::to_string(v));
    }
    void boolean(const wchar_t *name, bool v) {
        attr(name, v ? "true" : "false");
    }
    void target(const TargetDescriptor &d, bool action) {
        attr(action ? L"target-id" : L"id", d.id);
        attr(action ? L"target-name" : L"name", d.name);
        attr(L"record", record_name(d.record));
        attr(L"layout", layouts[static_cast<unsigned>(d.layout)]);
        attr(L"semantics", semantics[static_cast<unsigned>(d.semantics)]);
        attr(L"min", d.minimum);
        attr(L"max", d.maximum);
        attr(L"pressed", d.pressed);
        attr(L"released", d.released);
        if (!d.unit.empty())
            attr(L"unit", d.unit);
        if (d.owned_mask)
            attr(L"mask", d.owned_mask);
        if (d.transient_mask)
            attr(L"transient", d.transient_mask);
        if (d.shift)
            attr(L"shift", d.shift);
        if (d.field_count) {
            attr(L"field", d.field);
            attr(L"fields", d.field_count);
        }
        if (d.throttle_engine >= 0)
            attr(L"engine", d.throttle_engine);
        boolean(L"writable", d.writable);
    }
    void finish() {
        ok(xml->WriteEndDocument(), "Finish XML");
        ok(xml->Flush(), "Flush XML");
        ok(stream->Commit(STGC_DEFAULT), "Commit XML");
    }
};
} // namespace

void Catalogue::finalize() {
    if(schema_version<1 || schema_version>7)throw std::runtime_error("Unsupported catalogue schema version");
    if (chapters.empty() || groups.size() > max_entries)
        throw std::runtime_error("Empty or oversized catalogue");
    for (int n : physical_modes)
        if (std::find(servo_modes.begin(), servo_modes.end(), n) != servo_modes.end())
            throw std::runtime_error("Conflicting autothrottle modes");
    if (physical_modes.empty() || servo_modes.empty())
        throw std::runtime_error("Missing autothrottle authority modes");
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
        if (a.chapter != b.chapter)
            return a.chapter < b.chapter;
        if (a.breakers != b.breakers)
            return a.breakers < b.breakers;
        return a.name != b.name ? a.name < b.name : a.id < b.id;
    });
    targets.clear();
    actions.clear();
    target_ids.clear();
    action_ids.clear();
    group_ids.clear();
    group_actions.clear();
    chapter_actions.clear();
    numeric_records.clear();
    roles.fill(nullptr);
    std::size_t target_count = 0, action_count = 0;
    for (const auto &g : groups) {
        target_count += g.targets.size();
        action_count += g.actions.size();
    }
    if (target_count > max_entries || action_count > max_entries)
        throw std::runtime_error("Too many catalogue definitions");
    targets.reserve(target_count);
    actions.reserve(action_count);
    observed_controls.clear();
    observations.clear();
    digital_outputs.clear();
    cockpit.clear();
    displays.clear();
    authority_prefix = record_name(authority_record) + "=";
    observed_controls.insert(record_name(authority_record));
    for (auto &g : groups) {
        validation_line = g.source_line;
        identity(g.id);
        if (!chapters.count(g.chapter) || g.name.empty() || !group_ids.emplace(g.id, group_ids.size()).second)
            throw std::runtime_error("Unknown chapter, missing name or duplicate group: " + g.id);
        if (g.breakers && (g.panel.empty() || g.coordinate.empty()))
            throw std::runtime_error("Breaker requires panel and coordinate: " + g.id);
        if (!g.breakers && (!g.panel.empty() || !g.coordinate.empty()))
            throw std::runtime_error("Panel coordinates only apply to breakers");
        std::sort(g.observations.begin(),g.observations.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        for(const auto& o:g.observations){
            validation_line=o.source_line;identity(o.id);RecordKey validation_power;if(!o.divisor || (schema_version<7 && (o.divisor!=1 || o.demand || !o.power_record.empty() || o.power_mask)) || (!o.power_record.empty() && !parse_key(o.power_record,validation_power)) || (o.power_record.empty()!=!o.power_mask))throw std::runtime_error("Invalid numeric source metadata: "+o.id);
            if(schema_version<2 || o.name.empty() || o.reference.empty() || !o.fields || o.fields>256 || o.field>=o.fields ||
               o.character>=4096 || o.shift>31 || o.minimum>o.maximum || static_cast<unsigned>(o.encoding)>3 ||
               (o.motor_target && o.encoding!=ObservationEncoding::number) ||
               (o.mask && o.encoding==ObservationEncoding::text) || !observations.emplace(o.id,&o).second || observations.size()>max_entries)
                throw std::runtime_error("Invalid/duplicate output observation: "+o.id);
            if(o.encoding==ObservationEncoding::decimal_slice &&
               (schema_version<6 || o.record.kind!=RecordKind::qs || !o.width || o.width>9 ||
                !o.payload_minimum || o.payload_minimum>o.payload_maximum || o.payload_maximum>4096 ||
                o.character+o.width>o.payload_minimum || o.guard_value>127 ||
                (o.guard_value && o.guard_character>=o.payload_minimum) || o.mask))
                throw std::runtime_error("Invalid decimal observation contract: "+o.id);
            observed_controls.insert(record_name(o.record));
        }
        for(auto& output:g.digital_outputs) {
            validation_line=output.source_line;identity(output.id);output.chapter=g.chapter;
            if(schema_version<3 || output.name.empty() || output.reference.empty() ||
               (output.kind!="lamp" && output.kind!="actuator" && output.kind!="shutter" && output.kind!="lighting") ||
               output.branches.size()>16 || (output.qualified && output.branches.empty()) ||
               (!output.qualified && output.unavailable.empty()) || output.applicability.size()>8 || output.requirements.size()>8 ||
               (!output.requirements.empty() && output.requirement_reason.empty()) ||
               !digital_outputs.emplace(output.id,&output).second || digital_outputs.size()>1024)
                throw std::runtime_error("Invalid/duplicate digital output: "+output.id);
            auto checked_branches=output.branches;
            if(!output.requirements.empty())checked_branches.push_back(output.requirements);
            for(const auto& branch:checked_branches) {
                if(branch.empty() || branch.size()>8)throw std::runtime_error("Invalid digital output branch: "+output.id);
                for(const auto& operand:branch) {
                    if(!operand.fields || operand.fields>256 || operand.field>=operand.fields || operand.minimum>operand.maximum ||
                       static_cast<unsigned>(operand.test)>=std::size(digital_tests) || static_cast<unsigned>(operand.encoding)>4 || (operand.encoding==DigitalEncoding::mixed_fields && schema_version<5) ||
                       (operand.length_min && (operand.encoding!=DigitalEncoding::characters || operand.length_min>operand.length_max || operand.length_max>256 || operand.field>=operand.length_min)) ||
                       (operand.length_max && !operand.length_min) ||
                       (operand.prefix.size()>64 || (!operand.prefix.empty() && (operand.encoding==DigitalEncoding::characters || operand.encoding==DigitalEncoding::digits))) ||
                       ((operand.test==DigitalTest::any || operand.test==DigitalTest::not_all || operand.test==DigitalTest::clear) &&
                        (!operand.mask || (operand.minimum<0 && (operand.minimum!=INT32_MIN || operand.maximum!=INT32_MAX)) || operand.mask>static_cast<std::uint32_t>(operand.maximum))) ||
                       (operand.test==DigitalTest::equal && (operand.value<operand.minimum || operand.value>operand.maximum)))
                        throw std::runtime_error("Invalid digital output operand: "+output.id);
                    if(!operand.allowed.empty() && (operand.encoding!=DigitalEncoding::characters || operand.allowed.size()>95 || std::any_of(operand.allowed.begin(),operand.allowed.end(),[](unsigned char ch){return ch<32 || ch>126;})))throw std::runtime_error("Invalid output character alphabet");
                    if(!operand.prefix.empty()) {
                        unsigned parts=1;bool empty=true;
                        for(const unsigned char ch:operand.prefix){if(ch=='/'){if(empty)throw std::runtime_error("Empty output prefix alphabet");++parts;empty=true;}else{if(ch<33 || ch>126 || ch==';')throw std::runtime_error("Invalid output prefix alphabet");empty=false;}}
                        if(empty || parts>8)throw std::runtime_error("Invalid output prefix length");
                    }
                    observed_controls.insert(record_name(operand.record));
                }
            }
            for(const auto& p:output.applicability) {
                if(p.second.empty() || p.second.size()>256)throw std::runtime_error("Invalid output applicability: "+output.id);
                observed_controls.insert(record_name(p.first));
            }
        }
        std::sort(g.targets.begin(), g.targets.end(),
                  [](const auto &a, const auto &b) { return a.descriptor.id < b.descriptor.id; });
        for (auto &t : g.targets) {
            validation_line = t.source_line;
            target_valid(t.descriptor);
            if (!target_ids.emplace(t.descriptor.id, targets.size()).second)
                throw std::runtime_error("Duplicate target: " + t.descriptor.id);
            targets.push_back(t.descriptor);
            if (t.descriptor.throttle_engine >= 0 || t.descriptor.id.starts_with("fuel-run"))
                observed_controls.insert(record_name(t.descriptor.record));
            if (!t.role.empty()) {
                bool found = false;
                for (std::size_t i = 0; i < roles.size(); ++i)
                    if (role_names[i] == t.role) {
                        if (roles[i])
                            throw std::runtime_error("Duplicate flight role: " + t.role);
                        roles[i] = &t;
                        found = true;
                    }
                if (!found)
                    throw std::runtime_error("Unknown flight role: " + t.role);
                if (t.descriptor.minimum >= t.descriptor.maximum)
                    throw std::runtime_error("Flight role has no travel");
                const bool centered = t.role == "roll" || t.role == "pitch" || t.role == "rudder" ||
                                      t.role == "tiller" || t.role.starts_with("engine");
                if (centered && !(t.descriptor.minimum < 0 && t.descriptor.maximum > 0))
                    throw std::runtime_error("Centered role requires negative/positive travel");
                if (t.role.starts_with("engine") && t.descriptor.throttle_engine != t.role.back() - '1')
                    throw std::runtime_error("Engine role/index mismatch");
            }
            if (!t.detents.empty()) {
                if (t.role != "flaps" || t.detents.size() != 7)
                    throw std::runtime_error("Flaps require seven detent values");
                std::set<int> unique;
                for (int d : t.detents)
                    if (d < t.descriptor.minimum || d > t.descriptor.maximum || !unique.insert(d).second)
                        throw std::runtime_error("Invalid flap detent values");
            } else if (t.role == "flaps")
                throw std::runtime_error("Missing flap detent values");
            if (t.numeric_fields) {
                if (t.descriptor.layout != ValueLayout::semicolon_field)
                    throw std::runtime_error("numeric-fields requires a structured record");
                auto &fields = numeric_records[record_name(t.descriptor.record)];
                if (fields.empty())
                    fields.resize(t.descriptor.field_count);
                if (fields.size() != t.descriptor.field_count || fields[t.descriptor.field])
                    throw std::runtime_error("Inconsistent numeric record fields");
                fields[t.descriptor.field] = &t.descriptor;
            }
        }
        std::sort(g.actions.begin(), g.actions.end(),
                  [](const auto &a, const auto &b) { return a.order != b.order ? a.order < b.order : a.id < b.id; });
        std::set<int> orders;
        unsigned primaries = 0, toggles = 0;
        for (auto &a : g.actions) {
            validation_line = a.source_line;
            if (static_cast<unsigned>(a.operation) > 3 || static_cast<unsigned>(a.encoding) > 1)
                throw std::runtime_error("Unsupported operation or encoding: " + a.id);
            identity(a.id);
            identity(a.target.id);
            a.group = g.id;
            a.chapter = g.chapter;
            a.name = g.name;
            a.circuit_breaker = g.breakers;
            if (!a.target.writable || a.target.semantics == Semantics::read_only)
                throw std::runtime_error("Action cannot use a read-only target: " + a.id);
            if (!orders.insert(a.order).second || !action_ids.emplace(a.id, actions.size()).second)
                throw std::runtime_error("Duplicate action ID/position order: " + a.id);
            if (a.encoding == ButtonEncoding::numeric) {
                target_valid(a.target);
                if (!a.wrap && (a.value < a.target.minimum || a.value > a.target.maximum))
                    throw std::runtime_error("Action value outside range: " + a.id);
                if (a.operation != ButtonOperation::position &&
                    (a.release < a.target.minimum || a.release > a.target.maximum))
                    throw std::runtime_error("Release outside range: " + a.id);
            } else if (a.target.record.kind != RecordKind::qs || !a.length || a.length > 4095 ||
                       a.character >= a.length || a.value < 0 || a.value > 1 || a.release < 0 || a.release > 1)
                throw std::runtime_error("Invalid character target: " + a.id);
            if (a.circuit_breaker &&
                (a.encoding != ButtonEncoding::character || a.operation != ButtonOperation::position))
                throw std::runtime_error("Invalid breaker operation");
            for (const auto &condition : a.applicability) {
                if (condition.second.empty() || condition.second.size() > 256)
                    throw std::runtime_error("Empty/oversized applicability: " + a.id);
                std::set<int> values(condition.second.begin(), condition.second.end());
                if (values.size() != condition.second.size())
                    throw std::runtime_error("Duplicate applicability value");
            }
            const auto &first = g.actions.front();
            if (a.target.record != first.target.record || a.encoding != first.encoding ||
                a.target.layout != first.target.layout || a.target.field != first.target.field ||
                a.target.owned_mask != first.target.owned_mask || a.character != first.character ||
                a.length != first.length || a.wrap!=first.wrap || a.unset!=first.unset ||
                a.payload_minimum!=first.payload_minimum || a.payload_maximum!=first.payload_maximum ||
                a.guard_character!=first.guard_character || a.guard_value!=first.guard_value)
                throw std::runtime_error("Switch positions have inconsistent destinations: " + g.id);
            if (a.encoding == ButtonEncoding::numeric &&
                ((a.operation == ButtonOperation::pulse && a.target.semantics != Semantics::pulse) ||
                 (a.operation == ButtonOperation::step && a.target.semantics != Semantics::relative &&
                  !(a.target.layout==ValueLayout::fixed_decimal && a.wrap && a.target.semantics==Semantics::absolute))))
                throw std::runtime_error("Operation and target semantics disagree: " + a.id);
            if(a.target.layout==ValueLayout::fixed_decimal &&
               (schema_version<6 || a.encoding!=ButtonEncoding::numeric || a.operation!=ButtonOperation::step ||
                a.wrap<2 || a.target.minimum!=0 || a.target.maximum!=a.wrap-1 ||
                a.value==0 || a.value<=-a.wrap || a.value>=a.wrap ||
                a.unset<a.wrap || std::to_string(a.unset).size()>a.target.field_count ||
                !a.payload_minimum || a.payload_minimum>a.payload_maximum || a.payload_maximum>4096 ||
                static_cast<unsigned>(a.target.field)+a.target.field_count>a.payload_minimum || a.guard_value>127 ||
                (a.guard_value && a.guard_character>=a.payload_minimum)))
                throw std::runtime_error("Invalid bounded rotary decimal contract: "+a.id);
            if(a.wrap && a.target.layout!=ValueLayout::fixed_decimal)throw std::runtime_error("Wrap requires a decimal slice: "+a.id);
            primaries += a.primary;
            toggles += a.toggleable;
            group_actions[g.id].push_back(actions.size());
            chapter_actions[g.chapter].push_back(actions.size());
            actions.push_back(a);
        }
        if (toggles &&
            (toggles != 2 || g.actions.size() != 2 || primaries != 1 || g.actions[0].value == g.actions[1].value ||
             g.actions[0].operation != ButtonOperation::position ||
             g.actions[1].operation != ButtonOperation::position))
            throw std::runtime_error("Toggle requires one primary and two position actions: " + g.id);
    }
    for(auto& group:groups)for(auto& input:group.cockpit){
        validation_line=input.source_line;identity(input.id);input.group=group.id;input.chapter=group.chapter;
        if(schema_version<4 || input.name.empty() || input.reference.empty() || !cockpit.emplace(input.id,&input).second || cockpit.size()>512)
            throw std::runtime_error("Invalid/duplicate cockpit input: "+input.id);
        if(input.encoder){
            const auto* down=action(input.decrease);const auto* up=action(input.increase);
            if(!down || !up || down->operation!=ButtonOperation::step || up->operation!=ButtonOperation::step || down->value!=-1 || up->value!=1 ||
               down->encoding!=ButtonEncoding::numeric || up->encoding!=ButtonEncoding::numeric || !overlaps(down->target,up->target) ||
               ((!down->wrap && down->target.minimum>=0) || up->target.maximum<=0) || down->wrap!=up->wrap || down->unset!=up->unset)
                throw std::runtime_error("Encoder requires matching native -1/+1 turn actions: "+input.id);
            input.target=up->target;input.target.id=input.id;input.target.name=input.name;input.applicability=up->applicability;
        }else{
            target_valid(input.target);
            if(input.baseline_minimum>input.target.minimum || input.baseline_maximum<input.target.maximum)throw std::runtime_error("Cockpit baseline bounds do not contain command range");
            if(!input.target.writable || input.target.semantics!=Semantics::absolute || input.target.throttle_engine>=0 || input.detents.size()>128)
                throw std::runtime_error("Analogue input requires a writable absolute cockpit target: "+input.id);
            std::set<int> distinct;for(const auto value:input.detents)if(value<input.target.minimum || value>input.target.maximum || !distinct.insert(value).second)
                throw std::runtime_error("Invalid analogue detent values: "+input.id);
            if(input.detents.size()==1)throw std::runtime_error("A selector needs at least two detents");
        }
        if(input.applicability.size()>8)throw std::runtime_error("Too many cockpit predicates");
        for(const auto& predicate:input.applicability){if(predicate.second.empty() || predicate.second.size()>256)throw std::runtime_error("Invalid cockpit predicate");observed_controls.insert(record_name(predicate.first));}
        observed_controls.insert(record_name(input.target.record));
    }
    // Absolute packed cockpit records need every field's type/range, including
    // fields retained from digital-only controls. No unowned malformed field is
    // copied back to PSX. Relative commands compose fresh zero-filled records.
    for(const auto& [id,input]:cockpit)if(!input->encoder && input->target.layout==ValueLayout::semicolon_field){
        (void)id;auto& fields=numeric_records[record_name(input->target.record)];if(fields.empty())fields.resize(input->target.field_count);
        if(fields.size()!=input->target.field_count)throw std::runtime_error("Cockpit packed field count conflict");
        auto& field=fields[input->target.field];if(field && (field->minimum!=input->target.minimum || field->maximum!=input->target.maximum))throw std::runtime_error("Cockpit packed field bounds conflict");field=&input->target;
    }
    for(auto& [record,fields]:numeric_records)for(const auto& action:actions)if(record_name(action.target.record)==record && action.target.layout==ValueLayout::semicolon_field && fields.size()==action.target.field_count && !fields[action.target.field])fields[action.target.field]=&action.target;
    for (const auto &[key, fields] : numeric_records)
        if (std::any_of(fields.begin(), fields.end(), [](auto p) { return !p; }))
            throw std::runtime_error("Incomplete numeric record: " + key);
    for(auto& group:groups)for(auto& display:group.displays) {
        identity(display.id);
        const bool radio=display.style=="rcp-active" || display.style=="rcp-standby";
        const bool transponder=display.style=="transponder-lcd";
        const bool numeric=display.style=="integer" || display.style=="nav-course" || display.style=="mcp-speed" || display.style=="mcp-altitude" || display.style=="mcp-vs";
        const bool observation=observations.count(display.observation)!=0;
        const bool contract=transponder ? display.width==4 && observation && observations.at(display.observation)->encoding==ObservationEncoding::text &&
                observations.at(display.observation)->fields==1 && observations.at(display.observation)->field==0 && display.power.empty() && display.mode.empty() :
                radio ? display.width==6 && observation && observations.at(display.observation)->encoding==ObservationEncoding::text &&
                observations.at(display.observation)->fields==2 && observations.at(display.observation)->field==0 && display.power.empty() && display.mode.empty() :
                numeric && observation && (observations.at(display.observation)->encoding==ObservationEncoding::number || observations.at(display.observation)->encoding==ObservationEncoding::decimal_slice) &&
                observations.count(display.power) && observations.at(display.power)->encoding==ObservationEncoding::number &&
                (display.mode.empty() || (observations.count(display.mode) && observations.at(display.mode)->encoding==ObservationEncoding::number));
        if(schema_version<5 || display.name.empty() || display.reference.empty() || display.width<1 || display.width>12 ||
           !contract || (display.style=="nav-course" && (schema_version<6 || display.width!=3 || observations.at(display.observation)->encoding!=ObservationEncoding::decimal_slice || observations.at(display.observation)->width!=3)) ||
           !displays.emplace(display.id,&display).second || displays.size()>256)
            throw std::runtime_error("Invalid/duplicate integral display: "+display.id);
    }
    for(const auto& [id,output]:digital_outputs) { (void)output;if(observations.count(id))throw std::runtime_error("Output ID collides with observation: "+id); }
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto *t = roles[i];
        const auto name = t ? (t->label.empty() ? t->descriptor.name : t->label) : std::string(role_names[i]);
        labels[i] = (i < 4 || (i >= 14 && i < 16) ? L"CPT " : (i >= 4 && i < 8) || i >= 16 ? L"FO " : L"") + wide(name);
    }
}
std::unique_ptr<Catalogue> read_catalogue(const std::filesystem::path &file) {
    const auto started = std::chrono::steady_clock::now();
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > max_bytes)
        throw CatalogueError(file.string() + ": unreadable or oversized catalogue",
                             ec == std::errc::no_such_file_or_directory);
    Com<IStream> stream;
    Com<IXmlReader> xml;
    UINT line = 1, column = 1;
    try {
        ok(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
                                  nullptr, stream.out()),
           "Open catalogue");
        ok(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void **>(xml.out()), nullptr), "Create XML reader");
        ok(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit), "Prohibit DTD");
        ok(xml->SetProperty(XmlReaderProperty_MaxElementDepth, 8), "Bound XML depth");
        ok(xml->SetInput(stream.p), "Set XML input");
        auto c = std::make_unique<Catalogue>();
        std::vector<std::string> stack;
        int chapter = 0;
        bool root = false;
        std::size_t nodes = 0, targets_read = 0, actions_read = 0;
        XmlNodeType type;
        HRESULT hr;
        while ((hr = xml->Read(&type)) == S_OK) {
            xml->GetLineNumber(&line);
            xml->GetLinePosition(&column);
            if (++nodes > 250000)
                throw std::runtime_error("Too many XML nodes");
            if (type == XmlNodeType_Element) {
                const wchar_t *text{};
                UINT length{};
                ok(xml->GetLocalName(&text, &length), "Read element name");
                const auto name = utf8({text, length});
                ok(xml->GetNamespaceUri(&text, &length), "Read element namespace");
                if (length)
                    throw std::runtime_error("Namespaces are unsupported");
                Attributes a;
                if (xml->MoveToFirstAttribute() == S_OK)
                    do {
                        ok(xml->GetNamespaceUri(&text, &length), "Read attribute namespace");
                        if (length)
                            throw std::runtime_error("Namespaced attributes unsupported");
                        ok(xml->GetQualifiedName(&text, &length), "Read attribute name");
                        const auto key = utf8({text, length});
                        ok(xml->GetValue(&text, &length), "Read attribute value");
                        if (length > max_text)
                            throw std::runtime_error("Attribute too long");
                        if (!a.emplace(key, utf8({text, length})).second)
                            throw std::runtime_error("Duplicate XML attribute");
                    } while (xml->MoveToNextAttribute() == S_OK);
                xml->MoveToElement();
                const auto parent = stack.empty() ? std::string{} : stack.back();
                if (name == "catalogue" && parent.empty() && !root) {
                    const auto version=take(a,"version");
                    if (version != "1" && version != "2" && version != "3" && version != "4" && version != "5" && version != "6" && version != "7")
                        throw std::runtime_error("Unsupported catalogue schema version");
                    c->schema_version=static_cast<unsigned>(number(version,1,7));
                    root = true;
                    c->revision = take(a, "revision");
                    c->authority_record = record(take(a, "authority-record"));
                    c->authority_field = static_cast<unsigned>(num(a, "authority-field", 0, 255));
                    c->physical_modes = numbers(take(a, "physical-modes"));
                    c->servo_modes = numbers(take(a, "servo-modes"));
                } else if (name == "chapter" && parent == "catalogue") {
                    chapter = num(a, "number", 1, 99);
                    const auto title = take(a, "name");
                    if (title.empty() || !c->chapters.emplace(chapter, title).second)
                        throw std::runtime_error("Missing/duplicate chapter name/number");
                } else if (name == "group" && parent == "chapter") {
                    CatalogueGroup g;
                    g.source_line = line;
                    g.panel = take(a, "panel", "");
                    g.coordinate = take(a, "coordinate", "");
                    g.id = take(a, "id");
                    g.name = take(a, "name");
                    g.chapter = chapter;
                    const auto section = take(a, "section");
                    if (section != "controls" && section != "breakers")
                        throw std::runtime_error("Unknown group section");
                    g.breakers = section == "breakers";
                    c->groups.push_back(std::move(g));
                    if (c->groups.size() > max_entries)
                        throw std::runtime_error("Too many groups");
                } else if (name == "target" && parent == "group") {
                    if (++targets_read > max_entries)
                        throw std::runtime_error("Too many targets");
                    CatalogueTarget t;
                    t.source_line = line;
                    t.descriptor = read_target(a, false);
                    t.role = take(a, "role", "");
                    t.reference = take(a, "reference", "");
                    t.label = take(a, "label", "");
                    t.reverse_label = take(a, "reverse-label", "");
                    t.numeric_fields = flag(a, "numeric-fields");
                    t.detents = numbers(take(a, "detents", ""));
                    c->groups.back().targets.push_back(std::move(t));
                } else if((name=="encoder" || name=="analogue-input") && parent=="group" && c->schema_version>=4){
                    CockpitDefinition input;input.source_line=line;input.encoder=name=="encoder";
                    input.reference=take(a,"reference");
                    if(input.encoder){input.id=take(a,"id");input.name=take(a,"name");input.decrease=take(a,"decrease");input.increase=take(a,"increase");}
                    else{input.target=read_target(a,false);input.id=input.target.id;input.name=input.target.name;input.detents=numbers(take(a,"detents",""));input.baseline_minimum=num(a,"baseline-min",INT32_MIN,INT32_MAX,std::to_string(input.target.minimum).c_str());input.baseline_maximum=num(a,"baseline-max",INT32_MIN,INT32_MAX,std::to_string(input.target.maximum).c_str());}
                    c->groups.back().cockpit.push_back(std::move(input));
                    if(++targets_read>max_entries)throw std::runtime_error("Too many cockpit definitions");
                } else if(name=="when" && parent=="analogue-input"){
                    auto& predicates=c->groups.back().cockpit.back().applicability;predicates.push_back({record(take(a,"record")),numbers(take(a,"values"))});
                    if(predicates.size()>8)throw std::runtime_error("Too many cockpit predicates");
                } else if(name=="integral-display" && parent=="group" && c->schema_version>=5) {
                    CatalogueDisplay display;display.source_line=line;display.id=take(a,"id");display.name=take(a,"name");display.reference=take(a,"reference");
                    display.observation=take(a,"observation","");display.power=take(a,"power","");display.mode=take(a,"mode","");display.style=take(a,"style");display.width=static_cast<unsigned>(num(a,"width",1,12));
                    c->groups.back().displays.push_back(std::move(display));
                    if(++targets_read>max_entries)throw std::runtime_error("Too many integral displays");
                } else if (name == "observation" && parent == "group" && c->schema_version>=2) {
                    if(++targets_read>max_entries)throw std::runtime_error("Too many observation definitions");
                    CatalogueObservation o;o.source_line=line;o.id=take(a,"id");o.name=take(a,"name");
                    o.unit=take(a,"unit","");o.reference=take(a,"reference");o.record=record(take(a,"record"));
                    o.encoding=enumeration<ObservationEncoding>(take(a,"encoding"),observation_encodings);
                    o.field=static_cast<unsigned>(num(a,"field",0,255,"0"));o.fields=static_cast<unsigned>(num(a,"fields",1,256,"1"));
                    o.character=static_cast<unsigned>(num(a,"character",0,4095,"0"));
                    o.width=static_cast<unsigned>(num(a,"width",0,9,"0"));
                    o.payload_minimum=static_cast<unsigned>(num(a,"payload-min",0,4096,"0"));o.payload_maximum=static_cast<unsigned>(num(a,"payload-max",0,4096,"0"));
                    o.guard_character=static_cast<unsigned>(num(a,"guard-character",0,4095,"0"));o.guard_value=static_cast<unsigned>(num(a,"guard-value",0,127,"0"));
                    o.minimum=num(a,"min",INT32_MIN,INT32_MAX,"-2147483648");o.maximum=num(a,"max",INT32_MIN,INT32_MAX,"2147483647");
                    o.mask=static_cast<std::uint32_t>(number(take(a,"mask","0"),0,UINT32_MAX));o.shift=static_cast<unsigned>(num(a,"shift",0,31,"0"));
                    o.motor_target=flag(a,"motor-target");o.divisor=static_cast<unsigned>(num(a,"divisor",1,1000000000,"1"));o.demand=flag(a,"demand");o.power_record=take(a,"power-record","");o.power_mask=static_cast<std::uint32_t>(number(take(a,"power-mask","0"),0,UINT32_MAX));c->groups.back().observations.push_back(std::move(o));
                } else if(name=="digital-output" && parent=="group" && c->schema_version>=3) {
                    CatalogueDigitalOutput output;output.source_line=line;output.id=take(a,"id");output.name=take(a,"name");
                    output.kind=take(a,"kind");output.reference=take(a,"reference");output.qualified=flag(a,"qualified");
                    output.unavailable=take(a,"unavailable","");output.requirement_reason=take(a,"restricted","");c->groups.back().digital_outputs.push_back(std::move(output));
                    if(++targets_read>max_entries)throw std::runtime_error("Too many output definitions");
                } else if(name=="branch" && parent=="digital-output") {
                    auto& output=c->groups.back().digital_outputs.back();output.branches.emplace_back();
                    if(output.branches.size()>16)throw std::runtime_error("Too many output branches");
                } else if((name=="operand" && parent=="branch") || (name=="require" && parent=="digital-output")) {
                    DigitalOperand operand;operand.record=record(take(a,"record"));
                    operand.field=static_cast<unsigned>(num(a,"field",0,255,"0"));operand.fields=static_cast<unsigned>(num(a,"fields",1,256,"1"));
                    operand.minimum=num(a,"min",INT32_MIN,INT32_MAX,"0");operand.maximum=num(a,"max",INT32_MIN,INT32_MAX,"2147483647");
                    operand.test=enumeration<DigitalTest>(take(a,"test"),digital_tests);
                    operand.mask=static_cast<std::uint32_t>(number(take(a,"mask","0"),0,UINT32_MAX));operand.value=num(a,"value",INT32_MIN,INT32_MAX,"0");
                    operand.encoding=enumeration<DigitalEncoding>(take(a,"encoding","numbers"),digital_encodings);
                    operand.length_min=static_cast<unsigned>(num(a,"length-min",0,256,"0"));operand.length_max=static_cast<unsigned>(num(a,"length-max",0,256,"0"));operand.prefix=take(a,"prefix","");operand.allowed=take(a,"allowed","");
                    auto& output=c->groups.back().digital_outputs.back();
                    auto& branch=name=="require"?output.requirements:output.branches.back();branch.push_back(operand);
                    if(branch.size()>8)throw std::runtime_error("Too many output operands");
                } else if(name=="when" && parent=="digital-output") {
                    auto& predicates=c->groups.back().digital_outputs.back().applicability;
                    predicates.push_back({record(take(a,"record")),numbers(take(a,"values"))});
                    if(predicates.size()>8)throw std::runtime_error("Too many output predicates");
                } else if (name == "action" && parent == "group") {
                    if (++actions_read > max_entries)
                        throw std::runtime_error("Too many actions");
                    ButtonAction b;
                    b.source_line = line;
                    b.id = take(a, "id");
                    b.position = take(a, "position");
                    b.order = num(a, "order", 0, 16383);
                    b.operation = enumeration<ButtonOperation>(take(a, "operation"), operations);
                    b.encoding = enumeration<ButtonEncoding>(take(a, "encoding"), encodings);
                    b.value = num(a, "value");
                    b.release = num(a, "release");
                    b.primary = flag(a, "primary");
                    b.toggleable = flag(a, "toggle");
                    b.character = static_cast<std::size_t>(num(a, "character", 0, 4094, "0"));
                    b.length = static_cast<std::size_t>(num(a, "length", 0, 4095, "0"));
                    b.wrap=num(a,"wrap",0,1000000000,"0");b.unset=num(a,"unset",-1,1000000000,"-1");
                    b.payload_minimum=static_cast<unsigned>(num(a,"payload-min",0,4096,"0"));b.payload_maximum=static_cast<unsigned>(num(a,"payload-max",0,4096,"0"));
                    b.guard_character=static_cast<unsigned>(num(a,"guard-character",0,4095,"0"));b.guard_value=static_cast<unsigned>(num(a,"guard-value",0,127,"0"));
                    b.reference = take(a, "reference", "");
                    b.target = read_target(a, true);
                    c->groups.back().actions.push_back(std::move(b));
                } else if (name == "when" && parent == "action") {
                    c->groups.back().actions.back().applicability.push_back(
                        {record(take(a, "record")), numbers(take(a, "values"))});
                } else
                    throw std::runtime_error("Unexpected element: " + name + " in " + parent);
                if (!a.empty())
                    throw std::runtime_error("Unknown attribute: " + a.begin()->first);
                if (!xml->IsEmptyElement())
                    stack.push_back(name);
            } else if (type == XmlNodeType_EndElement) {
                if (stack.empty())
                    throw std::runtime_error("Unexpected XML end");
                stack.pop_back();
            } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
                const wchar_t *v{};
                UINT n{};
                ok(xml->GetValue(&v, &n), "Read XML text");
                for (UINT i = 0; i < n; ++i)
                    if (!iswspace(v[i]))
                        throw std::runtime_error("Unexpected element text");
            } else if (type != XmlNodeType_Whitespace && type != XmlNodeType_Comment &&
                       type != XmlNodeType_XmlDeclaration)
                throw std::runtime_error("Unsupported XML node");
        }
        ok(hr, "Parse XML");
        if (!root || !stack.empty())
            throw std::runtime_error("Incomplete catalogue");
        try {
            c->finalize();
        } catch (const std::exception &e) {
            throw CatalogueError(file.string() + ":" + std::to_string(c->validation_line) + ": " + e.what());
        }
        c->load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return c;
    } catch (const CatalogueError &) {
        throw;
    } catch (const std::exception &e) {
        if (xml.p) {
            xml->GetLineNumber(&line);
            xml->GetLinePosition(&column);
        }
        throw CatalogueError(file.string() + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " +
                             e.what());
    }
}
void write_catalogue(const Catalogue &c, const std::filesystem::path &file) {
    // Validate a copy before creating or replacing any output file.
    Catalogue checked = c;
    checked.finalize();
    const auto temporary = std::filesystem::path(file.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()));
    try {
        {
            Writer w(temporary);
            w.start(L"catalogue");
            w.attr(L"version", checked.schema_version);
            w.attr(L"revision", checked.revision);
            w.attr(L"authority-record", record_name(checked.authority_record));
            w.attr(L"authority-field", checked.authority_field);
            w.attr(L"physical-modes", join(checked.physical_modes));
            w.attr(L"servo-modes", join(checked.servo_modes));
            for (const auto &[chapter, title] : checked.chapters) {
                w.start(L"chapter");
                w.attr(L"number", chapter);
                w.attr(L"name", title);
                for (const auto &g : checked.groups)
                    if (g.chapter == chapter) {
                        w.start(L"group");
                        w.attr(L"id", g.id);
                        w.attr(L"name", g.name);
                        w.attr(L"section", g.breakers ? "breakers" : "controls");
                        if (g.breakers) {
                            w.attr(L"panel", g.panel);
                            w.attr(L"coordinate", g.coordinate);
                        }
                        for (const auto &t : g.targets) {
                            w.start(L"target");
                            w.target(t.descriptor, false);
                            if (!t.role.empty())
                                w.attr(L"role", t.role);
                            if (t.numeric_fields)
                                w.boolean(L"numeric-fields", true);
                            if (!t.detents.empty())
                                w.attr(L"detents", join(t.detents));
                            if (!t.reference.empty())
                                w.attr(L"reference", t.reference);
                            if (!t.label.empty())
                                w.attr(L"label", t.label);
                            if (!t.reverse_label.empty())
                                w.attr(L"reverse-label", t.reverse_label);
                            w.end();
                        }
                        for(const auto& input:g.cockpit){
                            w.start(input.encoder?L"encoder":L"analogue-input");w.attr(L"reference",input.reference);
                            if(input.encoder){w.attr(L"id",input.id);w.attr(L"name",input.name);w.attr(L"decrease",input.decrease);w.attr(L"increase",input.increase);}
                            else{w.target(input.target,false);w.attr(L"baseline-min",input.baseline_minimum);w.attr(L"baseline-max",input.baseline_maximum);if(!input.detents.empty())w.attr(L"detents",join(input.detents));for(const auto& predicate:input.applicability){w.start(L"when");w.attr(L"record",record_name(predicate.first));w.attr(L"values",join(predicate.second));w.end();}}
                            w.end();
                        }
                        for(const auto& o:g.observations){
                            w.start(L"observation");w.attr(L"id",o.id);w.attr(L"name",o.name);w.attr(L"unit",o.unit);
                            w.attr(L"reference",o.reference);w.attr(L"record",record_name(o.record));
                            w.attr(L"encoding",observation_encodings[static_cast<unsigned>(o.encoding)]);
                            w.attr(L"field",o.field);w.attr(L"fields",o.fields);w.attr(L"character",o.character);
                            if(o.encoding==ObservationEncoding::decimal_slice){w.attr(L"width",o.width);w.attr(L"payload-min",o.payload_minimum);w.attr(L"payload-max",o.payload_maximum);w.attr(L"guard-character",o.guard_character);w.attr(L"guard-value",o.guard_value);}
                            w.attr(L"min",o.minimum);w.attr(L"max",o.maximum);w.attr(L"mask",o.mask);w.attr(L"shift",o.shift);
                            w.boolean(L"motor-target",o.motor_target);if(checked.schema_version>=7){w.attr(L"divisor",o.divisor);w.boolean(L"demand",o.demand);w.attr(L"power-record",o.power_record);w.attr(L"power-mask",o.power_mask);}w.end();
                        }
                        for(const auto& display:g.displays) {
                            w.start(L"integral-display");w.attr(L"id",display.id);w.attr(L"name",display.name);w.attr(L"reference",display.reference);
                            w.attr(L"observation",display.observation);w.attr(L"power",display.power);if(!display.mode.empty())w.attr(L"mode",display.mode);
                            w.attr(L"style",display.style);w.attr(L"width",display.width);w.end();
                        }
                        for(const auto& output:g.digital_outputs) {
                            w.start(L"digital-output");w.attr(L"id",output.id);w.attr(L"name",output.name);w.attr(L"kind",output.kind);
                            w.attr(L"reference",output.reference);w.boolean(L"qualified",output.qualified);w.attr(L"unavailable",output.unavailable);if(!output.requirement_reason.empty())w.attr(L"restricted",output.requirement_reason);
                            auto write_operand=[&](const DigitalOperand& operand,const wchar_t* tag) {
                                w.start(tag);w.attr(L"record",record_name(operand.record));w.attr(L"field",operand.field);w.attr(L"fields",operand.fields);
                                w.attr(L"min",operand.minimum);w.attr(L"max",operand.maximum);w.attr(L"test",digital_tests[static_cast<unsigned>(operand.test)]);
                                w.attr(L"mask",operand.mask);w.attr(L"value",operand.value);
                                if(operand.encoding!=DigitalEncoding::numbers)w.attr(L"encoding",digital_encodings[static_cast<unsigned>(operand.encoding)]);
                                if(operand.length_min){w.attr(L"length-min",operand.length_min);w.attr(L"length-max",operand.length_max);}
                                if(!operand.prefix.empty())w.attr(L"prefix",operand.prefix);
                                if(!operand.allowed.empty())w.attr(L"allowed",operand.allowed);w.end();
                            };
                            for(const auto& branch:output.branches) {w.start(L"branch");for(const auto& operand:branch)write_operand(operand,L"operand");w.end();}
                            for(const auto& operand:output.requirements)write_operand(operand,L"require");
                            for(const auto& predicate:output.applicability) {w.start(L"when");w.attr(L"record",record_name(predicate.first));w.attr(L"values",join(predicate.second));w.end();}
                            w.end();
                        }
                        for (const auto &a : g.actions) {
                            w.start(L"action");
                            w.attr(L"id", a.id);
                            w.attr(L"position", a.position);
                            w.attr(L"order", a.order);
                            w.attr(L"operation", operations[static_cast<unsigned>(a.operation)]);
                            w.attr(L"encoding", encodings[static_cast<unsigned>(a.encoding)]);
                            w.attr(L"value", a.value);
                            w.attr(L"release", a.release);
                            if(a.wrap){w.attr(L"wrap",a.wrap);w.attr(L"unset",a.unset);w.attr(L"payload-min",a.payload_minimum);w.attr(L"payload-max",a.payload_maximum);w.attr(L"guard-character",a.guard_character);w.attr(L"guard-value",a.guard_value);}
                            w.boolean(L"primary", a.primary);
                            w.boolean(L"toggle", a.toggleable);
                            if (a.length) {
                                w.attr(L"character", static_cast<std::int64_t>(a.character));
                                w.attr(L"length", static_cast<std::int64_t>(a.length));
                            }
                            w.attr(L"reference", a.reference);
                            w.target(a.target, true);
                            for (const auto &p : a.applicability) {
                                w.start(L"when");
                                w.attr(L"record", record_name(p.first));
                                w.attr(L"values", join(p.second));
                                w.end();
                            }
                            w.end();
                        }
                        w.end();
                    }
                w.end();
            }
            w.end();
            w.finish();
        }
        const auto reread = read_catalogue(temporary);
        if (catalogue_fingerprint(checked) != catalogue_fingerprint(*reread))
            throw std::runtime_error("XML output verification failed");
        for (std::size_t i = 0; i < checked.groups.size(); ++i) {
            const auto &a = checked.groups[i], &b = reread->groups[i];
            if (a.panel != b.panel || a.coordinate != b.coordinate)
                throw std::runtime_error("XML coordinate verification failed");
            for (std::size_t j = 0; j < a.targets.size(); ++j)
                if (a.targets[j].label != b.targets[j].label ||
                    a.targets[j].reverse_label != b.targets[j].reverse_label)
                    throw std::runtime_error("XML label verification failed");
        }
        const bool exists = std::filesystem::exists(file);
        if (!(exists ? ReplaceFileW(file.c_str(), temporary.c_str(), (file.wstring() + L".bak").c_str(),
                                    REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)
                     : MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH)))
            throw std::runtime_error("Cannot atomically replace XML: " + std::to_string(GetLastError()));
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}
const ButtonAction *Catalogue::action(std::string_view id) const noexcept {
    const auto it = action_ids.find(id);
    return it == action_ids.end() ? nullptr : &actions[it->second];
}
const TargetDescriptor *Catalogue::target(std::string_view id) const noexcept {
    const auto it = target_ids.find(id);
    return it == target_ids.end() ? nullptr : &targets[it->second];
}
AtAuthority Catalogue::authority(std::string_view payload) const noexcept {
    for (unsigned i = 0; i < authority_field; ++i) {
        const auto n = payload.find(';');
        if (n == payload.npos)
            return AtAuthority::unavailable;
        payload.remove_prefix(n + 1);
    }
    int value{};
    if (!integer(payload.substr(0, payload.find(';')), value))
        return AtAuthority::unavailable;
    if (std::find(physical_modes.begin(), physical_modes.end(), value) != physical_modes.end())
        return AtAuthority::physical;
    return std::find(servo_modes.begin(), servo_modes.end(), value) != servo_modes.end() ? AtAuthority::servo
                                                                                         : AtAuthority::unavailable;
}
std::size_t merge_digital_catalogue(Catalogue& current,const Catalogue& defaults) {
    std::set<std::string> existing;
    for(const auto& group:current.groups){for(const auto& output:group.digital_outputs)existing.insert(output.id);for(const auto& observation:group.observations)existing.insert(observation.id);for(const auto& display:group.displays)existing.insert(display.id);for(const auto& action:group.actions)existing.insert(action.id);for(const auto& input:group.cockpit)existing.insert(input.id);}
    std::size_t added=0;
    for(const auto& group:defaults.groups)if(!group.digital_outputs.empty() || !group.displays.empty() || !group.observations.empty()) {
        auto found=std::find_if(current.groups.begin(),current.groups.end(),[&](const auto& g){return g.id==group.id;});
        if(found==current.groups.end()){CatalogueGroup new_group;new_group.id=group.id;new_group.name=group.name;new_group.chapter=group.chapter;current.groups.push_back(std::move(new_group));found=std::prev(current.groups.end());}
        current.chapters.try_emplace(group.chapter,defaults.chapters.at(group.chapter));
        // Integral compatibility windows may carry matching selector actions.
        // Add absent IDs only; operator-edited definitions remain authoritative.
        for(const auto& action:group.actions)if(existing.insert(action.id).second){found->actions.push_back(action);++added;}
        for(const auto& input:group.cockpit)if(existing.insert(input.id).second){found->cockpit.push_back(input);++added;}
        for(const auto& observation:group.observations)if(existing.insert(observation.id).second){found->observations.push_back(observation);++added;}
        for(const auto& display:group.displays)if(existing.insert(display.id).second){found->displays.push_back(display);++added;}
        for(const auto& output:group.digital_outputs)if(existing.insert(output.id).second) {
            if(found==current.groups.end()){CatalogueGroup new_group;new_group.id=group.id;new_group.name=group.name;new_group.chapter=group.chapter;current.groups.push_back(std::move(new_group));found=std::prev(current.groups.end());}
            current.chapters.try_emplace(group.chapter,defaults.chapters.at(group.chapter));found->digital_outputs.push_back(output);++added;
        }
    }
    if(added){current.schema_version=std::max(defaults.schema_version,current.schema_version);current.finalize();}return added;
}
namespace {std::filesystem::path selected_catalogue;bool catalogue_started{};}
void catalogue_source(const std::filesystem::path& path){if(catalogue_started)throw std::runtime_error("Catalogue already loaded");selected_catalogue=path;}
const Catalogue &active_catalogue() {
    catalogue_started=true;static const auto value = read_catalogue(selected_catalogue.empty()?std::filesystem::path(executable_directory()) / L"catalogue.xml":selected_catalogue);
    return *value;
}
const CatalogueTarget *flight_target(std::size_t slot) noexcept {
    return slot < flight_slots ? active_catalogue().roles[slot] : nullptr;
}
double flight_fraction(std::size_t slot, int value) noexcept {
    const auto *t = flight_target(slot);
    if (!t)
        return 0;
    if (slot == 13) {
        const int i = flight_detent_index(value);
        return i < 0 ? 0 : static_cast<double>(i) / 6;
    }
    const auto &d = t->descriptor;
    if (slot < 8 || (slot >= 9 && slot <= 12))
        return std::clamp(value / (value < 0 ? -static_cast<double>(d.minimum) : static_cast<double>(d.maximum)), -1.0,
                          1.0);
    return std::clamp((static_cast<double>(value) - d.minimum) / (static_cast<double>(d.maximum) - d.minimum), 0.0,
                      1.0);
}
int flight_detent(int position) noexcept {
    const auto *t = flight_target(13);
    return t && position >= 0 && position < 7 ? t->detents[static_cast<std::size_t>(position)] : -1;
}
int flight_detent_index(int value) noexcept {
    const auto *t = flight_target(13);
    if (!t)
        return -1;
    const auto found = std::find(t->detents.begin(), t->detents.end(), value);
    return found == t->detents.end() ? -1 : static_cast<int>(found - t->detents.begin());
}
bool catalogue_record_valid(const TargetDescriptor &d, const std::vector<std::string_view> &fields) noexcept {
    const auto &records = active_catalogue().numeric_records;
    const auto it = records.find(record_name(d.record));
    if (it == records.end())
        return true;
    if (fields.size() != it->second.size())
        return false;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        int n{};
        const auto *t = it->second[i];
        if (!integer(fields[i], n) || n < t->minimum || n > t->maximum)
            return false;
    }
    return true;
}
bool catalogue_target_missing(const TargetDescriptor &d) noexcept {
    static const std::set<std::string, std::less<>> legacy_ids{
#include "legacy_catalogue_ids.inc"
    };
    return legacy_ids.count(d.id) && !active_catalogue().target(d.id);
}
void resolve_catalogue_target(TargetDescriptor &d) {
    if (const auto *definition = active_catalogue().target(d.id))
        d = *definition;
}
std::string catalogue_fingerprint(const Catalogue &c) {
    std::ostringstream o;
    auto target = [&](const TargetDescriptor &d) {
        o << std::quoted(d.id) << ' ' << std::quoted(d.name) << ' ' << std::quoted(d.unit) << ' '
          << record_name(d.record) << ' ' << static_cast<int>(d.layout) << ' ' << static_cast<int>(d.semantics) << ' '
          << d.minimum << ' ' << d.maximum << ' ' << d.released << ' ' << d.pressed << ' ' << d.owned_mask << ' '
          << d.transient_mask << ' ' << static_cast<unsigned>(d.shift) << ' ' << d.field << ' ' << d.field_count << ' '
          << static_cast<int>(d.throttle_engine) << ' ' << d.writable;
    };
    o << c.revision << ' ' << record_name(c.authority_record) << ' ' << c.authority_field << ' '
      << join(c.physical_modes) << ' ' << join(c.servo_modes) << '\n';
    for (const auto &[chapter, title] : c.chapters)
        o << "chapter " << chapter << ' ' << std::quoted(title) << '\n';
    for (const auto &g : c.groups) {
        o << "group " << std::quoted(g.id) << ' ' << std::quoted(g.name) << ' ' << g.chapter << ' ' << g.breakers
          << '\n';
        for (const auto &t : g.targets) {
            o << "target ";
            target(t.descriptor);
            o << ' ' << t.role << ' ' << t.numeric_fields << ' ' << join(t.detents) << ' ' << std::quoted(t.reference)
              << '\n';
        }
        for (const auto &a : g.actions) {
            o << "action " << std::quoted(a.id) << ' ' << std::quoted(a.group) << ' ' << std::quoted(a.name) << ' '
              << std::quoted(a.position) << ' ' << a.chapter << ' ' << a.order << ' ' << a.value << ' ' << a.release
              << ' ' << static_cast<int>(a.operation) << ' ' << static_cast<int>(a.encoding) << ' ' << a.primary << ' '
              << a.toggleable << ' ' << a.circuit_breaker << ' ' << a.character << ' ' << a.length << ' '
              << std::quoted(a.reference) << ' ';
            target(a.target);
            if(a.target.layout==ValueLayout::fixed_decimal)o<<" decimal "<<a.wrap<<' '<<a.unset<<' '<<a.payload_minimum<<' '<<a.payload_maximum<<' '<<a.guard_character<<' '<<a.guard_value;
            for (const auto &p : a.applicability)
                o << ' ' << record_name(p.first) << '=' << join(p.second);
            o << '\n';
        }
        for(const auto& observation:g.observations){
            o<<"observation "<<std::quoted(observation.id)<<' '<<std::quoted(observation.name)<<' '<<std::quoted(observation.unit)<<' '
             <<record_name(observation.record)<<' '<<static_cast<unsigned>(observation.encoding)<<' '<<observation.field<<' '
             <<observation.fields<<' '<<observation.character<<' '<<observation.minimum<<' '<<observation.maximum<<' '
             <<observation.mask<<' '<<observation.shift<<' '<<observation.motor_target<<' '<<std::quoted(observation.reference);
            if(observation.encoding==ObservationEncoding::decimal_slice)o<<" decimal "<<observation.width<<' '<<observation.payload_minimum<<' '<<observation.payload_maximum<<' '<<observation.guard_character<<' '<<observation.guard_value;
            o<<'\n';
        }
        for(const auto& input:g.cockpit){o<<"cockpit "<<input.encoder<<' '<<std::quoted(input.id)<<' '<<std::quoted(input.name)<<' '<<std::quoted(input.reference)<<' '<<input.decrease<<' '<<input.increase<<' '<<join(input.detents)<<' '<<input.baseline_minimum<<' '<<input.baseline_maximum<<' ';if(!input.encoder)target(input.target);for(const auto& p:input.applicability)o<<' '<<record_name(p.first)<<'='<<join(p.second);o<<'\n';}
        for(const auto& display:g.displays)o<<"display "<<std::quoted(display.id)<<' '<<std::quoted(display.name)<<' '<<std::quoted(display.reference)<<' '<<display.observation<<' '<<display.power<<' '<<display.mode<<' '<<display.style<<' '<<display.width<<'\n';
        for(const auto& output:g.digital_outputs) {
            o<<"digital-output "<<std::quoted(output.id)<<' '<<std::quoted(output.name)<<' '<<output.kind<<' '<<output.qualified<<' '<<std::quoted(output.unavailable)<<' '<<std::quoted(output.reference);
            auto print_operand=[&](const DigitalOperand& operand){o<<' '<<record_name(operand.record)<<':'<<operand.field<<':'<<operand.fields<<':'<<operand.minimum<<':'<<operand.maximum<<':'<<static_cast<unsigned>(operand.test)<<':'<<operand.mask<<':'<<operand.value<<':'<<static_cast<unsigned>(operand.encoding)<<':'<<operand.length_min<<':'<<operand.length_max<<':'<<std::quoted(operand.prefix)<<':'<<std::quoted(operand.allowed);};
            for(const auto& branch:output.branches) {o<<" branch";for(const auto& operand:branch)print_operand(operand);}
            o<<" restricted "<<std::quoted(output.requirement_reason);
            for(const auto& operand:output.requirements){o<<" require";print_operand(operand);}
            for(const auto& predicate:output.applicability)o<<" when "<<record_name(predicate.first)<<'='<<join(predicate.second);o<<'\n';
        }
    }
    return o.str();
}
} // namespace hwio
