#pragma once
#include "config.hpp"
#include "diagnostics.hpp"
#include "runtime.hpp"
#include "frame_rates.hpp"
#include <application-icon.hpp>
#include <map>
namespace du_client
{
class Ui
{
    ass::graphics::Context context_;
    ass::graphics::Window window_;
    psx::windows::WindowIcons icons_;
    std::array<HWND, 3> input_windows_{};
    void attach_input(HWND, unsigned);
    void detach_input(unsigned) noexcept;
    std::unique_ptr<ass::graphics::Canvas> backdrop_;
    std::array<std::unique_ptr<ass::graphics::Canvas>, 4> edges_;
    std::unique_ptr<ass::graphics::Display> display_;
    std::unique_ptr<ass::graphics::Display> trim_display_;
    bool trim_visible_{}, trim_selected_{}, trim_budget_reserved_{};
    size_t main_replay_cpu_{},trim_replay_cpu_{};
    void submit_replay(ass::graphics::Display&,const psx::du::Frame*,size_t&);
    uint32_t trim_source_width_{1}, trim_source_height_{1};
    uint64_t trim_shown_epoch_{}, trim_shown_sequence_{};
    ag_rect trim_bounds_{};
    ass::graphics::Control warning_;
    SettingsStore store_;
    Config config_;
    std::unique_ptr<Diagnostics> diagnostics_;
    uint64_t qualification_deadline_{};
    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<ass::graphics::Dialog> dialog_;
    std::vector<ass::graphics::Control> labels_;
    std::map<unsigned, ass::graphics::Control> controls_;
    std::vector<std::wstring> adapter_ids_;
    bool saving_profile_{}, primary_{}, ready_{}, editing_{}, exiting_{}, hidden_{}, warp_{};
    bool start_input_{}, profile_invalid_{}, diagnostics_enabled_{};
    unsigned profile_{};
    uint64_t shown_epoch_{}, shown_sequence_{};
    FrameRates rates_;
    uint64_t drawn_frames_{}, draw_baseline_{};
    bool draw_pending_{}, reset_alignment_{}, reset_trim_{}, status_connected_{};
    std::wstring status_text_;
    std::wstring error_, replay_error_, snapshot_error_, warning_text_, trim_error_;
    ag_rect bounds_{};
    static void __cdecl callback(void *, uint32_t, uint32_t, double) noexcept;
    static void __cdecl context_request(void *, const ag_context_request *) noexcept;
    static LRESULT CALLBACK surface_input(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void update_trim(const View &, const TrimView &);
    void main_texture_budget(uint64_t);
    void trim_option_visibility();
    void event(unsigned, unsigned);
    void layout();
    bool present();
    void tick();
    void save();
    void restore(Config &);
    void options();
    void save_profile_dialog();
    void dismiss();
    void apply(bool);
    void error(std::wstring);
    void source_controls();
    void reset_fullscreen_position();
    ass::graphics::Control control(void *, unsigned, unsigned, const wchar_t *, ag_rect, float lo = 0,
                                   float hi = 0, float step = 1);

  public:
    Ui(std::wstring profile = L"1", std::filesystem::path root = {}, bool hidden = false, bool warp = false,
       bool start_input = true, std::filesystem::path diagnostics_output = {});
    ~Ui();
    int run();
    void qualify(const std::filesystem::path &p, unsigned seconds)
    {
        diagnostics_ = std::make_unique<Diagnostics>(p);
        runtime_->observe(diagnostics_->trace());
        qualification_deadline_ = qpc() + seconds * frequency();
    }
    void service()
    {
        tick();
    }
    void open_options()
    {
        options();
    }
    void open_save_profile()
    {
        if (!dialog_)
            options();
        save_profile_dialog();
    }
    void apply_options(bool close = false)
    {
        apply(close);
    }
    void cancel_options()
    {
        dismiss();
    }
    auto &window()
    {
        return window_;
    }
    auto &config() const
    {
        return config_;
    }
    auto &input(unsigned id)
    {
        return controls_.at(id);
    }
    bool primary() const
    {
        return primary_;
    }
    void reset_alignment()
    {
        reset_fullscreen_position();
    }
    const std::wstring &status_text() const { return status_text_; }
    double received_fps() const { return rates_.received; }
    double drawn_fps() const { return rates_.drawn; }
    bool editing() const
    {
        return editing_;
    }
    ag_rect display_bounds() const
    {
        return bounds_;
    }
    ag_handle display_handle() const
    {
        return display_->handle();
    }
    ag_handle trim_display_handle() const { return trim_display_ ? trim_display_->handle() : nullptr; }
    ag_rect trim_bounds() const { return trim_bounds_; }
    bool trim_visible() const { return trim_visible_; }
    bool trim_selected() const { return trim_selected_; }
    Runtime *runtime()
    {
        return runtime_.get();
    }
    void export_image(const std::filesystem::path &p)
    {
        display_->export_png(p.c_str());
    }
};
} // namespace du_client
