// HWIO application composition. ASS Graphics owns the UI mechanisms and font.
#include "hwio/axis_panel.hpp"
#include "hwio/catalogue.hpp"
#include "hwio/button_panel.hpp"
#include "hwio/flight_scene.hpp"
#include "hwio/tray_status.hpp"
#include "hwio/output_panel.hpp"
#include "hwio/cockpit_panel.hpp"
#include "hwio/module_panel.hpp"
#include "hwio/motor_panel.hpp"
#include "hwio/servo_panel.hpp"
#include "hwio/actuator_panel.hpp"
#include "hwio/application_instance.hpp"
#include "hwio/input_override_panel.hpp"
#include <application-icon.hpp>
#include <shellapi.h>
#include <CommCtrl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>

namespace {
namespace gfx = ass::graphics;
using namespace hwio;
enum : uint32_t {
    connect_id = 100,
    disconnect_id,
    settings_id,
    exit_id,
    title_id,
    status_id,
    axes_id,
    buttons_id,
    collapse_id,
    graphics_error_id = 109,
    assignment_tabs_id=111,
    flight_tabs_id=112,flight_fold_id=113,analogue_outputs_tabs_id=119,
    log_id = 200,
    smoothing_id,
    options_close_id,
    options_info_id,
    network_id,
    network_status_id,
    options_tabs_id,
    options_layout_id,
    fds_path_id,
    phidget_path_id=209,fds_folder_id=216,phidget_folder_id=217,
    options_dispatch = 103,
    close_dispatch = 104
};
struct Launch {
    RuntimeOptions options;
    std::wstring smoke;
    bool visible{},startup_check{};
    std::wstring catalogue_input, catalogue_output;
};
class HwioUi {
    bool primary_{}, ready_{}, sizing_{}, button_view_{}, exiting_{};
    int exit_code_{}, smoke_step_{};
    gfx::Context context_;
    gfx::Window window_;
    psx::windows::WindowIcons window_icons_;
    std::unique_ptr<gfx::Window> options_window_;
    std::optional<psx::windows::WindowIcons> options_icons_;
    gfx::Control options_tabs_,general_options_,override_options_;
    InputOverridePanel overrides_;
    std::unique_ptr<gfx::Tray> tray_;
    std::unique_ptr<gfx::Canvas> canvas_;
    std::vector<gfx::Control> controls_;
    std::map<uint32_t, std::size_t> ids_;
    std::unique_ptr<Runtime> runtime_;
    AxisPanel axes_;
    gfx::Control flight_tabs_,flight_page_,assignment_tabs_,input_page_,output_page_,encoder_page_,analogue_page_,motor_page_,module_page_;
    ButtonPanel buttons_;
    OutputPanel outputs_;
    CockpitPanel encoders_,analogue_;
    ModulePanel modules_;
    MotorPanel motors_;
    ActuatorPanel actuator_mappings_;gfx::Control analogue_outputs_tabs_,motors_subpage_,actuator_subpage_;
    FlightDrawing drawing_;
    RuntimeView view_;
    TrayHealth health_;
    std::vector<ControllerRequirement> required_;
    std::uint64_t profile_revision_{UINT64_MAX}, axis_revision_{UINT64_MAX}, button_revision_{UINT64_MAX},hardware_revision_{UINT64_MAX},cockpit_revision_{UINT64_MAX};
    std::future<void> shutdown_;
    Launch launch_;
    std::wstring last_tip_, last_status_;
    std::vector<std::wstring> adapter_guids_;
    std::uint64_t adapter_revision_{UINT64_MAX};
    std::string failure_;
    std::ostringstream smoke_log_;
    std::uint64_t frames_{}, hidden_frames_{}, measure_frames_{}, measure_started_{}, last_frame_us_{}, max_gap_us_{};
    bool smoke_ok_{true}, smoke_done_{};
    bool graphics_visible_{true};
    unsigned graphics_failures_{};
    std::uint64_t graphics_checked_frame_{}, graphics_retry_{};
    std::uint64_t measure_paints_{}, smoke_warm_until_{};
    std::vector<RECT> smoke_monitors_;
    std::size_t smoke_monitor_{};
    gfx::Control &control(uint32_t id) {
        return controls_.at(ids_.at(id));
    }
    void add(uint32_t id, uint32_t kind, const wchar_t *text, ag_rect bounds, void *parent = nullptr) {
        ag_control_desc d{};
        d.id = id;
        d.kind = kind;
        d.text = text;
        d.bounds = bounds;
        d.maximum = 1;
        d.step = 1;
        controls_.emplace_back(context_, parent ? parent : window_.hwnd(), d, callback, this);
        ids_[id] = controls_.size() - 1;
    }
    static void __cdecl callback(void *user, uint32_t id, uint32_t kind, double) noexcept {
        auto &self = *static_cast<HwioUi *>(user);
        try {
            self.event(id, kind);
        } catch (const std::exception &e) {
            self.failure_ = e.what();
            self.exit_code_ = 1;
            ag_window_post(self.window_.handle(), close_dispatch);
        }
    }
    static void __cdecl options_callback(void *user, uint32_t, uint32_t kind, double) noexcept {
        auto &self = *static_cast<HwioUi *>(user);
        if (kind == AG_CLOSE)
            ag_window_post(self.window_.handle(), options_dispatch);
        else if(kind==AG_RESIZE)ag_window_post(self.window_.handle(),options_layout_id);
    }
    void build() {
        add(connect_id, AG_BUTTON, L"Connect", {14, 8, 104, 36});
        add(disconnect_id, AG_BUTTON, L"Disconnect", {112, 8, 218, 36});
        add(title_id, AG_LABEL, L"Calculated control outputs", {232, 12, 484, 34});
        add(settings_id, AG_BUTTON, L"Settings", {610, 8, 694, 36});
        add(exit_id, AG_BUTTON, L"Exit", {704, 8, 786, 36});
        add(axes_id, AG_BUTTON, L"Axes", {228, 8, 306, 36});
        add(buttons_id, AG_BUTTON, L"Buttons", {314, 8, 400, 36});
        add(collapse_id, AG_BUTTON, L"Hide", {408, 8, 484, 36});
        add(status_id, AG_LABEL, L"Starting...", {14, 365, 786, 389});
        canvas_ = std::make_unique<gfx::Canvas>(context_, window_.hwnd(), ag_rect{0, 44, 800, 360});
        gfx::check(ag_canvas_present_mode(canvas_->handle(), 1));
        add(graphics_error_id, AG_STATUS, L"Graphics unavailable; retrying.", {96, 136, 704, 188});
        ag_visible(control(graphics_error_id).handle(), 0);
        add(flight_fold_id,AG_BUTTON,L"Expand or collapse assignments",{0,392,800,400});ag_control_style(control(flight_fold_id).handle(),AG_STYLE_FOLDOUT);
        ag_control_desc flight_tabs{};flight_tabs.kind=AG_TABS;flight_tabs.id=flight_tabs_id;flight_tabs.bounds={0,400,800,700};flight_tabs.text=L"";
        flight_tabs_=gfx::Control(context_,window_.hwnd(),flight_tabs,callback,this);flight_page_=flight_tabs_.tab(L"Flight axes");module_page_=flight_tabs_.tab(L"Modules");flight_tabs_.equal_width(true);flight_tabs_.value(0);
        axes_.attach(context_, window_, *runtime_,flight_page_.hwnd());
        ag_control_desc tabs{};tabs.kind=AG_TABS;tabs.id=assignment_tabs_id;tabs.bounds={810,0,1500,700};tabs.text=L"";
        assignment_tabs_=gfx::Control(context_,window_.hwnd(),tabs,callback,this);
        input_page_=assignment_tabs_.tab(L"Digital inputs");output_page_=assignment_tabs_.tab(L"Digital outputs");encoder_page_=assignment_tabs_.tab(L"Encoders");analogue_page_=assignment_tabs_.tab(L"Analogue inputs");motor_page_=assignment_tabs_.tab(L"Analogue outputs");assignment_tabs_.equal_width(true);assignment_tabs_.value(0);
        buttons_.attach(context_, window_, *runtime_,input_page_.hwnd());
        outputs_.attach(context_,window_,*runtime_,output_page_.hwnd());
        encoders_.attach(context_,window_,*runtime_,true,encoder_page_.hwnd());analogue_.attach(context_,window_,*runtime_,false,analogue_page_.hwnd());
        modules_.attach(context_,window_,*runtime_,module_page_.hwnd());
        ag_control_desc analogue_tabs{};analogue_tabs.kind=AG_TABS;analogue_tabs.id=analogue_outputs_tabs_id;analogue_tabs.text=L"";analogue_tabs.bounds={0,0,690,664};
        analogue_outputs_tabs_=gfx::Control(context_,motor_page_.hwnd(),analogue_tabs,callback,this);motors_subpage_=analogue_outputs_tabs_.tab(L"Devices");actuator_subpage_=analogue_outputs_tabs_.tab(L"Mappings");analogue_outputs_tabs_.equal_width(true);analogue_outputs_tabs_.value(0);
        motors_.attach(context_,window_,*runtime_,motors_subpage_.hwnd());actuator_mappings_.attach(context_,window_,*runtime_,actuator_subpage_.hwnd());
        const auto path = runtime_->data_dir() + L"\\settings.cfg";
        button_view_ = GetPrivateProfileIntW(L"button-pane", L"selected", 0, path.c_str()) != 0;
        axes_.set_layout_handler([this] { layout(); });
    }
    int assignment_header_width() const {
        const auto tab=static_cast<HWND>(assignment_tabs_.hwnd());const auto dc=GetDC(tab);if(!dc)return 690;
        const auto font=reinterpret_cast<HFONT>(SendMessageW(tab,WM_GETFONT,0,0));const auto previous=font?SelectObject(dc,font):nullptr;
        const int count=TabCtrl_GetItemCount(tab);int widest{};
        for(int n=0;n<count;++n){wchar_t caption[128]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=caption;item.cchTextMax=128;SIZE size{};if(SendMessageW(tab,TCM_GETITEMW,n,reinterpret_cast<LPARAM>(&item)) && GetTextExtentPoint32W(dc,caption,int(wcslen(caption)),&size))widest=std::max(widest,int(size.cx));}
        if(previous)SelectObject(dc,previous);ReleaseDC(tab,dc);const int dpi=int(GetDpiForWindow(tab));
        return dpi>0?MulDiv((widest+MulDiv(20,dpi,96))*count+MulDiv(4,dpi,96),96,dpi):690;
    }
    void layout() {
        if (!ready_ || sizing_)
            return;
        sizing_ = true;
        struct LayoutCompletion {bool& active;~LayoutCompletion(){active=false;}} completion{sizing_};
        ag_rect work{};
        gfx::check(ag_window_work_area(window_.handle(), &work));
        auto plan =
            assignment_layout(static_cast<int>(work.right), static_cast<int>(work.bottom), axes_.expanded());
        if(axes_.expanded() && !plan.narrow && plan.button_width<assignment_header_width())
            plan=assignment_layout(0,static_cast<int>(work.bottom),true);
        const bool tabs = axes_.expanded() && plan.narrow, buttons_only = tabs && button_view_;
        const bool compact_axes = axes_.expanded() && !buttons_only && plan.height < 600;
        gfx::check(ag_window_client_size(window_.handle(), plan.width, plan.height));
        for (const auto id : {axes_id, buttons_id, collapse_id})
            gfx::check(ag_visible(control(id).handle(), tabs));
        gfx::check(ag_visible(control(title_id).handle(), !tabs));
        ag_control_style(control(axes_id).handle(), AG_STYLE_FLAT | (!button_view_ ? AG_STYLE_SELECTED : 0));
        ag_control_style(control(buttons_id).handle(), AG_STYLE_FLAT | (button_view_ ? AG_STYLE_SELECTED : 0));
        graphics_visible_ = !buttons_only && !compact_axes;
        gfx::check(ag_visible(canvas_->handle(), graphics_visible_));
        ag_visible(control(graphics_error_id).handle(), graphics_visible_ && graphics_failures_ >= 3);
        const float status_top = buttons_only || compact_axes ? float(plan.height - 28) : 365.f;
        control(status_id).bounds({14, status_top, 786, status_top + 24});
        const float left_top=compact_axes?44.f:392.f;const bool left_visible=!buttons_only;
        control(flight_fold_id).bounds({0,left_top,800,left_top+8});ag_visible(control(flight_fold_id).handle(),left_visible);ag_control_style(control(flight_fold_id).handle(),AG_STYLE_FOLDOUT|(axes_.expanded()?AG_STYLE_EXPANDED:0));
        flight_tabs_.bounds({0,left_top+8,800,float(std::max(plan.height-32,int(left_top)+70))});ag_visible(flight_tabs_.handle(),left_visible && axes_.expanded());
        const auto left_selected=flight_tabs_.value();const auto left_page=left_selected==1?module_page_.viewport():flight_page_.viewport();const int left_height=int(left_page.bottom-left_page.top);
        axes_.arrange(left_height,left_visible && axes_.expanded() && left_selected==0,0);
        modules_.arrange(0,0,int(left_page.right-left_page.left),left_height,left_visible && axes_.expanded() && left_selected==1);
        const bool assignments=axes_.expanded() && (!plan.narrow || button_view_);
        assignment_tabs_.bounds(assignment_tab_bounds(plan));
        ag_visible(assignment_tabs_.handle(),assignments);
        const auto selected=assignment_tabs_.value();const gfx::Control* pages[]{&input_page_,&output_page_,&encoder_page_,&analogue_page_,&motor_page_};const auto page=pages[std::clamp(int(selected),0,4)]->viewport();
        buttons_.arrange(0,0,int(page.right-page.left),int(page.bottom-page.top),assignments && selected==0);
        outputs_.arrange(0,0,int(page.right-page.left),int(page.bottom-page.top),assignments && selected==1);
        encoders_.arrange(0,0,int(page.right-page.left),int(page.bottom-page.top),assignments && selected==2);
        analogue_.arrange(0,0,int(page.right-page.left),int(page.bottom-page.top),assignments && selected==3);
        analogue_outputs_tabs_.bounds({0,0,page.right-page.left,page.bottom-page.top});
        const auto analogue_selected=analogue_outputs_tabs_.value();const gfx::Control* analogue_pages[]{&motors_subpage_,&actuator_subpage_};const auto analogue_view=analogue_pages[std::clamp(int(analogue_selected),0,1)]->viewport();
        motors_.arrange(0,0,int(analogue_view.right-analogue_view.left),int(analogue_view.bottom-analogue_view.top),assignments && selected==4 && analogue_selected==0);
        actuator_mappings_.arrange(0,0,int(analogue_view.right-analogue_view.left),int(analogue_view.bottom-analogue_view.top),assignments && selected==4 && analogue_selected==1);

    }
    void options() {
        if (!options_window_) {
            options_window_ =
                std::make_unique<gfx::Window>(context_, L"HWIO - Settings", 640, 640, options_callback, this);
            options_icons_.emplace(static_cast<HWND>(options_window_->hwnd()));
            options_window_->defer_close(true);
            ag_control_desc tabs{};tabs.kind=AG_TABS;tabs.id=options_tabs_id;tabs.text=L"";tabs.bounds={12,12,620,590};
            options_tabs_=gfx::Control(context_,options_window_->hwnd(),tabs,callback,this);general_options_=options_tabs_.tab(L"General");override_options_=options_tabs_.tab(L"Input overrides");options_tabs_.equal_width(true);options_tabs_.value(0);
            overrides_.attach(context_,*runtime_,override_options_.hwnd());
            auto parent = general_options_.hwnd();
            add(210, AG_LABEL, L"Log level", {28, 30, 194, 52}, parent);
            add(log_id, AG_CHOICE, L"", {28, 58, 194, 84}, parent);
            add(211, AG_LABEL, L"Spike smooth interval", {220, 30, 414, 52}, parent);
            add(smoothing_id, AG_CHOICE, L"", {220, 58, 412, 84}, parent);
            for (const auto *name : {L"Off", L"Info", L"Warn"})
                control(log_id).add(name);
            for (const auto *name : {L"10 ms", L"25 ms", L"50 ms"})
                control(smoothing_id).add(name);
            add(212, AG_LABEL, L"Use network interface: ", {28, 102, 412, 124}, parent);
            add(network_id, AG_CHOICE, L"", {28, 130, 412, 158}, parent);
            add(network_status_id, AG_LABEL, L"", {28, 166, 412, 200}, parent);
            add(213, AG_LABEL, L"FDS library folder or DLL (x64 API 2.0.1.0)", {28, 214, 576, 240}, parent);
            add(fds_path_id, AG_EDIT, L"", {28, 246, 460, 278}, parent);
            add(fds_folder_id,AG_BUTTON,L"Choose folder",{470,246,600,278},parent);
            ag_control_events(control(fds_path_id).handle(),AG_CONTROL_NAV_TAB|AG_CONTROL_NOTIFY_FOCUS|AG_CONTROL_NAV_ACTIVATE);
            add(214,AG_LABEL,L"Phidgets library folder or DLL (Phidget22 x64)",{28,290,600,316},parent);
            add(phidget_path_id,AG_EDIT,L"",{28,322,460,354},parent);
            add(phidget_folder_id,AG_BUTTON,L"Choose folder",{470,322,600,354},parent);
            ag_control_events(control(phidget_path_id).handle(),AG_CONTROL_NAV_TAB|AG_CONTROL_NOTIFY_FOCUS|AG_CONTROL_NAV_ACTIVATE);
            add(215,AG_LABEL,L"Leave blank to find an installed library or use libraries/phidgets and libraries/fds beside HWIO. Otherwise choose the folder containing phidget22.dll or interfaceITAPI x64.dll. Phidgets folder changes take effect after restarting HWIO.",{28,366,600,446},parent);
            add(options_info_id, AG_LABEL, L"Changes save immediately.", {28, 452, 600, 494}, parent);
            add(options_close_id, AG_BUTTON, L"Close", {520, 598, 620, 628}, options_window_->hwnd());
        }
        const auto settings = runtime_->settings();
        control(log_id).value(static_cast<int>(settings.log_level));
        control(smoothing_id).value(settings.smoothing_ms == 10 ? 0 : settings.smoothing_ms == 50 ? 2 : 1);
        control(fds_path_id).text(settings.fds_api_path.c_str());
        control(phidget_path_id).text(settings.phidget_api_path.c_str());
        update_adapters(true);
        options_window_->fit();
        options_window_->show();
        layout_options();
    }
    void layout_options(){
        if(!options_window_ || !ids_.count(options_close_id))return;
        const auto hwnd=static_cast<HWND>(options_window_->hwnd());RECT bounds{};GetClientRect(hwnd,&bounds);const auto scale=96.0f/float(GetDpiForWindow(hwnd));const float width=std::max(344.0f,float(bounds.right)*scale),height=std::max(320.0f,float(bounds.bottom)*scale);
        options_tabs_.bounds({12,12,std::max(332.0f,width-12),std::max(272.0f,height-46)});control(options_close_id).bounds({std::max(232.0f,width-112),height-38,width-12,height-8});
        const auto viewport=override_options_.viewport();const bool selected=options_tabs_.value()==1 && IsWindowVisible(hwnd) && !IsIconic(hwnd);
        overrides_.arrange(int(viewport.right-viewport.left),int(viewport.bottom-viewport.top),selected);
    }
    void save_options() {
        auto settings=runtime_->settings();
        settings.log_level = static_cast<LogLevel>(static_cast<int>(control(log_id).value()));
        const auto selected = static_cast<int>(control(smoothing_id).value());
        settings.smoothing_ms = selected == 0 ? 10 : selected == 2 ? 50 : 25;
        const auto adapter=static_cast<std::size_t>(control(network_id).value());
        if(adapter<adapter_guids_.size())settings.network_interface=adapter_guids_[adapter];
        settings.network_selection_initialized=true;
        std::string error;
        control(options_info_id)
            .text(runtime_->save_settings(settings, error) ? L"Saved. Acquisition and delivery remain independent."
                                                           : wide(error).c_str());
        update_adapters(true);
    }
    void update_adapters(bool force=false) {
        if(!options_window_)return;
        const auto inventory=runtime_->adapters();if(!inventory || (!force && adapter_revision_==inventory->revision))return;
        adapter_revision_=inventory->revision;
        const auto settings=runtime_->settings();
        std::vector<std::wstring> labels{L"Select network interface"};adapter_guids_={L""};
        int selected=0;std::wstring status=L"Select an adapter for hardware devices. PSX is independent.";
        for(const auto& adapter:inventory->adapters){
            adapter_guids_.push_back(adapter.guid);labels.push_back(adapter.name);
            if(adapter.guid==settings.network_interface){selected=static_cast<int>(labels.size()-1);status=adapter.up?L"Available · Hardware networking only":L"Unavailable · Waiting for this adapter";}
        }
        if(!settings.network_interface.empty() && !selected){
            adapter_guids_.push_back(settings.network_interface);labels.push_back(L"Saved adapter (unavailable)");selected=static_cast<int>(labels.size()-1);
            status=L"Unavailable · No automatic adapter substitution";
        }
        if(!inventory->error.empty())status=wide(inventory->error);
        std::vector<const wchar_t*> choices;for(const auto& label:labels)choices.push_back(label.c_str());
        control(network_id).replace(choices,selected);control(network_status_id).text(status.c_str());
    }
    void tray_update() {
        if (profile_revision_ != view_.profile_revision || axis_revision_ != view_.axis_revision ||
            button_revision_ != view_.button_revision || hardware_revision_!=view_.hardware_revision || cockpit_revision_!=view_.cockpit_revision) {
            auto axes = std::make_unique<AxisConfiguration>(runtime_->axes());
            required_ = controller_requirements(runtime_->profiles(), *axes, runtime_->buttons(),
                                                runtime_->display_assignments(),runtime_->cockpit());
            profile_revision_ = view_.profile_revision;
            axis_revision_ = view_.axis_revision;
            button_revision_ = view_.button_revision;
            hardware_revision_=view_.hardware_revision;
            cockpit_revision_=view_.cockpit_revision;
            for(const auto& output:runtime_->hardware().outputs)if(output.enabled && std::none_of(required_.begin(),required_.end(),[&](const auto& r){return r.profile.identity.backend==BackendKind::pokeys_ethernet && r.profile.identity.hardware_key==output.device;})){
                ControllerRequirement r;r.profile.id="hardware-output";r.profile.identity.backend=BackendKind::pokeys_ethernet;r.profile.identity.hardware_key=output.device;r.profile.identity.name=output.device;required_.push_back(std::move(r));
            }
        }
        const auto health = health_.update(required_, view_, GetTickCount64());
        gfx::check(ag_tray_status(tray_->handle(), health.controllers.c_str(), health.controllers_state,
                                  health.psx.c_str(), health.psx_state));
        const auto status = health.psx + L" | " + health.controllers + L" | Master: " +
                            (view_.axes.effective_fo ? L"FO" : L"CPT") + L" | Calculated";
        if (last_status_ != status) {
            last_status_ = status;
            control(status_id).text(status.c_str());
        }
        gfx::check(ag_tooltip_set(
            control(status_id).handle(),
            (health.detail.substr(0, 3600) +
             L"\nCalculated positions; TCP completion is not PSX acknowledgement. PSX FPS is not exposed by Main TCP.")
                .c_str()));
    }
    void graphics_health() {
        if (!graphics_visible_ || !IsWindowVisible(static_cast<HWND>(window_.hwnd())) ||
            IsIconic(static_cast<HWND>(window_.hwnd())))
            return;
        ag_metrics metrics{};
        const auto result = ag_surface_metrics(canvas_->handle(), &metrics);
        if (metrics.frames == graphics_checked_frame_)
            return;
        graphics_checked_frame_ = metrics.frames;
        if (result >= 0) {
            if (graphics_failures_ >= 3)
                ag_visible(control(graphics_error_id).handle(), 0);
            graphics_failures_ = 0;
            return;
        }
        ++graphics_failures_;
        if (graphics_failures_ >= 3) {
            control(graphics_error_id).status(L"Graphics unavailable; retrying.", 2);
            ag_visible(control(graphics_error_id).handle(), 1);
        }
        const auto now = GetTickCount64();
        if (now - graphics_retry_ >= 1000) {
            graphics_retry_ = now;
            ag_device_reset(canvas_->handle());
        }
    }
    void refresh() {
        if (exiting_)
            return;
        view_ = runtime_->snapshot();
        if(options_window_){overrides_.refresh(view_);if(!IsWindowVisible(static_cast<HWND>(options_window_->hwnd())) || IsIconic(static_cast<HWND>(options_window_->hwnd())))overrides_.hide();}
        update_adapters();
        runtime_->request_snapshot(nullptr, 0);
        if (axes_.expanded()) {
            axes_.refresh(view_);
            buttons_.refresh(view_);
            outputs_.refresh(view_);
            encoders_.refresh(view_);analogue_.refresh(view_);
            modules_.refresh(view_);
            motors_.refresh(view_);
            actuator_mappings_.refresh(view_);
        }else modules_.refresh_card(view_);
        ag_point pointer{};
        uint32_t inside{};
        gfx::check(ag_pointer_position(canvas_->handle(), &pointer, &inside));
        if (inside)
            for (std::size_t i = 0; i < flight_slots; ++i) {
                if (i == 1 || i == 5)
                    continue;
                const auto r = flight_rect(i);
                if (ag_hit_rect(r, pointer)) {
                    const auto tip = flight_tooltip(view_.flight, i);
                    if (tip != last_tip_) {
                        last_tip_ = tip;
                        ag_tooltip_set(canvas_->handle(), tip.c_str());
                    }
                    break;
                }
            }
        if (!graphics_visible_)
            return;
        drawing_.update(view_.flight, GetTickCount64() / 1000.0);
        canvas_->submit(drawing_.commands, drawing_.points);
        ++frames_;
        const auto now =
            static_cast<uint64_t>(psx::diagnostics::qpc_now() * 1000000 / psx::diagnostics::qpc_frequency());
        if (last_frame_us_)
            max_gap_us_ = std::max(max_gap_us_, now - last_frame_us_);
        last_frame_us_ = now;
    }
    void exit() {
        if (exiting_)
            return;
        exiting_ = true;
        overrides_.hide();
        outputs_.clear_tests();
        modules_.clear_tests();
        ag_window_refresh(window_.handle(), 0);
        if (options_window_)
            options_window_->show(false);
        modules_.hide_inspection();
        shutdown_ = std::async(std::launch::async, [this] { runtime_->stop(); });
    }
    void timer() {
        if (!IsWindowVisible(static_cast<HWND>(window_.hwnd())) || IsIconic(static_cast<HWND>(window_.hwnd())))
            {outputs_.clear_tests();modules_.clear_tests();motors_.hide();actuator_mappings_.hide();}
        if (!IsWindowVisible(static_cast<HWND>(window_.hwnd()))) buttons_.flush_navigation();
        if (exiting_) {
            if (shutdown_.valid() && shutdown_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                shutdown_.get();
                window_.timer(1, 0);
                ready_ = false;
                smoke_done_ = true;
                window_.defer_close(false);
                window_.close();
            }
            return;
        }
        view_ = runtime_->snapshot();
        runtime_->request_snapshot(nullptr, 0);
        tray_update();
        update_adapters();
        graphics_health();
        if (!launch_.smoke.empty())
            smoke();
    }
    void event(uint32_t id, uint32_t kind) {
        if (!ready_)
            return;
        if (kind == AG_CLOSE) {
            exit();
            return;
        }
        if (kind == AG_RESIZE) {
            layout();
            return;
        }
        if (kind == AG_REFRESH) {
            refresh();
            return;
        }
        if (kind == AG_TIMER) {
            if(id==2){if(!exiting_){motors_.tick();}return;}
            timer();
            return;
        }
        if (kind == AG_DISPATCH) {
            if (id == AxisPanel::dispatch_id)
                axes_.dispatch();
            else if (id == ButtonPanel::dispatch_id)
                buttons_.dispatch();
            else if(id==OutputPanel::dispatch_id)
                outputs_.dispatch();
            else if(id==CockpitPanel::encoder_dispatch)encoders_.dispatch();
            else if(id==CockpitPanel::analogue_dispatch)analogue_.dispatch();
            else if(id==ModulePanel::dispatch_id)modules_.dispatch();
            else if(id==MotorPanel::dispatch_id)motors_.dispatch();
            else if(id==ServoPanel::dispatch_id)motors_.servo_dispatch();
            else if(id==StepperPanel::dispatch_id)motors_.stepper_dispatch();
            else if(id==ActuatorPanel::dispatch_id)actuator_mappings_.dispatch();
            else if(id==MotorMappingPanel::dispatch_id)actuator_mappings_.mapping_dispatch();
            else if (id == options_dispatch && options_window_){overrides_.hide();options_window_->show(false);}
            else if(id==options_layout_id)layout_options();
            else if (id == close_dispatch)
                exit();
            return;
        }
        if (exiting_)
            return;
        if((id==fds_folder_id || id==phidget_folder_id) && kind==AG_CLICK){
            struct Selected {std::wstring path;bool failed{};} selected;std::uint32_t accepted{};
            ag_file_dialog(context_.handle(),options_window_->hwnd(),2,id==fds_folder_id?L"Choose folder containing interfaceITAPI x64.dll":L"Choose folder containing phidget22.dll",L"",L"",L"",
                [](void* user,const wchar_t* path)noexcept{auto& s=*static_cast<Selected*>(user);try{s.path=path;}catch(...){s.failed=true;}},&selected,&accepted);
            if(accepted && !selected.failed){const auto field=id==fds_folder_id?fds_path_id:phidget_path_id;control(field).text(selected.path.c_str());event(field,AG_FOCUS_LOST);}
            return;
        }
        if((id==fds_path_id || id==phidget_path_id) && (kind==AG_FOCUS_LOST || kind==AG_NAVIGATE)){
            auto settings=runtime_->settings();if(id==fds_path_id)settings.fds_api_path=control(id).text();else settings.phidget_api_path=control(id).text();std::string error;
            const auto& path=id==fds_path_id?settings.fds_api_path:settings.phidget_api_path;
            if(!path.empty() && !std::filesystem::path(path).is_absolute()){control(options_info_id).text(L"Choose a complete library folder path, or leave blank for automatic discovery.");return;}
            if(runtime_->save_settings(settings,error))control(options_info_id).text(id==fds_path_id?L"FDS library selection saved.":L"Phidgets library selection saved. Restart HWIO to use it.");
            else {control(options_info_id).text(wide(error).c_str());const auto saved=runtime_->settings();control(id).text((id==fds_path_id?saved.fds_api_path:saved.phidget_api_path).c_str());}
            return;
        }
        if(kind==AG_VALUE_CHANGED && id==options_tabs_id){layout_options();return;}
        if(kind==AG_CLICK && id==flight_fold_id){if(axes_.expanded())axes_.collapse();else axes_.expand();layout();return;}
        if(kind==AG_VALUE_CHANGED && (id==assignment_tabs_id || id==flight_tabs_id || id==analogue_outputs_tabs_id)){layout();return;}
        if (kind == AG_VALUE_CHANGED && (id == log_id || id == smoothing_id || id == network_id)) {
            save_options();
            return;
        }
        if (kind != AG_CLICK)
            return;
        switch (id) {
        case connect_id: {
            const auto endpoint = runtime_->endpoint();
            runtime_->connect(endpoint.first, endpoint.second);
            break;
        }
        case disconnect_id:
            runtime_->disconnect();
            break;
        case settings_id:
            options();
            break;
        case options_close_id:
            overrides_.hide();
            options_window_->show(false);
            break;
        case exit_id:
            ag_window_post(window_.handle(), close_dispatch);
            break;
        case axes_id:
        case buttons_id:
            button_view_ = id == buttons_id;
            buttons_.save_view(button_view_);
            layout();
            break;
        case collapse_id:
            axes_.collapse();
            layout();
            break;
        default:
            break;
        }
    }
    void verify(bool value, const char *message) {
        smoke_ok_ = smoke_ok_ && value;
        smoke_log_ << (value ? "OK " : "FAIL ") << message << '\n';
    }
    void capture(const wchar_t *suffix) {
        gfx::check(ag_window_export_png(window_.handle(), (launch_.smoke + suffix).c_str()));
    }
    void measure_start() {
        measure_frames_ = frames_;
        measure_paints_ = canvas_->metrics().frames;
        measure_started_ = GetTickCount64();
        last_frame_us_ = 0;
        max_gap_us_ = 0;
    }
    void measure_end(const char *label) {
        const double elapsed = (GetTickCount64() - measure_started_) / 1000.0;
        ag_refresh_info info{};
        gfx::check(ag_window_refresh_info(window_.handle(), &info));
        const double updates = (frames_ - measure_frames_) / elapsed,
                     paints = (canvas_->metrics().frames - measure_paints_) / elapsed;
        smoke_log_ << label << " monitor_hz=" << info.monitor_hz << " target_hz=" << info.target_hz
                   << " updates_hz=" << updates << " paints_hz=" << paints
                   << " max_update_gap_ms=" << max_gap_us_ / 1000.0 << '\n';
        verify(updates >= info.monitor_hz && paints >= info.monitor_hz,
               "measured steady UI update/paint averages meet monitor rate");
    }
    void next_monitor() {
        const auto &r = smoke_monitors_[smoke_monitor_];
        const float scale = 96.f / GetDpiForWindow(static_cast<HWND>(window_.hwnd()));
        window_.bounds({(r.left + 20) * scale, (r.top + 20) * scale, (r.left + 836) * scale, (r.top + 460) * scale});
        layout();
    }
    void smoke() {
        switch (smoke_step_++) {
        case 0: {
            verify(!IsWindowVisible(static_cast<HWND>(window_.hwnd())), "startup hidden in tray");
            verify(!view_.network.enabled, "smoke never connects PSX");
            verify(window_icons_.valid() && SendMessageW(static_cast<HWND>(window_.hwnd()), WM_GETICON, ICON_SMALL, 0) &&
                       SendMessageW(static_cast<HWND>(window_.hwnd()), WM_GETICON, ICON_BIG, 0), "main window has embedded HW icons");
            LOGFONTW font{};
            GetObjectW(
                reinterpret_cast<HFONT>(SendMessageW(static_cast<HWND>(control(connect_id).hwnd()), WM_GETFONT, 0, 0)),
                sizeof(font), &font);
            verify(std::wstring(font.lfFaceName) == L"B612", "library default B612 font");
            window_.show();
            smoke_warm_until_ = GetTickCount64() + 2000;
            break;
        }
        case 1:
            // Exclude first presentation and asynchronous device discovery from steady-state timing.
            if (GetTickCount64() < smoke_warm_until_) {
                --smoke_step_;
                break;
            }
            measure_start();
            break;
        case 2:
        case 3:
            break;
        case 4: {
            measure_end("dashboard");
            capture(L".dashboard.png");
            canvas_->png((launch_.smoke + L".flight.png").c_str(), 800, 316);
            options();
            break;
        }
        case 5:
            for (int level = 0; level < 3; ++level)
                for (int interval = 0; interval < 3; ++interval) {
                    control(log_id).value(level);
                    control(smoothing_id).value(interval);
                    save_options();
                    const auto saved = runtime_->settings();
                    verify(static_cast<int>(saved.log_level) == level && saved.smoothing_ms == (interval == 0   ? 10
                                                                                                : interval == 2 ? 50
                                                                                                                : 25),
                           "settings persist selected values");
                }
            control(log_id).value(1);
            control(smoothing_id).value(1);
            save_options();
            verify(options_icons_ && options_icons_->valid(), "settings has embedded HW icons");
            gfx::check(ag_window_export_png(options_window_->handle(), (launch_.smoke + L".settings.png").c_str()));
            options_window_->show(false);
            axes_.expand();
            axes_.refresh(view_);
            buttons_.refresh(view_);
            layout();
            break;
        case 6:
            measure_start();
            break;
        case 7:
        case 8:
            break;
        case 9:
            measure_end("expanded");
            capture(L".expanded.png");
            window_.show(false);
            hidden_frames_ = frames_;
            break;
        case 10:
            verify(frames_ == hidden_frames_, "hidden window suspends drawing");
            window_.show();
            break;
        case 11:
            SendMessageW(static_cast<HWND>(window_.hwnd()), WM_SYSCOMMAND, SC_MINIMIZE, 0);
            hidden_frames_ = frames_;
            break;
        case 12:
            verify(frames_ == hidden_frames_, "minimized window suspends drawing");
            SendMessageW(static_cast<HWND>(window_.hwnd()), WM_SYSCOMMAND, SC_RESTORE, 0);
            break;
        case 13:
            verify(IsWindowVisible(static_cast<HWND>(window_.hwnd())), "restored status/config window");
            SendMessageW(static_cast<HWND>(window_.hwnd()), WM_CLOSE, 0, 0);
            verify(!IsWindowVisible(static_cast<HWND>(window_.hwnd())), "close hides to tray");
            window_.show();
            axes_.collapse();
            EnumDisplayMonitors(
                nullptr, nullptr,
                [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                    MONITORINFO info{sizeof(info)};
                    if (GetMonitorInfoW(monitor, &info))
                        reinterpret_cast<std::vector<RECT> *>(data)->push_back(info.rcWork);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&smoke_monitors_));
            if (smoke_monitors_.empty())
                exit();
            else
                next_monitor();
            break;
        case 14:
            --smoke_step_; // One actual monitor per message-loop turn, allowing native DPI notifications to settle.
            {
                const auto dpi = GetDpiForWindow(static_cast<HWND>(window_.hwnd()));
                RECT panel{};
                GetClientRect(static_cast<HWND>(canvas_->hwnd()), &panel);
                verify(panel.right == MulDiv(800, dpi, 96) && panel.bottom == MulDiv(316, dpi, 96),
                       "graphics pane preserves logical size across actual monitor changes");
                smoke_log_ << "actual_monitor=" << smoke_monitor_ << " dpi=" << dpi << " pane_pixels=" << panel.right
                           << 'x' << panel.bottom << '\n';
                const auto file = launch_.smoke + L".monitor-" + std::to_wstring(smoke_monitor_) + L".png";
                gfx::check(ag_window_export_png(window_.handle(), file.c_str()));
                if (++smoke_monitor_ < smoke_monitors_.size())
                    next_monitor();
                else
                    exit();
            }
            break;
        default:
            break;
        }
    }

  public:
    explicit HwioUi(Launch launch)
        : window_(context_, L"PSX Hardware I/O " HWIO_VERSION_WIDE, 800, 400, callback, this),
          window_icons_(static_cast<HWND>(window_.hwnd())), launch_(std::move(launch)) {
        const bool portable = launch_.options.data_dir.empty();
        if (portable)
            launch_.options.data_dir = executable_directory();
        // Private UI qualification uses fresh settings without actuator mappings or PSX;
        // it must never activate or contend with an ordinary cockpit session.
        const auto instance=launch_.smoke.empty()?std::wstring(application_instance_id):L"PSX-IOS.HWIO.Qualification."+std::to_wstring(GetCurrentProcessId());
        primary_ = window_.single_instance(instance.c_str());
        if (!primary_)
            return;
        runtime_ = std::make_unique<Runtime>();
        runtime_->start(launch_.options);
        build();
        tray_ = std::make_unique<gfx::Tray>(window_, L"PSX Hardware I/O");
        window_.defer_close(true);
        ready_ = true;
        layout();
        view_ = runtime_->snapshot();
        tray_update();
        gfx::check(ag_window_refresh(window_.handle(), 2));
        window_.timer(1, 250);
        window_.timer(2, 50);
    }
    ~HwioUi() {
        if (shutdown_.valid())
            shutdown_.wait();
        if (runtime_)
            runtime_->stop();
        while (!controls_.empty())
            controls_.pop_back();
    }
    int run() {
        if (primary_ && launch_.startup_check) {
            verify(!IsWindowVisible(static_cast<HWND>(window_.hwnd())), "full application starts hidden with folded assignments");
            axes_.expand();layout();
            auto check_headers=[&](gfx::Control& tabs,std::span<const wchar_t* const> captions){const auto hwnd=static_cast<HWND>(tabs.hwnd());verify(TabCtrl_GetItemCount(hwnd)==static_cast<int>(captions.size()),"native tab count");int first_width{};const auto dc=GetDC(hwnd);const auto font=reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0));const auto previous=font?SelectObject(dc,font):nullptr;for(std::size_t n=0;n<captions.size();++n){RECT rectangle{};verify(TabCtrl_GetItemRect(hwnd,static_cast<int>(n),&rectangle)!=FALSE,"native tab header rectangle");const int width=rectangle.right-rectangle.left;if(n==0)first_width=width;verify(width>0 && abs(width-first_width)<=1,"actual equal-width tab headers");wchar_t text[128]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=text;item.cchTextMax=128;verify(SendMessageW(hwnd,TCM_GETITEMW,n,reinterpret_cast<LPARAM>(&item))!=FALSE && std::wstring_view(text)==captions[n],"complete native tab caption");SIZE size{};verify(dc && GetTextExtentPoint32W(dc,text,int(wcslen(text)),&size) && size.cx+MulDiv(16,int(GetDpiForWindow(hwnd)),96)<=width,"full caption fits actual header pixels");}if(previous)SelectObject(dc,previous);if(dc)ReleaseDC(hwnd,dc);};
            const std::array<const wchar_t*,2> left_headers{L"Flight axes",L"Modules"};const std::array<const wchar_t*,5> right_headers{L"Digital inputs",L"Digital outputs",L"Encoders",L"Analogue inputs",L"Analogue outputs"};check_headers(flight_tabs_,left_headers);check_headers(assignment_tabs_,right_headers);verify(flight_tabs_.value()==0,"Flight axes opens first");
            assignment_tabs_.value(1);layout();
            verify(assignment_tabs_.value()==1, "expanded digital page layout succeeds");
            for(int index=2;index<5;++index){assignment_tabs_.value(index);layout();verify(assignment_tabs_.value()==index,"expanded cockpit/analogue-output page layout succeeds");}
            flight_tabs_.value(1);layout();verify(flight_tabs_.value()==1,"left Modules tab succeeds");flight_tabs_.value(0);layout();
            options();
            verify(control(fds_folder_id).text()==L"Choose folder" && control(phidget_folder_id).text()==L"Choose folder","Both providers have native folder selectors");
            verify(control(fds_path_id).text()==runtime_->settings().fds_api_path && control(phidget_path_id).text()==runtime_->settings().phidget_api_path,"Provider folder fields retain independent settings");
            RECT general{};GetClientRect(static_cast<HWND>(general_options_.hwnd()),&general);
            for(const auto id:{fds_folder_id,phidget_folder_id,options_info_id}){RECT box{};GetWindowRect(static_cast<HWND>(control(id).hwnd()),&box);MapWindowPoints(nullptr,static_cast<HWND>(general_options_.hwnd()),reinterpret_cast<POINT*>(&box),2);verify(box.left>=0 && box.top>=0 && box.right<=general.right && box.bottom<=general.bottom,"Provider selection controls fit the native Settings page");}
            gfx::check(ag_window_export_png(options_window_->handle(),(launch_.smoke+L".settings.png").c_str()));
            const std::array<const wchar_t*,2> option_headers{L"General",L"Input overrides"};check_headers(options_tabs_,option_headers);verify(options_tabs_.value()==0,"General options opens first");options_tabs_.value(1);layout_options();overrides_.refresh(view_);options_tabs_.value(0);layout_options();overrides_.hide();options_window_->show(false);
            assignment_tabs_.value(0);layout();axes_.collapse();layout();
            verify(!axes_.expanded(), "return to folded layout succeeds");
            runtime_->stop();ready_=false;smoke_done_=true;
        }
        if (primary_ && !launch_.startup_check)
            window_.run();
        if (!launch_.smoke.empty()) {
            smoke_ok_ = smoke_ok_ && smoke_done_;
            smoke_log_ << (smoke_ok_ && exit_code_ == 0 ? "PASS" : "FAIL")
                       << (launch_.startup_check?" full hidden application startup and folded/expanded layout; PSX transport disabled\n":
                           " shared graphics, tray lifecycle, settings, assignment panes; transport disabled\n");
            if (!failure_.empty())
                smoke_log_ << "error=" << failure_ << '\n';
            std::ofstream output{std::filesystem::path(launch_.smoke)};
            output << smoke_log_.str();
            output.flush();
            if (!output)
                exit_code_ = 1;
        }
        return smoke_ok_ ? exit_code_ : 1;
    }
};
Launch arguments() {
    Launch launch;
    int count{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
        throw std::runtime_error("Command line unavailable");
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = argv[i];
        if ((arg == L"--check-catalogue" && count == 3) || (arg == L"--write-catalogue" && count == 4)) {
            launch.catalogue_input=argv[++i];
            if (arg==L"--write-catalogue") launch.catalogue_output=argv[++i];
        } else if (arg == L"--data-dir" && i + 1 < count)
            launch.options.data_dir = argv[++i];
        else if (arg == L"--no-autoconnect")
            launch.options.autoconnect = false;
        else if (arg == L"--host" && i + 1 < count) {
            launch.options.host = utf8(argv[++i]);
            launch.options.endpoint_override = true;
        } else if (arg == L"--port" && i + 1 < count) {
            const int port = _wtoi(argv[++i]);
            if (port < 1 || port > 65535) {
                LocalFree(argv);
                throw std::runtime_error("Invalid port");
            }
            launch.options.port = static_cast<uint16_t>(port);
            launch.options.endpoint_override = true;
        } else if ((arg == L"--ui-smoke-test" || arg == L"--ui-smoke-visible" || arg == L"--ui-startup-check") && i + 1 < count) {
            launch.startup_check=arg==L"--ui-startup-check";
            launch.visible = arg == L"--ui-smoke-visible";
            launch.smoke = argv[++i];
            launch.options.autoconnect = false;
        } else {
            LocalFree(argv);
            throw std::runtime_error("Unknown command line option");
        }
    }
    LocalFree(argv);
    if (!launch.smoke.empty()) {
        launch.smoke=std::filesystem::absolute(launch.smoke).wstring();
        const auto folder = std::filesystem::path(launch.smoke).parent_path();
        std::filesystem::create_directories(folder);
        launch.options.data_dir = (folder / (L"graphics-smoke-" + std::to_wstring(GetCurrentProcessId()))).wstring();
    }
    return launch;
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Launch launch;
    try {
        launch = arguments();
        if (!launch.catalogue_input.empty()) {
            auto output=[](const std::string &s) {
                HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
                if(!h || h==INVALID_HANDLE_VALUE) { AttachConsole(ATTACH_PARENT_PROCESS); h=GetStdHandle(STD_OUTPUT_HANDLE); }
                DWORD written{};WriteFile(h,s.data(),static_cast<DWORD>(s.size()),&written,nullptr);
            };
            try {
                const auto c=read_catalogue(launch.catalogue_input);
                if(!launch.catalogue_output.empty()) {
                    std::error_code ec;
                    if(std::filesystem::equivalent(launch.catalogue_input,launch.catalogue_output,ec) ||
                       std::filesystem::absolute(launch.catalogue_input).lexically_normal()==std::filesystem::absolute(launch.catalogue_output).lexically_normal())
                        throw std::runtime_error("Output must differ from input");
                    write_catalogue(*c,launch.catalogue_output);
                }
                output(utf8(launch.catalogue_input)+": valid; targets="+std::to_string(c->targets.size())+
                       "; actions="+std::to_string(c->actions.size())+"; parse/index ms="+std::to_string(c->load_ms)+"\n");
                return 0;
            } catch(const std::exception &e) {output(std::string(e.what())+"\n");return 1;}
        }
        (void)active_catalogue(); // Before window, assignments, HID and TCP creation.
        auto app = std::make_unique<HwioUi>(launch);
        return app->run();
    } catch (const CatalogueError &e) {
        if(!launch.smoke.empty()) std::ofstream(std::filesystem::path(launch.smoke)) << "FAIL " << e.what() << '\n';
        else {
            gfx::Context context;
            ag_message_box(context.handle(),nullptr,L"PSX-HWIO",e.popup(),1);
        }
        return 1;
    } catch (const gfx::Error &e) {
        if (!launch.smoke.empty())
            std::ofstream(std::filesystem::path(launch.smoke)) << "FAIL " << utf8(e.detail) << '\n';
        else
            MessageBoxW(nullptr, e.detail.c_str(), L"HWIO graphics error", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception &e) {
        if (!launch.smoke.empty())
            std::ofstream(std::filesystem::path(launch.smoke)) << "FAIL " << e.what() << '\n';
        else
            MessageBoxW(nullptr, wide(e.what()).c_str(), L"HWIO startup error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
