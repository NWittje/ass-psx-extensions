#include "hwio/catalogue.hpp"
#include "hwio/button_panel.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <chrono>
#include <cwctype>
#include <iomanip>

namespace hwio {
namespace {
constexpr int title_id = 10, last_id = 11, up_id = 12, issue_id = 13, cb_id = 14, chapter_base = 100, row_base = 10000,
              stride = 8, header = 152;
constexpr int search_id = 16, clear_search_id = 17, breadcrumb_id = 18, measure_id = 19;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
bool same_bounds(ag_rect a,ag_rect b) noexcept {
    return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
}
std::wstring folded(std::wstring s) {
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring button_caption(const DeviceIdentity& device, std::string_view name) {
    const auto prefix = device.name + " - ";
    if (name.starts_with(prefix))
        name.remove_prefix(prefix.size());
    return wide(name);
}
std::wstring catalogue_caption(const std::string &text) {
    auto caption = wide(text);
    // Native STATIC captions interpret '&' as a mnemonic; catalogue names are literal.
    for (std::size_t i = 0; (i = caption.find(L'&', i)) != caption.npos; i += 2)
        caption.insert(i, 1, L'&');
    return caption;
}
const ButtonReadout *readout(const RuntimeView &view, const ButtonAction &a) {
    const auto found = active_catalogue().action_ids.find(a.id);
    if (found == active_catalogue().action_ids.end())
        return nullptr;
    const auto index = found->second;
    return view.buttons && index < view.buttons->size() && (*view.buttons)[index].id == a.id ? &(*view.buttons)[index]
                                                                                             : nullptr;
}
} // namespace
AssignmentLayout assignment_layout(int work_width, int work_height, bool expanded) noexcept {
    if (!expanded)
        return {};
    AssignmentLayout result;
    result.height = std::max(400, work_height);
    result.narrow = work_width < 1170;
    result.button_width = result.narrow ? 800 : std::clamp(work_width - 810, 360, 690);
    result.width = result.narrow ? 800 : 810 + result.button_width;
    return result;
}
ag_rect assignment_tab_bounds(const AssignmentLayout& plan) noexcept {
    const float left=plan.narrow?0.f:810.f;
    // Hidden controls still require valid bounds during folded startup.
    const float width=plan.button_width?float(plan.button_width):690.f;
    return {left,plan.narrow?44.f:0.f,left+width,float(plan.narrow?plan.height-32:plan.height)};
}
ass::graphics::Control &ButtonPanel::control(HWND h) {
    return widgets_.at(widget_index_.at(h));
}
void ButtonPanel::item(HWND h, const std::wstring &s) {
    control(h).add(s.c_str());
}
int ButtonPanel::selected(HWND h) {
    return static_cast<int>(control(h).value());
}
void ButtonPanel::choice(HWND h, int value) {
    control(h).value(value);
}
void ButtonPanel::update(HWND h, const std::wstring &s) {
    if (h && control(h).text() != s)
        control(h).text(s.c_str());
}
ButtonPanel::~ButtonPanel() {
    flush_navigation();
    while (!widgets_.empty())
        widgets_.pop_back();
}
HWND ButtonPanel::add(HWND p, uint32_t kind, const std::wstring &title, uint32_t flags, int x, int y, int w, int h,
                      int id) {
    ag_control_desc d{};
    d.kind = kind;
    d.id = static_cast<uint32_t>(id);
    d.text = title.c_str();
    d.bounds = {float(x), float(y), float(x + w), float(y + (kind == AG_CHOICE ? 24 : h))};
    d.maximum = 1;
    d.step = 1;
    widgets_.emplace_back(*context_, p, d, event, this);
    auto &widget = widgets_.back();
    const auto hwnd = static_cast<HWND>(widget.hwnd());
    widget_index_[hwnd] = widgets_.size() - 1;
    if (flags)
        ass::graphics::check(ag_control_style(widget.handle(), flags));
    return hwnd;
}
void __cdecl ButtonPanel::event(void *user, uint32_t id, uint32_t kind, double value) {
    auto &self = *static_cast<ButtonPanel *>(user);
    if (self.rebuilding_)
        return;
    if (kind == AG_VIEWPORT_CHANGED) {
        self.realize();
        return;
    }
    int notification = -1;
    if (kind == AG_CLICK)
        notification = BN_CLICKED;
    else if (kind == AG_VALUE_CHANGED)
        notification = CBN_SELCHANGE;
    else if (kind == AG_FOCUS_LOST)
        notification = EN_KILLFOCUS;
    if (kind == AG_TEXT_CHANGED)
        notification = -2;
    if (kind == AG_NAVIGATE)
        notification = -3;
    if (notification == -1)
        return;
    Action action{static_cast<int>(id), notification, static_cast<int>(value)};
    if (id >= row_base) {
        const auto index = (id - row_base) / stride;
        if (index >= self.rows_.size() || self.rows_[index].entry < 0)
            return;
        action.generation = self.rows_[index].generation;
        action.key = self.rows_[index].key;
    }
    self.actions_.push_back(std::move(action));
    ag_window_post(self.owner_->handle(), dispatch_id);
}
void ButtonPanel::dispatch() {
    auto actions = std::move(actions_);
    actions_.clear();
    for (const auto &a : actions) {
        if (a.id >= row_base) {
            const auto i = static_cast<std::size_t>((a.id - row_base) / stride);
            if (i >= rows_.size() || rows_[i].generation != a.generation || rows_[i].key != a.key)
                continue;
        }
        selection_override_ = a.selection;
        if (a.notification == -3)
            keyboard(a.id, a.selection);
        else if (a.notification == -2)
            set_search(control(search_).text());
        else
            command(a.id, a.notification);
        selection_override_ = -1;
    }
    realize();
}
void ButtonPanel::attach(ass::graphics::Context &context, ass::graphics::Window &owner, Runtime &runtime,void* parent) {
    context_ = &context;
    owner_ = &owner;
    parent_ = static_cast<HWND>(parent?parent:owner.hwnd());
    runtime_ = &runtime;
    configuration_ = runtime.buttons();
    ag_rect work{};
    if (ag_window_work_area(owner.handle(), &work) >= 0)
        height_ = std::max(400, int(work.bottom));
    const auto file = runtime.data_dir() + L"\\settings.cfg";
    chapter_ = static_cast<int>(GetPrivateProfileIntW(L"button-pane", L"chapter", 0, file.c_str()));
    breakers_ = GetPrivateProfileIntW(L"button-pane", L"breakers", 0, file.c_str()) != 0;
    if (chapter_ != 100 && !active_catalogue().chapter_actions.count(chapter_))
        chapter_ = 0;
    for (const auto &a : button_catalogue())
        captions_.emplace(a.id,
                          std::make_pair(wide(a.name + " - " + a.position), folded(wide(a.name + " - " + a.position))));
    ag_control_desc d{};
    d.kind = AG_PANEL;
    d.bounds = {810, 0, 1500, 700};
    pane_ = ass::graphics::Control(context, parent_, d);
    window_ = static_cast<HWND>(pane_.hwnd());
    rebuilding_ = true;
    add(window_, AG_LABEL, L"Button assignments", 0, 14, 12, width_ - 28, 24, title_id);
    add(window_, AG_LABEL, L"Latest input: press a hardware button", 0, 14, 44, width_ - 28, 32, last_id);
    add(window_, AG_LABEL, L"Search", 0, 14, 83, 48, 20, 0);
    search_ = add(window_, AG_EDIT, L"", 0, 68, 80, width_ - 180, 24, search_id);
    add(window_, AG_BUTTON, L"Clear search", 0, width_ - 106, 80, 92, 24, clear_search_id);
    add(window_, AG_BUTTON, L"Up", AG_STYLE_FLAT, 14, 112, 58, 26, up_id);
    add(window_, AG_LABEL, L"", 0, 80, 116, width_ - 94, 22, breadcrumb_id);
    add(window_, AG_LABEL, L"", 0, 14, 139, width_ - 28, 16, issue_id);
    number_measure_ = add(window_, AG_CHOICE, L"", 0, 0, 0, 120, 24, 20);
    ag_visible(control(number_measure_).handle(), 0);
    measure_ = add(window_, AG_LABEL, L"", 0, 0, 0, 1, 1, measure_id);
    ag_visible(control(measure_).handle(), 0);
    rows_window_ = add(window_, AG_PANEL, L"", 0, 0, header + 10, width_, height_ - header - 10, 15);
    ag_control_events(control(search_).handle(),
                      AG_CONTROL_NOTIFY_TEXT | AG_CONTROL_NAV_ACTIVATE | AG_CONTROL_NAV_VERTICAL);
    ag_panel_viewport_notify(control(rows_window_).handle(), 1);
    cache_devices();
    rebuilding_ = false;
    dpi_ = GetDpiForWindow(window_);
    build();
    warm_labels();
}
void ButtonPanel::warm_labels() {
    const auto measure = [&](const std::wstring &caption) {
        const auto key = std::make_pair(name_width(), caption);
        if (label_heights_.count(key))
            return;
        update(measure_, catalogue_caption(utf8(caption)));
        ag_point size{};
        ass::graphics::check(ag_control_text_extent(control(measure_).handle(), float(name_width()), &size));
        label_heights_.emplace(key, std::max(20, int(std::ceil(size.y)) + 2));
    };
    for (const auto &a : button_catalogue()) {
        measure(captions_.at(a.id).first);
        if (a.toggleable && a.primary)
            measure(wide(a.name));
    }
}
bool ButtonPanel::eligible(const ButtonAction &a) const {
    if (configuration_.toggles.count(a.group) && !a.primary)
        return false;
    const auto row = configuration_.rows.find(a.id);
    const bool assigned =
        row != configuration_.rows.end() &&
        (!row->second.applied.id.empty() || !row->second.draft.id.empty() || !row->second.pending.empty());
    const auto *v = readout(view_, a);
    return assigned || !((v && !v->installed) || (a.circuit_breaker && !v));
}
std::vector<const ButtonAction *> ButtonPanel::visible_actions() const {
    std::vector<const ButtonAction *> out;
    const auto found = active_catalogue().chapter_actions.find(chapter_);
    if (found != active_catalogue().chapter_actions.end())
        for (const auto index : found->second) {
            const auto &a = button_catalogue()[index];
            if (a.circuit_breaker == breakers_ && eligible(a))
                out.push_back(&a);
        }
    return out;
}
void ButtonPanel::cache_devices() {
    identities_.clear();
    device_names_ = {L"Unassigned"};
    device_choices_.clear();
    for (const auto &d : devices_) {
        identities_.push_back(d.identity);
        device_names_.push_back(wide(d.identity.name));
        DeviceChoices c;
        for (std::size_t i = 0; i < d.controls.size(); ++i)
            if (d.controls[i].key.kind == ControlKind::button && !d.controls[i].relative) {
                c.controls.push_back(i);
                // Match Latest input, including Windows' button numbering. HID
                // usages remain routing identities and can differ from names.
                c.labels.push_back(button_caption(d.identity, d.controls[i].name));
            }
        device_choices_.push_back(std::move(c));
    }
    // Channel captions/font determine this width; navigating a chapter does not.
    std::vector<const wchar_t*> measured{L"—"};
    for(const auto& choices:device_choices_)
        for(const auto& name:choices.labels)measured.push_back(name.c_str());
    ass::graphics::check(ag_choice_replace(control(number_measure_).handle(),measured.data(),static_cast<uint32_t>(measured.size()),0));
    float preferred{};
    ass::graphics::check(ag_choice_preferred_width(control(number_measure_).handle(),&preferred));
    number_width_=std::max(32,int(std::ceil(preferred)));
}
int ButtonPanel::number_width() const {
    return number_width_;
}
int ButtonPanel::name_width() const {
    return width_ - 45 - (compact_ ? 108 : 114 + 132 + 6 + number_width());
}
void ButtonPanel::build_buttons(std::size_t index) {
    auto &row = rows_[index];
    const auto h = row.fields[4];
    std::vector<std::wstring> labels{L"—"};
    row.controls.clear();
    const auto saved = configuration_.rows.find(row.action->id);
    const Profile *p = saved == configuration_.rows.end() ? nullptr : &saved->second.draft;
    int choice = 0;
    if (row.device >= 0) {
        const auto &d = devices_[static_cast<std::size_t>(row.device)];
        const auto &c = device_choices_[static_cast<std::size_t>(row.device)];
        row.controls = c.controls;
        for (std::size_t i = 0; i < c.controls.size(); ++i) {
            labels.push_back(c.labels[i]);
            if (p && !p->bindings.empty() && same(p->bindings[0].sources[0].key, d.controls[c.controls[i]].key))
                choice = static_cast<int>(i + 1);
        }
    }
    if (!choice && p && !p->bindings.empty()) {
        const auto key=p->bindings[0].sources[0].key;
        labels.push_back(p->bindings[0].name.empty() ? L"Unavailable channel " + std::to_wstring(key.channel+1) :
            button_caption(p->identity, p->bindings[0].name) + L" (unavailable)");
        choice = static_cast<int>(row.controls.size() + 1);
    }
    std::vector<const wchar_t *> pointers;
    for (const auto &label : labels)
        pointers.push_back(label.c_str());
    ass::graphics::check(
        ag_choice_replace(control(h).handle(), pointers.data(), static_cast<uint32_t>(pointers.size()), choice));
}
void ButtonPanel::build(bool preserve_scroll) {
    if (!window_)
        return;
    const auto begin = Clock::now();
    if (preserve_scroll)
        remember_scroll();
    rebuilding_ = true;
    deferred_build_ = false;
    timings_.measure_ms = 0;
    entries_.clear();
    visible_ids_.clear();
    const auto append = [&](Entry entry) {
        entry.y = entries_.empty() ? 4 : entries_.back().y + entries_.back().height;
        if (entry.kind == Kind::action) {
            auto key = std::make_pair(name_width(), entry.caption);
            auto measured = label_heights_.find(key);
            if (measured == label_heights_.end()) {
                const auto t = Clock::now();
                update(measure_, catalogue_caption(utf8(entry.caption)));
                ag_point size{};
                ass::graphics::check(ag_control_text_extent(control(measure_).handle(), float(name_width()), &size));
                measured = label_heights_.emplace(std::move(key), std::max(20, int(std::ceil(size.y)) + 2)).first;
                timings_.measure_ms += elapsed(t);
            }
            entry.label_height = measured->second;
            entry.height =
                std::max(26, entry.label_height + 7) + (compact_ ? 25 : 0) + 23 + (entry.replacement ? 26 : 0);
        } else
            entry.height = entry.kind == Kind::unavailable ? 74 : 38;
        entries_.push_back(std::move(entry));
    };
    if (!query_.empty()) {
        std::wistringstream words(folded(query_));
        std::vector<std::wstring> tokens;
        for (std::wstring word; words >> word;)
            tokens.push_back(std::move(word));
        for (const auto &a : button_catalogue())
            if (eligible(a)) {
                const bool toggle = configuration_.toggles.count(a.group) != 0;
                const auto &cache = captions_.at(a.id);
                const auto caption = toggle ? wide(a.name) : cache.first;
                const auto searchable = toggle ? folded(caption) : cache.second;
                if (std::all_of(tokens.begin(), tokens.end(),
                                [&](const auto &w) { return searchable.find(w) != std::wstring::npos; }))
                    append({Kind::result, &a, wide(a.id), caption,
                            wide(button_chapter(a.chapter)) + L" / " +
                                (a.circuit_breaker ? L"Circuit breakers / " : L"") + wide(a.id),
                            a.chapter, 0, 0, 0, a.circuit_breaker});
            }
        for (const auto &[id, r] : configuration_.rows)
            if (!button_action(id)) {
                const auto text = folded(wide(id));
                if (std::all_of(tokens.begin(), tokens.end(),
                                [&](const auto &w) { return text.find(w) != std::wstring::npos; }))
                    append({Kind::result, nullptr, wide(id), wide(id), L"Unavailable assignment", 100});
            }
        std::stable_sort(entries_.begin(), entries_.end(), [](const auto &a, const auto &b) {
            const auto x = folded(a.caption), y = folded(b.caption);
            return x == y ? a.key < b.key : x < y;
        });
        for (std::size_t i = 0; i < entries_.size(); ++i)
            entries_[i].y = 4 + int(i) * 38;
        if (entries_.empty())
            append({Kind::empty, nullptr, L"empty", L"No matching controls"});
    } else if (!chapter_) {
        std::set<int> chapters;
        for (const auto &a : button_catalogue())
            chapters.insert(a.chapter);
        for (const auto &[id, r] : configuration_.rows)
            if (!button_action(id))
                chapters.insert(100);
        for (int c : chapters)
            append({Kind::chapter, nullptr, L"chapter-" + std::to_wstring(c), wide(button_chapter(c)), {}, c});
    } else if (chapter_ == 100) {
        for (const auto &[id, r] : configuration_.rows)
            if (!button_action(id))
                append({Kind::unavailable, nullptr, wide(id), wide(id),
                        L"Unavailable; commands inhibited\n" + wide(r.applied.identity.name), 100});
    } else {
        const auto chapter = active_catalogue().chapter_actions.find(chapter_);
        if (!breakers_ && chapter != active_catalogue().chapter_actions.end() &&
            std::any_of(chapter->second.begin(), chapter->second.end(),
                        [](auto i) { return button_catalogue()[i].circuit_breaker; }))
            append({Kind::breakers, nullptr, L"breakers", L"Circuit breakers"});
        for (const auto *a : visible_actions()) {
            const auto row = configuration_.rows.find(a->id);
            const bool replace = row != configuration_.rows.end() &&
                                 row->second.pending.find("Replace existing assignment") != std::string::npos;
            append({Kind::action,
                    a,
                    wide(a->id),
                    configuration_.toggles.count(a->group) ? wide(a->name) : captions_.at(a->id).first,
                    {},
                    a->chapter,
                    0,
                    0,
                    0,
                    a->circuit_breaker,
                    replace});
            visible_ids_.push_back(a->id);
        }
        if (entries_.empty())
            append({Kind::empty, nullptr, L"empty",
                    breakers_ ? L"Connect PSX to discover installed circuit breakers."
                              : L"No controls installed for this chapter."});
    }
    content_height_ = entries_.empty() ? 0 : entries_.back().y + entries_.back().height;
    ag_visible(control(GetDlgItem(window_, up_id)).handle(), query_.empty() && chapter_ != 0);
    update(GetDlgItem(window_, breadcrumb_id),
           !query_.empty() ? L"Search results"
           : chapter_      ? wide(button_chapter(chapter_) + (breakers_ ? " / Circuit breakers" : ""))
                           : L"");
    control(rows_window_).extent(float(content_height_));
    ++model_revision_;
    ++timings_.builds;
    timings_.model_ms = elapsed(begin) - timings_.measure_ms;
    rebuilding_ = false;
    scroll(scroll_);
    timings_.total_ms = elapsed(begin);
}
void ButtonPanel::bind_row(std::size_t slot, std::size_t entry_index) {
    auto &row = rows_[slot];
    const auto &e = entries_[entry_index];
    const bool different = row.key != e.key;
    if (different) {
        row.generation = ++next_generation_;
        row.tip.clear();
        row.readout_text.clear();
        ++timings_.rebound;
    }
    row.bound_revision = model_revision_;
    row.entry = static_cast<int>(entry_index);
    row.key = e.key;
    row.action = e.action;
    row.y = e.y;
    const ag_rect frame_bounds{0,float(e.y),float(width_),float(e.y+e.height)};
    if(!same_bounds(row.frame_bounds,frame_bounds)){
        control(row.frame).bounds(frame_bounds);row.frame_bounds=frame_bounds;
    }
    if(!row.frame_visible){ag_visible(control(row.frame).handle(),1);row.frame_visible=true;}
    std::array<bool, 8> shown{};
    const auto place = [&](int field, int x, int y, int w, int h, const std::wstring &text) {
        y-=e.y;
        auto handle = row.fields[static_cast<std::size_t>(field)];
        update(handle, text);
        const ag_rect bounds{float(x),float(y),float(x+w),float(y+h)};
        auto& prior=row.bounds[static_cast<std::size_t>(field)];
        if(!same_bounds(prior,bounds)){control(handle).bounds(bounds);prior=bounds;}
        shown[static_cast<std::size_t>(field)] = true;
    };
    if (e.kind != Kind::action) {
        const int field = e.kind == Kind::chapter || e.kind == Kind::breakers || e.kind == Kind::result ? 7 : 0;
        place(field, 14, e.y, width_ - 45, e.height - 4, field == 0 ? catalogue_caption(utf8(e.caption)) : e.caption);
        if (e.kind == Kind::unavailable)
            place(5, 14, e.y + 30, width_ - 45, 40, e.detail);
        const auto tip = e.caption + L"\n" + e.detail;
        if (row.tip != tip) {
            ag_tooltip_set(control(row.fields[static_cast<std::size_t>(field)]).handle(), tip.c_str());
            row.tip = tip;
        }
        for (std::size_t f = 0; f < row.fields.size(); ++f)
            if (row.shown[f] != shown[f])
                ag_visible(control(row.fields[f]).handle(), shown[f]);
        row.shown = shown;
        return;
    }
    const int nw = name_width(), first = std::max(26, e.label_height + 7), source_y = e.y + (compact_ ? first : 0);
    place(0, 14, e.y + 3, nw, e.label_height, catalogue_caption(utf8(e.caption)));
    if (e.action->toggleable && e.action->primary) {
        place(1, 14 + nw + 6, e.y, 64, 22, L"Toggle");
        control(row.fields[1]).value(configuration_.toggles.count(e.action->group) ? 1 : 0);
    }
    place(2, 14 + nw + 76, e.y, 32, 22, L"L.I.");
    const int source_x = compact_ ? 14 : 14 + nw + 114;
    // Choice captions are owned by their item lists, never reset during layout.
    for (int field : {3, 4}) {
        const int x = source_x + (field == 4 ? 138 : 0), w = field == 3 ? 132 : number_width();
        const ag_rect bounds{float(x),float(source_y-e.y),float(x+w),float(source_y-e.y+24)};
        auto& prior=row.bounds[static_cast<std::size_t>(field)];
        if(!same_bounds(prior,bounds)){control(row.fields[static_cast<std::size_t>(field)]).bounds(bounds);prior=bounds;}
        shown[static_cast<std::size_t>(field)] = true;
    }
    row.li = row.fields[2];
    row.readout = row.fields[5];
    const int readout_y = compact_ ? source_y + 25 : e.y + first;
    place(5, 14, readout_y, width_ - 45, 17, wide(row.readout_text));
    if (e.replacement)
        place(6, 14, readout_y + 21, 190, 22, L"Replace existing assignment");
    const auto saved = configuration_.rows.find(e.action->id);
    const Profile *p = saved == configuration_.rows.end() ? nullptr : &saved->second.draft;
    const int device = p && !p->id.empty() ? match_profile(*p, identities_) : -1;
    std::wstring binding_key = std::to_wstring(device) + L":" + std::to_wstring(inventory_revision_);
    if (p)
        binding_key += wide(profile_encode(*p));
    if (row.binding_key != binding_key) {
        const auto start = Clock::now();
        row.generation = ++next_generation_;
        row.device = device;
        auto names = device_names_;
        int selected_device = std::max(0, device + 1);
        if (device < 0 && p && !p->id.empty()) {
            names.push_back(wide(p->identity.name + " (unavailable)"));
            selected_device = static_cast<int>(devices_.size() + 1);
        }
        std::vector<const wchar_t *> pointers;
        for (const auto &name : names)
            pointers.push_back(name.c_str());
        ass::graphics::check(ag_choice_replace(control(row.fields[3]).handle(), pointers.data(),
                                               static_cast<uint32_t>(pointers.size()), selected_device));
        build_buttons(slot);
        row.binding_key = std::move(binding_key);
        timings_.choices_ms += elapsed(start);
    }
    if(row.li_available!=view_.last_button.available){
        row.li_available=view_.last_button.available;
        control(row.li).enabled(row.li_available);
    }
    for (std::size_t f = 0; f < row.fields.size(); ++f)
        if (row.shown[f] != shown[f])
            ag_visible(control(row.fields[f]).handle(), shown[f]);
    row.shown = shown;
}
void ButtonPanel::realize() {
    if (realizing_ || rebuilding_ || !rows_window_)
        return;
    float position{};
    ag_panel_scroll_get(control(rows_window_).handle(), &position);
    scroll_ = int(position);
    if (realized_revision_ == model_revision_ && realized_scroll_ == scroll_ && realized_height_ == height_)
        return;
    realizing_ = true;
    rebuilding_ = true;
    const auto start = Clock::now();
    struct ResetFlags {bool& rebuilding;bool& realizing;~ResetFlags(){rebuilding=false;realizing=false;}} reset{rebuilding_,realizing_};
    auto update=control(rows_window_).update_scope();
    const int viewport = std::max(0, height_ - header - 10);
    auto first = std::lower_bound(entries_.begin(), entries_.end(), scroll_,
                                  [](const auto &e, int y) { return e.y + e.height <= y; });
    auto last = std::lower_bound(entries_.begin(), entries_.end(), scroll_ + viewport,
                                 [](const auto &e, int y) { return e.y < y; });
    const auto low = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, first - entries_.begin() - 2));
    const auto high = std::min(entries_.size(), static_cast<std::size_t>(last - entries_.begin()) + 2);
    std::vector<std::size_t> wanted;
    for (auto i = low; i < high; ++i)
        wanted.push_back(i);
    const HWND focus = GetFocus();
    for (const auto &row : rows_)
        if (std::find(row.fields.begin(), row.fields.end(), focus) != row.fields.end()) {
            const auto entry =
                std::find_if(entries_.begin(), entries_.end(), [&](const auto &e) { return e.key == row.key; });
            if (entry != entries_.end()) {
                const auto i = static_cast<std::size_t>(entry - entries_.begin());
                if (std::find(wanted.begin(), wanted.end(), i) == wanted.end())
                    wanted.push_back(i);
            }
        }
    // Prewarm a viewport-sized pool while hidden; never one HWND set per catalogue action.
    const auto capacity = std::max(wanted.size(), static_cast<std::size_t>(viewport / 34 + 6));
    while (rows_.size() < capacity) {
        const auto t = Clock::now();
        const int id = row_base + static_cast<int>(rows_.size()) * stride;
        Row row;
        // A non-scrolling ASS label hosts one retained row. Its descendants
        // keep row-local geometry, so scrolling/chapter changes move one HWND.
        row.frame=add(rows_window_,AG_LABEL,L"",0,0,0,width_,24,500000+static_cast<int>(rows_.size()));
        SetWindowLongPtrW(row.frame,GWL_STYLE,GetWindowLongPtrW(row.frame,GWL_STYLE)|WS_CLIPCHILDREN);
        SetWindowLongPtrW(row.frame,GWL_EXSTYLE,GetWindowLongPtrW(row.frame,GWL_EXSTYLE)|WS_EX_CONTROLPARENT);
        ag_visible(control(row.frame).handle(),0);
        const uint32_t kinds[]{AG_LABEL, AG_CHECKBOX, AG_BUTTON, AG_CHOICE, AG_CHOICE, AG_LABEL, AG_BUTTON, AG_BUTTON};
        for (int f = 0; f < stride; ++f) {
            row.fields[static_cast<std::size_t>(f)] =
                add(row.frame, kinds[f], L"", f == 7 ? AG_STYLE_FLAT : 0, 0, 0, 1, 24, id + f);
            ag_visible(control(row.fields[static_cast<std::size_t>(f)]).handle(), 0);
            // Allocate bounded tooltip resources during hidden preparation, not first chapter navigation.
            ag_tooltip_set(control(row.fields[static_cast<std::size_t>(f)]).handle(), L"Assignment details");
            const auto flags = AG_CONTROL_NAV_TAB | ((f == 7) ? AG_CONTROL_NAV_VERTICAL | AG_CONTROL_NAV_ACTIVATE : 0);
            ag_control_events(control(row.fields[static_cast<std::size_t>(f)]).handle(), flags);
        }
        std::vector<const wchar_t *> names;
        for (const auto &name : device_names_)
            names.push_back(name.c_str());
        ag_choice_replace(control(row.fields[3]).handle(), names.data(), static_cast<uint32_t>(names.size()), 0);
        const wchar_t *empty = L"—";
        ag_choice_replace(control(row.fields[4]).handle(), &empty, 1, 0);
        row.binding_key = L"-1:" + std::to_wstring(inventory_revision_);
        rows_.push_back(std::move(row));
        timings_.created += stride+1;
        timings_.controls_ms += elapsed(t);
    }
    // Keep every rebound child batched until the entire viewport is ready.
    // Ending one row early lets later MoveWindow(TRUE) calls repaint its
    // neighbouring HWNDs while the remaining rows still have old geometry.
    std::vector<ass::graphics::Control::UpdateScope> child_updates;
    child_updates.reserve(wanted.size() * (stride+1));
    std::vector<bool> used(rows_.size());
    std::vector<std::pair<std::size_t,std::size_t>> bindings;
    bindings.reserve(wanted.size());
    for (const auto i : wanted) {
        auto slot = rows_.size();
        for (std::size_t j = 0; j < rows_.size(); ++j)
            if (!used[j] && rows_[j].key == entries_[i].key) {
                slot = j;
                break;
            }
        if (slot == rows_.size())
            for (std::size_t j = 0; j < rows_.size(); ++j)
                if (!used[j] && std::none_of(wanted.begin(), wanted.end(),
                                             [&](auto k) { return rows_[j].key == entries_[k].key; })) {
                    slot = j;
                    break;
                }
        if (slot == rows_.size())
            continue;
        used[slot] = true;
        if (rows_[slot].entry != static_cast<int>(i) || rows_[slot].key != entries_[i].key ||
            rows_[slot].bound_revision != model_revision_)
            bindings.emplace_back(slot,i);
        // The native viewport already translates retained child windows when
        // scrolling. An unchanged logical row needs no second MoveWindow.
    }
    // Begin every changed row before moving the first child: otherwise that
    // MoveWindow can synchronously repaint a later, not-yet-batched sibling.
    for(const auto& [slot,index]:bindings) {
        (void)index;
        child_updates.emplace_back(control(rows_[slot].frame));
        for(auto h:rows_[slot].fields)child_updates.emplace_back(control(h));
    }
    for(const auto& [slot,index]:bindings)bind_row(slot,index);
    for (std::size_t j = 0; j < rows_.size(); ++j)
        if (!used[j]) {
            if(rows_[j].frame_visible)ag_visible(control(rows_[j].frame).handle(),0);
            rows_[j].frame_visible=false;
            rows_[j].entry = -1;
        }
    for (auto child=child_updates.rbegin();child!=child_updates.rend();++child)
        child->finish();
    update.finish();
    realized_revision_ = model_revision_;
    realized_scroll_ = scroll_;
    realized_height_ = height_;
    rebuilding_ = false;
    realizing_ = false;
    timings_.layout_ms += elapsed(start);
    readouts();
}
void ButtonPanel::arrange(int x, int y, int width, int height, bool visible) {
    if (!window_)
        return;
    if (!visible)
        flush_navigation();
    remember_scroll();
    const auto dpi = GetDpiForWindow(window_);
    if (!arranged_ || pane_visible_ != visible)
        ag_visible(pane_.handle(), visible);
    pane_visible_ = visible;
    if (arranged_ && pane_x_ == x && pane_y_ == y && width_ == width && height_ == height && dpi_ == dpi)
        return;
    pane_x_ = x;
    pane_y_ = y;
    arranged_ = true;
    const bool rebuild = width_ != width || dpi_ != dpi;
    if (dpi_ != dpi) {
        label_heights_.clear();
        cache_devices();
    }
    dpi_ = dpi;
    width_ = width;
    height_ = height;
    compact_ = width < 600;
    pane_.bounds({float(x), float(y), float(x + width), float(y + height)});
    control(search_).bounds({68, 80, float(width_ - 112), 104});
    control(GetDlgItem(window_, clear_search_id)).bounds({float(width_ - 106), 80, float(width_ - 14), 104});
    for (int id : {title_id, last_id, issue_id, breadcrumb_id}) {
        const int left = id == breadcrumb_id ? 80 : 14, top = id == title_id   ? 12
                                                              : id == last_id  ? 44
                                                              : id == issue_id ? 139
                                                                               : 116;
        control(GetDlgItem(window_, id))
            .bounds({float(left), float(top), float(width_ - 14), float(top + (id == last_id ? 32 : 24))});
    }
    control(rows_window_).bounds({0, float(header + 10), float(width), float(height)});
    if (rebuild)
        build();
    else {
        scroll(scroll_);
        realize();
    }
}
void ButtonPanel::remember_scroll() {
    if (!rows_window_)
        return;
    float y{};
    ag_panel_scroll_get(control(rows_window_).handle(), &y);
    scroll_ = int(y);
    if (query_.empty())
        positions_[{chapter_, breakers_}] = scroll_;
}
void ButtonPanel::scroll(int value) {
    const int target = std::clamp(value, 0, std::max(0, content_height_ - std::max(0, height_ - header - 10)));
    float actual{};
    ass::graphics::check(ag_panel_scroll_get(control(rows_window_).handle(), &actual));
    if (std::abs(actual - float(target)) > 0.1f)
        ass::graphics::check(ag_panel_scroll_set(control(rows_window_).handle(), float(target)));
    remember_scroll();
    realize();
}
void ButtonPanel::navigate(int chapter, bool breakers) {
    remember_scroll();
    chapter_ = chapter;
    breakers_ = breakers;
    scroll_ = positions_[{chapter_, breakers_}];
    build(false);
    if (chapter_)
        ag_focus(control(GetDlgItem(window_, up_id)).handle());
    else if (!entries_.empty())
        focus_entry(0, 7);
    navigation_dirty_ = true;
}
void ButtonPanel::flush_navigation() {
    if (!navigation_dirty_ || !runtime_)
        return;
    std::string error;
    if (save_button_navigation(runtime_->data_dir(), chapter_, breakers_, error))
        navigation_dirty_ = false;
    else
        problem_ = error;
}

void ButtonPanel::set_search(const std::wstring &text) {
    const auto first = text.find_first_not_of(L" \t\r\n"), last = text.find_last_not_of(L" \t\r\n");
    const auto query = first == std::wstring::npos ? std::wstring{} : text.substr(first, last - first + 1);
    if (query == query_)
        return;
    remember_scroll();
    if (query_.empty()) {
        restore_chapter_ = chapter_;
        restore_breakers_ = breakers_;
        restore_scroll_ = scroll_;
    }
    query_ = query;
    if (query.empty()) {
        chapter_ = restore_chapter_;
        breakers_ = restore_breakers_;
        scroll_ = restore_scroll_;
    } else
        scroll_ = 0;
    if (text.empty()) {
        rebuilding_ = true;
        update(search_, L"");
        rebuilding_ = false;
    }
    build(false);
}
void ButtonPanel::activate_result(const Entry &entry) {
    const auto key = entry.key;
    const auto chapter = entry.chapter;
    const auto breakers = entry.breakers;
    query_.clear();
    rebuilding_ = true;
    update(search_, L"");
    rebuilding_ = false;
    navigate(chapter, breakers);
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto &e) { return e.key == key; });
    if (found != entries_.end())
        focus_entry(static_cast<std::size_t>(found - entries_.begin()), found->kind == Kind::action ? 3 : 0, true);
}
void ButtonPanel::focus_entry(std::size_t index, int field, bool center) {
    if (index >= entries_.size())
        return;
    const auto &e = entries_[index];
    if (center)
        scroll(e.y + e.height / 2 - std::max(0, height_ - header - 10) / 2);
    else if (e.y < scroll_ || e.y + e.height > scroll_ + height_ - header - 10)
        scroll(e.y);
    realize();
    for (auto &r : rows_)
        if (r.entry == static_cast<int>(index)) {
            ag_focus(control(r.fields[static_cast<std::size_t>(field)]).handle());
            break;
        }
}
void ButtonPanel::keyboard(int id, int direction) {
    if (direction == AG_NAV_ESCAPE) {
        set_search(L"");
        ag_focus(control(search_).handle());
        return;
    }
    if (id == search_id) {
        if (!query_.empty() && !entries_.empty() && entries_[0].kind == Kind::result) {
            if (direction == AG_NAV_ENTER)
                activate_result(entries_[0]);
            else if (direction == AG_NAV_DOWN)
                focus_entry(0, 7);
        }
        return;
    }
    if (id < row_base)
        return;
    const auto slot = static_cast<std::size_t>((id - row_base) / stride);
    if (slot >= rows_.size() || rows_[slot].entry < 0)
        return;
    auto index = static_cast<std::size_t>(rows_[slot].entry);
    int field = (id - row_base) % stride;
    if (direction == AG_NAV_ENTER) {
        command(row_base + int(slot) * stride + 7, BN_CLICKED);
        return;
    }
    auto fields = [&](std::size_t i) {
        std::vector<int> out;
        const auto &e = entries_[i];
        if (e.kind == Kind::action) {
            if (e.action->toggleable && e.action->primary)
                out.push_back(1);
            if (view_.last_button.available)
                out.push_back(2);
            out.push_back(3);
            out.push_back(4);
            if (e.replacement)
                out.push_back(6);
        } else if (e.kind == Kind::chapter || e.kind == Kind::breakers || e.kind == Kind::result)
            out.push_back(7);
        return out;
    };
    const bool previous = direction == AG_NAV_TAB_PREVIOUS || direction == AG_NAV_UP || direction == AG_NAV_PAGE_UP;
    if (direction == AG_NAV_TAB_NEXT || direction == AG_NAV_TAB_PREVIOUS) {
        const auto choices = fields(index);
        const auto found = std::find(choices.begin(), choices.end(), field);
        if (found != choices.end() &&
            ((!previous && found + 1 != choices.end()) || (previous && found != choices.begin()))) {
            focus_entry(index, previous ? *(found - 1) : *(found + 1));
            return;
        }
    }
    int next = int(index) + (previous ? -1 : 1);
    if (direction == AG_NAV_HOME)
        next = 0;
    if (direction == AG_NAV_END)
        next = int(entries_.size()) - 1;
    if (direction == AG_NAV_PAGE_UP || direction == AG_NAV_PAGE_DOWN) {
        const int target = entries_[index].y + (previous ? -1 : 1) * std::max(1, height_ - header - 10);
        const auto found =
            std::lower_bound(entries_.begin(), entries_.end(), target, [](const auto &e, int y) { return e.y < y; });
        next = std::min(int(entries_.size()) - 1, int(found - entries_.begin()));
    }
    while (next >= 0 && next < int(entries_.size())) {
        const auto choices = fields(static_cast<std::size_t>(next));
        if (!choices.empty()) {
            focus_entry(static_cast<std::size_t>(next), previous ? choices.back() : choices.front());
            return;
        }
        next += previous ? -1 : 1;
    }
    ag_focus(control(search_).handle());
}
void ButtonPanel::save(const std::string &replace_action) {
    std::string error;
    if (!runtime_->save_buttons(configuration_, error, replace_action))
        problem_ = error;
    else {
        problem_.clear();
        configuration_ = runtime_->buttons();
    }
    build();
}
void ButtonPanel::command(int id, int notification) {
    if (rebuilding_)
        return;
    if (id == search_id) {
        set_search(control(search_).text());
        return;
    }
    if (id == clear_search_id && notification == BN_CLICKED) {
        set_search(L"");
        ag_focus(control(search_).handle());
        return;
    }
    if (id == up_id && notification == BN_CLICKED) {
        navigate(breakers_ ? chapter_ : 0, false);
        return;
    }
    if (id == cb_id && notification == BN_CLICKED) {
        navigate(chapter_, true);
        return;
    }
    if (id >= chapter_base && id <= chapter_base + 100 && notification == BN_CLICKED) {
        navigate(id - chapter_base, false);
        return;
    }
    if (id < row_base)
        return;
    const auto index = static_cast<std::size_t>((id - row_base) / stride);
    const int field = (id - row_base) % stride;
    if (index >= rows_.size())
        return;
    const auto row = rows_[index];
    if (row.entry < 0)
        return;
    const auto entry = entries_[static_cast<std::size_t>(row.entry)];
    if (field == 7 && notification == BN_CLICKED) {
        if (entry.kind == Kind::result)
            activate_result(entry);
        else if (entry.kind == Kind::chapter)
            navigate(entry.chapter, false);
        else if (entry.kind == Kind::breakers)
            navigate(chapter_, true);
        return;
    }
    if (!row.action || !query_.empty())
        return;
    const auto &a = *row.action;
    auto &config = configuration_.rows[a.id];
    if (field == 1 && notification == BN_CLICKED) {
        button_toggle(configuration_, a.group,
                      (selection_override_ >= 0 ? selection_override_ : selected(row.fields[static_cast<std::size_t>(field)])) != 0);
        save();
        return;
    }
    if (field == 2 && notification == BN_CLICKED && view_.last_button.available) {
        for (const auto &d : devices_)
            if (d.slot == view_.last_button.sample.device && d.generation == view_.last_button.sample.generation &&
                view_.last_button.sample.control < d.controls.size()) {
                config.draft = button_source(d, d.controls[view_.last_button.sample.control]);
                config.managed = true;
                save();
                return;
            }
    }
    if (field == 3 && notification == CBN_SELCHANGE) {
        const int choice = selection_override_ >= 0 ? selection_override_ : selected(row.fields[static_cast<std::size_t>(field)]);
        if (choice == 0) {
            config.draft = {};
            config.managed = true;
            save();
            return;
        }
        if (choice > 0 && static_cast<std::size_t>(choice) <= devices_.size()) {
            const auto &d = devices_[static_cast<std::size_t>(choice - 1)];
            config.draft = {};
            config.draft.id = "button-source";
            config.draft.name = d.identity.name;
            config.draft.identity = d.identity;
            config.managed = true;
            save();
            return;
        }
    }
    if (field == 4 && notification == CBN_SELCHANGE && row.device >= 0) {
        const int choice = selection_override_ >= 0 ? selection_override_ : selected(row.fields[static_cast<std::size_t>(field)]);
        if (choice == 0) {
            config.draft = {};
            config.managed = true;
            save();
            return;
        }
        if (choice > 0 && static_cast<std::size_t>(choice) <= row.controls.size()) {
            const auto &d = devices_[static_cast<std::size_t>(row.device)];
            config.draft = button_source(d, d.controls[row.controls[static_cast<std::size_t>(choice - 1)]]);
            config.managed = true;
            save();
            return;
        }
    }
    if (field == 6 && notification == BN_CLICKED) {
        config.managed = true;
        save(a.id);
    }
}
void ButtonPanel::readouts() {
    update(GetDlgItem(window_, last_id), view_.last_button.present
                                             ? L"Latest input: " +
                                                   wide(view_.last_button.device + " - " + view_.last_button.button) +
                                                   (view_.last_button.available ? L"" : L" (unavailable)")
                                             : L"Latest input: press a hardware button");
    update(GetDlgItem(window_, issue_id), wide(problem_));
    for (auto &row : rows_) {
        if (row.entry < 0 || !row.action || entries_[static_cast<std::size_t>(row.entry)].kind != Kind::action)
            continue;
        if (row.li_available != view_.last_button.available) {
            row.li_available = view_.last_button.available;
            control(row.li).enabled(row.li_available);
        }
        const auto *r = readout(view_, *row.action);
        const auto configured = configuration_.rows.find(row.action->id);
        const auto pending = configured == configuration_.rows.end() ? std::string{} : configured->second.pending;
        const auto label = !pending.empty()     ? pending
                           : !r                 ? std::string{}
                           : !r->reason.empty() ? r->reason
                           : r->assigned        ? std::string(r->pressed ? "Pressed - " : "") +
                                                      (r->delivery == DeliveryState::sent     ? "Sent"
                                                       : r->delivery == DeliveryState::queued ? "Queued"
                                                                                              : "Assigned")
                                                : std::string{};
        if (row.readout_text != label) {
            row.readout_text = label;
            update(row.readout, wide(label));
        }
    }
    tips();
}
bool ButtonPanel::dropdown_open() const {
    for (const auto &row : rows_)
        if (row.entry >= 0)
            for (int field : {3, 4}) {
                uint32_t state{};
                const auto found = widget_index_.find(row.fields[static_cast<std::size_t>(field)]);
                if (found != widget_index_.end() && ag_choice_open(widgets_[found->second].handle(), &state) >= 0 &&
                    state)
                    return true;
            }
    return false;
}
void ButtonPanel::refresh(const RuntimeView &view) {
    const bool readouts_changed = view_.buttons != view.buttons ||
                                  view_.last_button.available != view.last_button.available ||
                                  view_.last_button.present != view.last_button.present ||
                                  view_.last_button.sample.sequence != view.last_button.sample.sequence ||
                                  view_.last_button.sample.device != view.last_button.sample.device ||
                                  view_.last_button.sample.generation != view.last_button.sample.generation;
    view_.buttons = view.buttons;
    view_.last_button = view.last_button;
    view_.button_revision = view.button_revision;
    if (dropdown_open()) {
        if (readouts_changed)
            readouts();
        return;
    }
    bool changed = false;
    std::size_t index = 0;
    for (const auto &d : view.devices)
        if (d.connected && std::any_of(d.controls.begin(), d.controls.end(),
                                       [](const auto &c) { return c.key.kind == ControlKind::button; })) {
            if (index >= devices_.size() || devices_[index].slot != d.slot ||
                devices_[index].generation != d.generation)
                changed = true;
            ++index;
        }
    changed = changed || index != devices_.size();
    if (changed || inventory_dirty_) {
        devices_.clear();
        for (const auto &d : view.devices)
            if (d.connected && std::any_of(d.controls.begin(), d.controls.end(),
                                           [](const auto &c) { return c.key.kind == ControlKind::button; }))
                devices_.push_back(d);
        cache_devices();
        ++inventory_revision_;
        std::vector<const wchar_t *> names;
        for (const auto &name : device_names_)
            names.push_back(name.c_str());
        for (auto &row : rows_)
            if (!row.action) {
                ag_choice_replace(control(row.fields[3]).handle(), names.data(), static_cast<uint32_t>(names.size()),
                                  0);
                const wchar_t *empty = L"—";
                ag_choice_replace(control(row.fields[4]).handle(), &empty, 1, 0);
                row.controls.clear();
                row.device = -1;
                row.binding_key = L"-1:" + std::to_wstring(inventory_revision_);
            }
        inventory_dirty_ = false;
        changed = true;
    }
    if (view.button_revision != revision_) {
        configuration_ = runtime_->buttons();
        revision_ = view.button_revision;
        changed = true;
    }
    if (!changed && !readouts_changed && !deferred_build_)
        return;
    if (readouts_changed && query_.empty() && chapter_ && chapter_ != 100) {
        std::vector<std::string> ids;
        for (const auto *a : visible_actions())
            ids.push_back(a->id);
        changed = changed || ids != visible_ids_;
    }
    // Search eligibility only changes when installed/applicability state changes, not on every pressed readout.
    if (!query_.empty() && readouts_changed) {
        std::vector<std::string> ids;
        for (const auto &a : button_catalogue())
            if (eligible(a))
                ids.push_back(a.id);
        if (ids != search_eligible_) {
            search_eligible_ = std::move(ids);
            changed = true;
        }
    }
    if (changed || deferred_build_)
        build();
    else
        readouts();
}
void ButtonPanel::tips() {
    const auto start = Clock::now();
    for (auto &row : rows_) {
        if (row.entry < 0 || !row.action || entries_[static_cast<std::size_t>(row.entry)].kind != Kind::action)
            continue;
        const auto &a = *row.action;
        std::string tip = a.name + " - " + a.position + "\n" + button_chapter(a.chapter) + "\n" +
                          record_name(a.target.record) + "\n" + a.reference;
        const auto r = configuration_.rows.find(a.id);
        if (r != configuration_.rows.end()) {
            const auto &p = r->second.draft;
            tip += "\n" + p.identity.name + "\nSerial: " + p.identity.serial + "\n" + p.identity.collection_identity +
                   "\n" + r->second.pending;
            if (!p.bindings.empty()) {
                const auto &key = p.bindings[0].sources[0].key;
                if(key.backend==BackendKind::usb_hid)tip += "\n" + p.bindings[0].name + "\nHID usage " + std::to_string(key.usage) + "; report " + std::to_string(key.report_id) +
                       "; collection " + std::to_string(key.link_collection);
                else tip += "\n"+p.identity.hardware_key+"; resource "+std::to_string(key.channel_kind)+"; native channel "+std::to_string(key.channel);
            }
        }
        if (const auto *v = readout(view_, a))
            tip += "\n" + v->reason;
        const auto label = wide(tip);
        if (row.tip != label) {
            for (auto h : row.fields)
                ag_tooltip_set(control(h).handle(), label.c_str());
            row.tip = label;
        }
    }
    timings_.tips_ms += elapsed(start);
}
} // namespace hwio
