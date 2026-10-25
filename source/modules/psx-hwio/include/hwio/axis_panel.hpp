#pragma once
#include "runtime.hpp"
#include <functional>
#include <ass_graphics/graphics.hpp>

namespace hwio {
class AxisPanel {
    friend struct AxisPanelProbe;
    struct Row {
        std::size_t slot{};
        bool reverse{};
        bool stiffness_pending{};
        int y{}, device{-1};
        std::vector<std::size_t> controls;
        int last_input_enabled{-1}, capture_enabled{-1};
        std::wstring raw_text, status_text;
        std::vector<std::wstring> device_labels, axis_labels;
    };
    HWND parent_{}, window_{}, rows_{};
    ass::graphics::Context *context_{};
    ass::graphics::Window *owner_{};
    ass::graphics::Control pane_, grip_;
    std::vector<ass::graphics::Control> widgets_;
    std::unordered_map<HWND, std::size_t> widget_index_;
    std::deque<std::pair<int, int>> actions_;
    Runtime *runtime_{};
    AxisConfiguration configuration_;
    RuntimeView view_;
    std::vector<DeviceInfo> devices_;
    std::vector<Row> rows;
    std::string problem_;
    std::uint64_t revision_{UINT64_MAX};
    int scroll_{}, content_height_{}, visible_height_{};
    bool rebuilding_{}, learning_{}, sizing_{}, expanded_{};
    std::function<void()> layout_handler_;
    HWND add(HWND parent, uint32_t kind, const std::wstring &text, uint32_t flags, int x, int y, int width, int height,
             int id);
    void build();
    bool sync_rows();
    void build_axes(std::size_t index);
    void save();
    void command(int id, int notification);
    void remember_scroll();
    void scroll(int position);
    void layout();
    ass::graphics::Control &control(HWND);
    std::wstring text(HWND);
    void update(HWND, const std::wstring &);
    void item(HWND, const std::wstring &);
    int selected(HWND);
    void choice(HWND, int);
    void tips();
    static void __cdecl event(void *, uint32_t, uint32_t, double);

  public:
    ~AxisPanel();
    void attach(ass::graphics::Context &, ass::graphics::Window &, Runtime &runtime,void* parent=nullptr);
    void dispatch();
    static constexpr uint32_t dispatch_id = 101;
    void refresh(const RuntimeView &view);
    void resize();
    void expand();
    void collapse();
    void set_layout_handler(std::function<void()> handler) {
        layout_handler_ = std::move(handler);
        layout();
    }
    void arrange(int client_height, bool visible = true, int top = 392);
    bool expanded() const {
        return expanded_;
    }
    HWND window() const {
        return window_;
    }
};
} // namespace hwio
