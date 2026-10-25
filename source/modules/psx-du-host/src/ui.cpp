#include <algorithm>
#include <bit>
#include <du_host/ui.hpp>
#include <sddl.h>
#include <sstream>
namespace du_host
{
namespace gfx = ass::graphics;
namespace
{
enum Id
{
    tabs = 1,
    status = 2,
    reconnect = 3,
    psx = 10,
    psx_port = 11,
    group = 12,
    port = 13,
    adapter = 14,
    apply = 15,
    cancel = 16,
    notice = 17,
    diagnostics = 31
};
ag_rect rect(float x, float y, float width, float height)
{
    return {x, y, x + width, y + height};
}
gfx::Control make(gfx::Context &c, void *parent, unsigned kind, unsigned id, const wchar_t *text,
                  ag_event_fn fn = nullptr, void *user = nullptr)
{
    ag_control_desc d{};
    d.kind = kind;
    if(kind==AG_CHECKBOX)d.maximum=1;
    d.id = id;
    d.bounds = {0, 0, 100, 28};
    d.text = text;
    return gfx::Control(c, parent, d, fn, user);
}
std::wstring identity()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot identify host user");
    DWORD n{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<std::uint8_t> bytes(n);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), n, &n))
    {
        CloseHandle(token);
        throw std::runtime_error("Cannot read host user");
    }
    CloseHandle(token);
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot format host user");
    std::wstring id = L"PSX-IOS.CduHost.";
    id += sid;
    LocalFree(sid);
    return id;
}
std::uint16_t parse_port(const std::wstring &s)
{
    if (s.empty() || s.size() > 5)
        throw std::runtime_error("Port must be from 1 to 65535");
    unsigned n{};
    for (auto c : s)
    {
        if (c < L'0' || c > L'9')
            throw std::runtime_error("Port must be an integer");
        n = n * 10 + c - L'0';
    }
    if (!n || n > 65535)
        throw std::runtime_error("Port must be from 1 to 65535");
    return std::uint16_t(n);
}
const wchar_t *state(psx::cdu::SourceState s)
{
    static const wchar_t *names[]{L"Disconnected", L"Connecting", L"Initializing", L"Connected"};
    return names[unsigned(s)];
}
std::wstring age(std::uint64_t stamp, std::uint64_t now)
{
    if (!stamp)
        return L"unknown";
    return std::to_wstring((now >= stamp ? now - stamp : 0) / 1000000) + L" ms";
}
} // namespace
void __cdecl Ui::callback(void *user, std::uint32_t id, std::uint32_t kind, double value) noexcept
{
    auto self = static_cast<Ui *>(user);
    try
    {
        self->event(id, kind, value);
    }
    catch (const gfx::Error &e)
    {
        self->notice(e.detail);
    }
    catch (const std::exception &e)
    {
        self->notice(widen(e.what()));
    }
}
Ui::Ui(UiOptions options)
    : window_(context_, L"PSX DU host " PSX_DU_HOST_VERSION_WIDE, 760, 650, callback, this),
      icons_(static_cast<HWND>(window_.hwnd())), settings_(options.settings_path)
{
    auto id = options.instance_id.empty() ? identity() : options.instance_id;
    primary_ = window_.single_instance(id.c_str());
    if (!primary_)
        return;
    auto loaded = settings_.load();
    if(!loaded.warning.empty())notice_text_=widen(loaded.warning);
    effective_ = loaded.config;
    psx::network::Inventory inventory;
    try { inventory.adapters = psx::network::enumerate(); }
    catch (const std::exception &e) { inventory.error = e.what(); }
    bool allowed = loaded.valid;
    if (allowed)
    {
        auto error = validate(effective_, inventory);
        if (!error.empty())
        {
            allowed = false;
            notice_text_ = widen(error);
        }
    }
    if (!loaded.valid)
        notice_text_ = widen(loaded.error);
    build();
    refresh_adapters(inventory);
    fill();
    try
    {
        if (options.tray_enabled)
            tray_ = std::make_unique<gfx::Tray>(window_, L"PSX DU host — starting");
    }
    catch (const gfx::Error &e)
    {
        notice_text_ = L"Tray unavailable; Close exits. " + e.detail;
    }
    if (!tray_ && notice_text_.empty())
        notice_text_ = L"Tray unavailable; Close exits.";
    window_.defer_close(true);
    gfx::check(ag_window_minimum(window_.handle(), 640, 550));
    runtime_ = std::make_unique<Runtime>(effective_, options.diagnostics, allowed,options.du_trace_output);
    ready_ = true;
    layout();
    notice(notice_text_);
    window_.timer(1, 250);
    if (!allowed)
    {
        tabs_.value(1);
        if (options.show_errors)
            window_.show();
    }
    else if (!tray_ && options.show_errors)
        window_.show();
}
Ui::~Ui()
{
    if (runtime_)
    {
        runtime_->request_stop();
        runtime_->join();
    }
}
int Ui::run()
{
    if (!primary_)
        return 0;
    window_.run();
    return exit_code_;
}
void Ui::build()
{
    tabs_ = make(context_, window_.hwnd(), AG_TABS, Id::tabs, L"", callback, this);
    status_page_ = tabs_.tab(L"Status");
    settings_page_ = tabs_.tab(L"Settings");
    status_text_ = make(context_, status_page_.hwnd(), AG_STATUS, Id::status, L"");
    reconnect_button_ = make(context_, status_page_.hwnd(), AG_BUTTON, Id::reconnect, L"Reconnect", callback, this);
    du_page_=tabs_.tab(L"DUs");
    du_source_=make(context_,du_page_.hwnd(),AG_CHOICE,18,L"",callback,this);
    du_refresh_=make(context_,du_page_.hwnd(),AG_BUTTON,19,L"Refresh sources",callback,this);
    du_apply_=make(context_,du_page_.hwnd(),AG_BUTTON,28,L"Apply DU settings",callback,this);
    du_tcp_port_=make(context_,du_page_.hwnd(),AG_EDIT,29,L"");
    du_freshness_=make(context_,du_page_.hwnd(),AG_EDIT,30,L"");
    du_tcp_label_=make(context_,du_page_.hwnd(),AG_LABEL,0,L"DU listen port");
    du_freshness_label_=make(context_,du_page_.hwnd(),AG_LABEL,0,L"Freshness reconnect (ms)");
    for(unsigned u=0;u<6;++u)du_enabled_[u]=make(context_,du_page_.hwnd(),AG_CHECKBOX,20+u,(L"Publish "+std::wstring(u==0?L"Captain outboard":u==1?L"Captain inboard":u==2?L"First officer inboard":u==3?L"First officer outboard":u==4?L"Upper EICAS":L"Lower EICAS")).c_str(),callback,this);
    auto p = settings_page_.hwnd();
    const wchar_t *captions[]{L"PSX address or computer name", L"PSX Main server port", L"CDU publication", L"CDU multicast group",
                              L"CDU multicast port"};
    for (auto text : captions)
        labels_.push_back(make(context_, p, AG_LABEL, 0, text));
    psx_edit_ = make(context_, p, AG_EDIT, Id::psx, L"");
    psx_port_ = make(context_, p, AG_EDIT, Id::psx_port, L"");
    group_edit_ = make(context_, p, AG_EDIT, Id::group, L"");
    port_ = make(context_, p, AG_EDIT, Id::port, L"");
    adapter_ = make(context_, p, AG_LABEL, Id::adapter, L"All eligible IPv4 adapters");
    diagnostics_ = make(context_, p, AG_CHECKBOX, Id::diagnostics, L"Diagnostics (this run only)", callback, this);
    apply_ = make(context_, p, AG_BUTTON, Id::apply, L"Apply", callback, this);
    cancel_ = make(context_, p, AG_BUTTON, Id::cancel, L"Cancel", callback, this);
    notice_ = make(context_, window_.hwnd(), AG_STATUS, Id::notice, L"");
}
void Ui::layout()
{
    if (!primary_ || !tabs_.handle() || IsIconic(hwnd()))
        return;
    auto bounds = window_.client_bounds();
    float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    tabs_.bounds(rect(12, 12, width - 24, height - 106));
    notice_.bounds(rect(12, height - 86, width - 24, 70));
    auto page = status_page_.client_bounds();
    float page_width = page.right - page.left, page_height = page.bottom - page.top;
    status_text_.bounds(rect(12, 12, page_width - 24, page_height - 65));
    reconnect_button_.bounds(rect(12, page_height - 45, 128, 30));
    auto du_bounds=du_page_.client_bounds();du_source_.bounds(rect(12,20,du_bounds.right-24,32));
    for(unsigned u=0;u<6;++u)du_enabled_[u].bounds(rect(12,float(72+u*42),du_bounds.right-24,32));
    du_tcp_label_.bounds(rect(12,332,210,30));du_tcp_port_.bounds(rect(232,332,du_bounds.right-244,30));
    du_freshness_label_.bounds(rect(12,370,210,30));du_freshness_.bounds(rect(232,370,du_bounds.right-244,30));
    du_refresh_.bounds(rect(12,414,150,32));du_apply_.bounds(rect(176,414,170,32));
    auto settings = settings_page_.client_bounds();
    float settings_width = settings.right - settings.left, y = 20;
    std::array<gfx::Control *, 5> fields{&psx_edit_, &psx_port_, &adapter_, &group_edit_, &port_};
    auto dpi = GetDpiForWindow(hwnd());
    if (dpi != label_dpi_)
    {
        for (unsigned i = 0; i < fields.size(); ++i)
            label_heights_[i] = std::max(30.0f, labels_[i].text_extent(210).y + 4);
        label_dpi_ = dpi;
    }
    for (unsigned i = 0; i < fields.size(); ++i)
    {
        float h = label_heights_[i];
        labels_[i].bounds(rect(12, y, 210, h));
        fields[i]->bounds(rect(232, y, settings_width - 244, 30));
        y += h + 18;
    }
    diagnostics_.bounds(rect(12,y+8,settings_width-24,30));
    apply_.bounds(rect(12, y + 50, 112, 32));
    cancel_.bounds(rect(136, y + 50, 112, 32));
}
void Ui::fill()
{
    diagnostics_.value(diagnostics_enabled_);
    du_sources_=psx::du::transport::discover(0);std::span<const wchar_t*> empty;du_source_.replace(empty,-1);du_source_.add(L"Automatic (one qualified PSX process)");int selection=0;
    for(unsigned i=0;i<du_sources_.size();++i){du_source_.add(du_sources_[i].label.c_str());if(du_sources_[i].pid==effective_.du_pid&&du_sources_[i].created==effective_.du_created)selection=int(i+1);}
    if(effective_.du_pid&&!selection){du_sources_.push_back({L"",L"Saved PSX source unavailable",effective_.du_pid,effective_.du_created});du_source_.add(L"Saved PSX source unavailable");selection=int(du_sources_.size());}du_source_.value(selection);
    for(unsigned u=0;u<6;++u)du_enabled_[u].value(bool(effective_.du_mask&(1u<<u)));
    du_tcp_port_.text(std::to_wstring(effective_.du_tcp_port).c_str());du_freshness_.text(std::to_wstring(effective_.du_freshness_ms).c_str());
    psx_edit_.text(widen(effective_.psx).c_str());
    psx_port_.text(std::to_wstring(effective_.psx_port).c_str());
    group_edit_.text(widen(effective_.group).c_str());
    port_.text(std::to_wstring(effective_.port).c_str());
    adapter_.text(L"All eligible IPv4 adapters");
}
void Ui::refresh_du_sources(){
    uint32_t pid{};uint64_t creation{};auto selected=unsigned(du_source_.value());if(selected&&selected<=du_sources_.size()){pid=du_sources_[selected-1].pid;creation=du_sources_[selected-1].created;}
    du_sources_=psx::du::transport::discover(0);std::vector<std::wstring> labels{L"Automatic (one qualified PSX process)"};int next=0;
    for(unsigned n=0;n<du_sources_.size();++n){labels.push_back(du_sources_[n].label);if(du_sources_[n].pid==pid&&du_sources_[n].created==creation)next=int(n+1);}
    if(pid&&!next){du_sources_.push_back({L"",L"Selected PSX source unavailable",pid,creation});labels.push_back(du_sources_.back().label);next=int(du_sources_.size());}
    std::vector<const wchar_t*> pointers;for(auto&label:labels)pointers.push_back(label.c_str());du_source_.replace(pointers,next);
}
void Ui::refresh_adapters(const psx::network::Inventory &inventory)
{
    (void)inventory;
    adapter_.text(L"All eligible IPv4 adapters");
}
void Ui::notice(std::wstring text)
{
    notice_text_ = std::move(text);
    if (notice_.handle())
        notice_.status(notice_text_.c_str(), notice_text_.empty() ? 0 : 1);
}
void Ui::apply()
{
    if (pending_id_ || exiting_)
        return;
    Config c=effective_;c.du_mask=0;for(unsigned u=0;u<6;++u)if(du_enabled_[u].value())c.du_mask|=1u<<u;
    c.du_tcp_port=parse_port(du_tcp_port_.text());c.du_freshness_ms=parse_port(du_freshness_.text());
    auto selected_du=unsigned(du_source_.value());if(selected_du>du_sources_.size())throw std::runtime_error("Select a qualified PSX source");c.du_pid=selected_du?du_sources_[selected_du-1].pid:0;c.du_created=selected_du?du_sources_[selected_du-1].created:0;
    c.psx = narrow(psx_edit_.text());
    c.group = narrow(group_edit_.text());
    c.psx_port = parse_port(psx_port_.text());
    c.port = parse_port(port_.text());
    auto s = runtime_->status();
    auto error = validate(c, s.inventory ? *s.inventory : psx::network::Inventory{});
    if (!error.empty())
        throw std::runtime_error(error);
    const bool diagnostics=diagnostics_.value()!=0;
    if(c==effective_&&diagnostics!=diagnostics_enabled_){
        diagnostics_enabled_=diagnostics;runtime_->diagnostics(diagnostics);notice(L"");return;
    }
    settings_.save(c);
    diagnostics_enabled_=diagnostics;runtime_->diagnostics(diagnostics);
    pending_ = c;
    pending_id_ = runtime_->configure(std::move(c));
    apply_.enabled(false);
    du_apply_.enabled(false);
    cancel_.enabled(false);
    notice(L"Applying saved settings…");
}
void Ui::cancel()
{
    if (pending_id_)
        return;
    fill();
    notice(L"");
}
void Ui::request_exit()
{
    if (!exiting_)
        window_.close();
}
void Ui::event(std::uint32_t id, std::uint32_t kind, double)
{
    if (!ready_)
        return;
    if (kind == AG_CLOSE)
    {
        exiting_ = true;
        runtime_->request_stop();
        apply_.enabled(false);
        du_apply_.enabled(false);
        reconnect_button_.enabled(false);
        notice(L"Stopping Main observation and publication…");
    }
    else if (id==19&&kind==AG_CLICK)refresh_du_sources();
    else if (id==28&&kind==AG_CLICK)apply();
    else if (kind == AG_TIMER)
        tick();
    else if (kind == AG_RESIZE || (id == Id::tabs && kind == AG_VALUE_CHANGED))
        layout();
    else if (kind == AG_CLICK)
    {
        if (id == Id::reconnect)
            runtime_->reconnect();
        else if (id == Id::apply)
            apply();
        else if (id == Id::cancel)
            cancel();
    }
}
void Ui::tick()
{
    if (!runtime_ || !ready_)
        return;
    auto s = runtime_->status();
    if (s.inventory && s.inventory->revision != inventory_revision_ && !(adapter_.activity() & AG_ACTIVITY_DROPDOWN))
    {
        refresh_adapters(*s.inventory);
        inventory_revision_ = s.inventory->revision;
    }
    if (pending_id_ && s.configuration_ack >= pending_id_)
    {
        if (s.configuration_applied)
            effective_ = pending_;
        pending_id_ = 0;
        apply_.enabled(true);
        du_apply_.enabled(true);
        cancel_.enabled(true);
        notice(s.configuration_applied ? L"" : L"Saved settings were not applied: " + widen(s.error.data()));
    }
    if (s.fatal && !exiting_)
    {
        exit_code_ = 1;
        notice(widen(s.error.data()));
        window_.show();
        gfx::check(ag_message_box(context_.handle(), window_.hwnd(), L"DU host startup failed",
                                  widen(s.error.data()).c_str(), 1));
        request_exit();
    }
    auto now = psx::norm::monotonic_ns();
    std::wostringstream out;
    out << L"PSX Main: " << state(s.source_state) << L" — " << widen(effective_.psx) << L":" << effective_.psx_port
        << L"\r\n";
    if (s.main_socket_error)
        out << L"Main socket error: " << s.main_socket_error << L"\r\n";
    const auto &label = s.screens[0];
    out << L"PSX version: "
        << (label.source_label_size ? widen({label.source_label.data(), label.source_label_size}) : L"unknown")
        << L"\r\n";
    out << L"Multicast: " << (s.publication_active ? L"Active local publication" : L"Unavailable") << L" — "
        << widen(effective_.group) << L":" << effective_.port << L"\r\n";
    out << L"Loopback 127.0.0.1: " << (s.loopback_active ? L"Active local publication" : L"Unavailable") << L"\r\n";
    out<<L"DU Ethernet: "<<(s.du.ethernet?L"active":L"unavailable")<<L"; loopback: "<<(s.du.loopback?L"active":L"unavailable")<<L"; published "<<s.du.published<<L"; expired "<<s.du.expired<<L"\r\n";
    for(unsigned u=0;u<6;++u){out<<L"DU "<<u+1<<L": "<<s.du.units[u].status;if(s.du.units[u].origin.pid)out<<L" (PSX PID "<<s.du.units[u].origin.pid<<L")";const auto& c=s.du.units[u].counters;if(c.valid)out<<L"; invalid "<<c.values[2]<<L", retries "<<c.values[3]<<L", abandoned "<<c.values[14]<<L" (lifetime)";out<<L"\r\n";}
    if(!s.du.error.empty())out<<L"DU transport: "<<widen(s.du.error)<<L"\r\n";
    out << L"IPv4 adapters: automatic; CDU bindings " << s.multicast_bindings
        << L"; unavailable " << s.multicast_unavailable << L"; PSX source address "
        << (s.address[0]?widen(s.address.data()):L"waiting") << L"\r\n";
    if (s.inventory) {
        if (!s.inventory->error.empty()) out << L"LAN inventory: " << widen(s.inventory->error) << L"\r\n";
        auto lan_error = validate(effective_, *s.inventory, true);
        if (!lan_error.empty()) out << L"LAN: " << widen(lan_error) << L"; local communication remains enabled\r\n";
    }
    for (unsigned u = 0; u < 3; ++u)
    {
        const auto &c = s.screens[u];
        out << (u == 0   ? L"CPT"
                : u == 1 ? L"FO"
                         : L"Centre")
            << L": text " << std::popcount(c.text_valid) << L"/14, colour " << std::popcount(c.colour_valid)
            << L"/14; appearance " << bool(c.scalar_valid & 1) << L", brightness " << bool(c.scalar_valid & 2)
            << L", blanking " << bool(c.scalar_valid & 4) << L"\r\n";
    }
    out << L"Generation " << s.generation << L"; connections " << s.connections << L"\r\n";
    out << L"Relevant records " << s.records << L"; accepted CDU records " << s.accepted << L"; rejected "
        << s.rejections << L"\r\n";
    out << L"Published " << s.published << L"; coalesced " << s.coalesced << L"; busy " << s.busy << L"; owned "
        << s.owned << L"/64 (peak " << s.owned_peak << L")\r\n";
    out << L"Last accepted CDU record: " << age(s.last_record_ns, now) << L"; local publication: "
        << age(s.last_publication_ns, now) << L"\r\n";
    out << L"Journal: "
        << (s.journal.degraded      ? L"Degraded"
            : s.journal.persistence ? L"Active"
                                    : L"Unavailable")
        << L"; lost " << s.journal.lost << L"\r\n";
    if (s.error[0])
        out << L"Transport error: " << widen(s.error.data()) << L"\r\n";
    if (s.journal.last_failure[0])
        out << L"Journal error: " << widen(s.journal.last_failure.data()) << L"\r\n";
    auto text = out.str();
    if (text != last_status_)
    {
        status_text_.status(text.c_str(), s.error[0] ? 1 : 0);
        last_status_ = std::move(text);
    }
    if (tray_)
    {
        auto tip = std::wstring(L"PSX DU host — ") + state(s.source_state) +
                   (s.publication_active ? L" / multicast active" : L" / multicast unavailable");
        if (tip != last_tooltip_)
        {
            gfx::check(ag_tooltip_set(tray_->handle(), tip.c_str()));
            last_tooltip_ = std::move(tip);
        }
    }
    if (exiting_ && runtime_->stopped())
    {
        runtime_->join();
        window_.timer(1, 0);
        ready_ = false;
        window_.defer_close(false);
        window_.close();
    }
}
void Ui::export_image(const std::filesystem::path &path)
{
    gfx::check(ag_window_export_png(window_.handle(), path.c_str()));
}
void Ui::select_page(unsigned p)
{
    tabs_.value(p);
    layout();
}
HWND Ui::control(std::uint32_t id) const
{
    const gfx::Control *result = nullptr;
    switch (id)
    {
    case Id::diagnostics:
        result=&diagnostics_;
        break;
    case Id::psx:
        result = &psx_edit_;
        break;
    case Id::psx_port:
        result = &psx_port_;
        break;
    case Id::group:
        result = &group_edit_;
        break;
    case Id::port:
        result = &port_;
        break;
    case Id::adapter:
        result = &adapter_;
        break;
    case Id::apply:
        result = &apply_;
        break;
    case Id::cancel:
        result = &cancel_;
        break;
    case Id::reconnect:
        result = &reconnect_button_;
        break;
    }
    return result ? static_cast<HWND>(result->hwnd()) : nullptr;
}
} // namespace du_host
