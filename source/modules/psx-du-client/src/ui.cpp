#include <algorithm>
#include <cmath>
#include <cwchar>
#include <psx/display/instrument_protocol.hpp>
#include <du_client/ui.hpp>
#include <psx/network/io.hpp>
#include <commctrl.h>
namespace du_client
{
namespace gfx = ass::graphics;
namespace
{
ag_rect rect(float x, float y, float w, float h)
{
    return {x, y, x + w, y + h};
}
constexpr unsigned STATUS = 14, SOURCE_MODE = 19, HOST = 45, ADAPTER = 46, TCP_PORT = 47,
                   UNIT = 15, SAVE_PROFILE = 16, PROFILE = 17, SAVE = 18,
                   APPLY = 40, OK = 41, CANCEL = 42, NOTICE = 43, RESET = 48, TRIM = 49, TRIM_ALWAYS = 50, DIAGNOSTICS = 51;
bool alignment_key(WPARAM key) noexcept
{
    return key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
           key == VK_OEM_PLUS || key == VK_OEM_MINUS || key == VK_ADD || key == VK_SUBTRACT;
}
} // namespace
void __cdecl Ui::callback(void *u, uint32_t id, uint32_t kind, double) noexcept
{
    try
    {
        static_cast<Ui *>(u)->event(id, kind);
    }
    catch (const gfx::Error &e)
    {
        static_cast<Ui *>(u)->error(e.detail);
    }
    catch (const std::exception &e)
    {
        std::wstring s;
        for (auto p = e.what(); *p; p++)
            s.push_back(wchar_t(uint8_t(*p)));
        static_cast<Ui *>(u)->error(s);
    }
    catch (...)
    {
    }
}
void __cdecl Ui::context_request(void *u, const ag_context_request *) noexcept
{
    try
    {
        static_cast<Ui *>(u)->options();
    }
    catch (...)
    {
    }
}
gfx::Control Ui::control(void *parent, unsigned kind, unsigned id, const wchar_t *text, ag_rect bounds,
                         float lo, float hi, float step)
{
    ag_control_desc d{};
    d.kind = kind;
    d.id = id;
    d.text = text;
    d.bounds = bounds;
    d.minimum = lo;
    d.maximum = kind == AG_CHECKBOX ? 1 : hi;
    d.value = std::clamp(0.f,lo,hi);
    d.step = step;
    return gfx::Control(context_, parent, d, callback, this);
}
Ui::Ui(std::wstring profile, std::filesystem::path root, bool hidden, bool warp, bool start_input, std::filesystem::path diagnostics_output)
    : window_(context_, (L"PSX DU — " + profile_name(profile)).c_str(), 800, 600, callback, this),
      icons_(static_cast<HWND>(window_.hwnd())),
      store_((root.empty() ? executable_directory() : root) / L"settings.cfg",
             unsigned(std::stoul(profile_name(profile)))),
      hidden_(hidden), warp_(warp), start_input_(start_input)
{
    profile_ = unsigned(std::stoul(profile_name(profile)));
    auto instance = L"PSX-IOS.DuClient." + user_identity() + L"." + profile_name(profile);
    // Explicit settings roots are independent; keep the installed legacy key exact.
    if (!root.empty())
        instance += L".Settings." + std::to_wstring(std::hash<std::wstring>{}(
            std::filesystem::absolute(root).lexically_normal().wstring()));
    primary_ = window_.single_instance(instance.c_str());
    if (!primary_)
        return;
    auto loaded = store_.load();
    config_ = loaded.config;
    if(!loaded.warning.empty())error_=loaded.warning;
    if (loaded.missing)
        config_.placement = gfx::placement(window_);
    if (!loaded.valid)
        error_ = loaded.error;
    profile_invalid_ = !loaded.valid;
    // Windows places newly created child windows below existing siblings.
    // Create overlays first and the opaque background last.
    for (auto &e : edges_)
    {
        e = std::make_unique<gfx::Canvas>(context_, window_.hwnd(), rect(0, 0, 2, 2));
        gfx::check(ag_visible(e->handle(), 0));
        gfx::check(ag_canvas_present_mode(e->handle(), 1));
        gfx::check(ag_context_requests(e->handle(), context_request, this));
    }
    warning_ = control(window_.hwnd(), AG_LABEL, 0, L"", rect(0, 576, 800, 24));
    ag_display_desc d{sizeof d,
                      1,
                      uint32_t(warp),
                      AG_RASTER_OUTPUT,
                      662,
                      685,
                      662,
                      685,
                      800,
                      600,
                      256ull * 1024 * 1024};
    display_ = std::make_unique<gfx::Display>(context_, window_.hwnd(), rect(0, 0, 800, 576), d);
    backdrop_ = std::make_unique<gfx::Canvas>(context_, window_.hwnd(), rect(0, 0, 800, 600));
    for (auto h : {window_.handle(), backdrop_->handle(), display_->handle()})
        gfx::check(ag_context_requests(h, context_request, this));
    gfx::check(ag_canvas_present_mode(backdrop_->handle(), 1));
    attach_input(static_cast<HWND>(window_.hwnd()), 0);
    attach_input(static_cast<HWND>(display_->hwnd()), 1);
    gfx::check(ag_window_shortcut(window_.handle(), VK_F11, 0, 1));
    gfx::check(ag_window_shortcut(window_.handle(), VK_ESCAPE, 0, 2));
    gfx::check(ag_window_shortcut(window_.handle(), VK_F12, 0, 3));
    for (unsigned shift = 0; shift < 2; shift++)
        for (unsigned key :
             {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_OEM_PLUS, VK_OEM_MINUS, VK_ADD, VK_SUBTRACT})
            gfx::check(ag_window_shortcut(window_.handle(), key, shift | AG_SHORTCUT_REPEAT,
                                          100 + key + shift * 256));
    window_.defer_close(true);
    restore(config_);
    runtime_ = std::make_unique<Runtime>(false, 0, config_.unit, false, std::move(diagnostics_output));
    runtime_->configure_source(config_.network,config_.host,config_.adapter_guid,config_.tcp_port);
    runtime_->select_trim(config_.trim.enabled);
    if(loaded.valid&&start_input)runtime_->connect();
    window_.timer(1, 16);
    ready_ = true;
    layout();
    if (hidden)
        SetWindowPos(static_cast<HWND>(window_.hwnd()), nullptr, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    window_.show(!hidden);
    if (!loaded.valid)
    {
        options();
        if (hidden)
            dialog_->show(false, false);
    }
}
Ui::~Ui()
{
    ready_ = false;
    for (unsigned id = 0; id < input_windows_.size(); ++id)
        detach_input(id);
    if (runtime_)
    {
        runtime_->shutdown();
        runtime_->join();
    }
    // The ordinary message loop may already have destroyed every HWND. Release
    // owned ASS objects without asking their wrappers for a live native window.
    controls_.clear();
    labels_.clear();
    dialog_.reset();
    trim_display_.reset();display_.reset();
    psx::du::transport::tcp::release_model(main_replay_cpu_+trim_replay_cpu_);
}
void Ui::submit_replay(gfx::Display&display,const psx::du::Frame*frame,size_t&resident)
{
    // Charge both ASS-owned snapshots/cache and conservative submission scratch
    // to the same payload pool as decoded models. A failed admission preserves
    // the current scene and never delays the acquisition reader.
    constexpr size_t mib=1024*1024,retained_limit=6*mib;
    const size_t temporary=frame?4*frame->bytes()+mib:0;
    if(temporary&&!psx::du::transport::tcp::reserve_model(temporary))
        throw gfx::Error(E_OUTOFMEMORY,L"Drawing resource budget reached - last valid display retained");
    auto reserved=resident+temporary;
    try{
        ag_replay_snapshot snapshot=frame?frame->snapshot():ag_replay_snapshot{sizeof snapshot,1};
        gfx::check(ag_display_submit_replay(display.handle(),&snapshot));
        ag_replay_metrics metrics{sizeof metrics,1};gfx::check(ag_display_replay_metrics(display.handle(),&metrics));
        // The fixed root-resource sentinel is metadata in the existing scratch
        // allowance, so clearing a scene never needs an additional reservation.
        const auto actual=metrics.cpu_bytes+metrics.cache_bytes-std::min<uint64_t>(metrics.cpu_bytes,sizeof(std::shared_ptr<void>));
        if(actual>retained_limit||actual>reserved)
            throw gfx::Error(E_UNEXPECTED,L"Replay resource accounting exceeds the qualified ASS profile");
        psx::du::transport::tcp::release_model(reserved-size_t(actual));resident=size_t(actual);
    }catch(...){
        // The library may retain newly interned resources even if a complete
        // scene is rejected. Its qualified3MiB scene+3MiB cache cap still applies.
        const auto held=std::min(reserved,retained_limit);
        psx::du::transport::tcp::release_model(reserved-held);resident=held;throw;
    }
}
int Ui::run()
{
    return primary_ ? window_.run() : 0;
}
void Ui::error(std::wstring s)
{
    error_ = std::move(s);
    if (warning_.handle())
        warning_.text(error_.c_str());
    if (controls_.contains(NOTICE))
        controls_.at(NOTICE).text(error_.c_str());
}
void Ui::restore(Config &c)
{
    if (c.placement.mode == 2)
    {
        std::vector<ag_monitor_info> m;
        gfx::check(ag_monitors(
            context_.handle(), [](void *u, const ag_monitor_info *x)
            { static_cast<std::vector<ag_monitor_info> *>(u)->push_back(*x); }, &m));
        auto it = std::find_if(m.begin(), m.end(),
                               [&](auto &v)
                               {
                                   return c.anchor ? (v.left == c.fullscreen_x && v.top == c.fullscreen_y)
                                                   : !wcscmp(v.identity, c.placement.monitor);
                               });
        if (it == m.end())
        {
            it = std::find_if(m.begin(), m.end(), [](auto &v) { return v.primary != 0; });
            if (it == m.end())
                throw std::runtime_error("Primary monitor unavailable");
            auto w = c.placement.right - c.placement.left, h = c.placement.bottom - c.placement.top;
            c.placement.left = it->left;
            c.placement.top = it->top;
            c.placement.right = it->left + w;
            c.placement.bottom = it->top + h;
        }
        wcscpy_s(c.placement.monitor, it->identity);
        c.fullscreen_x = it->left;
        c.fullscreen_y = it->top;
        c.anchor = true;
    }
    gfx::check(ag_window_placement_set(window_.handle(), &c.placement));
}
void Ui::save()
{
    config_.placement = gfx::placement(window_);
    if (config_.placement.mode == 2)
    {
        RECT r{};
        GetWindowRect(static_cast<HWND>(window_.hwnd()), &r);
        std::vector<ag_monitor_info> m;
        gfx::check(ag_monitors(
            context_.handle(), [](void *u, const ag_monitor_info *x)
            { static_cast<std::vector<ag_monitor_info> *>(u)->push_back(*x); }, &m));
        for (auto &v : m)
            if (v.left == r.left && v.top == r.top)
            {
                config_.fullscreen_x = v.left;
                config_.fullscreen_y = v.top;
                config_.anchor = true;
                wcscpy_s(config_.placement.monitor, v.identity);
                break;
            }
    }
    // Placement is collected in memory; only explicit Save profile persists it.
}
void Ui::layout()
{
    if (!ready_)
        return;
    auto b = window_.client_bounds();
    auto dip = GetDpiForWindow(static_cast<HWND>(window_.hwnd())) / 96.f;
    bool full = gfx::placement(window_).mode == 2;
    auto w = std::max(0.f, b.right), h = std::max(0.f, b.bottom),
         available = full ? h : std::max(0.f, h - 24);
    auto width =
        full && config_.width ? float(config_.width) : std::min(w * dip, available * dip * 662 / 685);
    auto height = width * 685 / 662;
    bounds_ = rect((w - width / dip) / 2 + (full ? config_.x / dip : 0),
                   (available - height / dip) / 2 + (full ? config_.y / dip : 0), width / dip, height / dip);
    gfx::check(ag_bounds_set(display_->handle(), bounds_));
    backdrop_->bounds(rect(0, 0, w, h));
    ag_draw black{};
    black.kind = AG_FILL_RECT;
    black.rect = rect(0, 0, w, h);
    black.color = {0, 0, 0, 1};
    backdrop_->submit({&black, 1});
    const auto tw=config_.trim.width?float(config_.trim.width):available*dip*.4f*trim_source_width_/trim_source_height_;
    const auto th=tw*trim_source_height_/trim_source_width_;
    trim_bounds_=rect((w-tw/dip)/2+config_.trim.x/dip,(available-th/dip)/2+config_.trim.y/dip,tw/dip,th/dip);
    if(trim_visible_)gfx::check(ag_bounds_set(trim_display_->handle(),trim_bounds_));
    const auto selected=trim_selected_&&trim_visible_?trim_bounds_:bounds_;
    float line = 2 / dip;
    std::array<ag_rect, 4> e{rect(selected.left, selected.top, selected.right - selected.left, line),
                             rect(selected.left, selected.bottom - line, selected.right - selected.left, line),
                             rect(selected.left, selected.top, line, selected.bottom - selected.top),
                             rect(selected.right - line, selected.top, line, selected.bottom - selected.top)};
    for (unsigned i = 0; i < 4; i++)
    {
        edges_[i]->bounds(e[i]);
        ag_draw d{};
        d.kind = AG_FILL_RECT;
        d.rect = rect(0, 0, e[i].right - e[i].left, e[i].bottom - e[i].top);
        d.color = {0, 1, 0, 1};
        edges_[i]->submit({&d, 1});
        gfx::check(ag_visible(edges_[i]->handle(), editing_ && full));
    }
    if(trim_visible_)SetWindowPos(static_cast<HWND>(trim_display_->hwnd()),HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    for(auto&e:edges_)SetWindowPos(static_cast<HWND>(e->hwnd()),HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetWindowPos(static_cast<HWND>(warning_.hwnd()),HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    warning_.bounds(rect(0, std::max(0.f, h - 24), w, 24));
    gfx::check(ag_visible(warning_.handle(), !full || !warning_text_.empty()));
    present();
    if(trim_visible_&&trim_display_)trim_display_->render(double(qpc())/frequency());
}
LRESULT CALLBACK Ui::surface_input(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data)
{
    auto self=reinterpret_cast<Ui*>(data);
    if (message == WM_NCDESTROY)
    {
        self->detach_input(unsigned(id));
        return DefSubclassProc(window, message, w, l);
    }
    if (self->ready_ && !self->dialog_ && self->editing_ && alignment_key(w))
    {
        // ASS owns the message loop. Preserve only our alignment keys through
        // its dialog-navigation pass; Options retains normal keyboard handling.
        if (message == WM_GETDLGCODE)
            return DefSubclassProc(window, message, w, l) | DLGC_WANTMESSAGE;
        if (message == WM_KEYDOWN && id != 0 && self->input_windows_[0])
            return SendMessageW(self->input_windows_[0], message, w, l);
    }
    if(message==WM_LBUTTONDOWN&&id!=0&&self->ready_&&!self->dialog_){
        self->trim_selected_=id==2&&self->trim_visible_;
        SetFocus(self->input_windows_[0]);
        try{self->layout();}catch(...){}
    }
    return DefSubclassProc(window,message,w,l);
}
void Ui::attach_input(HWND window, unsigned id)
{
    if (!SetWindowSubclass(window, surface_input, id, reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("DU surface input routing unavailable");
    input_windows_[id] = window;
}
void Ui::detach_input(unsigned id) noexcept
{
    if (auto window = std::exchange(input_windows_[id], nullptr))
        RemoveWindowSubclass(window, surface_input, id);
}
void Ui::trim_option_visibility()
{
    if(!dialog_||saving_profile_||!controls_.contains(TRIM)||!controls_.contains(TRIM_ALWAYS))return;
    const auto unit=unsigned(controls_.at(UNIT).value());
    gfx::check(ag_visible(controls_.at(TRIM).handle(),unit==4||unit==5));
    gfx::check(ag_visible(controls_.at(TRIM_ALWAYS).handle(),unit==5));
    controls_.at(TRIM_ALWAYS).enabled(unit==5&&controls_.at(TRIM).value()!=0);
}
void Ui::main_texture_budget(uint64_t bytes)
{
    auto m=display_->metrics();
    ag_display_desc d{sizeof d,1,uint32_t(warp_),AG_RASTER_OUTPUT,662,685,662,685,
                      std::max(1u,m.viewport_width),std::max(1u,m.viewport_height),bytes};
    display_->configure(d);
}
void Ui::update_trim(const View&main,const TrimView&trim)
{
    const bool configured=config_.trim.enabled&&(config_.unit==4||config_.unit==5);
    if(!configured){
        trim_visible_=trim_selected_=false;trim_error_.clear();
        if(trim_display_){detach_input(2);trim_display_.reset();psx::du::transport::tcp::release_model(trim_replay_cpu_);trim_replay_cpu_=0;}
        if(trim_budget_reserved_){try{main_texture_budget(256ull*1024*1024);trim_budget_reserved_=false;}catch(const gfx::Error&e){trim_error_=e.detail;}}
        return;
    }
    bool visible=trim.frame&&trim.frame->header.kind!=3&&main.source_pid&&
        trim.origin.pid==main.source_pid&&trim.origin.created==main.source_creation;
    if(config_.unit==5)visible=visible&&main.frame&&main.connected&&main.source_ready&&trim.connected&&trim.observation_current&&
        main.last_received&&qpc()-main.last_received<2*frequency()&&trim.received&&qpc()-trim.received<2*frequency()&&
        psx::instrument::powered_lower_overlay(main.frame->header,trim.frame->header,config_.trim.always_show);
    bool relayout=visible!=trim_visible_;
    try{
        if(visible){
            const auto&frame=*trim.frame;
            const bool dimensions=trim_source_width_!=frame.header.width||trim_source_height_!=frame.header.height;
            if(!trim_display_){
                // Fixed sub-budgets keep the aggregate texture ceiling at 256 MiB.
                // Failure to reserve the instrument leaves the current DU intact.
                main_texture_budget(224ull*1024*1024);trim_budget_reserved_=true;
                ag_display_desc d{sizeof d,1,uint32_t(warp_),AG_RASTER_OUTPUT,float(frame.header.width),float(frame.header.height),frame.header.width,frame.header.height,1,1,32ull*1024*1024};
                auto candidate=std::make_unique<gfx::Display>(context_,window_.hwnd(),rect(0,0,1,1),d);
                gfx::check(ag_context_requests(candidate->handle(),context_request,this));
                attach_input(static_cast<HWND>(candidate->hwnd()), 2);
                trim_display_=std::move(candidate);
                trim_shown_sequence_=0;relayout=true;
            }else if(dimensions){
                ag_display_desc d{sizeof d,1,uint32_t(warp_),AG_RASTER_OUTPUT,float(frame.header.width),float(frame.header.height),frame.header.width,frame.header.height,1,1,32ull*1024*1024};
                trim_display_->configure(d);relayout=true;
            }
            trim_source_width_=frame.header.width;trim_source_height_=frame.header.height;
            const bool changed=trim_shown_epoch_!=trim.epoch||trim_shown_sequence_!=frame.header.sequence;
            if(changed){
                auto trace=diagnostics_?diagnostics_->trace():nullptr;
                if(trace){trace->event(2,{trim.correlation,qpc(),0,frame.header.end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::ui_adopt),trim.connection});trace->event(2,{trim.correlation,qpc(),0,frame.header.end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::render_enter),trim.connection});}
                submit_replay(*trim_display_,&frame,trim_replay_cpu_);
                trim_shown_epoch_=trim.epoch;trim_shown_sequence_=frame.header.sequence;
                trim_display_->render(double(qpc())/frequency());
                if(trace)trace->event(2,{trim.correlation,qpc(),0,frame.header.end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::render_return),trim.connection});
            }
            if(!changed)trim_display_->render(double(qpc())/frequency());
            trim_error_.clear();
        }
    }catch(const gfx::Error&e){visible=false;trim_error_=L"STAB TRIM replay unavailable: "+e.detail;}
     catch(const std::exception&){visible=false;trim_error_=L"STAB TRIM unavailable; main DU continues";}
    if(!visible&&!trim_display_&&trim_budget_reserved_){try{main_texture_budget(256ull*1024*1024);trim_budget_reserved_=false;}catch(...) {}}
    if(!visible)trim_selected_=false;
    if(visible!=trim_visible_)relayout=true;
    trim_visible_=visible;
    if(trim_display_&&relayout)gfx::check(ag_visible(trim_display_->handle(),visible));
    if(relayout)layout();
}
bool Ui::present()
{
    try
    {
        display_->render(double(qpc()) / frequency());
        const auto metrics = display_->metrics();
        if (metrics.suspended)
            return false;
        if (!metrics.error)
        {
            if (draw_pending_)
            {
                if (metrics.source_draws > draw_baseline_)
                {++drawn_frames_;runtime_->recovered(FailureStage::presentation,shown_epoch_,shown_sequence_,true);}
                draw_pending_ = false;
            }
            replay_error_.clear();
            runtime_->recovered(FailureStage::presentation,shown_epoch_,shown_sequence_);
            return true;
        }
        gfx::check(metrics.error);
        return false;
    }
    catch (const gfx::Error &e)
    {
        replay_error_ = e.detail;
        runtime_->failure(FailureStage::presentation,shown_epoch_,shown_sequence_,uint32_t(e.result),e.detail);
        return false;
    }
}
void Ui::tick()
{
    if (!ready_ || !runtime_)
        return;
    auto v = runtime_->view(true);
    if (diagnostics_ && diagnostics_->failed())
        error(L"Qualification diagnostics write failed");
    if (qualification_deadline_ && qpc() >= qualification_deadline_ && !exiting_)
    {
        qualification_deadline_ = 0;
        save();
        runtime_->shutdown();
        exiting_ = true;
    }
    if (exiting_)
    {
        if (v.stopped)
        {
            runtime_->join();
            window_.defer_close(false);
            window_.close();
        }
        return;
    }
    if (!v.frame && v.connection_epoch != shown_epoch_)
    {
        draw_pending_ = false;
        submit_replay(*display_,nullptr,main_replay_cpu_);
        shown_epoch_ = v.connection_epoch;
        shown_sequence_ = 0;
        present();
    }
    if (v.frame && (v.connection_epoch != shown_epoch_ || v.frame->header.sequence != shown_sequence_))
    {
        try
        {
            if(diagnostics_)diagnostics_->trace()->event(2,{v.correlation,qpc(),0,v.frame->header.end,GetCurrentThreadId(),0,uint16_t(config_.unit),uint16_t(psx::norm::Stage::render_enter),v.connection});
            submit_replay(*display_,v.frame.get(),main_replay_cpu_);
            draw_baseline_ = display_->metrics().source_draws;
            draw_pending_ = true;
            if(diagnostics_)diagnostics_->trace()->event(2,{v.correlation,qpc(),0,v.frame->header.end,GetCurrentThreadId(),0,uint16_t(config_.unit),uint16_t(psx::norm::Stage::render_return),v.connection});
            snapshot_error_.clear();
            shown_epoch_ = v.connection_epoch;
            shown_sequence_ = v.frame->header.sequence;
            bool presented = present();
            runtime_->acknowledge(shown_epoch_, shown_sequence_);
            if (diagnostics_ && presented)
                diagnostics_->sample(v, qpc(), display_->metrics().cpu_ms, display_->metrics().frames);
        }
        catch (const gfx::Error &e)
        {
            snapshot_error_ = e.detail;
            shown_epoch_ = v.connection_epoch;
            shown_sequence_ = v.frame->header.sequence;
            runtime_->rejected(shown_epoch_, shown_sequence_,uint32_t(e.result),e.detail);
        }
    }
    else if(v.frame && diagnostics_)
        diagnostics_->trace()->event(2,{v.correlation,qpc(),0,v.frame->header.end,GetCurrentThreadId(),0,uint16_t(config_.unit),uint16_t(psx::norm::Stage::unchanged),v.connection});
    if (!replay_error_.empty())
        present();
    const auto trim=runtime_->trim_view();
    update_trim(v,trim);
    auto message = !error_.empty()            ? error_
                   : !snapshot_error_.empty() ? snapshot_error_
                   : !replay_error_.empty()   ? replay_error_
                                              : v.status;
    if(!trim_error_.empty()){if(!message.empty())message+=L" — ";message+=trim_error_;}
    if(config_.trim.enabled&&(config_.unit==4||config_.unit==5)&&!trim.status.empty()){if(!message.empty())message+=L" — ";message+=trim.status;}
    if (message != warning_text_)
    {
        warning_text_ = message;
        warning_.text(message.c_str());
        gfx::check(ag_visible(warning_.handle(), gfx::placement(window_).mode != 2 || !message.empty()));
    }
    if (rates_.sample(qpc(), frequency(), v.validated, drawn_frames_) ||
        status_connected_ != v.connected || status_text_.empty())
    {
        wchar_t status[160]{};
        swprintf_s(status, L"%ls | received %.1f fps | drawn %.1f fps",
                   v.connected ? L"connected" : L"not connected", rates_.received, rates_.drawn);
        status_connected_ = v.connected;
        status_text_ = status;
        if (controls_.contains(STATUS))
            controls_.at(STATUS).text(status_text_.c_str());
    }
}
void Ui::options()
{
    if (dialog_)
    {
        dialog_->show(true, true);
        return;
    }
    saving_profile_ = false;
    reset_alignment_ = false;
    dialog_ = std::make_unique<gfx::Dialog>(window_, L"DU Options", 550, 494, callback, this);
    auto parent = dialog_->hwnd();
    auto label=[&](void*p,const wchar_t*t,float y){labels_.push_back(control(p,AG_LABEL,0,t,rect(12,y,190,26)));};
    label(parent,L"Display unit",12);controls_.emplace(UNIT,control(parent,AG_CHOICE,UNIT,L"",rect(200,12,300,30)));
    for(auto t:{L"Captain outboard",L"Captain inboard",L"First officer inboard",L"First officer outboard",L"Upper EICAS",L"Lower EICAS"})controls_.at(UNIT).add(t);controls_.at(UNIT).value(config_.unit);
    label(parent,L"Input",58);controls_.emplace(SOURCE_MODE,control(parent,AG_CHOICE,SOURCE_MODE,L"",rect(200,58,300,30)));
    controls_.at(SOURCE_MODE).add(L"Network TCP");controls_.at(SOURCE_MODE).add(L"Direct local pipe");controls_.at(SOURCE_MODE).value(config_.network?0:1);
    label(parent,L"Host address or name",104);controls_.emplace(HOST,control(parent,AG_EDIT,HOST,std::wstring(config_.host.begin(),config_.host.end()).c_str(),rect(200,104,300,30)));
    label(parent,L"Network adapter: automatic",150);
    label(parent,L"DU host port",196);controls_.emplace(TCP_PORT,control(parent,AG_NUMERIC,TCP_PORT,L"",rect(200,196,300,28),1,65535));controls_.at(TCP_PORT).value(config_.tcp_port);
    controls_.emplace(TRIM,control(parent,AG_CHECKBOX,TRIM,L"STAB TRIM INDICATOR",rect(12,238,360,30)));
    controls_.at(TRIM).value(config_.trim.enabled?1:0);
    controls_.emplace(TRIM_ALWAYS,control(parent,AG_CHECKBOX,TRIM_ALWAYS,L"Always show, including active EICAS pages",rect(28,276,490,30)));
    controls_.at(TRIM_ALWAYS).value(config_.trim.always_show?1:0);
    controls_.emplace(RESET,control(parent,AG_BUTTON,RESET,L"Reset fullscreen position",rect(12,322,242,30)));
    controls_.emplace(DIAGNOSTICS,control(parent,AG_CHECKBOX,DIAGNOSTICS,L"Diagnostics (this run only)",rect(270,322,265,30)));
    controls_.at(DIAGNOSTICS).value(diagnostics_enabled_);
    controls_.emplace(STATUS,control(parent,AG_LABEL,STATUS,status_text_.c_str(),rect(12,364,518,30)));
    controls_.emplace(SAVE_PROFILE,control(parent,AG_BUTTON,SAVE_PROFILE,L"Save profile…",rect(16,404,132,30)));
    controls_.emplace(NOTICE,control(parent,AG_LABEL,NOTICE,error_.c_str(),rect(16,446,518,36)));
    controls_.emplace(APPLY,control(parent,AG_BUTTON,APPLY,L"Apply",rect(284,404,76,30)));controls_.emplace(OK,control(parent,AG_BUTTON,OK,L"OK",rect(368,404,70,30)));controls_.emplace(CANCEL,control(parent,AG_BUTTON,CANCEL,L"Cancel",rect(446,404,86,30)));
    source_controls();
    trim_option_visibility();
    dialog_->show(!hidden_, !hidden_);
}
void Ui::save_profile_dialog()
{
    apply(false);
    dismiss();
    saving_profile_ = true;
    dialog_ = std::make_unique<gfx::Dialog>(window_, L"Save profile", 360, 170, callback, this);
    labels_.push_back(control(dialog_->hwnd(), AG_LABEL, 0, L"Profile number", rect(16, 16, 326, 24)));
    controls_.emplace(PROFILE, control(dialog_->hwnd(), AG_CHOICE, PROFILE, L"", rect(16, 46, 326, 30)));
    for (unsigned n = 1; n <= 10; ++n)
        controls_.at(PROFILE).add(std::to_wstring(n).c_str());
    controls_.at(PROFILE).value(profile_ - 1);
    controls_.emplace(NOTICE,
                      control(dialog_->hwnd(), AG_LABEL, NOTICE, error_.c_str(), rect(16, 82, 326, 24)));
    controls_.emplace(SAVE, control(dialog_->hwnd(), AG_BUTTON, SAVE, L"Save", rect(182, 120, 72, 30)));
    controls_.emplace(CANCEL, control(dialog_->hwnd(), AG_BUTTON, CANCEL, L"Cancel", rect(266, 120, 76, 30)));
    dialog_->show(!hidden_, !hidden_);
}

void Ui::dismiss()
{
    if (!dialog_)
        return;
    dialog_->show(false, false);
    controls_.clear();
    labels_.clear();
    dialog_.reset();
    reset_alignment_ = false;
}
void Ui::source_controls()
{
    const bool network = controls_.at(SOURCE_MODE).value() == 0;
    for (auto id : {HOST, TCP_PORT})
        controls_.at(id).enabled(network);
}
void Ui::reset_fullscreen_position()
{
    if (!dialog_ || saving_profile_)
        return;
    reset_alignment_ = true;
    reset_trim_ = trim_selected_ && trim_visible_;
    controls_.at(RESET).text(L"Reset fullscreen position (pending)");
}
void Ui::apply(bool close)
{
    if (!dialog_)
        return;
    if (saving_profile_)
    {
        save();
        store_.save(config_, unsigned(controls_.at(PROFILE).value()) + 1);
        if (profile_invalid_ && start_input_)
            runtime_->connect();
        profile_invalid_ = false;
        error_.clear();
        dismiss();
        return;
    }
    Config next = config_;
    next.unit = unsigned(controls_.at(UNIT).value());
    next.trim.enabled=controls_.at(TRIM).value()!=0;
    next.trim.always_show=controls_.at(TRIM_ALWAYS).value()!=0;
    next.network=controls_.at(SOURCE_MODE).value()==0;auto host=controls_.at(HOST).text();
    next.tcp_port=uint16_t(controls_.at(TCP_PORT).value());
    next.host.clear();for(auto ch:host){if(ch>127)throw std::runtime_error("Enter the DU host IPv4 address or computer name");next.host.push_back(char(ch));}

    if (reset_alignment_){if(reset_trim_)next.trim.x=next.trim.y=next.trim.width=0;else next.x=next.y=next.width=0;}
    validate(next);
    const bool changed = next.unit != config_.unit;
    const bool input=next.network!=config_.network||next.host!=config_.host||next.adapter_guid!=config_.adapter_guid||next.tcp_port!=config_.tcp_port;
    diagnostics_enabled_=controls_.at(DIAGNOSTICS).value()!=0;
    runtime_->diagnostics(diagnostics_enabled_);
    config_ = next;
    if (changed)
        runtime_->select_unit(config_.unit);
    runtime_->select_trim(config_.trim.enabled);
    if(!config_.trim.enabled||config_.unit<4){trim_selected_=false;trim_visible_=false;if(trim_display_)gfx::check(ag_visible(trim_display_->handle(),0));}
    if(input)runtime_->configure_source(config_.network,config_.host,config_.adapter_guid,config_.tcp_port);
    if (start_input_ && (input || changed || profile_invalid_))
        runtime_->connect();
    profile_invalid_ = false;
    if (input || changed)
        rates_.reset(qpc(), runtime_->view().validated, drawn_frames_);
    reset_alignment_ = false;
    controls_.at(RESET).text(L"Reset fullscreen position");
    layout();
    error_.clear();
    if (close)
        dismiss();
}

void Ui::event(unsigned id, unsigned kind)
{
    if (!ready_)
        return;
    if (kind == AG_CLOSE)
    {
        if (dialog_)
        {
            dismiss();
            return;
        }
        save();
        runtime_->shutdown();
        exiting_ = true;
        return;
    }
    if (kind == AG_RESIZE || kind == AG_VIEWPORT_CHANGED)
    {
        layout();
        return;
    }
    if (kind == AG_TIMER)
    {
        tick();
        return;
    }
    if (kind == AG_REFRESH)
    {
        present();
        return;
    }
    if (kind == AG_DIALOG_ACCEPT)
    {
        apply(true);
        return;
    }
    if (kind == AG_DIALOG_CANCEL)
    {
        dismiss();
        return;
    }
    if (kind == AG_SHORTCUT && !dialog_)
    {
        auto p = gfx::placement(window_);
        if (id == 1 || id == 2)
        {
            editing_ = false;
            gfx::borderless(window_, id == 1 && p.mode != 2);
            layout();
            save();
        }
        else if (id == 3 && p.mode == 2)
        {
            editing_ = !editing_;
            layout();
            if (!editing_)
                save();
        }
        else if (editing_ && p.mode == 2 && id >= 100)
        {
            auto value = id - 100, key = value % 256;
            int step = value >= 256 ? 10 : 1;
            const bool trim=trim_selected_&&trim_visible_;
            auto&x=trim?config_.trim.x:config_.x;auto&y=trim?config_.trim.y:config_.y;
            auto&width=trim?config_.trim.width:config_.width;const auto bounds=trim?trim_bounds_:bounds_;
            if (key == VK_LEFT)
                x -= step;
            if (key == VK_RIGHT)
                x += step;
            if (key == VK_UP)
                y -= step;
            if (key == VK_DOWN)
                y += step;
            if (key == VK_ADD || key == VK_OEM_PLUS || key == VK_SUBTRACT || key == VK_OEM_MINUS)
            {
                auto dip = GetDpiForWindow(static_cast<HWND>(window_.hwnd())) / 96.f;
                width = std::clamp(int(std::lround((bounds.right - bounds.left) * dip)) +
                                               ((key == VK_ADD || key == VK_OEM_PLUS) ? step : -step),
                                           32, 16384);
            }
            x = std::clamp(x, -65536, 65536);
            y = std::clamp(y, -65536, 65536);
            layout();
        }
        return;
    }
    if ((kind == AG_VALUE_CHANGED || kind == AG_DROPDOWN_CHANGED) && id == SOURCE_MODE)
    {
        source_controls();
        return;
    }
    if (((kind==AG_VALUE_CHANGED||kind==AG_DROPDOWN_CHANGED)&&id==UNIT)||
        ((kind==AG_VALUE_CHANGED||kind==AG_CLICK)&&id==TRIM)){trim_option_visibility();return;}
    if (kind != AG_CLICK)
        return;
    if (id == SAVE_PROFILE)
        save_profile_dialog();
    else if (id == APPLY || id == OK || id == SAVE)
        apply(id == OK || id == SAVE);
    else if (id == CANCEL)
        dismiss();
    else if (id == RESET)
        reset_fullscreen_position();
}
} // namespace du_client
