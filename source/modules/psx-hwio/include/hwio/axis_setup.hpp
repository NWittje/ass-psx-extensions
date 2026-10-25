#pragma once
#include "flight_display.hpp"

namespace hwio {
// Fixed slots retain the original dashboard numbering. Reversers belong to their engine.
struct AxisSetup {
    Profile source, reverse, inactive_device;
    std::array<int, 7> points{};
    std::array<int, 3> reverse_points{};
    unsigned captured{}, reverse_captured{};
    double deadzone{2}, reverse_deadzone{2};
    double control_stiffness{};
    bool custom{}, reverse_custom{}, detents{};
};
struct AxisRow {
    bool managed{}, active{};
    DisplayReference replaces;
    AxisSetup applied, draft;
    std::string pending;
};
struct AxisConfiguration {
    std::array<AxisRow, flight_slots> rows;
    bool default_fo{}, override_assigned{};
    int throttle_choice{-1}; // -1 preserves per-engine layouts; 0 combined, 1 separate.
    Profile override_source;
};
struct LastAxisMovement {
    bool present{}, available{}, reversed{};
    Sample sample;
    std::string device, axis;
};
class AxisMovement {
    struct Anchor {
        bool known{};
        int value{};
        std::uint64_t generation{};
    };
    std::array<std::array<Anchor, max_controls>, max_devices> anchors_{};
    LastAxisMovement last_;

  public:
    void input(const DeviceInfo &device, const Sample &sample);
    LastAxisMovement snapshot(const std::vector<DeviceInfo> &devices) const;
};
struct AxisRowReadout {
    bool available{}, reverse_available{}, active{}, selected{}, descriptor_defaults{};
    int raw{}, reverse_raw{};
    std::string status;
};
struct AxisStatus {
    std::array<AxisRowReadout, flight_slots> rows;
    bool default_fo{}, effective_fo{}, override_available{}, override_held{};
};
int axis_partner(std::size_t slot) noexcept;
bool axis_centered(std::size_t slot, ThrottleLayout layout) noexcept;
TargetDescriptor axis_target(std::size_t slot);
AxisSetup axis_from_binding(const Profile &profile, const Binding &binding, const std::vector<Profile> &profiles);
void axis_source(AxisSetup &setup, std::size_t slot, const DeviceInfo &device, const ControlInfo &control,
                 bool reversed, bool reverse);
bool axis_calibrate(AxisSetup &setup, std::size_t slot, std::string &error);
bool axis_apply(AxisRow &row, std::size_t slot);
void axis_layout(AxisSetup &setup, bool separate);
std::string axis_configuration_encode(const AxisConfiguration &configuration);
bool axis_configuration_decode(std::string_view text, AxisConfiguration &configuration, std::string &error);
bool save_axis_configuration(const std::wstring &path, const AxisConfiguration &configuration, std::string &error);
bool load_axis_configuration(const std::wstring &path, AxisConfiguration &configuration, std::string &error);
bool axis_replaces(const AxisConfiguration &configuration, const std::string &profile, std::uint64_t binding);

// Bounded read-only candidate evaluators, followed by one emitter per PSX target.
class AxisRouter {
    friend class SituationControls;
    struct Candidate {
        Mapped mapped;
        int device{-1}, control{-1}, reverse_device{-1}, reverse_control{-1};
        std::uint64_t generation{}, reverse_generation{};
        std::string signature;
        int flap{-1};
    };
    AxisConfiguration configuration_;
    std::array<Candidate, flight_slots> candidates_;
    std::array<int, flight_slots> draft_devices_{}, draft_controls_{}, draft_reverse_devices_{},
        draft_reverse_controls_{};
    Mapper evaluator_;
    std::vector<SourceValue> sources_;
    AxisStatus status_;
    int override_device_{-1}, override_control_{-1};
    bool changed_{};

  public:
    AxisRouter();
    void configure(const AxisConfiguration &configuration, const std::vector<DeviceInfo> &devices, Mapper &output);
    void update(const SourceBank &values, Mapper &output, double ms);
    void display(FlightSnapshot &snapshot, const Mapper &output) const;
    AxisStatus status() const {
        return status_;
    }
    bool authority_changed() noexcept {
        const bool result = changed_;
        changed_ = false;
        return result;
    }
    void filter_interval(int milliseconds) noexcept;
    bool settling() const noexcept;
};
} // namespace hwio
