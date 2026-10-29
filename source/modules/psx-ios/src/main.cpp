#include <algorithm>
#include <psx/windows/executable_path.hpp>
#include <ass_graphics/graphics.hpp>
#include <ios/web.hpp>
#include <shellapi.h>
#include <application-icon.hpp>
namespace {
namespace gfx = ass::graphics;
std::filesystem::path directory() { return psx::windows::executable_path().parent_path(); }
class Ui {
  gfx::Context context_;
  gfx::Window window_;
  psx::windows::WindowIcons icons_;
  std::unique_ptr<gfx::Tray> tray_;
  std::unique_ptr<ios::Runtime> runtime_;
  std::unique_ptr<ios::Web> web_;
  std::vector<gfx::Control> labels_, buttons_;
  gfx::Control psx_, main_port_, web_port_, adapter_, status_;
  std::vector<std::string> guids_;
  std::uint64_t inventory_revision_{};
  bool primary_{};
  std::wstring notice_;
  static void __cdecl callback(void *p, std::uint32_t id, std::uint32_t event,
                               double) noexcept {
    auto self = static_cast<Ui *>(p);
    try {
      self->event(id, event);
    } catch (const gfx::Error &e) {
      self->notice(e.detail);
    } catch (const std::exception &e) {
      self->notice(ios::wide(e.what()));
    }
  }
  gfx::Control make(unsigned kind, unsigned id, const wchar_t *text,
                    ag_rect bounds) {
    ag_control_desc d{};
    d.kind = kind;
    d.id = id;
    d.text = text;
    d.bounds = bounds;
    return gfx::Control(context_, window_.hwnd(), d, callback, this);
  }
  void notice(std::wstring s) {
    notice_ = std::move(s);
    if (status_.handle())
      status_.status(notice_.c_str(), 1);
  }
  void adapters() {
    auto inv = runtime_->inventory();
    if (!inv || inv->revision == inventory_revision_)
      return;
    if (adapter_.activity() & AG_ACTIVITY_DROPDOWN)
      return;
    std::string selected = runtime_->config().adapter;
    auto old = int(adapter_.value());
    if (old >= 0 && std::size_t(old) < guids_.size())
      selected = guids_[std::size_t(old)];
    inventory_revision_ = inv->revision;
    guids_ = {""};
    std::vector<std::wstring> names{
        L"Automatic — Windows IP route"};
    for (const auto &a : inv->adapters) {
      guids_.push_back(ios::narrow(a.guid));
      names.push_back(a.name + (a.up ? L"" : L" (disconnected)"));
    }
    if (!selected.empty() &&
        std::find(guids_.begin(), guids_.end(), selected) == guids_.end()) {
      guids_.push_back(selected);
      names.push_back(L"Saved adapter unavailable");
    }
    std::vector<const wchar_t *> ptrs;
    for (const auto &n : names)
      ptrs.push_back(n.c_str());
    auto it = std::find(guids_.begin(), guids_.end(), selected);
    adapter_.replace(ptrs, it == guids_.end() ? 0 : int(it - guids_.begin()));
  }
  static std::uint16_t port(const gfx::Control &control) {
    auto o = ios::json("{\"port\":" + ios::narrow(control.text()) + "}");
    auto n = ios::field<std::uint64_t>(o, "port");
    if (n < 1 || n > 65535)
      throw std::invalid_argument("Port must be 1..65535");
    return std::uint16_t(n);
  }
  void event(unsigned id, unsigned event) {
    if (event == AG_TIMER) {
      adapters();
      auto b = runtime_->web_binding();
      auto web = runtime_->web_status();
      auto s = runtime_->status();
      std::string text =
          "PSX IOS " IOS_PRODUCT_VERSION "\r\nLocal URL: http://127.0.0.1:" + std::to_string(b.port) +
          "\r\nActive listeners: " + (web.listeners.empty() ? "None" : web.listeners) +
          "\r\n" + web.notice;
      text += s.find("\"ready\":true") != std::string::npos
                  ? "\r\nPSX Main connections synchronized"
                  : "\r\nPSX Main unavailable / synchronizing";
      if (!notice_.empty())
        text += "\r\n" + ios::narrow(notice_);
      status_.status(ios::wide(text).c_str(), 0);
      return;
    }
    if (event != AG_CLICK)
      return;
    if (id == 1) {
      auto url =
          L"http://127.0.0.1:" + std::to_wstring(runtime_->config().web_port);
      ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
    } else if (id == 5)
      window_.close();
    else if (id == 6) {
      ios::Config c = runtime_->config();
      c.psx = ios::narrow(psx_.text());
      c.main_port = port(main_port_);
      c.web_port = port(web_port_);
      auto n = int(adapter_.value());
      if (n < 0 || std::size_t(n) >= guids_.size())
        throw std::invalid_argument("Select an adapter");
      c.adapter = guids_[std::size_t(n)];
      runtime_->configure(c);
      notice(L"Settings queued for persistence.");
    }
  }

public:
  Ui()
      : window_(context_, ios::wide("PSX Instructor Station " IOS_PRODUCT_VERSION).c_str(), 760, 550, callback,
                this), icons_(static_cast<HWND>(window_.hwnd())) {
    if (ag_abi_version() != 1)
      throw std::runtime_error("ASS Graphics ABI mismatch");
    primary_ = window_.single_instance(L"PSX-IOS.WebFoundation.v1");
    if (!primary_)
      return;
    runtime_ = std::make_unique<ios::Runtime>(directory());
    web_ = std::make_unique<ios::Web>(*runtime_);
    tray_ = std::make_unique<gfx::Tray>(window_, L"PSX Instructor Station");
    const wchar_t *captions[]{L"Open IOS", L"Exit"};
    const unsigned ids[]{1, 5};
    const float left[]{16, 572};
    for (unsigned n = 0; n < 2; ++n)
      buttons_.push_back(
          make(AG_BUTTON, ids[n], captions[n],
               {left[n], 16, left[n] + 128, 52}));
    const wchar_t *fields[]{L"PSX Main IP address", L"Main port", L"Web port",
                            L"PSX adapter (outgoing)"};
    std::array<gfx::Control *, 4> editors{&psx_, &main_port_, &web_port_,
                                          &adapter_};
    for (unsigned n = 0; n < 4; ++n) {
      labels_.push_back(
          make(AG_LABEL, 0, fields[n],
               {16, 80 + float(n) * 49, 215, 116 + float(n) * 49}));
      *editors[n] = make(n == 3 ? AG_CHOICE : AG_EDIT, 10 + n, L"",
                         {222, 80 + float(n) * 49, 700, 112 + float(n) * 49});
    }
    buttons_.push_back(
        make(AG_BUTTON, 6, L"Apply settings", {222, 275, 390, 311}));
    status_ = make(AG_STATUS, 0, L"", {16, 332, 700, 475});
    auto c = runtime_->config();
    psx_.text(ios::wide(c.psx).c_str());
    main_port_.text(std::to_wstring(c.main_port).c_str());
    web_port_.text(std::to_wstring(c.web_port).c_str());
    adapters();
    window_.timer(1, 500);
    window_.fit();
    // The tray owns initial visibility, as in HWIO; restore is user-operated.
    window_.show(false);
  }
  int run() { return primary_ ? window_.run() : 0; }
  ~Ui() {
    web_.reset();
    runtime_.reset();
  }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  try {
    Ui ui;
    return ui.run();
  } catch (const ass::graphics::Error &e) {
    MessageBoxW(nullptr, e.detail.c_str(), L"IOS startup failed",
                MB_OK | MB_ICONERROR);
  } catch (const std::exception &e) {
    MessageBoxW(nullptr, ios::wide(e.what()).c_str(), L"IOS startup failed",
                MB_OK | MB_ICONERROR);
  }
  return 1;
}
