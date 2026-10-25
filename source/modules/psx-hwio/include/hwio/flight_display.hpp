#pragma once
#include "hid.hpp"

namespace hwio {
enum class FlightSlot : unsigned {
    cpt_roll,
    cpt_pitch,
    cpt_rudder,
    cpt_tiller,
    fo_roll,
    fo_pitch,
    fo_rudder,
    fo_tiller,
    spoiler,
    engine1,
    engine2,
    engine3,
    engine4,
    flaps,
    cpt_left_brake,
    cpt_right_brake,
    fo_left_brake,
    fo_right_brake,
    count
};
constexpr std::size_t flight_slots = static_cast<std::size_t>(FlightSlot::count);
enum class DisplayChoice : unsigned { automatic, none, binding };
struct DisplayReference {
    DisplayChoice choice{DisplayChoice::automatic};
    std::string profile;
    std::uint64_t binding{};
};
using DisplayAssignments = std::array<DisplayReference, flight_slots>;
struct FlightReadout {
    FlightSlot slot{FlightSlot::count};
    bool assigned{}, available{}, shared{}, display_only{}, split{}, reverse_blocked{};
    int value{}, raw{}, reverse_raw{};
    double normalized{}, forward{}, reverse{};
    bool transmitted_available{}, deadzone_available{};
    double physical{}, transmitted{}, deadzone_low{}, deadzone_high{};
    DeliveryState delivery{DeliveryState::unavailable};
    std::string source, reason;
};
using FlightSnapshot = std::array<FlightReadout, flight_slots>;
using SourceBank = std::array<std::array<SourceValue, max_controls>, max_devices>;
const wchar_t *flight_slot_name(std::size_t slot) noexcept;
bool flight_binding_matches(std::size_t slot, const Binding &binding) noexcept;
bool load_display_assignments(const std::wstring &file, DisplayAssignments &assignments, std::string &error);
bool save_display_assignments(const std::wstring &file, const DisplayAssignments &assignments, std::string &error);
void flight_filter(Binding &binding, int milliseconds) noexcept;
void flight_physical(FlightReadout &, const Binding &, int raw) noexcept;
void flight_transmitted(FlightReadout &, std::size_t slot, const Mapper &);
class FlightDisplay {
    struct Entry {
        FlightReadout reading;
        DisplayReference reference;
        int preview{-1};
    };
    std::array<Entry, flight_slots> entries_;
    std::array<std::size_t, flight_slots> mirrors_{};
    // This mapper is an evaluator only: next(), decision transport and command
    // submission are never called. Its readiness stays false.
    Mapper preview_;
    std::vector<SourceValue> sources_;

  public:
    void configure(const std::vector<Profile> &profiles, const std::vector<DeviceInfo> &devices,
                   const DisplayAssignments &assignments);
    void update(const Mapper &live, const SourceBank &values, double ms);
    FlightSnapshot snapshot() const;
    void filter_interval(int milliseconds) noexcept;
    bool settling() const noexcept;
};
} // namespace hwio
