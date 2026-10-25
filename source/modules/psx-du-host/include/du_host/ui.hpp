#pragma once
#include "runtime.hpp"
#include <application-icon.hpp>
namespace du_host
{
struct UiOptions
{
    std::filesystem::path settings_path;
    std::filesystem::path du_trace_output;
    std::wstring instance_id;
    psx::diagnostics::Options diagnostics{diagnostics_options()};
    bool tray_enabled{true}; // Dependency injection used by the separate background UI tests.
    bool show_errors{true};
};
class Ui
{
    ass::graphics::Context context_;
    ass::graphics::Window window_;
    psx::windows::WindowIcons icons_;
    ass::graphics::Control tabs_, status_page_, settings_page_, status_text_, notice_,
        diagnostics_, reconnect_button_, psx_edit_, psx_port_, group_edit_, port_, adapter_, apply_, cancel_;
    ass::graphics::Control du_page_,du_source_,du_refresh_,du_apply_;
    ass::graphics::Control du_tcp_port_,du_freshness_,du_tcp_label_,du_freshness_label_;
    std::array<ass::graphics::Control,6> du_enabled_;
    std::vector<psx::du::transport::Source> du_sources_;
    std::vector<ass::graphics::Control> labels_;
    std::vector<std::wstring> adapter_ids_;
    std::vector<std::wstring> adapter_labels_;
    std::unique_ptr<ass::graphics::Tray> tray_;
    SettingsStore settings_;
    Config effective_, pending_;
    std::unique_ptr<Runtime> runtime_;
    bool primary_{}, exiting_{}, ready_{}, diagnostics_enabled_{};
    std::uint64_t pending_id_{}, inventory_revision_{};
    std::wstring notice_text_, last_status_, last_tooltip_;
    unsigned label_dpi_{};
    std::array<float, 5> label_heights_{};
    int exit_code_{};
    static void __cdecl callback(void *, std::uint32_t, std::uint32_t, double) noexcept;
    void event(std::uint32_t, std::uint32_t, double);
    void build();
    void layout();
    void fill();
    void refresh_du_sources();
    void refresh_adapters(const psx::network::Inventory &);
    void notice(std::wstring);

  public:
    explicit Ui(UiOptions = {});
    ~Ui();
    int run();
    void tick();
    void apply();
    void cancel();
    void request_exit();
    void show()
    {
        if (primary_)
            window_.show();
    }
    bool primary() const noexcept
    {
        return primary_;
    }
    bool exiting() const noexcept
    {
        return exiting_;
    }
    HWND hwnd() const
    {
        return static_cast<HWND>(window_.hwnd());
    }
    Runtime *runtime() noexcept
    {
        return runtime_.get();
    }
    bool has_tray() const noexcept
    {
        return bool(tray_);
    }
    void export_image(const std::filesystem::path &path);
    void select_page(unsigned page);
    // Access native controls for independent background contract tests.
    HWND control(std::uint32_t id) const;
};
} // namespace du_host
