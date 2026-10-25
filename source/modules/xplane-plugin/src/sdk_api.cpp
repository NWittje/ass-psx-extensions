#include "psx_plugin/sdk_api.hpp"
#include <Windows.h>

namespace psx::xp {
void SdkApi::attach_thread() noexcept {
    // Called once by XPluginStart, never at DLL initialization.
    unsigned long expected = 0;
    owner_.compare_exchange_strong(expected, GetCurrentThreadId());
}
bool SdkApi::on_thread() const noexcept { return owner_.load() == GetCurrentThreadId(); }
Status SdkApi::ready() const noexcept {
    if (!on_thread()) return Status::wrong_thread;
    return active_ ? Status::ok : Status::inactive;
}
void SdkApi::activate() noexcept { if (on_thread()) { ++generation_; active_ = true; } }
void SdkApi::deactivate() noexcept { if (on_thread()) { active_ = false; ++generation_; } }
void SdkApi::invalidate() noexcept { if (on_thread()) ++generation_; }
Status SdkApi::inspect(XPLMDataRef ref, XPLMDataTypeID type, bool writing) const {
    if (auto s = ready(); s != Status::ok) return s;
    if (!ref) return Status::not_found;
    // Intentionally strict for explicit diagnostic operations, never a per-frame scan.
    // Good handles can become orphaned. SDK default zero is not a validity signal.
    if (!XPLMIsDataRefGood(ref)) return Status::unavailable;
    XPLMDataRefInfo_t info{};
    info.structSize = sizeof(info);
    info.owner = XPLM_NO_PLUGIN_ID;
    XPLMGetDataRefInfo(ref, &info);
    if (info.owner == XPLM_NO_PLUGIN_ID ||
        (info.owner != XPLM_PLUGIN_XPLANE && !XPLMIsPluginEnabled(info.owner)))
        return Status::unavailable;
    if ((XPLMGetDataRefTypes(ref) & type) != type) return Status::type_mismatch;
    if (writing && !XPLMCanWriteDataRef(ref)) return Status::read_only;
    return Status::ok;
}
} // namespace psx::xp
