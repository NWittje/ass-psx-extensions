#pragma once
#include "runtime.hpp"
#include <functional>
#include <ass_graphics/graphics.hpp>

namespace hwio {
struct AssignmentLayout {
    int width{800}, height{400}, button_width{};
    bool narrow{};
};
AssignmentLayout assignment_layout(int work_width, int work_height, bool expanded) noexcept;
ag_rect assignment_tab_bounds(const AssignmentLayout&) noexcept;
class ButtonPanel {
    friend struct ButtonPanelProbe;
    struct Row {
        const ButtonAction *action{};
        int y{}, device{-1}, entry{-1};
        std::uint64_t generation{}, bound_revision{};
        std::array<HWND, 8> fields{};
        std::array<ag_rect, 8> bounds{};
        std::array<bool, 8> shown{};
        HWND frame{};
        ag_rect frame_bounds{};
        bool frame_visible{};
        std::wstring key, binding_key, tip;
        std::vector<std::size_t> controls;
        HWND li{}, readout{};
        bool li_available{true};
        std::string readout_text;
    };
    enum class Kind { action, chapter, breakers, result, unavailable, empty };
    struct Entry {
        Kind kind{};
        const ButtonAction *action{};
        std::wstring key, caption, detail;
        int chapter{}, y{}, height{}, label_height{};
        bool breakers{}, replacement{};
    };
    struct Action {
        int id{}, notification{}, selection{};
        std::uint64_t generation{};
        std::wstring key;
    };
    struct DeviceChoices {
        std::vector<std::wstring> labels;
        std::vector<std::size_t> controls;
    };
    HWND parent_{}, window_{}, rows_window_{};
    ass::graphics::Context *context_{};
    ass::graphics::Window *owner_{};
    ass::graphics::Control pane_;
    std::vector<ass::graphics::Control> widgets_;
    std::unordered_map<HWND, std::size_t> widget_index_;
    std::deque<Action> actions_;
    Runtime *runtime_{};
    RuntimeView view_;
    ButtonConfiguration configuration_;
    std::vector<DeviceInfo> devices_;
    std::vector<Row> rows_;
    std::vector<Entry> entries_;
    std::vector<DeviceChoices> device_choices_;
    std::vector<std::wstring> device_names_;
    std::vector<DeviceIdentity> identities_;
    std::unordered_map<std::string, std::pair<std::wstring, std::wstring>> captions_;
    std::map<std::pair<int, std::wstring>, int> label_heights_;
    std::wstring query_;
    int restore_chapter_{}, restore_scroll_{};
    bool restore_breakers_{};
    HWND search_{}, measure_{}, number_measure_{};
    int number_width_{46};
    std::vector<std::string> visible_ids_;
    std::map<std::pair<int, bool>, int> positions_;
    std::string problem_;
    int chapter_{}, width_{690}, height_{700}, scroll_{}, content_height_{};
    UINT dpi_{};
    bool breakers_{}, compact_{}, rebuilding_{}, deferred_build_{}, realizing_{}, inventory_dirty_{true},
        navigation_dirty_{};
    int selection_override_{-1}, pane_x_{}, pane_y_{};
    bool arranged_{}, pane_visible_{};
    std::uint64_t model_revision_{}, next_generation_{}, inventory_revision_{}, realized_revision_{UINT64_MAX};
    int realized_scroll_{-1}, realized_height_{-1};
    std::vector<std::string> search_eligible_;
    struct Timings {
        double model_ms{}, measure_ms{}, controls_ms{}, choices_ms{}, tips_ms{}, layout_ms{}, total_ms{};
        std::uint64_t builds{}, created{}, rebound{};
    } timings_;
    std::uint64_t revision_{UINT64_MAX};
    HWND add(HWND, uint32_t, const std::wstring &, uint32_t, int, int, int, int, int);
    void build(bool preserve_scroll = true);
    void build_buttons(std::size_t);
    void realize();
    void keyboard(int id, int direction);
    void bind_row(std::size_t, std::size_t);
    void cache_devices();
    void warm_labels();
    void set_search(const std::wstring &);
    void activate_result(const Entry &);
    bool eligible(const ButtonAction &) const;
    bool dropdown_open() const;
    int name_width() const;
    int number_width() const;
    void focus_entry(std::size_t, int field = 3, bool center = false);
    void command(int, int);
    void save(const std::string &replace_action = {});
    void remember_scroll();
    void scroll(int);
    std::vector<const ButtonAction *> visible_actions() const;
    void navigate(int, bool);
    void readouts();
    ass::graphics::Control &control(HWND);
    void item(HWND, const std::wstring &);
    void update(HWND, const std::wstring &);
    int selected(HWND);
    void choice(HWND, int);
    void tips();
    static void __cdecl event(void *, uint32_t, uint32_t, double);

  public:
    ~ButtonPanel();
    void flush_navigation();
    void attach(ass::graphics::Context &, ass::graphics::Window &, Runtime &, void* parent = nullptr);
    void dispatch();
    static constexpr uint32_t dispatch_id = 102;
    void arrange(int x, int y, int width, int height, bool visible);
    void refresh(const RuntimeView &);
    void save_view(bool selected) {
        std::string error;
        if (!save_button_navigation(runtime_->data_dir(), chapter_, breakers_, error, selected ? 1 : 0))
            problem_ = error;
        else
            navigation_dirty_ = false;
    }
    HWND window() const {
        return window_;
    }
};
} // namespace hwio
