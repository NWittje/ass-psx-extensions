#pragma once
#include "axis_setup.hpp"
#include <memory>
#include <unordered_map>
#include <functional>

namespace hwio {
constexpr std::size_t max_button_assignments = 4096, max_button_events = 1024;
enum class ButtonOperation { position, pulse, held, step };
enum class ButtonEncoding { numeric, character };
struct ButtonAction {
    unsigned source_line{};
    std::string id, group, name, position, reference;
    int chapter{}, order{}, value{}, release{};
    ButtonOperation operation{ButtonOperation::position};
    ButtonEncoding encoding{ButtonEncoding::numeric};
    TargetDescriptor target;
    bool primary{}, toggleable{}, circuit_breaker{};
    // Character records have fixed length; lowercase is the asserted/IN state.
    std::size_t character{}, length{};
    // Packed radio courses preserve every byte outside their decimal slice.
    unsigned payload_minimum{}, payload_maximum{}, guard_character{}, guard_value{};
    int wrap{}, unset{-1};
    // An empty predicate list means the operation is common to all PSX variants.
    std::vector<std::pair<RecordKey, std::vector<int>>> applicability;
};
const std::vector<ButtonAction> &button_catalogue();
const ButtonAction *button_action(std::string_view id);
std::string button_owner(const ButtonAction&);
std::string button_chapter(int number);
std::optional<int> button_extract(const ButtonAction &, std::string_view payload);
std::optional<std::string> button_compose(const ButtonAction &, std::string_view payload, int value);
bool button_overlap(const ButtonAction &, const TargetDescriptor &);
std::string button_availability(const ButtonAction &, const Mapper &);
std::uint64_t button_command_id(RecordKey) noexcept;
std::uint64_t button_control_command_id(std::string_view) noexcept;
struct ButtonRow {
    Profile applied, draft; // Identity plus one button source; never submitted as a profile mapping.
    bool managed{};
    std::string pending;
    std::vector<std::pair<std::string, std::uint64_t>> replaces;
    // Compiled module fan-in only. These sources are never serialized or edited
    // in the generic assignment page; the external module profile owns them.
    std::vector<Profile> module_sources;
    std::vector<Profile> module_contacts;
    std::vector<unsigned> module_states;
};
struct ButtonConfiguration {
    std::map<std::string, ButtonRow> rows;
    std::set<std::string> toggles;
    std::map<std::string,bool> toggle_drafts;
};
Profile button_source(const DeviceInfo &, const ControlInfo &);
bool button_source_valid(const Profile &) noexcept;
bool button_configuration_valid(const ButtonConfiguration &, std::string &error);
std::string button_configuration_encode(const ButtonConfiguration &);
bool button_configuration_decode(std::string_view, ButtonConfiguration &, std::string &error);
void button_toggle(ButtonConfiguration &, const std::string &group, bool enabled);
bool button_replaces(const ButtonConfiguration &, const std::string &profile, std::uint64_t binding);
void button_import_legacy(ButtonConfiguration &, const std::vector<Profile> &);
struct LastButtonInput {
    bool present{}, available{};
    Sample sample;
    std::string device, button;
};
class ButtonMovement {
    struct State { bool known{}, down{}; std::uint64_t generation{}; };
    std::array<std::array<State, max_controls>, max_devices> states_{};
    LastButtonInput last_;
  public:
    void input(const DeviceInfo &, const Sample &);
    LastButtonInput snapshot(const std::vector<DeviceInfo> &) const;
};
struct ButtonReadout {
    std::string id, reason;
    bool assigned{}, available{}, pressed{}, installed{true};
    DeliveryState delivery{DeliveryState::unavailable};
};
class ButtonRouter {
    friend class SituationControls;
    struct Entry {
        struct Contact {
            int device{-1},control{-1};std::uint64_t generation{};
            bool known{},down{};
        };
        const ButtonAction *action{};
        int device{-1}, control{-1};
        std::uint64_t generation{};
        bool toggle{}, known{}, down{},coherent{true};
        std::size_t group{};
        std::vector<Contact> contacts;
        std::vector<unsigned> states;
    };
    struct Group {
        std::string id, record;
        std::vector<std::size_t> entries;
        std::vector<const ButtonAction*> positions;
        const ButtonAction* down_only_gear_up{};
        bool dirty{true}, enforcing{}, pending{};
        int desired{};
        std::int64_t qpc{};
        const ButtonAction *intent{};
        // Last dequeued Qh418 switch value; not a received PSX baseline.
        std::optional<int> trim_value;
        std::string reason;
        DeliveryState delivery{DeliveryState::unavailable};
        bool inhibited{},capture_muted{},awaiting_report{};
        bool module_arm{},server_selected{};
    };
    struct Event { const ButtonAction *action{}; int value{}; std::int64_t qpc{}; std::size_t group{}; bool cleanup{}; };
    std::vector<Entry> entries_;
    std::vector<Group> groups_;
    std::unordered_map<std::string, std::vector<std::size_t>> record_groups_;
    std::unique_ptr<std::array<std::vector<std::size_t>, max_devices * max_controls>> routes_;
    std::deque<Event> events_;
    std::map<std::string, double> last_event_;
    std::map<std::string, std::size_t> held_owner_;
    ButtonConfiguration configuration_;
    std::set<std::string> retired_;
    std::vector<RecordKey> cancellations_;
    std::vector<std::uint64_t> override_cancellations_;
    struct OverrideRelease {ButtonAction action;std::size_t group{};std::int64_t after{};};
    std::map<std::string,OverrideRelease> override_releases_;
    std::set<std::string> watched_;
    mutable std::shared_ptr<const std::vector<ButtonReadout>> cached_view_;
    std::uint64_t epoch_{};
    bool batch_{}, ready_seen_{};
    std::set<std::string> override_actions_;
    std::set<std::string> capture_muted_actions_;
    std::function<bool(const std::vector<std::string>&)> capture_;
    void enqueue(const ButtonAction &, int, std::int64_t, std::size_t);
    void positions(const SourceBank &, Mapper &);
  public:
    void capture_handler(std::function<bool(const std::vector<std::string>&)> handler){capture_=std::move(handler);}
    void overrides(const std::set<std::string>&,Mapper&);
    bool overflow{};
    ButtonRouter();
    void configure(const ButtonConfiguration &, const std::vector<DeviceInfo> &, Mapper &);
    std::vector<RecordKey> changed_records(const ButtonConfiguration &) const;
    void retire(const std::vector<RecordKey> &, Mapper &);
    void baseline(const SourceBank &, Mapper &);
    void boundary(std::uint64_t, bool same_connection_load = false);
    void input(const Sample &, const SourceBank &, Mapper &, bool edge);
    void end_report() noexcept { batch_ = false; }
    void received(std::string_view record);
    void sent(std::string_view line,std::uint64_t binding=0,std::int64_t qpc=0);
    void release_all(Mapper &);
    std::vector<RecordKey> take_cancellations() { auto result=std::move(cancellations_);cancellations_.clear();return result; }
    std::vector<std::uint64_t> take_override_cancellations(){auto result=std::move(override_cancellations_);override_cancellations_.clear();return result;}
    std::optional<Command> next(const SourceBank &, Mapper &, double ms, std::int64_t qpc);
    std::vector<ButtonReadout> snapshot(const ButtonConfiguration &, const Mapper &) const;
    std::shared_ptr<const std::vector<ButtonReadout>> shared_snapshot(const ButtonConfiguration &c,const Mapper &m) const {
        if(!cached_view_)cached_view_=std::make_shared<const std::vector<ButtonReadout>>(snapshot(c,m));
        return cached_view_;
    }
    std::vector<RecordKey> interests() const;
    bool pending() const noexcept { return !events_.empty(); }
    std::vector<std::pair<TargetDescriptor,int>> trim_intents() const;
    void failure_state_changed() {cached_view_.reset();}
};
} // namespace hwio
