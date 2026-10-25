#include <cdu_client/ui.hpp>
#include <cdu_client/shader-help.hpp>
#include <charconv>
#include <cmath>
#include <sddl.h>
namespace cdu_client
{
namespace gfx = ass::graphics;
namespace
{
ag_rect rect(float x, float y, float w, float h)
{
    return {x, y, x + w, y + h};
}
std::wstring identity(std::string_view profile)
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot identify client user");
    DWORD n{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> data(n);
    if (!GetTokenInformation(token, TokenUser, data.data(), n, &n))
    {
        CloseHandle(token);
        throw std::runtime_error("Cannot read client user");
    }
    CloseHandle(token);
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot format client user");
    auto id = std::wstring(L"PSX-IOS.CduClient.") + sid + L"." + widen(profile);
    LocalFree(sid);
    return id;
}
const wchar_t *state(psx::cdu::SourceState s)
{
    static const wchar_t *names[]{L"Disconnected", L"Connecting", L"Initializing", L"Connected"};
    return names[unsigned(s)];
}
constexpr unsigned HOST = 10, CDU = 11, GROUP = 12, PORT = 13, ADAPTER = 14, CONNECTION = 16, EFFECTS = 30, RESET = 32,
                   APPLY = 40, OK = 41, CANCEL = 42, NOTICE = 43, PROFILE = 50, SAVE_PROFILE = 51,
                   SHADER_OPTIONS = 52, SHADER_DONE = 60, SHADER_RESET = 61, SHADER_NOTICE = 62,
                   CLOSE_SHADER = 70, CLOSE_OPTIONS = 71, DIAGNOSTICS = 72;
Config visual_config(Config c)
{
    // Preparing an unconfigured display never starts reception. The shared
    // render validator still requires a syntactically valid host.
    if (c.host.empty())
        c.host = "127.0.0.1";
    return c;
}
} // namespace
void __cdecl Ui::callback(void *user, uint32_t id, uint32_t kind, double value) noexcept
{
    auto self = static_cast<Ui *>(user);
    try
    {
        self->event(id, kind, value);
    }
    catch (const gfx::Error &e)
    {
        self->error(e.detail);
    }
    catch (const std::exception &e)
    {
        self->error(widen(e.what()));
    }
}
void __cdecl Ui::context(void *user, const ag_context_request *) noexcept
{
    auto self = static_cast<Ui *>(user);
    try
    {
        self->options();
    }
    catch (const std::exception &e)
    {
        self->error(widen(e.what()));
    }
}
gfx::Control Ui::make(void *parent, unsigned kind, unsigned id, const wchar_t *text, ag_rect bounds, float lo, float hi,
                      float step)
{
    ag_control_desc d{};
    d.kind = kind;
    d.id = id;
    d.text = text;
    d.bounds = bounds;
    d.minimum = lo;
    d.maximum = kind == AG_CHECKBOX ? 1 : hi;
    d.step = step;
    d.value = lo;
    return gfx::Control(context_, parent, d, callback, this);
}
Ui::Ui(std::string profile, bool warp, std::filesystem::path root, bool hidden, psx::diagnostics::Options diagnostics)
    : window_(context_, (L"PSX CDU — " + widen(profile)).c_str(), 744, 675, callback, this),
      window_icons_(static_cast<HWND>(window_.hwnd())),
      store_((root.empty() ? executable_directory() : root) /
                 (profile_slot(profile_name(profile)) ? std::filesystem::path(L"settings.cfg")
                    : std::filesystem::path(L"profiles") / (widen(profile_name(profile)) + L".cfg")),
             profile_slot(profile_name(profile))),
      slots_((root.empty() ? executable_directory() : root) / L"settings.cfg", 1),
      profile_(profile_name(profile)), warp_(warp), hidden_(hidden)
{
    auto instance_identity = identity(profile_);
    if (!root.empty())
    {
        // Authored fixtures use their explicit store scope so they cannot
        // activate or disable an operator's running numbered-profile window.
        uint64_t hash = 14695981039346656037ull;
        for (auto ch : std::filesystem::absolute(root).lexically_normal().wstring())
        {
            hash ^= uint16_t(towlower(ch));
            hash *= 1099511628211ull;
        }
        instance_identity += L".Store." + std::to_wstring(hash);
    }
    primary_ = window_.single_instance(instance_identity.c_str());
    if (!primary_)
        return;
    auto loaded = store_.load();
    effective_ = loaded.config;
    effective_.parameters.try_emplace("mask_sample_mode_desired", 1.f);
    psx::network::Inventory inv;
    inv.adapters = psx::network::enumerate(psx::network::InventoryPolicy::display);
    if(effective_.subnet.empty())effective_.subnet=configured_subnet(effective_,inv);
    // Win32 appends new child windows at the bottom of the sibling Z order.
    // Construct overlays first and the full-window background last so it cannot
    // obscure the display, warnings or fullscreen alignment frame.
    for (auto &edge : frame_)
    {
        edge = std::make_unique<gfx::Canvas>(context_, window_.hwnd(), rect(0, 0, 2, 2));
        gfx::check(ag_visible(edge->handle(), 0));
        gfx::check(ag_canvas_present_mode(edge->handle(), 1));
        gfx::check(ag_context_requests(edge->handle(), context, this));
    }
    warning_ = make(window_.hwnd(), AG_LABEL, 0, L"Host address required", rect(0, 630, 744, 24));
    renderer_ =
        std::make_unique<psx::cdu::SurfaceRenderer>(context_.handle(), window_.hwnd(), rect(0, 0, 744, 630), warp_);
    backdrop_ = std::make_unique<gfx::Canvas>(context_, window_.hwnd(), rect(0, 0, 744, 675));
    gfx::check(ag_context_requests(backdrop_->handle(), context, this));
    gfx::check(ag_canvas_present_mode(backdrop_->handle(), 1));
    gfx::check(ag_context_requests(window_.handle(), context, this));
    gfx::check(ag_context_requests(renderer_->handle(), context, this));
    gfx::check(ag_window_shortcut(window_.handle(), VK_F11, 0, 1));
    gfx::check(ag_window_shortcut(window_.handle(), VK_ESCAPE, 0, 2));
    gfx::check(ag_window_shortcut(window_.handle(), VK_F12, 0, 3));
    for (unsigned shift = 0; shift < 2; ++shift)
    {
        for (unsigned key : {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_OEM_PLUS, VK_OEM_MINUS, VK_ADD, VK_SUBTRACT})
            gfx::check(ag_window_shortcut(window_.handle(), key, shift | AG_SHORTCUT_REPEAT, 100 + key + shift * 256));
    }
    window_.defer_close(true);
    if (loaded.valid)
    {
        restore(effective_);
        try
        {
            auto c = prepare(effective_);
            c.commit(renderer_->handle());
            effective_effects_ = effective_.effects;
        }
        catch (const gfx::Error &e)
        {
            shader_error_ = e.detail;
        }
        catch (const std::exception &e)
        {
            shader_error_ = widen(e.what());
        }
    }
    if (loaded.valid && !effective_effects_)
    {
        auto bypass = effective_;
        bypass.effects = false;
        auto c = prepare(bypass);
        c.commit(renderer_->handle());
    }
    if (hidden_)
        SetWindowPos(static_cast<HWND>(window_.hwnd()), nullptr, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    runtime_ = std::make_unique<Runtime>(effective_, loaded.valid, profile_, std::move(diagnostics));
    window_.timer(1, 250);
    window_.timer(2, 16);
    ready_ = true;
    layout();
    window_.show(!hidden);
    tick();
    if(!loaded.warning.empty())options_error_=widen(loaded.warning);
    if (!loaded.valid)
    {
        error(widen(loaded.missing ? "Enter the host address or computer name and select a CDU subnet" : loaded.error));
        options();
        if (hidden)
            dialog_->show(false, false);
    }
}
Ui::~Ui()
{
    if (runtime_)
    {
        runtime_->request_stop();
        runtime_->join();
    }
    dismiss(false);
}
int Ui::run()
{
    return primary_ ? window_.run() : 0;
}
void Ui::layout()
{
    if (!ready_ || !renderer_)
        return;
    auto b = window_.client_bounds();
    const float dip = GetDpiForWindow(static_cast<HWND>(window_.hwnd())) / 96.f;
    const bool full = gfx::placement(window_).mode == 2;
    const float w = std::max(0.f, b.right), h = std::max(0.f, b.bottom);
    const float available = full ? h : std::max(0.f, h - 24);
    const float fit_width = std::min(w * dip, available * dip * 372.f / 315.f);
    const float width = full && effective_.display_width ? float(effective_.display_width) : fit_width;
    const float height = width * 315.f / 372.f;
    display_bounds_ =
        rect((w - width / dip) * .5f + (full ? effective_.display_x / dip : 0),
             (available - height / dip) * .5f + (full ? effective_.display_y / dip : 0), width / dip, height / dip);
    gfx::check(ag_bounds_set(renderer_->handle(), display_bounds_));
    backdrop_->bounds(rect(0, 0, w, h));
    ag_draw background{};
    background.kind = AG_FILL_RECT;
    background.rect = rect(0, 0, w, h);
    background.color = {0, 0, 0, 1};
    backdrop_->submit({&background, 1});
    const float line = 2 / dip;
    const auto area = display_bounds_;
    std::array<ag_rect, 4> edges{rect(area.left, area.top, area.right - area.left, line),
                                 rect(area.left, area.bottom - line, area.right - area.left, line),
                                 rect(area.left, area.top, line, area.bottom - area.top),
                                 rect(area.right - line, area.top, line, area.bottom - area.top)};
    for (unsigned i = 0; i < 4; ++i)
    {
        frame_[i]->bounds(edges[i]);
        ag_draw green{};
        green.kind = AG_FILL_RECT;
        green.rect = rect(0, 0, edges[i].right - edges[i].left, edges[i].bottom - edges[i].top);
        green.color = {0, 1, 0, 1};
        frame_[i]->submit({&green, 1});
        gfx::check(ag_visible(frame_[i]->handle(), editing_ && full));
    }
    warning_.bounds(rect(0, std::max(0.f, h - 24), w, 24));
    gfx::check(ag_visible(warning_.handle(), !full || !warning_text_.empty()));
}
void Ui::error(const std::wstring &s)
{
    shader_error_ = s;
    options_error_ = s;
    if (dialog_ && controls_.contains(NOTICE))
        controls_.at(NOTICE).text(s.c_str());
    if (warning_.handle())
        warning_.text(s.c_str());
}
void Ui::draw(const View &v)
{
    if (!exiting_ && renderer_ && v.epoch >= pending_)
    {
        const auto now = psx::norm::monotonic_ns();
        if (v.epoch != drawing_epoch_ || v.source.session != drawing_source_.session ||
            v.source.generation != drawing_source_.generation)
        {
            drawn_.reset(now);
            drawing_epoch_ = v.epoch;
            drawing_source_ = v.source;
        }
        observe_draw();
        const auto revision = renderer_->content_revision();
        const auto before = renderer_->metrics();
        psx::cdu::CduSnapshot empty;
        renderer_->page(presentation_view(v,empty));
        if (renderer_->content_revision() != revision)
        {
            if (v.has_screen)
                drawn_.submitted(renderer_->content_revision(), before.source_draws, before.frames,
                                 before.device_recreations);
            else
                drawn_.clear_pending();
        }
        if (drawn_.pending())
            render_display();
    }
}
void Ui::observe_draw()
{
    const auto m = renderer_->metrics();
    const auto hwnd = static_cast<HWND>(window_.hwnd());
    drawn_.rendered(m.source_draws, m.frames, m.device_recreations,
                    !hidden_ && IsWindowVisible(hwnd) && !IsIconic(hwnd) && !m.error && !m.suspended);
}
void Ui::render_display()
{
    const auto hwnd = static_cast<HWND>(window_.hwnd());
    if (hidden_ || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return;
    renderer_->render(double(psx::norm::monotonic_ns()) / 1e9);
    observe_draw();
}
void Ui::tick()
{
    if (!runtime_ || !renderer_)
        return;
    auto v = runtime_->view();
    if (exiting_)
    {
        if (runtime_->stopped())
        {
            runtime_->join();
            window_.defer_close(false);
            window_.close();
        }
        return;
    }
    if (pending_ && v.ack >= pending_)
    {
        pending_ = 0;
        if (dialog_)
        {
            controls_.at(APPLY).enabled(true);
            controls_.at(OK).enabled(true);
            controls_.at(CANCEL).enabled(true);
            controls_.at(RESET).enabled(true);
            controls_.at(PROFILE).enabled(true);
            controls_.at(SAVE_PROFILE).enabled(true);
            controls_.at(SHADER_OPTIONS).enabled(true);
            if (closing_ok_)
            {
                closing_ok_ = false;
                dismiss();
            }
        }
    }
    draw(v);
    std::wstring message = v.fatal          ? L"Reception failed: " + widen(v.error.data())
                           : !v.active      ? L"Reception inactive: " + widen(v.error.data())
                           : !v.healthy     ? L"Host traffic missing — last screen retained"
                           : !v.has_screen  ? L"Waiting for selected CDU"
                           : !v.initialized ? L"CDU initializing"
                           : v.screen.source_state != psx::cdu::SourceState::connected
                               ? L"PSX Main " + std::wstring(state(v.screen.source_state)) + L" — cached screen"
                               : L"";
    if (display_config().effects && !effective_effects_)
        message +=
            (message.empty() ? L"" : L" | ") + std::wstring(L"Shader requested; bypass active: ") + shader_error_;
    if (v.journal.running && !v.journal.persistence)
        message += (message.empty() ? L"" : L" | ") + std::wstring(L"Diagnostics storage unavailable");
    if (renderer_->metrics().error)
        message += (message.empty() ? L"" : L" | ") + std::wstring(L"Display device unavailable; retry pending");
    if (message != warning_text_)
    {
        warning_text_ = message;
        warning_.text(message.c_str());
        gfx::check(ag_visible(warning_.handle(), !message.empty() || gfx::placement(window_).mode != 2));
    }
    if (dialog_)
    {
        wchar_t s[160]{};
        swprintf_s(s, L"%s  |  Received: %.1f FPS  |  Drawn: %.1f FPS",
                   v.active && v.healthy ? L"Connected" : L"Not connected", v.received_fps,
                   drawn_.sample(psx::norm::monotonic_ns()));
        if (controls_.at(CONNECTION).text() != s)
            controls_.at(CONNECTION).text(s);
        const auto &notice = options_error_.empty() ? warning_text_ : options_error_;
        if (controls_.at(NOTICE).text() != notice)
            controls_.at(NOTICE).text(notice.c_str());
    }
    else
        drawn_.sample(psx::norm::monotonic_ns());
}
void Ui::event(unsigned id, unsigned kind, double)
{
    if (!ready_)
        return;
    if (kind == AG_DISPATCH)
    {
        if (id == CLOSE_SHADER)
            close_shader();
        else if (id == CLOSE_OPTIONS && !pending_)
            dismiss();
        return;
    }
    if (kind == AG_CLOSE)
    {
        if (dialog_)
        {
            gfx::check(ag_window_post(window_.handle(), CLOSE_OPTIONS));
            return;
        }
        shutdown();
        return;
    }
    if (kind == AG_RESIZE)
    {
        layout();
        if (effective_effects_)
        {
            try
            {
                validate_viewport(renderer_->handle(), display_config());
                render_display();
            }
            catch (const gfx::Error &e)
            {
                shader_error_ = e.detail;
                Config bypass = display_config();
                bypass.effects = false;
                renderer_->reset();
                auto c = prepare(bypass);
                c.commit(renderer_->handle());
                effective_effects_ = false;
            }
            catch (const std::exception &e)
            {
                shader_error_ = widen(e.what());
                Config bypass = display_config();
                bypass.effects = false;
                renderer_->reset();
                auto c = prepare(bypass);
                c.commit(renderer_->handle());
                effective_effects_ = false;
            }
        }
        return;
    }
    if (kind == AG_TIMER)
    {
        if (id == 2)
        {
            draw(runtime_->view());
            return;
        }
        tick();
        return;
    }
    if (kind == AG_REFRESH)
    {
        if (effective_effects_)
            render_display();
        return;
    }
    if (kind == AG_DIALOG_ACCEPT)
    {
        apply(true);
        return;
    }
    if (kind == AG_DIALOG_CANCEL)
    {
        if (!pending_)
            gfx::check(ag_window_post(window_.handle(), CLOSE_OPTIONS));
        return;
    }
    if (kind == AG_SHORTCUT)
    {
        if (dialog_)
            return;
        auto p = gfx::placement(window_);
        if (id == 1 || id == 2)
        {
            editing_ = false;
            gfx::borderless(window_, id == 1 && p.mode != 2);
            layout();
            remember();
        }
        else if (id == 3 && p.mode == 2)
        {
            editing_ = !editing_;
            layout();
            if (!editing_)
                remember();
        }
        else if (editing_ && p.mode == 2 && id >= 100)
        {
            const unsigned value = id - 100, key = value % 256;
            const int step = value >= 256 ? 10 : 1;
            if (key == VK_LEFT)
                effective_.display_x -= step;
            if (key == VK_RIGHT)
                effective_.display_x += step;
            if (key == VK_UP)
                effective_.display_y -= step;
            if (key == VK_DOWN)
                effective_.display_y += step;
            if (key == VK_ADD || key == VK_OEM_PLUS || key == VK_SUBTRACT || key == VK_OEM_MINUS)
            {
                float dip = GetDpiForWindow(static_cast<HWND>(window_.hwnd())) / 96.f;
                int width = int(std::lround((display_bounds_.right - display_bounds_.left) * dip));
                effective_.display_width =
                    std::clamp(width + ((key == VK_ADD || key == VK_OEM_PLUS) ? step : -step), 32, 16384);
            }
            effective_.display_x = std::clamp(effective_.display_x, -65536, 65536);
            effective_.display_y = std::clamp(effective_.display_y, -65536, 65536);
            layout();
            render_display();
        }
        return;
    }
    if (kind != AG_CLICK)
        return;
    if (id == SHADER_OPTIONS)
        shader_options();
    else if (id == EFFECTS && !pending_)
    {
        auto c = draft_;
        c.effects = controls_.at(EFFECTS).value() != 0;
        preview_shader(c);
    }
    else if (id == RESET)
        reset_alignment();
    else if (id == APPLY)
        apply(false);
    else if (id == OK)
        apply(true);
    else if (id == SAVE_PROFILE)
        save_profile(unsigned(controls_.at(PROFILE).value()) + 1);
    else if (id == CANCEL && !pending_)
        gfx::check(ag_window_post(window_.handle(), CLOSE_OPTIONS));
}
void __cdecl Ui::shader_callback(void *user, uint32_t id, uint32_t kind, double value) noexcept
{
    auto self = static_cast<Ui *>(user);
    try
    {
        self->shader_event(id, kind, value);
    }
    catch (const gfx::Error &e)
    {
        if (self->shader_controls_.contains(SHADER_NOTICE))
            try { self->shader_controls_.at(SHADER_NOTICE).text(e.detail.c_str()); } catch (...) {}
    }
    catch (const std::exception &e)
    {
        if (self->shader_controls_.contains(SHADER_NOTICE))
            try { self->shader_controls_.at(SHADER_NOTICE).text(widen(e.what()).c_str()); } catch (...) {}
    }
}
Candidate Ui::prepare(const Config &c)
{
    // The explicit constructor root scopes fixture settings, not runtime assets.
    // Keep verified bytes across toggles so Cancel works even after asset files
    // disappear. Candidates themselves are valid for one display revision only.
    if (c.effects && !shader_assets_)
        shader_assets_ = std::make_unique<ShaderAssets>(executable_directory() / L"shaders");
    return prepare_display(renderer_->handle(), visual_config(c), executable_directory() / L"shaders", warp_,
                           shader_assets_.get());
}
void Ui::preview_shader(const Config &next)
{
    auto c = visual_config(next);
    const auto error = validate(c, {}, false);
    if (!error.empty())
        throw std::runtime_error(error);
    if (c.effects)
        validate_viewport(renderer_->handle(), c);
    const auto previous = visual_config(display_config());
    if (preview_active_ && c.effects == effective_effects_ && c.parameters == previous.parameters)
        return;
    if (!preview_active_)
    {
        if (effective_effects_ && !shader_assets_)
            shader_assets_ = std::make_unique<ShaderAssets>(executable_directory() / L"shaders");
        preview_effects_before_ = effective_effects_;
        preview_error_before_ = shader_error_;
    }
    if (c.effects && effective_effects_)
    {
        try
        {
            update_parameters(renderer_->handle(), c);
        }
        catch (...)
        {
            try { update_parameters(renderer_->handle(), previous); } catch (...) {}
            throw;
        }
    }
    else if (c.effects != effective_effects_)
    {
        auto candidate = prepare(c);
        candidate.commit(renderer_->handle());
    }
    preview_ = next;
    preview_active_ = true;
    effective_effects_ = c.effects;
    draft_.parameters = next.parameters;
    draft_.effects = next.effects;
    if (controls_.contains(EFFECTS))
        controls_.at(EFFECTS).value(next.effects ? 1 : 0);
    shader_error_.clear();
    options_error_.clear();
    gfx::check(ag_display_history_clear(renderer_->handle()));
    render_display();
}
void Ui::shader_event(unsigned id, unsigned kind, double)
{
    if (!shader_dialog_ || filling_shader_)
        return;
    if (kind == AG_CLOSE || kind == AG_DIALOG_ACCEPT || kind == AG_DIALOG_CANCEL ||
        (kind == AG_CLICK && id == SHADER_DONE))
    {
        // Destruction happens on the main window after the source callback
        // returns, including when Escape/Enter originated in a nested control.
        gfx::check(ag_window_post(window_.handle(), CLOSE_SHADER));
        return;
    }
    if (kind == AG_CLICK && id == EFFECTS)
    {
        auto c = draft_;
        c.effects = shader_controls_.at(EFFECTS).value() != 0;
        preview_shader(c);
    }
    else if (kind == AG_CLICK && id == SHADER_RESET)
    {
        auto c = draft_;
        c.parameters.clear();
        for (const auto &p : parameter_registry())
            c.parameters[p.id] = std::string_view(p.id) == "mask_sample_mode_desired" ? 1.f : p.initial;
        preview_shader(c);
        fill_shader();
    }
    else if ((kind == AG_VALUE_CHANGED || kind == AG_TEXT_CHANGED) && id >= 1000 &&
             id - 1000 < parameter_registry().size())
    {
        const auto &p = parameter_registry()[id - 1000];
        if (p.fixed)
            return;
        auto c = draft_;
        c.parameters[p.id] = float(shader_controls_.at(id).value());
        // Partial/invalid typing does not replace the most recent valid state.
        preview_shader(c);
    }
    else
        return;
    shader_controls_.at(SHADER_NOTICE).text(draft_.effects ? L"Live preview. Cancel in Options restores the applied settings."
                                                        : L"Shader disabled. Values are retained; enable the shader to preview them.");
}
void Ui::shader_options()
{
    if (!dialog_ || pending_ || exiting_)
        return;
    if (shader_dialog_)
    {
        shader_dialog_->show(true, !hidden_);
        return;
    }
    try
    {
        shader_dialog_ = std::make_unique<OwnedDialog>(*dialog_, L"CRT-Royale shader settings", 840, 620,
                                                     shader_callback, this);
        shader_icons_ = std::make_unique<psx::windows::WindowIcons>(static_cast<HWND>(shader_dialog_->hwnd()));
        gfx::check(ag_window_minimum(shader_dialog_->handle(), 840, 620));
        auto make_shader = [&](void *parent, unsigned kind, unsigned id, const wchar_t *text, ag_rect bounds,
                               const Parameter *p = nullptr) {
            ag_control_desc d{};
            d.kind = kind; d.id = id; d.text = text; d.bounds = bounds;
            d.maximum = kind == AG_CHECKBOX ? 1 : 0;
            if (p)
            {
                d.minimum = p->minimum; d.maximum = p->maximum; d.step = p->step; d.value = p->initial;
                d.tooltip = parameter_help(p->id);
            }
            return gfx::Control(context_, parent, d, shader_callback, this);
        };
        auto hwnd = shader_dialog_->hwnd();
        shader_labels_.push_back(make_shader(hwnd, AG_LABEL, 0, L"CRT-Royale CDU revision 1 — hover for help.",
                                             rect(14, 10, 590, 26)));
        shader_controls_.emplace(EFFECTS, make_shader(hwnd, AG_CHECKBOX, EFFECTS, L"Enable effect", rect(642, 8, 184, 30)));
        gfx::check(ag_tooltip_set(shader_controls_.at(EFFECTS).handle(),
                                L"Preview the effect on or off immediately, retaining all parameter values. Options Cancel restores the applied display."));
        shader_tabs_ = make_shader(hwnd, AG_TABS, 80, L"", rect(14, 44, 812, 440));
        std::vector<std::wstring> groups;
        std::vector<unsigned> rows;
        for (unsigned i = 0; i < parameter_registry().size(); ++i)
        {
            const auto &p = parameter_registry()[i];
            if (!parameter_help(p.id))
                throw std::runtime_error("Shader help missing: " + std::string(p.id));
            auto group = std::find(groups.begin(), groups.end(), p.group);
            unsigned index = unsigned(group - groups.begin());
            if (group == groups.end())
            {
                groups.emplace_back(p.group);
                shader_pages_.push_back(shader_tabs_.tab(p.group));
                rows.push_back(0);
            }
            auto parent = shader_pages_[index].hwnd();
            const float y = 12.f + 38.f * rows[index]++;
            auto title = std::wstring(p.label) + (p.fixed ? L" (fixed in this build)" : L"");
            auto label = make_shader(parent, AG_LABEL, 0, title.c_str(), rect(14, y, 520, 30));
            gfx::check(ag_tooltip_set(label.handle(), parameter_help(p.id)));
            shader_labels_.push_back(std::move(label));
            auto control = make_shader(parent, AG_NUMERIC, 1000 + i, L"", rect(552, y, 190, 28), &p);
            gfx::check(ag_control_events(control.handle(), AG_CONTROL_NOTIFY_TEXT));
            control.enabled(!p.fixed);
            shader_controls_.emplace(1000 + i, std::move(control));
        }
        for (unsigned i = 0; i < rows.size(); ++i)
            shader_pages_[i].extent(24.f + rows[i] * 38.f);
        shader_controls_.emplace(SHADER_NOTICE, make_shader(hwnd, AG_LABEL, SHADER_NOTICE, L"", rect(14, 494, 812, 52)));
        shader_controls_.emplace(SHADER_RESET, make_shader(hwnd, AG_BUTTON, SHADER_RESET, L"Reset shader defaults", rect(14, 562, 220, 32)));
        shader_controls_.emplace(SHADER_DONE, make_shader(hwnd, AG_BUTTON, SHADER_DONE, L"Done", rect(716, 562, 110, 32)));
        gfx::check(ag_tooltip_set(shader_controls_.at(SHADER_RESET).handle(),
                                L"Preview the CDU shader defaults. This does not save a profile; Options Cancel restores the applied values."));
        gfx::check(ag_tooltip_set(shader_controls_.at(SHADER_DONE).handle(),
                                L"Return to Options, keeping this preview. Apply/OK keep it for this instance; Save profile saves it."));
        fill_shader();
        if (hidden_)
            SetWindowPos(static_cast<HWND>(hwnd), nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        shader_dialog_->show(true, !hidden_);
    }
    catch (...)
    {
        close_shader();
        throw;
    }
}
void Ui::fill_shader()
{
    filling_shader_ = true;
    try
    {
        shader_controls_.at(EFFECTS).value(draft_.effects ? 1 : 0);
        for (unsigned i = 0; i < parameter_registry().size(); ++i)
        {
            const auto &p = parameter_registry()[i];
            auto value = draft_.parameters.find(p.id);
            shader_controls_.at(1000 + i).value(value == draft_.parameters.end() ? p.initial : value->second);
        }
        shader_controls_.at(SHADER_NOTICE).text(draft_.effects ? L"Live preview. Cancel in Options restores the applied settings."
                                                            : L"Shader disabled. Values are retained; enable the shader to preview them.");
    }
    catch (...)
    {
        filling_shader_ = false;
        throw;
    }
    filling_shader_ = false;
}
void Ui::close_shader()
{
    if (!shader_dialog_)
        return;
    filling_shader_ = true;
    shader_dialog_->show(false, false);
    shader_controls_.clear();
    shader_labels_.clear();
    shader_pages_.clear();
    shader_tabs_ = {};
    shader_icons_.reset();
    shader_dialog_.reset();
    filling_shader_ = false;
}
void Ui::options()
{
    if (exiting_)
        return;
    if (dialog_)
    {
        dialog_->show(true, !hidden_);
        return;
    }
    remember();
    draft_ = effective_;
    dialog_ = std::make_unique<gfx::Dialog>(window_, L"CDU Options", 740, 660, callback, this);
    dialog_icons_ = std::make_unique<psx::windows::WindowIcons>(static_cast<HWND>(dialog_->hwnd()));
    dialog_->defer_close(true);
    gfx::check(ag_window_minimum(dialog_->handle(), 740, 660));
    void *hwnd = dialog_->hwnd();
    auto field = [&](unsigned id, const wchar_t *label, unsigned kind, float y, float lo = 0, float hi = 0,
                     float step = 0) {
        labels_.push_back(make(hwnd, AG_LABEL, 0, label, rect(16, y, 310, 28)));
        controls_.emplace(id, make(hwnd, kind, id, L"", rect(340, y, 330, 28), lo, hi, step));
    };
    field(HOST, L"Host address or computer name", AG_EDIT, 20);
    field(CDU, L"CDU", AG_CHOICE, 62);
    for (auto t : {L"Captain (CPT)", L"First officer (FO)", L"Centre"})
        controls_.at(CDU).add(t);
    controls_.emplace(EFFECTS,
                      make(hwnd, AG_CHECKBOX, EFFECTS, L"Enable CRT-Royale render shader", rect(16, 108, 430, 32)));
    controls_.emplace(SHADER_OPTIONS,
                      make(hwnd, AG_BUTTON, SHADER_OPTIONS, L"Shader settings...", rect(490, 108, 210, 32)));
    gfx::check(ag_tooltip_set(controls_.at(SHADER_OPTIONS).handle(),
                            L"Open CRT-Royale settings with live preview and help for each value."));
    gfx::check(ag_tooltip_set(controls_.at(EFFECTS).handle(),
                            L"Preview the CRT effect immediately. Cancel restores the applied display; Save profile stores it."));
    controls_.emplace(RESET, make(hwnd, AG_BUTTON, RESET, L"Reset fullscreen position", rect(16, 158, 310, 32)));
    controls_.emplace(DIAGNOSTICS,make(hwnd,AG_CHECKBOX,DIAGNOSTICS,L"Diagnostics (this run only)",rect(350,158,350,32)));
    field(GROUP, L"Advanced: multicast group", AG_EDIT, 220);
    field(PORT, L"Advanced: multicast port", AG_NUMERIC, 262, 1, 65535, 1);
    field(ADAPTER, L"CDU network", AG_CHOICE, 304);
    controls_.emplace(CONNECTION, make(hwnd, AG_LABEL, CONNECTION, L"", rect(16, 356, 690, 32)));
    labels_.push_back(make(
        hwnd, AG_LABEL, 0,
        L"F11 toggles fullscreen; saved mode is restored on startup. F12 toggles fullscreen alignment: arrows move, "
        L"+/- resize around the centre; Shift uses ten-pixel steps. F12 again ends editing. "
        L"Save profile persists placement and scale; Apply/OK change this instance only.",
        rect(16, 402, 690, 50)));
    labels_.push_back(make(hwnd, AG_LABEL, 0, L"Save to profile", rect(16, 466, 180, 28)));
    controls_.emplace(PROFILE, make(hwnd, AG_CHOICE, PROFILE, L"", rect(196, 466, 130, 28)));
    for (unsigned slot = 1; slot <= 10; ++slot)
        controls_.at(PROFILE).add(std::to_wstring(slot).c_str());
    controls_.at(PROFILE).value(profile_slot(profile_) ? profile_slot(profile_) - 1 : 0);
    controls_.emplace(SAVE_PROFILE, make(hwnd, AG_BUTTON, SAVE_PROFILE, L"Save profile", rect(340, 464, 158, 32)));
    controls_.emplace(APPLY, make(hwnd, AG_BUTTON, APPLY, L"Apply", rect(402, 594, 96, 32)));
    controls_.emplace(OK, make(hwnd, AG_BUTTON, OK, L"OK", rect(510, 594, 96, 32)));
    controls_.emplace(CANCEL, make(hwnd, AG_BUTTON, CANCEL, L"Cancel", rect(618, 594, 96, 32)));
    controls_.emplace(NOTICE, make(hwnd, AG_LABEL, NOTICE, shader_error_.c_str(), rect(14, 516, 700, 60)));
    fill();
    if (hidden_)
        SetWindowPos(static_cast<HWND>(dialog_->hwnd()), nullptr, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    dialog_->show(true, !hidden_);
    tick();
}
void Ui::fill()
{
    controls_.at(DIAGNOSTICS).value(diagnostics_enabled_);
    controls_.at(HOST).text(widen(draft_.host).c_str());
    controls_.at(CDU).value(unsigned(draft_.unit));
    controls_.at(GROUP).text(widen(draft_.group).c_str());
    controls_.at(PORT).value(draft_.port);

    controls_.at(EFFECTS).value(draft_.effects ? 1 : 0);
    psx::network::Inventory inventory{psx::network::enumerate(psx::network::InventoryPolicy::display)};
    adapter_ids_={L""};std::vector<std::wstring> labels{L"Localhost (automatic)"};
    int selected=draft_.subnet.empty()?0:-1;
    for(const auto&key:psx::network::subnets(inventory)){
        adapter_ids_.push_back(widen(key));labels.push_back(widen(psx::network::subnet_label(key)));
        if(key==draft_.subnet)selected=int(adapter_ids_.size()-1);
    }
    if(selected<0&&!draft_.subnet.empty()){
        adapter_ids_.push_back(widen(draft_.subnet));labels.push_back(widen(psx::network::subnet_label(draft_.subnet))+L" (unavailable)");
        selected=int(adapter_ids_.size()-1);
    }
    std::vector<const wchar_t*> names;for(auto&label:labels)names.push_back(label.c_str());
    controls_.at(ADAPTER).replace(names,selected);

}
Config Ui::read()
{
    Config c = draft_;
    c.host = narrow(controls_.at(HOST).text());
    c.group = narrow(controls_.at(GROUP).text());
    auto port = controls_.at(PORT).value();
    if (port < 1 || port > 65535 || port != std::floor(port))
        throw std::runtime_error("Invalid multicast port");
    c.port = uint16_t(port);
    c.unit = psx::cdu::Unit(unsigned(controls_.at(CDU).value()));
    int a = int(controls_.at(ADAPTER).value());
    if (a < 0 || size_t(a) >= adapter_ids_.size())
        throw std::runtime_error("Select the CDU network subnet");
    c.subnet = narrow(adapter_ids_[a]);
    c.raster = 0; // Legacy fixed-raster fields remain readable; explicit Apply adopts output sizing.
    c.effects = controls_.at(EFFECTS).value() != 0;
    capture(c);
    c.parameters.try_emplace("mask_sample_mode_desired", 1.f);
    return c;
}
void Ui::apply(bool close, unsigned save_slot)
{
    if (!dialog_ || pending_)
        return;
    auto c = read();
    psx::network::Inventory inv;
    inv.adapters = psx::network::enumerate(psx::network::InventoryPolicy::display);
    auto err = validate(c, inv);
    if (!err.empty())
        throw std::runtime_error(err);
    auto candidate = prepare(c);
    if (save_slot)
        slots_.save(c, save_slot);
    candidate.commit(renderer_->handle());
    close_shader();
    preview_active_ = false;
    bool selection = c.host != effective_.host || c.unit != effective_.unit;
    effective_ = c;
    effective_effects_ = c.effects;
    shader_error_.clear();
    options_error_ = save_slot ? L"Saved profile " + std::to_wstring(save_slot) +
                                   L". Launch with --profile " + std::to_wstring(save_slot) : L"";
    draft_ = c;
    if (selection)
    {
        psx::cdu::CduSnapshot empty;
        renderer_->page(empty.view());
    }
    diagnostics_enabled_=controls_.at(DIAGNOSTICS).value()!=0;
    runtime_->diagnostics(diagnostics_enabled_);
    pending_ = runtime_->configure(c);
    closing_ok_ = close;
    controls_.at(APPLY).enabled(false);
    controls_.at(OK).enabled(false);
    controls_.at(CANCEL).enabled(false);
    controls_.at(RESET).enabled(false);
    controls_.at(PROFILE).enabled(false);
    controls_.at(SAVE_PROFILE).enabled(false);
    controls_.at(SHADER_OPTIONS).enabled(false);
    gfx::check(ag_window_refresh(window_.handle(), 0));
    editing_ = false;
    restore(effective_);
    layout();
    remember();
    render_display();
}
void Ui::dismiss(bool restore_preview)
{
    if (!dialog_)
        return;
    close_shader();
    if (preview_active_ && restore_preview)
    {
        auto original = effective_;
        original.effects = preview_effects_before_;
        if (original.effects && effective_effects_)
        {
            update_parameters(renderer_->handle(), original);
            gfx::check(ag_display_history_clear(renderer_->handle()));
        }
        else if (original.effects != effective_effects_)
        {
            auto candidate = prepare(original);
            candidate.commit(renderer_->handle());
        }
        effective_effects_ = preview_effects_before_;
        shader_error_ = preview_error_before_;
    }
    preview_active_ = false;
    render_display();
    dialog_->show(false, false);
    controls_.clear();
    labels_.clear();
    dialog_icons_.reset();
    dialog_.reset();
    adapter_ids_.clear();
}
void Ui::restore(Config &c)
{
    if (c.placement.mode == 2)
    {
        std::vector<ag_monitor_info> values;
        gfx::check(ag_monitors(
            context_.handle(),
            [](void *u, const ag_monitor_info *m) { static_cast<std::vector<ag_monitor_info> *>(u)->push_back(*m); },
            &values));
        auto chosen = values.end();
        for (auto it = values.begin(); it != values.end(); ++it)
            if (c.fullscreen_anchor ? (it->left == c.fullscreen_x && it->top == c.fullscreen_y)
                                    : !wcscmp(it->identity, c.placement.monitor))
            {
                chosen = it;
                break;
            }
        if (chosen == values.end())
        {
            chosen = std::find_if(values.begin(), values.end(), [](auto &m) { return m.primary != 0; });
            if (chosen == values.end())
                throw std::runtime_error("No primary monitor available");
            const auto width = c.placement.right - c.placement.left, height = c.placement.bottom - c.placement.top;
            c.placement.left = chosen->left;
            c.placement.top = chosen->top;
            c.placement.right = chosen->left + width;
            c.placement.bottom = chosen->top + height;
        }
        wcscpy_s(c.placement.monitor, chosen->identity);
        c.fullscreen_x = chosen->left;
        c.fullscreen_y = chosen->top;
        c.fullscreen_anchor = true;
    }
    gfx::check(ag_window_placement_set(window_.handle(), &c.placement));
}
void Ui::remember()
{
    capture(effective_);
}
void Ui::capture(Config &config)
{
    config.placement = gfx::placement(window_);
    if (config.placement.mode == 2)
    {
        RECT bounds{};
        GetWindowRect(static_cast<HWND>(window_.hwnd()), &bounds);
        std::vector<ag_monitor_info> values;
        gfx::check(ag_monitors(
            context_.handle(),
            [](void *u, const ag_monitor_info *m) { static_cast<std::vector<ag_monitor_info> *>(u)->push_back(*m); },
            &values));
        for (auto &m : values)
            if (m.left == bounds.left && m.top == bounds.top)
            {
                config.fullscreen_x = m.left;
                config.fullscreen_y = m.top;
                config.fullscreen_anchor = true;
                wcscpy_s(config.placement.monitor, m.identity);
                break;
            }
    }
    // Like the DU client, placement remains in memory until explicit Save profile.
}
void Ui::shutdown()
{
    if (exiting_)
        return;
    exiting_ = true;
    dismiss(false);
    try
    {
        remember();
    }
    catch (const gfx::Error &e)
    {
        error(e.detail);
    }
    catch (const std::exception &e)
    {
        error(widen(e.what()));
    }
    runtime_->request_stop();
    warning_.text(L"Closing reception…");
}
} // namespace cdu_client
