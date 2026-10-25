#include "psx_plugin/plugin.hpp"
#include "../../../shared/windows-runtime/single_instance.hpp"
#include <cstring>

namespace {
// Inert constant initialization: acquisition belongs to XPluginStart, not DLL load.
constinit psx::windows::SingleInstance instance;
#ifdef PSX_PLUGIN_OFFLINE_TEST
psx::xp::Plugin plugin({L"\\\\.\\pipe\\psx-ios.absent-plugin-entrypoint-test"});
constexpr auto instance_name=L"Local\\PSX-IOS.XPlanePlugin.OfflineFixture";
#else
psx::xp::Plugin plugin;
constexpr auto instance_name=L"Local\\PSX-IOS.XPlanePlugin";
#endif
template<std::size_t N> void copy_text(char* target, const char (&text)[N]) noexcept {
    static_assert(N <= 256);
    std::memcpy(target, text, N);
}
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) noexcept {
    if (!name || !signature || !description) return 0;
    // SDK provides at least 256 bytes for each output.
    copy_text(name, "PSX X-Plane Visual");
    copy_text(signature, "org.psx-ios.xplane.foundation");
    copy_text(description, "PSX visual motion with explicit Follow/Stop and native diagnostics.");
    const auto result=instance.result()==psx::windows::InstanceResult::primary
        ? psx::windows::InstanceResult::already_running
        : instance.acquire(instance_name);
    if(result!=psx::windows::InstanceResult::primary) {
        if(result==psx::windows::InstanceResult::already_running)
            copy_text(description,"PSX synchronization plugin already running in this Windows session.");
        else
            copy_text(description,"PSX synchronization plugin start failed: singleton guard unavailable.");
        return 0;
    }
    try {
        if (plugin.start()) return 1;
    } catch (...) {}
    plugin.stop();
    instance.release();
    copy_text(description, "Foundation start failed: requires X-Plane, SDK 4.0 and free diagnostic names.");
    return 0;
}
PLUGIN_API int XPluginEnable() noexcept {
    try { return plugin.enable() ? 1 : 0; }
    catch (...) { plugin.disable(); return 0; }
}
PLUGIN_API void XPluginDisable() noexcept { plugin.disable(); }
PLUGIN_API void XPluginStop() noexcept { plugin.stop(); instance.release(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID from, int message, void* parameter) noexcept {
    plugin.message(from, message, parameter);
}
