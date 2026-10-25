#include "hwio/catalogue.hpp"
#include "hwio/button_setup.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace hwio {
namespace {
std::string owner(const ButtonAction &a) {
    // Mutually exclusive key values and switch positions share one owner.
    auto key = record_name(a.target.record);
    if (a.encoding == ButtonEncoding::character) return key + ":c" + std::to_string(a.character);
    if (a.target.layout == ValueLayout::fixed_decimal) return key + ":d" + std::to_string(a.target.field);
    if (a.target.layout == ValueLayout::bit_field) return key + ":b" + std::to_string(a.target.owned_mask);
    if (a.target.layout == ValueLayout::semicolon_field) return key + ":f" + std::to_string(a.target.field);
    return key;
}
bool assigned(const Profile &p) { return !p.id.empty(); }
bool trim_action(const ButtonAction& a) {
    return a.operation==ButtonOperation::held && a.encoding==ButtonEncoding::numeric && !a.release &&
           aileron_trim_command(a.target) &&
           ((a.target.owned_mask==5 && (a.value==1 || a.value==4)) ||
            (a.target.owned_mask==10 && (a.value==2 || a.value==8)));
}
const ControlKey &source_key(const Profile &p) { return p.bindings[0].sources[0].key; }
std::optional<int> decimal_baseline(const ButtonAction& a,std::string_view payload) {
    const auto& d=a.target;
    if(payload.size()<a.payload_minimum || payload.size()>a.payload_maximum ||
       d.field>payload.size() || d.field_count>payload.size()-d.field ||
       (a.guard_value && (a.guard_character>=payload.size() ||
                        static_cast<unsigned char>(payload[a.guard_character])!=a.guard_value)))return {};
    const auto slice=payload.substr(d.field,d.field_count);int value{};
    if(slice.find_first_not_of("0123456789")!=slice.npos || !integer(slice,value))return {};
    if(value==a.unset || value==a.wrap)return 0;
    return value>=0 && value<a.wrap?std::optional<int>(value):std::nullopt;
}
}
std::uint64_t button_command_id(RecordKey key) noexcept {
    return 0x4000000000000000ULL + (static_cast<std::uint64_t>(key.kind) << 16) + key.index;
}
std::uint64_t button_control_command_id(std::string_view id) noexcept {
    std::uint64_t hash=14695981039346656037ULL;for(const auto c:id){hash^=static_cast<unsigned char>(c);hash*=1099511628211ULL;}
    return 0x4100000000000000ULL | (hash&0x00ffffffffffffffULL);
}
const ButtonAction *button_action(std::string_view id) {
    return active_catalogue().action(id);
}
std::string button_owner(const ButtonAction& action){return owner(action);}
std::optional<int> button_extract(const ButtonAction &a, std::string_view payload) {
    if(a.target.layout==ValueLayout::fixed_decimal && a.wrap)return decimal_baseline(a,payload);
    if (a.encoding == ButtonEncoding::numeric) return extract(a.target, payload);
    if (payload.size() != a.length || a.character >= payload.size()) return {};
    const char c = payload[a.character];
    if (a.circuit_breaker) {
        if (c == 'n' || c == 'y' || c == 'b') return 1;
        if (c == 'N' || c == 'Y' || c == 'B' || c == 'R') return 0;
        return {};
    }
    if (!std::isalpha(static_cast<unsigned char>(c))) return {};
    return std::islower(static_cast<unsigned char>(c)) ? 1 : 0;
}
std::optional<std::string> button_compose(const ButtonAction &a, std::string_view payload, int value) {
    if(a.target.layout==ValueLayout::fixed_decimal && a.wrap) {
        const auto current=decimal_baseline(a,payload);if(!current)return {};
        const auto next=(static_cast<std::int64_t>(*current)+value%a.wrap+a.wrap)%a.wrap;
        return compose(a.target,payload,static_cast<int>(next));
    }
    if (a.encoding == ButtonEncoding::numeric) {
        const auto& d=a.target;
        // ThrustDisc/ThrustToga contain only two auto-cleared DELTA push
        // bits. It has no retained neighbour or startup value to preserve.
        // Keep this exception exact; other packed controls need their baseline.
        if((d.record==RecordKey{RecordKind::qh,386} || d.record==RecordKey{RecordKind::qh,387}) && d.layout==ValueLayout::bit_field &&
           d.semantics==Semantics::pulse && d.writable && d.transient_mask==3 &&
           !d.shift && !d.minimum && (d.owned_mask==1 || d.owned_mask==2) &&
           d.maximum==static_cast<int>(d.owned_mask) && a.operation==ButtonOperation::pulse &&
           a.value==static_cast<int>(d.owned_mask) && !a.release)
            return compose(d,"0",value);
        if (a.operation == ButtonOperation::step && a.target.layout == ValueLayout::semicolon_field) {
            std::string zeroes;
            for (unsigned i=0;i<a.target.field_count;++i) {if(i)zeroes+=';';zeroes+='0';}
            return compose(a.target,zeroes,value); // Never replay another rotary field's last DELTA.
        }
        if(payload.empty() && aileron_trim_command(d) && !trim_action(a))return {};
        return compose(a.target, payload, value);
    }
    if (value < 0 || value > 1 || !button_extract(a, payload)) return {};
    std::string result(payload);
    const auto c = static_cast<unsigned char>(result[a.character]);
    if (a.circuit_breaker && c == 'R') {
        if (value) return {};
    } else result[a.character] = static_cast<char>(value ? std::tolower(c) : std::toupper(c));
    return record_name(a.target.record) + "=" + result;
}
bool button_overlap(const ButtonAction &a, const TargetDescriptor &b) {
    return a.encoding == ButtonEncoding::character ? a.target.record == b.record : overlaps(a.target, b);
}
std::string button_availability(const ButtonAction &a, const Mapper &m) {
    for (const auto &p : a.applicability) {
        const auto it = m.records.find(record_name(p.first));
        int value{};
        if (it == m.records.end() || !integer(it->second.observed, value)) return "Awaiting aircraft configuration";
        if (std::find(p.second.begin(), p.second.end(), value) == p.second.end()) return "Not installed in this aircraft";
    }
    const auto record = m.records.find(record_name(a.target.record));
    if(a.target.layout==ValueLayout::fixed_decimal && a.wrap) {
        if(record==m.records.end())return "Awaiting current radio navigation record";
        const auto& payload=record->second.effective;
        if(payload.size()<a.payload_minimum || payload.size()>a.payload_maximum)return "Radio navigation record shape unavailable";
        if(a.guard_value && (a.guard_character>=payload.size() || static_cast<unsigned char>(payload[a.guard_character])!=a.guard_value))
            return "Radio is not manually tuned; FMC tuning preserved";
        if(!decimal_baseline(a,payload))return "Radio course baseline unavailable";
    }
    if (a.encoding == ButtonEncoding::character) {
        if (record == m.records.end()) return "Awaiting PSX panel";
        const auto &payload = record->second.effective;
        if (payload.size() != a.length) return "PSX panel shape unavailable";
        if (a.circuit_breaker && payload[a.character] == 'x') return "Not installed in this aircraft";
        if (!button_extract(a, payload)) return "PSX panel state unavailable";
        if (a.circuit_breaker && payload[a.character] == 'R' && a.value) return "Red security ring: IN inhibited";
    }
    return {};
}
Profile button_source(const DeviceInfo &d, const ControlInfo &c) {
    Profile p;
    p.id = "button-source";
    p.name = d.identity.name;
    p.identity = d.identity;
    p.explicit_path = d.identity.serial.empty() && d.identity.container_id.empty();
    Binding b;
    b.id = 1; b.enabled = false; b.name = c.name; b.mode = MappingMode::momentary;
    b.sources.push_back({c.key, {}});
    b.target.id = "source-only"; b.target.name = "Button source identity";
    b.target.minimum = 0; b.target.maximum = 1;
    p.bindings.push_back(std::move(b));
    return p;
}
bool button_source_valid(const Profile &p) noexcept {
    return !assigned(p) || (p.bindings.size() == 1 && p.bindings[0].sources.size() == 1 &&
                            source_key(p).kind == ControlKind::button);
}
bool button_configuration_valid(const ButtonConfiguration &c, std::string &error) {
    const auto valid_id=[](const std::string &id) {return !id.empty() && id.size()<=256 &&
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:")==id.npos;};
    const auto toggle_group=[&](const std::string &id) {
        if(!valid_id(id))return false;
        if(!active_catalogue().group_ids.count(id))return true; // Retained orphan.
        const auto found=active_catalogue().group_actions.find(id);
        return found!=active_catalogue().group_actions.end() && std::any_of(found->second.begin(),found->second.end(),
            [](auto index){const auto &a=button_catalogue()[index];return a.toggleable && a.primary;});
    };
    if (c.rows.size() > max_button_assignments || c.toggles.size() > max_button_assignments || c.toggle_drafts.size()>max_button_assignments) {
        error = "Too many button assignments"; return false;
    }
    for (const auto &r : c.rows) {
        if (!valid_id(r.first) || !button_source_valid(r.second.applied) ||
            (!r.second.draft.bindings.empty() && !button_source_valid(r.second.draft)) ||
            r.second.pending.size() > 2048 || r.second.replaces.size() > max_bindings) {
            error = "Invalid button assignment: " + r.first; return false;
        }
    }
    for(const auto &draft:c.toggle_drafts)if(!toggle_group(draft.first)){error="Invalid pending Toggle group";return false;}
    for (const auto &group : c.toggles) {
        if(!toggle_group(group)){error="Invalid Toggle group";return false;}
        if (!active_catalogue().group_ids.count(group)) continue; // Retained orphan, never routed.
        for (const auto &a : button_catalogue()) if (a.group == group && !a.primary) {
            const auto r = c.rows.find(a.id);
            if (r != c.rows.end() && assigned(r->second.applied)) { error = "Toggle retains a second position"; return false; }
        }
    }
    return true;
}
std::string button_configuration_encode(const ButtonConfiguration &c) {
    std::ostringstream out;
    out << "buttons-v2\n" << c.toggles.size() << '\n';
    for (const auto &g : c.toggles) out << std::quoted(g) << '\n';
    out<<c.toggle_drafts.size()<<'\n';
    for(const auto &g:c.toggle_drafts)out<<std::quoted(g.first)<<' '<<g.second<<'\n';
    out << c.rows.size() << '\n';
    for (const auto &r : c.rows) {
        out << std::quoted(r.first) << ' ' << r.second.managed << ' ' << std::quoted(r.second.pending) << ' '
            << std::quoted(assigned(r.second.applied) ? profile_encode(r.second.applied) : "") << ' '
            << std::quoted(assigned(r.second.draft) ? profile_encode(r.second.draft) : "") << ' ' << r.second.replaces.size();
        for (const auto &old : r.second.replaces) out << ' ' << std::quoted(old.first) << ' ' << old.second;
        out << '\n';
    }
    return out.str();
}
bool button_configuration_decode(std::string_view text, ButtonConfiguration &c, std::string &error) {
    std::istringstream in{std::string(text)};
    ButtonConfiguration next;
    std::string header, id, source;
    std::size_t n{};
    if (!(in >> header >> n) || (header != "buttons-v1" && header!="buttons-v2") || n > max_button_assignments) { error = "Invalid button header"; return false; }
    for (std::size_t i = 0; i < n; ++i)
        if (!(in >> std::quoted(id)) || !next.toggles.insert(id).second) { error = "Invalid Toggle group"; return false; }
    if(header=="buttons-v2") {
        if(!(in>>n)||n>max_button_assignments)return false;
        for(std::size_t i=0;i<n;++i){bool value{};if(!(in>>std::quoted(id)>>value)||!next.toggle_drafts.emplace(id,value).second)return false;}
    }
    if (!(in >> n) || n > max_button_assignments) { error = "Invalid button row count"; return false; }
    for (std::size_t i = 0; i < n; ++i) {
        ButtonRow r;
        if (!(in >> std::quoted(id) >> r.managed >> std::quoted(r.pending) >> std::quoted(source))) return false;
        if (!source.empty() && !profile_decode(source, r.applied, error)) return false;
        if (!(in >> std::quoted(source))) return false;
        if (!source.empty() && !profile_decode(source, r.draft, error)) return false;
        std::size_t count{};
        if (!(in >> count) || count > max_bindings) return false;
        for (std::size_t j = 0; j < count; ++j) {
            std::pair<std::string, std::uint64_t> old;
            if (!(in >> std::quoted(old.first) >> old.second)) return false;
            r.replaces.push_back(std::move(old));
        }
        if (!next.rows.emplace(id, std::move(r)).second) return false;
    }
    in >> std::ws;
    if (!in.eof() || !button_configuration_valid(next, error)) return false;
    c = std::move(next); return true;
}
void button_toggle(ButtonConfiguration &c, const std::string &group, bool enabled) {
    const ButtonAction *primary = nullptr, *other = nullptr;
    for (const auto &a : button_catalogue()) if (a.group == group && a.toggleable) {
        if (a.primary) primary = &a; else other = &a;
    }
    if (!primary || !other) return;
    c.toggle_drafts.erase(group);
    auto &first = c.rows[primary->id];
    auto &second = c.rows[other->id];
    if (enabled) {
        if (!assigned(first.draft) && assigned(second.draft)) first = second;
        first.replaces.insert(first.replaces.end(), second.replaces.begin(), second.replaces.end());
        second = {}; second.managed = true;
        c.toggles.insert(group);
    } else c.toggles.erase(group);
    first.managed = true;
}
bool button_replaces(const ButtonConfiguration &c, const std::string &p, std::uint64_t b) {
    for (const auto &r : c.rows) if (r.second.managed)
        for (const auto &old : r.second.replaces) if (old.first == p && old.second == b) return true;
    return false;
}
void button_import_legacy(ButtonConfiguration &c, const std::vector<Profile> &profiles) {
    for (const auto &p : profiles) for (const auto &b : p.bindings) {
        if (!b.enabled || b.sources.size() != 1 || b.sources[0].key.kind != ControlKind::button ||
            (!b.sources[0].profile.empty() && b.sources[0].profile != p.id)) continue;
        for (const auto &a : button_catalogue()) {
            if (a.encoding != ButtonEncoding::numeric || !overlaps(a.target, b.target) ||
                a.target.layout != b.target.layout || a.target.owned_mask != b.target.owned_mask ||
                a.value != b.target.pressed || (a.target.layout == ValueLayout::semicolon_field && a.target.field != b.target.field)) continue;
            const bool compatible = (b.mode == MappingMode::toggle && a.toggleable && a.primary) ||
                (b.mode == MappingMode::momentary && a.operation != ButtonOperation::position && a.operation != ButtonOperation::step) ||
                (b.mode == MappingMode::relative && a.operation == ButtonOperation::step && b.step == a.value) ||
                (b.mode == MappingMode::maintained && a.operation == ButtonOperation::position && a.primary);
            if (!compatible || c.rows.count(a.id)) continue;
            ButtonRow r;
            r.draft = p; r.draft.bindings = {b}; r.applied = r.draft;
            r.replaces.push_back({p.id, b.id});
            c.rows.emplace(a.id, std::move(r)); // Display legacy source without creating an owner.
            if (b.mode == MappingMode::toggle) c.toggles.insert(a.group);
            break;
        }
    }
}
void ButtonMovement::input(const DeviceInfo &d, const Sample &s) {
    if (s.device >= max_devices || s.control >= max_controls || s.control >= d.controls.size() ||
        d.controls[s.control].key.kind != ControlKind::button || s.report_end) return;
    auto &state = states_[s.device][s.control];
    const bool known = state.known && state.generation == s.generation;
    if (known && !s.baseline && !state.down && s.value != 0)
        last_ = {true, true, s, d.identity.name, d.controls[s.control].name};
    state = {true, s.value != 0, s.generation};
}
LastButtonInput ButtonMovement::snapshot(const std::vector<DeviceInfo> &devices) const {
    auto result = last_;
    result.available = result.present && std::any_of(devices.begin(), devices.end(), [&](const auto &d) {
        return d.connected && d.slot == result.sample.device && d.generation == result.sample.generation;
    });
    return result;
}
ButtonRouter::ButtonRouter() : routes_(std::make_unique<std::array<std::vector<std::size_t>, max_devices * max_controls>>()) {}
void ButtonRouter::boundary(std::uint64_t epoch, bool same_connection_load) {
    cached_view_.reset();
    cancellations_.clear();
    epoch_ = epoch; events_.clear(); held_owner_.clear(); last_event_.clear(); overflow = false; batch_ = false; ready_seen_ = false;
    override_releases_.clear();override_cancellations_.clear();
    if(!same_connection_load)for (auto &e : entries_) { e.known = false; e.down = false;for(auto& c:e.contacts){c.known=false;c.down=false;} }
    for (auto &g : groups_) {
        // A loaded native ARM change must still release its magnetic contact
        // before that contact falls; do not turn its retained UP into a new edge.
        if(!same_connection_load || !g.module_arm){g.enforcing=false;g.server_selected=false;}
        g.dirty=true;g.pending=false;g.intent=nullptr;g.trim_value.reset();g.delivery=DeliveryState::unavailable;
    }
}
void ButtonRouter::configure(const ButtonConfiguration &c, const std::vector<DeviceInfo> &devices, Mapper &m) {
    cached_view_.reset();
    const auto previous_entries=std::move(entries_);
    const auto previous_groups=std::move(groups_);
    const auto previous_events=std::move(events_);
    const auto previous_held=std::move(held_owner_);
    entries_.clear();groups_.clear();events_.clear();held_owner_.clear();
    record_groups_.clear();
    for (auto &r : *routes_) r.clear();
    std::vector<DeviceIdentity> identities;
    std::vector<const DeviceInfo *> connected;
    for (const auto &d : devices) if (d.connected) { identities.push_back(d.identity); connected.push_back(&d); }
    std::map<std::string, std::size_t> owners;
    auto add_source=[&](const ButtonAction* a,const Profile& source,const ButtonRow* row=nullptr){
        if(!assigned(source))return;
        Entry e; e.action = a; e.toggle = c.toggles.count(a->group) != 0;
        if (e.toggle && !a->primary) return;
        const auto key = owner(*a);
        auto it = owners.find(key);
        if (it == owners.end()) {
            Group g; g.id = key; g.record = record_name(a->target.record);
            it = owners.emplace(key, groups_.size()).first;
            groups_.push_back(std::move(g));
            record_groups_[record_name(a->target.record)].push_back(it->second);
        }
        e.group = it->second;
        const int match = match_profile(source, identities);
        if (match >= 0) {
            const auto &d = *connected[static_cast<std::size_t>(match)];
            e.coherent=d.coherent_reports;
            // A spring-return MCP ARM contact must allow PSX to release its
            // holding coil before the physical contact can follow it down.
            groups_[e.group].module_arm=groups_[e.group].module_arm ||
                (!e.toggle && groups_[e.group].record=="Qh58" && d.complete_module &&
                 (d.identity.backend==BackendKind::fds_usb || d.identity.backend==BackendKind::fds_ethernet || d.identity.backend==BackendKind::cpflight_serial));
            for (std::size_t i = 0; i < d.controls.size(); ++i) if (same(source_key(source), d.controls[i].key)) {
                e.device = d.slot; e.control = static_cast<int>(i); e.generation = d.generation; break;
            }
        }
        if(row && !row->module_contacts.empty()){
            e.states=row->module_states;e.coherent=false;
            for(const auto& profile:row->module_contacts){
                Entry::Contact contact;const auto contact_match=match_profile(profile,identities);
                if(contact_match>=0){const auto& d=*connected[static_cast<std::size_t>(contact_match)];contact.generation=d.generation;
                    for(std::size_t i=0;i<d.controls.size();++i)if(same(source_key(profile),d.controls[i].key)){contact.device=d.slot;contact.control=static_cast<int>(i);break;}}
                if(contact.device>=0 && contact.control>=0)(*routes_)[static_cast<std::size_t>(contact.device)*max_controls+static_cast<std::size_t>(contact.control)].push_back(entries_.size());
                e.contacts.push_back(contact);
            }
        }else if (e.device >= 0 && e.control >= 0)
            (*routes_)[static_cast<std::size_t>(e.device) * max_controls + static_cast<std::size_t>(e.control)].push_back(entries_.size());
        groups_[e.group].entries.push_back(entries_.size());
        entries_.push_back(std::move(e));
    };
    for (const auto &row : c.rows) {
        const auto &r = row.second;
        const auto *a = button_action(row.first);
        if (!a || !r.managed) continue;
        add_source(a,r.applied,&r);
        if(a->operation==ButtonOperation::pulse || a->operation==ButtonOperation::step || a->operation==ButtonOperation::held)
            for(const auto& source:r.module_sources)add_source(a,source);
    }
    for (const auto &key : interests()) {m.watched_records.insert(record_name(key));watched_.insert(record_name(key));}
    for(auto& g:groups_) {
        std::set<std::string> scanned;
        for(const auto i:g.entries) {
        const auto& entry=entries_[i];
        if(entry.toggle || entry.action->operation!=ButtonOperation::position || !scanned.insert(entry.action->group).second)continue;
        for(const auto index:active_catalogue().group_actions.at(entry.action->group)) {
            const auto& candidate=button_catalogue()[index];
            if(candidate.operation==ButtonOperation::position && owner(candidate)==g.id)
                g.positions.push_back(&candidate);
        }
        }
    }
    // One maintained DOWN contact cannot distinguish the two raised detents.
    // Count applied entries, including disconnected sources, rather than only
    // currently available contacts: a lost UP/OFF input must not change wiring.
    for(auto& g:groups_)if(g.record=="Qh170" && g.entries.size()==1 && g.positions.size()==3){
        const auto& e=entries_[g.entries.front()];
        if(!e.toggle && e.contacts.empty() && e.action->operation==ButtonOperation::position &&
           e.action->encoding==ButtonEncoding::numeric && e.action->target.layout==ValueLayout::integer && e.action->value==3)
            for(const auto* a:g.positions)if(a->value==1)g.down_only_gear_up=a;
    }
    configuration_=c;
    std::map<std::size_t,std::size_t> remap;
    // UI edits do not reset unrelated held controls or replay their presses.
    if(epoch_==m.epoch) for(std::size_t n=0;n<groups_.size();++n) {
        auto &g=groups_[n];
        if(retired_.count(g.record))continue;
        for(std::size_t old=0;old<previous_groups.size();++old) {
            const auto &p=previous_groups[old];
            if(p.id!=g.id || p.entries.size()!=g.entries.size())continue;
            bool same_group=true;
            for(std::size_t j=0;j<g.entries.size();++j) {
                const auto &a=entries_[g.entries[j]],&b=previous_entries[p.entries[j]];
                if(a.action!=b.action||a.device!=b.device||a.control!=b.control||a.generation!=b.generation||a.toggle!=b.toggle||a.coherent!=b.coherent)same_group=false;
                if(a.states!=b.states || a.contacts.size()!=b.contacts.size())same_group=false;
                else for(std::size_t k=0;k<a.contacts.size();++k)if(a.contacts[k].device!=b.contacts[k].device || a.contacts[k].control!=b.contacts[k].control || a.contacts[k].generation!=b.contacts[k].generation)same_group=false;
            }
            if(!same_group)continue;
            const auto indices=g.entries;g=p;g.entries=indices;remap[old]=n;
            for(std::size_t j=0;j<g.entries.size();++j) {
                entries_[g.entries[j]]=previous_entries[p.entries[j]];entries_[g.entries[j]].group=n;
                const auto held=previous_held.find(g.id);
                if(held!=previous_held.end()&&held->second==p.entries[j])held_owner_[g.id]=g.entries[j];
            }
            break;
        }
    }
    for(auto event:previous_events)if(remap.count(event.group)){event.group=remap[event.group];events_.push_back(event);}
    for(auto& [id,release]:override_releases_){
        const auto found=std::find_if(groups_.begin(),groups_.end(),[&](const auto& g){return g.id==owner(release.action);});
        if(found==groups_.end()){Group g;g.id=owner(release.action);g.record=record_name(release.action.target.record);g.dirty=false;groups_.push_back(std::move(g));release.group=groups_.size()-1;}
        else release.group=static_cast<std::size_t>(found-groups_.begin());
        if(std::none_of(events_.begin(),events_.end(),[&](const auto& event){return event.cleanup && event.action->id==id;}))events_.push_back({&release.action,release.action.release,0,release.group,true});
    }
    retired_.clear();
    if(epoch_!=m.epoch)boundary(m.epoch);
}
std::vector<RecordKey> ButtonRouter::changed_records(const ButtonConfiguration &next) const {
    const auto signatures=[](const ButtonConfiguration &c) {
        std::map<std::string,std::string> result;
        for(const auto &r:c.rows)if(r.second.managed&&!r.second.applied.id.empty()) {
            const auto *a=button_action(r.first);if(!a)continue;
            result[record_name(a->target.record)]+=r.first+":"+std::to_string(c.toggles.count(a->group))+":"+profile_encode(r.second.applied);
            for(const auto& source:r.second.module_sources)result[record_name(a->target.record)]+=":module:"+profile_encode(source);
            for(const auto& source:r.second.module_contacts)result[record_name(a->target.record)]+=":contact:"+profile_encode(source);
            for(const auto state:r.second.module_states)result[record_name(a->target.record)]+=":state:"+std::to_string(state);
        }
        return result;
    };
    const auto before=signatures(configuration_),after=signatures(next);
    std::vector<RecordKey> result;
    std::set<std::string> seen;
    for(const auto &a:button_catalogue()) {
        const auto key=record_name(a.target.record);
        if(!seen.insert(key).second)continue;
        const auto b=before.find(key),n=after.find(key);
        if((b==before.end())!=(n==after.end()) || (b!=before.end()&&n!=after.end()&&b->second!=n->second))result.push_back(a.target.record);
    }
    return result;
}
void ButtonRouter::retire(const std::vector<RecordKey> &records,Mapper &m) {
    for(const auto key:records)retired_.insert(record_name(key));
    events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto &e){return retired_.count(groups_[e.group].record)!=0;}),events_.end());
    for(auto it=held_owner_.begin();it!=held_owner_.end();) {
        const auto &e=entries_[it->second];
        if(retired_.count(groups_[e.group].record)) {
            if(m.ready)enqueue(*e.action,e.action->release,0,e.group);
            it=held_owner_.erase(it);
        } else ++it;
    }
    for(auto &g:groups_)if(retired_.count(g.record)){g.enforcing=false;g.dirty=false;g.pending=false;}
    batch_=false;
}
void ButtonRouter::enqueue(const ButtonAction &a, int value, std::int64_t qpc, std::size_t group) {
    if (events_.size() >= max_button_events) { overflow = true; events_.clear(); return; }
    events_.push_back({&a, value, qpc, group});
}
void ButtonRouter::overrides(const std::set<std::string>& actions,Mapper& m){
    const auto previous=override_actions_;override_actions_=actions;cached_view_.reset();
    for(std::size_t n=0;n<groups_.size();++n){auto& g=groups_[n];
        const auto contains=[&](auto i){return actions.count(entries_[i].action->id)!=0;};
        const bool blocked=g.positions.empty()?(!g.entries.empty() && std::all_of(g.entries.begin(),g.entries.end(),contains)):
            (std::any_of(g.entries.begin(),g.entries.end(),contains) || std::any_of(g.positions.begin(),g.positions.end(),[&](auto* a){return actions.count(a->id)!=0;}));
        const bool changed=std::any_of(g.entries.begin(),g.entries.end(),[&](auto i){const auto& id=entries_[i].action->id;return actions.count(id)!=previous.count(id);});
        if(changed){
            events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto& e){return e.group==n && !e.cleanup && actions.count(e.action->id)!=0;}),events_.end());
            for(const auto i:g.entries){const auto& a=*entries_[i].action;if(actions.count(a.id) && !previous.count(a.id)){override_cancellations_.push_back(button_control_command_id(a.id));if(a.operation==ButtonOperation::position || entries_[i].toggle)cancellations_.push_back(a.target.record);}}
            const auto held=held_owner_.find(g.id);
            // A submitted press may have completed before its transport acknowledgement
            // reaches this worker. One release also covers that in-transit case.
            if(held!=held_owner_.end() && actions.count(entries_[held->second].action->id)){const auto& e=entries_[held->second];if(m.ready){LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);auto& release=override_releases_[e.action->id];release={*e.action,n,clock.QuadPart};events_.push_back({&release.action,release.action.release,0,n,true});}held_owner_.erase(held);}
        }
        if(g.inhibited!=blocked){g.enforcing=false;g.pending=false;g.dirty=false;g.awaiting_report=!blocked;g.capture_muted=false;if(!blocked)g.reason.clear();}
        g.inhibited=blocked;if(blocked)g.reason="Blocked by input override";
    }
}
void ButtonRouter::baseline(const SourceBank &values, Mapper &m) {
    for(const auto& e:entries_)for(const auto& c:e.contacts)if(c.device>=0 && c.control>=0){
        const auto& v=values[static_cast<std::size_t>(c.device)][static_cast<std::size_t>(c.control)];
        if(v.available)input({static_cast<std::uint16_t>(c.device),static_cast<std::uint16_t>(c.control),c.generation,v.sequence,v.qpc,v.value,true,false,false},values,m,false);
    }
    for (const auto &e : entries_) if (e.device >= 0 && e.control >= 0) {
        const auto &v=values[static_cast<std::size_t>(e.device)][static_cast<std::size_t>(e.control)];
        if (v.available) input({static_cast<std::uint16_t>(e.device),static_cast<std::uint16_t>(e.control),e.generation,
                               v.sequence,v.qpc,v.value,true,false,false},values,m,false);
    }
}
void ButtonRouter::input(const Sample &s, const SourceBank &, Mapper &m, bool edge) {
    if (s.device >= max_devices || s.control >= max_controls) return;
    if (epoch_ != m.epoch) boundary(m.epoch);
    if (edge) batch_ = true;
    std::vector<std::string> captured;
    if(capture_ && !s.invalid && !s.baseline && edge && s.value){
        for(const auto i:(*routes_)[s.device*max_controls+s.control]){const auto& e=entries_[i];if(e.contacts.empty() && e.generation==s.generation && e.known && !e.down)captured.push_back(e.action->id);}
        if(!captured.empty() && !capture_(captured))captured.clear();
    }
    for (const auto index : (*routes_)[s.device * max_controls + s.control]) {
        auto &e = entries_[index]; auto &g = groups_[e.group]; const auto &a = *e.action;
        if(!e.contacts.empty()){
            bool matched=false;for(auto& c:e.contacts)if(c.device==s.device && c.control==s.control && c.generation==s.generation){c.known=!s.invalid;c.down=!s.invalid && s.value!=0;matched=true;}
            if(!matched)continue;
            const bool was_known=e.known,was=e.known && e.down;unsigned mask{};e.known=true;
            for(std::size_t n=0;n<e.contacts.size();++n){const auto& c=e.contacts[n];e.known=e.known && c.known;if(c.down)mask|=1u<<n;}
            e.down=e.known && std::find(e.states.begin(),e.states.end(),mask)!=e.states.end();
            if(was_known && e.down && !was && edge && !s.baseline){
                if(capture_ && capture_({a.id})){if(!override_actions_.count(a.id))capture_muted_actions_.insert(a.id);else capture_muted_actions_.erase(a.id);}
                else for(const auto i:g.entries)capture_muted_actions_.erase(entries_[i].action->id);
            }
            g.capture_muted=std::any_of(g.entries.begin(),g.entries.end(),[&](auto i){return capture_muted_actions_.count(entries_[i].action->id)!=0;});
            g.dirty=true;g.qpc=s.qpc;g.pending=false;g.awaiting_report=false;cached_view_.reset();continue;
        }
        if (e.generation != s.generation) continue;
        if(s.invalid){
            const auto held=held_owner_.find(g.id);
            if(m.ready && held!=held_owner_.end() && held->second==index && a.operation==ButtonOperation::held){enqueue(a,a.release,s.qpc,e.group);held_owner_.erase(held);}
            e.known=false;e.down=false;g.dirty=true;g.reason="Physical input unavailable";cached_view_.reset();continue;
        }
        if(!e.known || e.down!=(s.value!=0))cached_view_.reset();
        const bool press = e.known && !e.down && s.value != 0 && edge && !s.baseline;
        const bool release = e.known && e.down && s.value == 0;
        if(press || release)g.server_selected=false;
        const auto prior_qpc=g.qpc;
        e.known = true; e.down = s.value != 0; g.dirty = true; g.qpc = s.qpc;
        const bool identifying=std::find(captured.begin(),captured.end(),a.id)!=captured.end();
        if(identifying){if(!override_actions_.count(a.id))capture_muted_actions_.insert(a.id);else capture_muted_actions_.erase(a.id);g.enforcing=false;g.pending=false;}
        else if(press)for(const auto i:g.entries)if(entries_[i].action->group==a.group)capture_muted_actions_.erase(entries_[i].action->id);
        g.capture_muted=!g.positions.empty() && std::any_of(g.entries.begin(),g.entries.end(),[&](auto i){return capture_muted_actions_.count(entries_[i].action->id)!=0;});
        if(!s.baseline || s.qpc>prior_qpc)g.awaiting_report=false;
        if(g.inhibited || g.capture_muted || override_actions_.count(a.id) || capture_muted_actions_.count(a.id)){if(g.inhibited || g.capture_muted){g.enforcing=false;g.pending=false;g.dirty=false;}continue;}
        if (!m.ready) continue;
        if(!e.toggle && a.operation==ButtonOperation::position && press) {
            g.intent=&a;g.desired=a.value;g.pending=true;
        }
        if (e.toggle && press) {
            const auto r = m.records.find(g.record);
            const auto value = g.pending ? std::optional<int>(g.desired) :
                r == m.records.end() ? std::nullopt : button_extract(a, r->second.effective);
            if (!value) { g.reason = "Toggle awaits known PSX state"; continue; }
            const ButtonAction *other = nullptr;
            for (const auto candidate_index : active_catalogue().group_actions.at(a.group)) { const auto &candidate=button_catalogue()[candidate_index]; if(!candidate.primary) {other=&candidate;break;} }
            if (!other) continue;
            g.intent = &a; g.desired = *value == a.value ? other->value : a.value; g.pending = true; g.enforcing = false;
            enqueue(a, g.desired, s.qpc, e.group);
        } else if (!e.toggle && a.operation != ButtonOperation::position) {
            if (press) {
                if (a.operation == ButtonOperation::held) held_owner_[g.id] = index;
                else held_owner_.erase(g.id);
                enqueue(a, a.value, s.qpc, e.group);
            } else if (release && a.operation == ButtonOperation::held) {
                const auto held = held_owner_.find(g.id);
                if (held != held_owner_.end() && held->second == index) {
                    enqueue(a, a.release, s.qpc, e.group); held_owner_.erase(held);
                }
            }
        }
    }
}
void ButtonRouter::positions(const SourceBank &values, Mapper &m) {
    if (batch_) return;
    for (auto &g : groups_) {
        if(g.inhibited || g.capture_muted || g.awaiting_report || g.server_selected)continue;
        if (!g.dirty) continue;
        g.dirty = false;
        if(held_owner_.count(g.id)){g.enforcing=false;g.pending=false;continue;}
        const Entry *active = nullptr; bool conflict = false, unavailable_contact=false;
        for (const auto i : g.entries) {
            const auto &e = entries_[i];
            if (e.toggle || e.action->operation != ButtonOperation::position || e.device < 0 || e.control < 0) continue;
            const auto &v = values[static_cast<std::size_t>(e.device)][static_cast<std::size_t>(e.control)];
            if (e.contacts.empty() ? (v.available && v.value) : (e.known && e.down)) {
                const auto unavailable=button_availability(*e.action,m);
                if(!unavailable.empty()){g.reason=unavailable;unavailable_contact=true;continue;}
                if (active && active->action->value != e.action->value) conflict = true;
                active = &e;
            }
        }
        if (conflict) {
            if(g.intent)cancellations_.push_back(g.intent->target.record);
            g.enforcing=false;g.pending=false;g.reason="Conflicting hardware positions; release the other contact";continue;
        }
        if (active) {
            g.intent = active->action; g.desired = active->action->value;
            g.reason = button_availability(*active->action, m); g.enforcing = g.reason.empty();
            if(!g.enforcing){cancellations_.push_back(active->action->target.record);g.pending=false;}
        } else {
            g.enforcing=false;
            if(std::any_of(g.entries.begin(),g.entries.end(),[&](auto i){return !entries_[i].contacts.empty();})){
                if(g.intent){
                    cancellations_.push_back(g.intent->target.record);
                    // The transport cancels the whole maintained record. Drop
                    // its unsent prediction before composing a sibling field,
                    // rather than letting that sibling resurrect a stale mode.
                    const auto record=m.records.find(g.record);
                    if(record!=m.records.end())record->second.effective=record->second.committed;
                }
                g.pending=false;g.reason="Compound selector awaits valid, unambiguous explicit contacts";continue;
            }
            if(unavailable_contact && g.intent){cancellations_.push_back(g.intent->target.record);g.pending=false;}
            if(!g.positions.empty()) {
                bool valid=!unavailable_contact;
                for(const auto i:g.entries) {
                    const auto& e=entries_[i];
                    if(button_availability(*e.action,m)=="Not installed in this aircraft")continue;
                    if(e.toggle || !e.known || e.device<0 || e.control<0 || (g.entries.size()>1 && !e.coherent)) {valid=false;break;}
                    const auto& v=values[static_cast<std::size_t>(e.device)][static_cast<std::size_t>(e.control)];
                    if(!v.available || v.value) {valid=false;break;}
                }
                const ButtonAction* missing=nullptr;unsigned missing_count=0;
                for(const auto* candidate:g.positions) {
                    const auto issue=button_availability(*candidate,m);
                    if(issue=="Not installed in this aircraft")continue;
                    if(!issue.empty() && issue!="Red security ring: IN inhibited")valid=false;
                    if(std::none_of(g.entries.begin(),g.entries.end(),[&](auto i){return entries_[i].action->id==candidate->id;})) {
                        missing=candidate;++missing_count;
                    }
                }
                if(g.down_only_gear_up){missing=g.down_only_gear_up;missing_count=1;}
                if(missing_count==1) {
                    if(valid && button_availability(*missing,m).empty()) {
                        g.intent=missing;g.desired=missing->value;g.enforcing=true;g.pending=true;g.reason.clear();
                    } else {
                        if(g.intent)cancellations_.push_back(g.intent->target.record);
                        g.pending=false;g.reason=std::any_of(g.entries.begin(),g.entries.end(),[&](const auto index){return !entries_[index].coherent;}) && g.entries.size()>1?
                            "Implicit position requires coherent hardware reports; this transport provides individual edges":
                            "Implicit position awaits valid inactive contacts and available PSX state";
                    }
                }
            }
            if(g.reason.find("Conflicting hardware positions")==0)g.reason.clear();
        }
    }
}
void ButtonRouter::received(std::string_view record) {
    if(watched_.count(std::string(record)))cached_view_.reset();
    const auto it = record_groups_.find(std::string(record));
    if (it != record_groups_.end()) for (const auto index : it->second) {
        auto &g=groups_[index];g.dirty=true;
        if(g.module_arm && g.enforcing){
            if(g.intent)cancellations_.push_back(g.intent->target.record);
            g.server_selected=true;g.enforcing=false;g.pending=false;g.dirty=false;
        }
        // The next press uses received effective state once earlier intents have left the router.
        if(std::none_of(events_.begin(),events_.end(),[&](const auto &e){return e.group==index;}))g.pending=false;
    }
    if (record.substr(0, 2) == "Qi") for (auto &g : groups_) g.dirty = true;
}
void ButtonRouter::sent(std::string_view line,std::uint64_t binding,std::int64_t qpc) {
    const auto equal=line.find('=');auto payload=equal==line.npos?std::string_view{}:line.substr(equal+1);while(!payload.empty() && (payload.back()=='\n' || payload.back()=='\r'))payload.remove_suffix(1);
    for(auto it=override_releases_.begin();it!=override_releases_.end();){const auto& release=it->second;
        const auto value=button_extract(release.action,payload);
        if(line.substr(0,equal)==record_name(release.action.target.record) && (!binding || binding==button_control_command_id(it->first)) && (!qpc || qpc>=release.after) && value && *value==release.action.release){
            const auto id=it->first;events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto& event){return event.cleanup && event.action->id==id;}),events_.end());it=override_releases_.erase(it);
        }else ++it;
    }
    const auto it = record_groups_.find(std::string(line.substr(0, line.find('='))));
    if (it != record_groups_.end()) {cached_view_.reset();for (const auto i : it->second) groups_[i].delivery = DeliveryState::sent;}
}
void ButtonRouter::release_all(Mapper &m) {
    cached_view_.reset();
    events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto& event){return !event.cleanup || !m.ready;}),events_.end());
    if(m.ready)for(auto& [id,release]:override_releases_)if(std::none_of(events_.begin(),events_.end(),[&](const auto& event){return event.cleanup && event.action->id==id;}))events_.push_back({&release.action,release.action.release,0,release.group,true});
    if (m.ready) for (const auto &held : held_owner_) {
        const auto &e = entries_[held.second];
        enqueue(*e.action, e.action->release, 0, e.group);
    }
    held_owner_.clear();
    for (auto &g : groups_) { g.enforcing = false; g.dirty = false; g.pending = false; }
    batch_ = false;
}
std::optional<Command> ButtonRouter::next(const SourceBank &values, Mapper &m, double ms, std::int64_t qpc) {
    if (epoch_ != m.epoch) boundary(m.epoch);
    if (!m.ready) return {};
    if (!ready_seen_) { cached_view_.reset();ready_seen_ = true; for (auto &g : groups_) g.dirty = true; }
    positions(values, m);
    // Per-record ordering prevents an older held-key release cancelling a newer key.
    std::set<std::string> blocked;
    for (auto it = events_.begin(); it != events_.end();) {
        const auto event = *it; const auto &a = *event.action;
        auto &g = groups_[event.group]; const auto record = g.record;
        if((g.inhibited || g.capture_muted || override_actions_.count(a.id) || capture_muted_actions_.count(a.id)) && !event.cleanup){it=events_.erase(it);continue;}
        const bool paced = a.operation == ButtonOperation::pulse || a.operation == ButtonOperation::step || a.circuit_breaker;
        if (blocked.count(record) || (paced && last_event_.count(record) && ms - last_event_[record] < 50)) {
            blocked.insert(record); ++it; continue;
        }
        auto r = m.records.find(record);
        g.reason = button_availability(a, m);
        if (a.circuit_breaker && event.value == 0 && g.reason == "Red security ring: IN inhibited") g.reason.clear();
        auto payload=r==m.records.end()?std::string{}:r->second.effective;
        if(trim_action(a)) {
            if(r!=m.records.end() && r->second.sequence && payload.empty())g.reason="Invalid PSX trim command word";
            // Preserve dequeued sibling intent through late echoes/receipts.
            // Input edges not yet dequeued must retain their original order.
            for(const auto sibling:record_groups_[record]) {
                const auto& state=groups_[sibling];
                if(!state.trim_value || state.entries.empty())continue;
                const auto& source=*entries_[state.entries.front()].action;
                if(!trim_action(source))continue;
                const auto merged=button_compose(source,payload,*state.trim_value);
                if(!merged){g.reason="Current PSX baseline unavailable";break;}
                payload=merged->substr(merged->find('=')+1);
            }
        }
        auto line = g.reason.empty() ? button_compose(a, payload, event.value) : std::nullopt;
        it = events_.erase(it);
        if (!line) { if (g.reason.empty()) g.reason = "Current PSX baseline unavailable"; g.pending = false; continue; }
        if (paced) last_event_[record] = ms;
        if(trim_action(a))g.trim_value=event.value;
        if (r != m.records.end() && a.operation != ButtonOperation::pulse && a.operation != ButtonOperation::step)
            r->second.effective = line->substr(line->find('=') + 1);
        g.delivery = DeliveryState::queued;
        cached_view_.reset();
        Command command{button_control_command_id(a.id), m.epoch, event.qpc, qpc, *line + "\n", true, 0, event.value};
        command.minimum_interval_ms=paced?50u:0u;
        return command;
    }
    if(batch_)return {}; // Evaluate maintained contacts only after the whole HID report.
    for (auto &g : groups_) {
        if(g.inhibited || g.capture_muted || g.awaiting_report)continue;
        if ((!g.enforcing && !g.pending) || !g.intent || blocked.count(g.record)) continue;
        if(std::any_of(g.entries.begin(),g.entries.end(),[&](const auto i){return entries_[i].toggle;}))continue;
        const auto &a = *g.intent;
        g.reason=button_availability(a,m);
        if(!g.reason.empty()){g.pending=false;continue;}
        const auto r = m.records.find(g.record);
        if (r == m.records.end()) { g.reason = "Awaiting current PSX record"; continue; }
        const auto current = button_extract(a, r->second.effective);
        if (!current) { g.reason = "Current PSX baseline unavailable"; continue; }
        if (*current == g.desired) {g.pending=false;continue;}
        if (a.circuit_breaker && last_event_.count(g.record) && ms - last_event_[g.record] < 50) continue;
        auto line = button_compose(a, r->second.effective, g.desired);
        if (!line) { g.reason = "PSX position unavailable or locked out"; continue; }
        // Compose all currently asserted owned fields of this record in the same line.
        for (const auto index : record_groups_[g.record]) {
            const auto &sibling = groups_[index];
            if (!sibling.enforcing || !sibling.intent || sibling.inhibited || sibling.capture_muted || sibling.awaiting_report) continue;
            auto merged = button_compose(*sibling.intent, line->substr(line->find('=') + 1), sibling.desired);
            if (merged) line = std::move(merged);
        }
        r->second.effective = line->substr(line->find('=') + 1);
        if (a.circuit_breaker) last_event_[g.record] = ms;
        g.reason.clear(); g.pending=false;g.delivery = DeliveryState::queued;
        cached_view_.reset();
        Command command{button_command_id(a.target.record), m.epoch, g.qpc, qpc, *line + "\n", false, 0, g.desired};
        command.minimum_interval_ms=a.circuit_breaker?50u:0u;
        return command;
    }
    return {};
}
std::vector<RecordKey> ButtonRouter::interests() const {
    std::vector<RecordKey> result;
    // Catalogue availability must also work before a button has been assigned.
    std::set<std::string> unique;
    for (const auto &a : button_catalogue()) {
        if (unique.insert(record_name(a.target.record)).second) result.push_back(a.target.record);
        for (const auto &p : a.applicability) if (unique.insert(record_name(p.first)).second) result.push_back(p.first);
    }
    for (const auto &e : entries_) if (unique.insert(record_name(e.action->target.record)).second) result.push_back(e.action->target.record);
    return result;
}
std::vector<std::pair<TargetDescriptor,int>> ButtonRouter::trim_intents() const {
    std::vector<std::pair<TargetDescriptor,int>> result;
    for(const auto& g:groups_)if(!g.entries.empty()){
        if(g.inhibited || g.capture_muted || g.awaiting_report)continue;
        const auto& first=entries_[g.entries.front()];const auto key=first.action->target.record;
        if(key.kind!=RecordKind::qh || (key.index!=396 && key.index!=398 && key.index!=399))continue;
        const auto held=held_owner_.find(g.id);const Entry* active=held==held_owner_.end()?nullptr:&entries_[held->second];
        if(active && active->known && active->down)result.emplace_back(active->action->target,active->action->value);
        else if(g.enforcing && g.intent)result.emplace_back(g.intent->target,g.desired);
        else result.emplace_back(first.action->target,first.action->release);
    }
    return result;
}
std::vector<ButtonReadout> ButtonRouter::snapshot(const ButtonConfiguration &c, const Mapper &m) const {
    std::vector<ButtonReadout> out;
    out.reserve(button_catalogue().size());
    std::unordered_map<const ButtonAction *,const Entry *> assigned_entries;
    for(const auto &entry:entries_)assigned_entries.emplace(entry.action,&entry);
    for (const auto &a : button_catalogue()) {
        ButtonReadout r; r.id = a.id; r.reason = button_availability(a, m);
        r.installed = r.reason != "Not installed in this aircraft" && (!a.circuit_breaker || r.reason != "Awaiting PSX panel");
        const auto row = c.rows.find(a.id);
        r.assigned = row != c.rows.end() && assigned(row->second.applied);
        if (r.assigned) {
            const auto found = assigned_entries.find(&a);
            if (found != assigned_entries.end()) {
                const auto &e=*found->second;
                r.available = e.device >= 0 && e.control >= 0 && e.known;
                r.pressed = e.down;
                const auto &g = groups_[e.group];
                r.delivery = g.delivery;
                if (r.reason.empty()) r.reason = g.reason;
                if (!r.available) r.reason = "Source disconnected, ambiguous, or awaiting baseline";
            } else r.reason = "Existing profile assignment";
        }
        if (row != c.rows.end() && !row->second.pending.empty()) r.reason = row->second.pending;
        if(r.assigned && m.runaway_trim && a.target.record.kind==RecordKind::qh && (a.target.record.index==398 || a.target.record.index==399 || (a.target.record.index==396 && (a.target.owned_mask&m.trim_owned_mask))))r.reason="IOS trim runaway: physical output suppressed";
        if(override_actions_.count(a.id))r.reason="Blocked by input override";
        out.push_back(std::move(r));
    }
    return out;
}
} // namespace hwio
