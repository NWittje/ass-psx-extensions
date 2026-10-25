#pragma once
#include <ass_graphics/display.hpp>

namespace cdu_client
{
// The public C API permits an owned window to own another dialog. Retain that
// owner's RAII state until the nested controls and native window are released.
class OwnedDialog : public ass::graphics::Object
{
  public:
    OwnedDialog(const ass::graphics::Object &owner, const wchar_t *title, unsigned width, unsigned height,
                ag_event_fn callback, void *user)
    {
        ag_handle handle{};
        ass::graphics::check(ag_dialog_create(owner.handle(), title, width, height, callback, user, &handle));
        own(handle, &owner);
        ass::graphics::check(ag_window_defer_close(handle, 1));
    }
    void show(bool visible, bool activate)
    {
        ass::graphics::check(ag_dialog_show(handle(), visible, activate));
    }
};
}
