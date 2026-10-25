#pragma once
#include <windows.h>

namespace psx::windows {
inline constexpr WORD application_icon_id = 101;

// ASS windows belong to the graphics DLL. Icons belong to this executable,
// so GWLP_HINSTANCE would select the wrong module. Each handle is owned here;
// LR_SHARED caches by resource name and cannot select two independent sizes.
class WindowIcons final {
  HWND window_{};
  HICON small_{}, large_{}, previous_small_{}, previous_large_{};

public:
  explicit WindowIcons(HWND window) noexcept : window_(window) {
    const UINT observed_dpi = GetDpiForWindow(window);
    const UINT dpi = observed_dpi ? observed_dpi : 96;
    const auto module = GetModuleHandleW(nullptr);
    small_ = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(application_icon_id), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
    large_ = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(application_icon_id), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
    if (small_)
      previous_small_ = reinterpret_cast<HICON>(SendMessageW(window_, WM_SETICON, ICON_SMALL,
          reinterpret_cast<LPARAM>(small_)));
    if (large_)
      previous_large_ = reinterpret_cast<HICON>(SendMessageW(window_, WM_SETICON, ICON_BIG,
          reinterpret_cast<LPARAM>(large_)));
  }
  WindowIcons(const WindowIcons&) = delete;
  WindowIcons& operator=(const WindowIcons&) = delete;
  ~WindowIcons() {
    // Restore a still-live window before releasing its handles; also safe when
    // the owning UI has already closed the window. No class-wide mutation.
    if (IsWindow(window_)) {
      if (small_) SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(previous_small_));
      if (large_) SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(previous_large_));
    }
    if (small_) DestroyIcon(small_);
    if (large_) DestroyIcon(large_);
  }
  [[nodiscard]] bool valid() const noexcept { return small_ && large_; }
};
}
