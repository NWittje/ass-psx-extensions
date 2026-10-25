#pragma once
#include "trim/state.hpp"
#include "trim/layout.hpp"
#include <span>
namespace trim {
constexpr int width=320, height=480, strip_rows=8, max_strip_pixels=480*strip_rows;
struct Rect {
    int x{}, y{}, w{}, h{};
    bool contains(int px,int py) const noexcept {return px>=x&&py>=y&&px<x+w&&py<y+h;}
    bool operator==(const Rect&) const = default;
};
constexpr Rect screen{0,0,width,height}, viewport{80,14,160,452};
constexpr Rect tape{148,49,24,330}, bar{layout::bar_x,layout::bar_y,layout::bar_width,layout::bar_max_height};
constexpr Rect green_down{130,93,14,40}, green_up{130,233,14,45};
constexpr Rect off_plate{151,49,17,82}, link_badge{244,18,68,22};
struct DirtyRegions {
    std::array<Rect,4> rects{};
    std::size_t count{};
    void add(Rect) noexcept;
};
class TrimScene {
public:
    explicit TrimScene(Role role) noexcept:role_(role){}
    DirtyRegions changes(const Visual& next, bool full=false) const noexcept;
    bool compose(Rect strip,const Visual&,std::span<std::uint16_t> pixels) const noexcept;
    void commit(const Visual& v) noexcept {last_=v;has_last_=true;}
    bool has_last() const noexcept {return has_last_;}
private:
    Role role_;
    Visual last_{};
    bool has_last_{};
};
// Task-owned FIFO mirrors submission order. ISR signals a count, never mutates these slots.
class DmaSlots {
public:
    int claim() noexcept;
    bool submit(int slot) noexcept;
    bool abandon(int slot) noexcept;
    bool complete() noexcept;
    bool idle() const noexcept {return pending_==0;}
private:
    std::array<std::uint8_t,2> states_{}, fifo_{};
    unsigned head_{}, pending_{};
};

namespace assets {
std::uint16_t background(Role,int x,int y) noexcept;
std::uint16_t off_pixel(Role,int x,int y) noexcept;
std::uint16_t bar_pixel(Role,int x,int y) noexcept;
std::uint16_t green_pixel(Role,bool up,int x,int y) noexcept;
std::array<std::uint8_t,32> identity(Role) noexcept;
constexpr std::uint16_t lettering=0xf7bc, outer_green=0x07ef, unavailable_color=0xfc00;
}

}
