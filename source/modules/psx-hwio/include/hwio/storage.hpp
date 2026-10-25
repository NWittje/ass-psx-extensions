#pragma once
#include "button_setup.hpp"
#include "hardware_config.hpp"
#include "cockpit.hpp"
#include "fds_modules.hpp"
#include "fdstq.hpp"
#include "trim_displays.hpp"
#include "cpflight.hpp"

namespace hwio {
struct InputOverrideConfiguration;
bool save_input_overrides(const std::wstring&,const InputOverrideConfiguration&,std::string&);
enum class LogLevel { off, info, warn };
struct AppSettings {
    LogLevel log_level{LogLevel::info};
    int smoothing_ms{25};
    std::wstring network_interface;
    bool network_selection_initialized{};
    std::wstring fds_api_path,phidget_api_path;
    bool pokeys_enabled{true},fds_enabled{},phidgets_enabled{},provider_selection_initialized{};
    FdstqConfiguration fdstq;
    TrimDisplayConfiguration trim_displays;
    CpflightConfiguration cpflight;
    bool flightillusion_enabled{};
};
AppSettings load_settings(const std::wstring &directory);
bool save_settings(const std::wstring &directory, const AppSettings &settings, std::string &error);
std::wstring executable_directory();
bool load_assignments(const std::wstring &file, std::vector<Profile> &profiles, DisplayAssignments &display,
                      AxisConfiguration &axes, std::string &error, ButtonConfiguration *buttons = nullptr,
                      HardwareConfiguration *hardware = nullptr, CockpitConfiguration *cockpit = nullptr,ModuleConfiguration* modules=nullptr);
bool save_assignments(const std::wstring &file, const std::vector<Profile> &profiles, const DisplayAssignments &display,
                      const AxisConfiguration &axes, std::string &error, const ButtonConfiguration *buttons = nullptr,
                      const HardwareConfiguration *hardware = nullptr, const CockpitConfiguration *cockpit = nullptr,const ModuleConfiguration* modules=nullptr);
// Import old files once, only when the corresponding new cfg is absent. Never remove the originals.
bool prepare_configuration(const std::wstring &directory, const std::wstring &legacy, std::string &error);
bool save_endpoint(const std::wstring &directory, const std::string &host, std::uint16_t port, std::string &error);
bool save_button_navigation(const std::wstring &directory, int chapter, bool breakers, std::string &error, int view = -1);
} // namespace hwio
