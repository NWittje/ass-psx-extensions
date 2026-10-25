#include "hwio/catalogue.hpp"
#include "hwio/axis_panel.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hwio {
namespace {
constexpr int header = 104, base = 1000, stride = 32;
constexpr int last_input = 10, master = 11, learn_button = 12, clear_button = 13, override_label = 14, issue = 15,
              layout_check = 16;
std::wstring number(double v) {
    std::wostringstream s;
    s << std::setprecision(6) << v;
    return s.str();
}
bool has(const AxisSetup &s) {
    return !s.source.bindings.empty();
}
const Binding *binding(const AxisSetup &s) {
    return has(s) ? &s.source.bindings[0] : nullptr;
}
std::vector<std::wstring> capture_labels(std::size_t slot, bool reverse, const Binding *b, int throttle_choice) {
    std::vector<std::wstring> captures;
    if (slot == 13)
        captures = {L"UP", L"1", L"5", L"10", L"20", L"25", L"30"};
    else if (reverse)
        captures = {L"Stowed", L"", L"Full rev"};
    else if (slot < 8)
        captures = slot % 4 == 1 ? std::vector<std::wstring>{L"Forward", L"Centre", L"Aft"}
                                 : std::vector<std::wstring>{L"Left", L"Centre", L"Right"};
    else if (slot >= 9 && slot <= 12 &&
             ((!b && throttle_choice != 1) || (b && b->throttle_layout == ThrottleLayout::combined)))
        captures = {L"Full rev", L"Idle", L"Full fwd"};
    else
        captures = {slot >= 14 ? L"Released" : L"Zero", L"", L"Full"};
    return captures;
}
} // namespace
ass::graphics::Control &AxisPanel::control(HWND h) {
    return widgets_.at(widget_index_.at(h));
}
void AxisPanel::item(HWND h, const std::wstring &s) {
    control(h).add(s.c_str());
}
int AxisPanel::selected(HWND h) {
    return static_cast<int>(control(h).value());
}
void AxisPanel::choice(HWND h, int value) {
    control(h).value(value);
}
void AxisPanel::update(HWND h, const std::wstring &s) {
    if (h && control(h).text() != s)
        control(h).text(s.c_str());
}
AxisPanel::~AxisPanel() {
    while (!widgets_.empty())
        widgets_.pop_back();
}
HWND AxisPanel::add(HWND p, uint32_t kind, const std::wstring &title, uint32_t flags, int x, int y, int w, int h,
                    int id) {
    ag_control_desc d{};
    d.kind = kind;
    d.id = static_cast<uint32_t>(id);
    d.text = title.c_str();
    d.bounds = {float(x), float(y), float(x + w), float(y + (kind == AG_CHOICE ? 24 : h))};
    d.maximum = 1;
    d.step = 1;
    if (kind == AG_SLIDER) { d.minimum = -1; d.step = .01; }
    widgets_.emplace_back(*context_, p, d, event, this);
    auto &widget = widgets_.back();
    const auto hwnd = static_cast<HWND>(widget.hwnd());
    widget_index_[hwnd] = widgets_.size() - 1;
    if (flags)
        ass::graphics::check(ag_control_style(widget.handle(), flags));
    return hwnd;
}
void __cdecl AxisPanel::event(void *user, uint32_t id, uint32_t kind, double) {
    auto &self = *static_cast<AxisPanel *>(user);
    if (self.rebuilding_)
        return;
    int notification = -1;
    if (kind == AG_CLICK)
        notification = BN_CLICKED;
    else if (kind == AG_VALUE_CHANGED)
        notification = CBN_SELCHANGE;
    else if (kind == AG_FOCUS_LOST)
        notification = EN_KILLFOCUS;
    if (notification < 0)
        return;
    if (id >= base && (id - base) % stride == 10)
        notification = EN_KILLFOCUS;
    self.actions_.emplace_back(static_cast<int>(id), notification);
    ag_window_post(self.owner_->handle(), dispatch_id);
}
void AxisPanel::dispatch() {
    auto actions = std::move(actions_);
    actions_.clear();
    for (const auto &a : actions)
        command(a.first, a.second);
}
std::wstring AxisPanel::text(HWND h) {
    return control(h).text();
}
void AxisPanel::attach(ass::graphics::Context &context, ass::graphics::Window &owner, Runtime &runtime,void* parent) {
    context_ = &context;
    owner_ = &owner;
    parent_ = static_cast<HWND>(parent?parent:owner.hwnd());
    runtime_ = &runtime;
    configuration_ = runtime.axes();
    ag_control_desc d{};
    d.kind = AG_PANEL;
    d.bounds = {0, 392, 800, 400};
    d.text = L"";
    pane_ = ass::graphics::Control(context, parent_, d);
    window_ = static_cast<HWND>(pane_.hwnd());
    d.kind = AG_BUTTON;
    d.id = 9;
    d.text = L"Expand or collapse assignments";
    d.bounds = {0, 0, 800, 8};
    grip_ = ass::graphics::Control(context, window_, d, event, this);
    ass::graphics::check(ag_control_style(grip_.handle(), AG_STYLE_FOLDOUT));
    build();
    layout();
}
void AxisPanel::build_axes(std::size_t index) {
    auto &row = rows[index];
    const int id = base + static_cast<int>(index) * stride;
    const auto h = GetDlgItem(rows_, id + 1);
    std::vector<std::wstring> labels{L"Choose axis..."};
    row.controls.clear();
    int choice = 0;
    const auto &draft = configuration_.rows[row.slot].draft;
    const auto *b = binding(draft);
    if (row.device >= 0 && static_cast<std::size_t>(row.device) < devices_.size()) {
        const auto &d = devices_[static_cast<std::size_t>(row.device)];
        for (std::size_t c = 0; c < d.controls.size(); ++c) {
            const auto &control = d.controls[c];
            if (control.relative || control.key.kind != ControlKind::axis)
                continue;
            for (int reversed = 0; reversed < 2; ++reversed) {
                row.controls.push_back(c);
                labels.push_back(wide(control.name) + (reversed ? L" reversed" : L" normal"));
                if (b && same(control.key, row.reverse ? b->reverse_source.key : b->sources[0].key) &&
                    (row.reverse ? b->reverse_calibration.inverted : b->calibration.inverted) == (reversed != 0))
                    choice = static_cast<int>(row.controls.size());
            }
        }
    }
    if (labels != row.axis_labels) {
        std::vector<const wchar_t *> items;
        for (const auto &label : labels)
            items.push_back(label.c_str());
        ass::graphics::check(
            ag_choice_replace(control(h).handle(), items.data(), static_cast<uint32_t>(items.size()), choice));
        row.axis_labels = std::move(labels);
    } else if (selected(h) != choice)
        this->choice(h, choice);
}
bool AxisPanel::sync_rows() {
    if (!rows_)
        return false;
    std::vector<std::pair<std::size_t, bool>> wanted;
    for (std::size_t i = 0; i < 8; ++i)
        wanted.emplace_back(i, false);
    for (std::size_t i = 9; i < 13; ++i)
        wanted.emplace_back(i, false);
    for (std::size_t i = 9; i < 13; ++i) {
        const auto *b = binding(configuration_.rows[i].draft);
        if (configuration_.throttle_choice == 1 ||
            (b && b->throttle_layout == ThrottleLayout::separate_forward_reverse))
            wanted.emplace_back(i, true);
    }
    wanted.emplace_back(8, false);
    wanted.emplace_back(13, false);
    for (std::size_t i = 14; i < 18; ++i)
        wanted.emplace_back(i, false);
    if (wanted.size() != rows.size())
        return false;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (wanted[i] != std::make_pair(rows[i].slot, rows[i].reverse))
            return false;
    rebuilding_ = true;
    ag_control_update_begin(control(rows_).handle());
    std::vector<DeviceIdentity> identities;
    std::vector<std::wstring> names{L"Unassigned"};
    for (const auto &d : devices_) {
        identities.push_back(d.identity);
        names.push_back(wide(d.identity.name));
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto &r = rows[i];
        const auto &draft = configuration_.rows[r.slot].draft;
        const auto &p = r.reverse ? draft.reverse : draft.source;
        r.device = p.id.empty() ? -1 : match_profile(p, identities);
        auto labels = names;
        int selection = std::max(0, r.device + 1);
        if (r.device < 0 && !p.id.empty()) {
            labels.push_back(L"[missing] " + wide(p.identity.name));
            selection = static_cast<int>(devices_.size() + 1);
        }
        const int id = base + static_cast<int>(i) * stride;
        const auto device = GetDlgItem(rows_, id);
        if (labels != r.device_labels) {
            std::vector<const wchar_t *> pointers;
            for (const auto &label : labels)
                pointers.push_back(label.c_str());
            ag_choice_replace(control(device).handle(), pointers.data(), static_cast<uint32_t>(pointers.size()),
                              selection);
            r.device_labels = std::move(labels);
        } else if (selected(device) != selection)
            choice(device, selection);
        build_axes(i);
        const auto labels_capture = capture_labels(r.slot, r.reverse, binding(draft), configuration_.throttle_choice);
        for (std::size_t c = 0; c < labels_capture.size(); ++c)
            if (const auto h = GetDlgItem(rows_, id + 3 + static_cast<int>(c))) {
                update(h, labels_capture[c]);
                ag_visible(control(h).handle(), !labels_capture[c].empty());
            }
        const auto dz = GetDlgItem(rows_, id + 10);
        if (GetFocus() != dz)
            update(dz, (r.reverse ? draft.reverse_custom : draft.custom)
                           ? L"Custom"
                           : number(r.reverse ? draft.reverse_deadzone : draft.deadzone));
    }
    choice(GetDlgItem(window_, master), configuration_.default_fo ? 1 : 0);
    int split = 0, count = 0;
    for (std::size_t i = 9; i < 13; ++i)
        if (const auto *b = binding(configuration_.rows[i].draft)) {
            ++count;
            split += b->throttle_layout == ThrottleLayout::separate_forward_reverse;
        }
    const auto check = GetDlgItem(rows_, layout_check);
    control(check).value(configuration_.throttle_choice >= 0 ? configuration_.throttle_choice : split > 0 ? 1 : 0);
    ag_control_indeterminate(control(check).handle(), configuration_.throttle_choice < 0 && split > 0 && split < count);
    tips();
    ag_control_update_end(control(rows_).handle());
    rebuilding_ = false;
    layout();
    return true;
}
void AxisPanel::build() {
    if (sync_rows())
        return;
    remember_scroll();
    const int saved_scroll = scroll_;
    rebuilding_ = true;
    while (!widgets_.empty())
        widgets_.pop_back();
    widget_index_.clear();
    rows_ = nullptr;
    add(window_, AG_LABEL, L"Move an axis to identify it", 0, 14, 12, 772, 20, last_input);
    add(window_, AG_LABEL, L"Default master", 0, 14, 38, 95, 20, 0);
    auto h = add(window_, AG_CHOICE, L"", 0, 110, 34, 70, 150, master);
    item(h, L"CPT");
    item(h, L"FO");
    choice(h, configuration_.default_fo ? 1 : 0);
    add(window_, AG_BUTTON, L"Learn override button", 0, 190, 34, 148, 24, learn_button);
    add(window_, AG_BUTTON, L"Clear", 0, 344, 34, 45, 24, clear_button);
    add(window_, AG_LABEL, L"", 0, 400, 38, 385, 20, override_label);
    add(window_, AG_LABEL, wide(problem_), 0, 14, 61, 772, 18, issue);
    add(window_, AG_LABEL, L"Controller", 0, 128, 83, 140, 18, 0);
    add(window_, AG_LABEL, L"Axis / direction", 0, 274, 83, 120, 18, 0);
    add(window_, AG_LABEL, L"Capture current position", 0, 436, 83, 202, 18, 0);
    add(window_, AG_LABEL, L"DZ %", 0, 646, 83, 44, 18, 0);
    add(window_, AG_LABEL, L"Raw", 0, 700, 83, 72, 18, 0);
    rows_ = add(window_, AG_PANEL, L"", 0, 0, header, 800, 400, 17);
    rows.clear();
    int y = 0;
    auto make = [&](std::size_t slot, bool reverse) {
        const int id = base + static_cast<int>(rows.size()) * stride;
        Row r;
        r.slot = slot;
        r.reverse = reverse;
        r.y = y;
        std::wstring title = flight_slot_name(slot);
        if (reverse) {
            const auto *t = flight_target(slot);
            title = t && !t->reverse_label.empty() ? wide(t->reverse_label) : L"Reverse " + title;
        }
        add(rows_, AG_LABEL, title, AG_STYLE_ALIGN_RIGHT, 6, y + 4, 111, 20, id + 12);
        add(rows_, AG_LABEL, L"", 0, 121, y, 1, 26, id + 14);
        auto dev = add(rows_, AG_CHOICE, L"", 0, 128, y, 140, 300, id);
        item(dev, L"Unassigned");
        for (const auto &d : devices_)
            item(dev, wide(d.identity.name));
        const auto &draft = configuration_.rows[slot].draft;
        const Profile &p = reverse ? draft.reverse : draft.source;
        std::vector<DeviceIdentity> identities;
        for (const auto &d : devices_)
            identities.push_back(d.identity);
        r.device = p.id.empty() ? -1 : match_profile(p, identities);
        if (r.device < 0 && !p.id.empty()) {
            item(dev, L"[missing] " + wide(p.identity.name));
            choice(dev, static_cast<int>(devices_.size() + 1));
        } else
            choice(dev, r.device + 1);
        add(rows_, AG_CHOICE, L"", 0, 274, y, 120, 300, id + 1);
        add(rows_, AG_BUTTON, L"L.I.", 0, 400, y, 30, 24, id + 2);
        const auto *b = binding(draft);
        const auto captures = capture_labels(slot, reverse, b, configuration_.throttle_choice);
        const int width = slot == 13 ? 28 : 64, gap = slot == 13 ? 1 : 5;
        for (std::size_t c = 0; c < captures.size(); ++c) {
            const auto capture = add(rows_, AG_BUTTON, captures[c], 0, 436 + static_cast<int>(c) * (width + gap), y,
                                     width, 24, id + 3 + static_cast<int>(c));
            ag_visible(control(capture).handle(), !captures[c].empty());
        }
        const bool custom = reverse ? draft.reverse_custom : draft.custom;
        const auto dz =
            add(rows_, AG_EDIT, custom ? L"Custom" : number(reverse ? draft.reverse_deadzone : draft.deadzone), 0, 646,
                y, 44, 24, id + 10);
        (void)dz;
        add(rows_, AG_LABEL, L"--", AG_STYLE_ALIGN_RIGHT, 696, y + 4, 76, 20, id + 11);
        add(rows_, AG_LABEL, L"", 0, 128, y + 26, 644, 18, id + 13);
        rows.push_back(std::move(r));
        y += 48;
        if (!reverse && slot < 8 && slot % 4 < 3) {
            add(rows_, AG_LABEL, L"Control stiffness", 0, 128, y + 4, 140, 22, id + 16);
            const auto slider = add(rows_, AG_SLIDER, L"", 0, 274, y, 370, 24, id + 15);
            control(slider).value(draft.control_stiffness);
            add(rows_, AG_LABEL, L"Softer", 0, 274, y + 48, 90, 20, id + 17);
            add(rows_, AG_LABEL, L"Linear", 0, 432, y + 48, 64, 20, id + 19);
            add(rows_, AG_LABEL, L"Harder", AG_STYLE_ALIGN_RIGHT, 554, y + 48, 90, 20, id + 18);
            y += 72;
        }
    };
    for (std::size_t i = 0; i < 8; ++i)
        make(i, false);
    auto checkbox = add(rows_, AG_CHECKBOX, L"Separate reversers", 0, 128, y, 220, 24, layout_check);
    int split = 0, count = 0;
    for (std::size_t i = 9; i < 13; ++i)
        if (const auto b = binding(configuration_.rows[i].draft)) {
            ++count;
            if (b->throttle_layout == ThrottleLayout::separate_forward_reverse)
                ++split;
        }
    const bool mixed = configuration_.throttle_choice < 0 && split > 0 && split < count;
    control(checkbox).value(configuration_.throttle_choice >= 0 ? configuration_.throttle_choice : split > 0 ? 1 : 0);
    if (mixed)
        ass::graphics::check(ag_control_indeterminate(control(checkbox).handle(), 1));
    y += 30;
    for (std::size_t i = 9; i < 13; ++i)
        make(i, false);
    for (std::size_t i = 9; i < 13; ++i) {
        const auto b = binding(configuration_.rows[i].draft);
        if (configuration_.throttle_choice == 1 ||
            (b && b->throttle_layout == ThrottleLayout::separate_forward_reverse))
            make(i, true);
    }
    make(8, false);
    make(13, false);
    for (std::size_t i = 14; i < 18; ++i)
        make(i, false);
    content_height_ = y;
    for (std::size_t i = 0; i < rows.size(); ++i)
        build_axes(i);
    control(rows_).extent(static_cast<float>(content_height_));
    tips();
    layout();
    scroll(saved_scroll);
    rebuilding_ = false;
}
void AxisPanel::layout() {
    if (sizing_ || !window_)
        return;
    sizing_ = true;
    if (layout_handler_)
        layout_handler_();
    else
        arrange(static_cast<int>(owner_->client_bounds().bottom));
    sizing_ = false;
}
void AxisPanel::arrange(int client_height, bool visible, int top) {
    if (!rebuilding_)
        remember_scroll();
    visible_height_ = std::max(0, client_height - top - header);
    pane_.bounds({0, float(top), 800, float(std::max(top + 8, client_height))});
    control(rows_).bounds({0, float(header), 800, float(header + visible_height_)});
    ass::graphics::check(ag_visible(pane_.handle(), visible));
    for (auto &widget : widgets_)
        ag_visible(widget.handle(), expanded_ && visible);
    ag_control_style(grip_.handle(), AG_STYLE_FOLDOUT | (expanded_ ? AG_STYLE_EXPANDED : 0));
    scroll(scroll_);
}
void AxisPanel::remember_scroll() {
    if (!rows_)
        return;
    float actual{};
    ass::graphics::check(ag_panel_scroll_get(control(rows_).handle(), &actual));
    scroll_ = static_cast<int>(actual);
}
void AxisPanel::scroll(int position) {
    ass::graphics::check(ag_panel_scroll_set(control(rows_).handle(), float(position)));
    float actual{};
    ag_panel_scroll_get(control(rows_).handle(), &actual);
    scroll_ = static_cast<int>(actual);
}
void AxisPanel::save() {
    std::string failure;
    if (!runtime_->save_axes(configuration_, failure)) {
        problem_ = failure;
        configuration_ = runtime_->axes();
        build();
        update(GetDlgItem(window_, issue), wide(failure));
        return;
    }
    problem_.clear();
    configuration_ = runtime_->axes();
    update(GetDlgItem(window_, issue),
           L"Saved immediately | L.I. uses the last movement direction | Hover for calibration details");
}
void AxisPanel::expand() {
    expanded_ = true;
    layout();
    ass::graphics::check(ag_focus(grip_.handle()));
}
void AxisPanel::collapse() {
    expanded_ = false;
    layout();
    ass::graphics::check(ag_focus(grip_.handle()));
}
void AxisPanel::resize() {
    if (!sizing_)
        build();
}
void AxisPanel::command(int id, int notification) {
    if (rebuilding_)
        return;
    if (id == 9) {
        expanded_ = !expanded_;
        layout();
        return;
    }
    if (id == master && notification == CBN_SELCHANGE) {
        configuration_.default_fo = selected(GetDlgItem(window_, master)) == 1;
        save();
        return;
    }
    if (id == learn_button) {
        runtime_->learn_override();
        learning_ = true;
        update(GetDlgItem(window_, issue), L"Press the dedicated hardware override button");
        return;
    }
    if (id == clear_button) {
        learning_ = false;
        configuration_.override_assigned = false;
        configuration_.override_source = {};
        save();
        return;
    }
    if (id == layout_check) {
        const bool separate = control(GetDlgItem(rows_, layout_check)).value() != 0;
        configuration_.throttle_choice = separate ? 1 : 0;
        for (std::size_t i = 9; i < 13; ++i) {
            auto &r = configuration_.rows[i];
            axis_layout(r.draft, separate);
            if (has(r.draft))
                axis_apply(r, i);
        }
        save();
        build();
        return;
    }
    if (id < base)
        return;
    const auto index = static_cast<std::size_t>((id - base) / stride);
    const int field = (id - base) % stride;
    if (index >= rows.size())
        return;
    auto &row = rows[index];
    auto &config = configuration_.rows[row.slot];
    const int rowid = base + static_cast<int>(index) * stride;
    if (field == 15) {
        const auto slider = GetDlgItem(rows_, rowid + 15);
        row.stiffness_pending = GetCapture() == slider;
        if (row.stiffness_pending) return;
        const double value = control(slider).value();
        if (value == config.draft.control_stiffness) return;
        config.draft.control_stiffness = value;
        if (has(config.draft)) axis_apply(config, row.slot);
        save();
        return;
    }
    if (field == 0 && notification == CBN_SELCHANGE) {
        const int device = selected(GetDlgItem(rows_, rowid)) - 1;
        if (device < 0) {
            if (row.reverse) {
                config.draft.reverse = {};
                axis_apply(config, row.slot);
            } else {
                const double stiffness = config.draft.control_stiffness;
                config.draft = {};
                config.applied = {};
                config.draft.control_stiffness = config.applied.control_stiffness = stiffness;
                config.active = false;
                config.managed = true;
                config.pending.clear();
            }
            save();
            build();
            return;
        }
        row.device = device;
        build_axes(index);
        choice(GetDlgItem(rows_, rowid + 1), 0);
        return;
    }
    if ((field == 1 && notification == CBN_SELCHANGE) || field == 2) {
        int device = row.device, control = -1;
        bool reversed = false;
        if (field == 2) {
            if (!view_.last_axis.available)
                return;
            for (std::size_t d = 0; d < devices_.size(); ++d)
                if (devices_[d].slot == view_.last_axis.sample.device &&
                    devices_[d].generation == view_.last_axis.sample.generation) {
                    device = static_cast<int>(d);
                    control = view_.last_axis.sample.control;
                    reversed = view_.last_axis.reversed;
                }
        } else {
            const int c = selected(GetDlgItem(rows_, rowid + 1)) - 1;
            if (c < 0 || static_cast<std::size_t>(c) >= row.controls.size())
                return;
            control = static_cast<int>(row.controls[static_cast<std::size_t>(c)]);
            reversed = c % 2 != 0;
        }
        if (device < 0 || static_cast<std::size_t>(device) >= devices_.size() || control < 0)
            return;
        const auto &d = devices_[static_cast<std::size_t>(device)];
        if (static_cast<std::size_t>(control) >= d.controls.size())
            return;
        const auto *b = binding(config.draft);
        const auto &profile = row.reverse ? config.draft.reverse : config.draft.source;
        const bool same_source = b && identity_score(profile.identity, d.identity) > 0 &&
                                 same(row.reverse ? b->reverse_source.key : b->sources[0].key,
                                      d.controls[static_cast<std::size_t>(control)].key);
        if (same_source) {
            auto &cal = row.reverse ? config.draft.source.bindings[0].reverse_calibration
                                    : config.draft.source.bindings[0].calibration;
            if (cal.inverted != reversed) {
                cal.inverted = reversed;
                if (row.reverse)
                    std::swap(config.draft.reverse_points[0], config.draft.reverse_points[2]);
                else if (row.slot == 13 && config.draft.detents)
                    std::reverse(config.draft.points.begin(), config.draft.points.end());
                else
                    std::swap(config.draft.points[0], config.draft.points[2]);
            }
        } else {
            axis_source(config.draft, row.slot, d, d.controls[static_cast<std::size_t>(control)], reversed,
                        row.reverse);
            if (row.slot >= 9 && row.slot <= 12 && configuration_.throttle_choice >= 0)
                axis_layout(config.draft, configuration_.throttle_choice == 1);
        }
        axis_apply(config, row.slot);
        save();
        build();
        return;
    }
    if (field >= 3 && field <= 9) {
        const auto &v = view_.axes.rows[row.slot];
        if (!(row.reverse ? v.reverse_available : v.available))
            return;
        const auto point = static_cast<std::size_t>(field - 3);
        if (row.reverse) {
            if (point > 2)
                return;
            config.draft.reverse_points[point] = v.reverse_raw;
            config.draft.reverse_captured |= 1u << point;
        } else {
            config.draft.points[point] = v.raw;
            config.draft.captured |= 1u << point;
            if (row.slot == 13) {
                if (!config.draft.detents)
                    config.draft.captured = 1u << point;
                config.draft.detents = true;
            }
        }
        axis_apply(config, row.slot);
        save();
        build();
        return;
    }
    if (field == 10 && notification == EN_KILLFOCUS) {
        auto value = text(GetDlgItem(rows_, rowid + 10));
        if (value == L"Custom")
            return;
        try {
            std::size_t n{};
            double amount = std::stod(value, &n);
            if (n != value.size() || !std::isfinite(amount) || amount < 0 || amount >= 100)
                throw std::invalid_argument("deadzone");
            if (row.reverse) {
                config.draft.reverse_deadzone = amount;
                config.draft.reverse_custom = false;
            } else {
                config.draft.deadzone = amount;
                config.draft.custom = false;
            }
            axis_apply(config, row.slot);
            save();
        } catch (...) {
            config.pending = "Enter a finite deadzone percentage from 0 to less than 100";
            update(GetDlgItem(rows_, rowid + 13), wide(config.pending));
        }
    }
}
void AxisPanel::refresh(const RuntimeView &view) {
    view_.axes = view.axes;
    view_.last_axis = view.last_axis;
    remember_scroll();
    const auto focus = GetFocus();
    std::vector<int> released;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].stiffness_pending && GetCapture() != GetDlgItem(rows_, base + static_cast<int>(i) * stride + 15)) {
            rows[i].stiffness_pending = false;
            released.push_back(base + static_cast<int>(i) * stride + 15);
        }
    }
    for (const auto id : released) command(id, CBN_SELCHANGE);
    const bool editing = (focus && GetParent(focus) == rows_ && (GetDlgCtrlID(focus) - base) % stride == 10) ||
        std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.stiffness_pending; });
    bool opened = false;
    if (const auto found = widget_index_.find(focus); found != widget_index_.end()) {
        uint32_t state{};
        if (ag_control_activity(widgets_[found->second].handle(), &state) >= 0 && (state & AG_ACTIVITY_DROPDOWN)) {
            opened = true;
        }
    }
    bool changed = false;
    std::size_t count = 0;
    for (const auto &d : view.devices)
        if (d.connected) {
            if (count >= devices_.size() || devices_[count].generation != d.generation ||
                devices_[count].slot != d.slot)
                changed = true;
            ++count;
        }
    changed = changed || count != devices_.size();
    if ((changed || revision_ != view.axis_revision) && !editing && !opened) {
        if (changed) {
            devices_.clear();
            for (const auto &d : view.devices)
                if (d.connected)
                    devices_.push_back(d);
        }
        configuration_ = runtime_->axes();
        revision_ = view.axis_revision;
        const int position = scroll_;
        build();
        scroll(position);
    }
    if (learning_)
        if (const auto sample = runtime_->learned()) {
            for (const auto &d : devices_)
                if (d.slot == sample->device && d.generation == sample->generation &&
                    sample->control < d.controls.size() &&
                    d.controls[sample->control].key.kind == ControlKind::button) {
                    Profile p;
                    p.id = "seat-override";
                    p.name = d.controls[sample->control].name;
                    p.identity = d.identity;
                    p.explicit_path = true;
                    Binding b;
                    b.id = 1;
                    b.name = "Seat override";
                    b.enabled = false;
                    b.target = axis_target(0);
                    b.sources = {{d.controls[sample->control].key, {}}};
                    p.bindings = {b};
                    configuration_.override_source = std::move(p);
                    configuration_.override_assigned = true;
                    learning_ = false;
                    save();
                }
        }
    if (!expanded_)
        return;
    const auto &last = view.last_axis;
    update(GetDlgItem(window_, last_input),
           last.present ? wide(last.device + " - " + last.axis + (last.reversed ? " - Reversed" : " - Normal")) +
                              (last.available ? L"" : L" [disconnected]")
                        : L"Move an axis to identify it");
    update(GetDlgItem(window_, override_label),
           configuration_.override_assigned
               ? wide(configuration_.override_source.identity.name + " - " + configuration_.override_source.name) +
                     (view.axes.override_held        ? L" [held]"
                      : view.axes.override_available ? L""
                                                     : L" [unavailable]")
               : L"No override button assigned");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto &r = rows[i];
        const auto &v = view.axes.rows[r.slot];
        const int id = base + static_cast<int>(i) * stride;
        if (r.y + 48 <= scroll_ || r.y >= scroll_ + visible_height_)
            continue;
        const auto raw = (r.reverse ? v.reverse_available : v.available)
                             ? std::to_wstring(r.reverse ? v.reverse_raw : v.raw)
                             : L"--";
        if (raw != r.raw_text) {
            update(GetDlgItem(rows_, id + 11), raw);
            r.raw_text = raw;
        }
        const auto &config = configuration_.rows[r.slot];
        std::wstring status = wide(config.pending.empty() ? v.status : config.pending);
        if (config.active && !config.managed)
            status += config.applied.source.bindings[0].enabled ? L" | Existing mapping" : L" | Display-only";
        else if (!config.managed && has(config.draft))
            status += L" | Display-only";
        else if (v.active)
            status += v.selected ? L" | Selected for PSX" : L" | Standby";
        if (status != r.status_text) {
            update(GetDlgItem(rows_, id + 13), status);
            r.status_text = status;
        }
        if (r.last_input_enabled != static_cast<int>(last.available)) {
            r.last_input_enabled = last.available;
            control(GetDlgItem(rows_, id + 2)).enabled(last.available);
        }
        const int enabled = r.reverse ? v.reverse_available : v.available;
        if (r.capture_enabled != enabled) {
            r.capture_enabled = enabled;
            for (int c = 3; c <= 9; ++c)
                if (const HWND control = GetDlgItem(rows_, id + c))
                    this->control(control).enabled(enabled != 0);
        }
    }
}
void AxisPanel::tips() {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto &r = rows[index];
        const auto &config = configuration_.rows[r.slot];
        const auto &s = config.draft;
        const auto &p = r.reverse ? s.reverse : s.source;
        std::wostringstream tip;
        if (const auto *t = flight_target(r.slot))
            tip << wide(t->descriptor.name) << L" | " << wide(record_name(t->descriptor.record)) << L"\n";
        tip << wide(p.identity.name) << L"\nSerial: " << wide(p.identity.serial) << L"\nProfile: " << wide(p.id)
            << L"\nCaptured raw positions: ";
        if (r.reverse)
            for (auto n : s.reverse_points)
                tip << n << L" ";
        else
            for (std::size_t i = 0; i < (r.slot == 13 ? 7u : 3u); ++i)
                tip << s.points[i] << L" ";
        tip << L"\n"
            << wide(config.pending) << L"\n"
            << (r.slot == 13 ? L"Deadzone: total width around each flap detent"
                : r.slot < 8 ? L"Deadzone: total width around centre"
                             : L"Deadzone: total width at combined idle; each endpoint for sliders");
        const auto label = tip.str();
        for (int field = 0; field < 15; ++field)
            if (auto h = GetDlgItem(rows_, base + static_cast<int>(index) * stride + field))
                ag_tooltip_set(control(h).handle(), label.c_str());
    }
}
} // namespace hwio
