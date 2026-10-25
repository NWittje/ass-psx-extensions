#pragma once
#include "runtime.hpp"
#include "shader.hpp"
#include "rates.hpp"
#include "owned-dialog.hpp"
#include <psx/cdu/render.hpp>
#include <application-icon.hpp>
#include <stdexcept>
namespace cdu_client
{
class Ui
{
    ass::graphics::Context context_;
    ass::graphics::Window window_;
    psx::windows::WindowIcons window_icons_;
    ass::graphics::Control warning_;
    std::unique_ptr<ass::graphics::Canvas> backdrop_;
    std::array<std::unique_ptr<ass::graphics::Canvas>, 4> frame_;
    std::unique_ptr<psx::cdu::SurfaceRenderer> renderer_;
    std::unique_ptr<Runtime> runtime_;
    SettingsStore store_;
    SettingsStore slots_;
    Config effective_;
    std::string profile_;
    bool primary_{}, ready_{}, exiting_{}, warp_{}, hidden_{}, effective_effects_{};
    uint64_t pending_{};
    std::wstring shader_error_, warning_text_, options_error_;
    std::unique_ptr<ass::graphics::Dialog> dialog_;
    std::unique_ptr<psx::windows::WindowIcons> dialog_icons_;
    std::vector<ass::graphics::Control> labels_;
    std::map<unsigned, ass::graphics::Control> controls_;
    std::vector<std::wstring> adapter_ids_;
    ContentDrawRate drawn_;
    uint64_t drawing_epoch_{};
    psx::cdu::ReceiverStatus drawing_source_{};
    Config draft_;
    Config preview_;
    std::unique_ptr<ShaderAssets> shader_assets_;
    std::wstring preview_error_before_;
    bool preview_effects_before_{}, preview_active_{}, filling_shader_{};
    std::unique_ptr<OwnedDialog> shader_dialog_;
    std::unique_ptr<psx::windows::WindowIcons> shader_icons_;
    ass::graphics::Control shader_tabs_;
    std::vector<ass::graphics::Control> shader_pages_, shader_labels_;
    std::map<unsigned, ass::graphics::Control> shader_controls_;
    bool diagnostics_enabled_{};
    bool closing_ok_{};
    bool editing_{};
    ag_rect display_bounds_{};
    void remember();
    void capture(Config &);
    void restore(Config &);
    static void __cdecl callback(void *, uint32_t, uint32_t, double) noexcept;
    static void __cdecl context(void *, const ag_context_request *) noexcept;
    static void __cdecl shader_callback(void *, uint32_t, uint32_t, double) noexcept;
    void shader_event(unsigned, unsigned, double);
    void shader_options();
    void fill_shader();
    void preview_shader(const Config &);
    Candidate prepare(const Config &);
    void close_shader();
    const Config &display_config() const { return preview_active_ ? preview_ : effective_; }
    void event(unsigned, unsigned, double);
    void layout();
    void tick();
    void draw(const View &);
    void observe_draw();
    void render_display();
    void options();
    void fill();
    Config read();
    void apply(bool close, unsigned save_slot = 0);
    void dismiss(bool restore_preview = true);
    void shutdown();
    void error(const std::wstring &);
    ass::graphics::Control make(void *, unsigned, unsigned, const wchar_t *, ag_rect, float lo = 0, float hi = 0,
                                float step = 0);

  public:
    Ui(std::string profile, bool warp = false, std::filesystem::path root = {}, bool hidden = false, psx::diagnostics::Options diagnostics = {});
    ~Ui();
    int run();
    void open_options()
    {
        options();
    }
    void apply_options(bool close = false)
    {
        apply(close);
    }
    void save_profile(unsigned slot)
    {
        if (slot < 1 || slot > 10)
            throw std::runtime_error("Profile number must be 1..10");
        apply(false, slot);
    }
    void cancel_options()
    {
        dismiss();
    }
    void reset_alignment()
    {
        if (dialog_ && !pending_)
            draft_.display_x = draft_.display_y = draft_.display_width = 0;
    }
    void *options_hwnd() const
    {
        return dialog_ ? dialog_->hwnd() : nullptr;
    }
    void open_shader_options() { shader_options(); }
    void *shader_options_hwnd() const { return shader_dialog_ ? shader_dialog_->hwnd() : nullptr; }
    ass::graphics::Control &shader_control(unsigned index) { return shader_controls_.at(1000 + index); }
    const Config &preview_config() const { return display_config(); }
    uint64_t drawn_updates() const
    {
        return drawn_.count();
    }
    ag_display_metrics render_metrics() const
    {
        return renderer_->metrics();
    }
    ass::graphics::Control &control(unsigned id)
    {
        return controls_.at(id);
    }
    Runtime *runtime()
    {
        return runtime_.get();
    }
    auto &window()
    {
        return window_;
    }
    bool options_open() const
    {
        return bool(dialog_);
    }
    bool applying() const
    {
        return pending_ != 0;
    }
    bool editing() const
    {
        return editing_;
    }
    ag_rect display_bounds() const
    {
        return display_bounds_;
    }
    void service()
    {
        tick();
    }
    void export_image(const std::filesystem::path &p)
    {
        renderer_->export_png(p);
    }
    void export_frame(const std::filesystem::path &p)
    {
        frame_[0]->png(p.c_str(), 100, 2, float(GetDpiForWindow(static_cast<HWND>(window_.hwnd()))));
    }
    const Config &effective() const
    {
        return effective_;
    }
    bool primary() const
    {
        return primary_;
    };
};
} // namespace cdu_client
