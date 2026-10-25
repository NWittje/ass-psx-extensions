#pragma once
#include "runtime.hpp"

namespace hwio {
struct ControllerRequirement {
    Profile profile;
    std::vector<ControlKey> controls;
};
std::vector<ControllerRequirement> controller_requirements(const std::vector<Profile> &, const AxisConfiguration &,
                                                           const ButtonConfiguration &, const DisplayAssignments &,const CockpitConfiguration& = {});
struct TraySummary {
    unsigned controllers_state{3}, psx_state{2}, assigned{}, found{};
    std::wstring controllers, psx, detail;
};
// Presentation policy only. No acquisition cadence or command ownership changes.
class TrayHealth {
    bool initialized_{};
    std::uint64_t network_overflow_{}, input_overflow_{}, warning_until_{};

  public:
    TraySummary update(const std::vector<ControllerRequirement> &, const RuntimeView &, std::uint64_t now_ms);
};
} // namespace hwio
