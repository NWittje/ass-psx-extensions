#include "psx/cdu/model.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
namespace psx::cdu
{
namespace
{
constexpr unsigned text_base[]{62, 90, 76}, colour_base[]{500, 528, 514}, blank_id[]{89, 91, 90},
    brt_id[]{404, 406, 405}, lcd_bit[]{19, 20, 21};
struct Key
{
    char type{};
    unsigned id{};
    std::string_view value;
    bool ok{};
};
Key key(std::string_view s)
{
    Key k;
    if (s.size() < 4 || s[0] != 'Q')
        return k;
    auto eq = s.find('=');
    if (eq == s.npos || eq < 3)
        return k;
    k.type = s[1];
    auto n = s.substr(2, eq - 2);
    auto r = std::from_chars(n.data(), n.data() + n.size(), k.id);
    k.ok = r.ec == std::errc{} && r.ptr == n.data() + n.size();
    k.value = s.substr(eq + 1);
    return k;
}
bool relevant_key(Key k)
{
    if (!k.ok)
        return false;
    if (k.type == 'i')
        return k.id == 248 || (k.id >= 89 && k.id <= 91);
    if (k.type == 'h')
        return k.id >= 404 && k.id <= 406;
    return k.type == 's' && ((k.id >= 62 && k.id <= 103) || (k.id >= 500 && k.id <= 541));
}
void accept(Receipt &r, std::uint64_t gen, std::uint64_t seq, std::uint64_t time)
{
    r.valid = true;
    r.generation = gen;
    r.sequence = seq;
    r.received_ns = time;
}
Outcome reject(Receipt &r, Reject why, std::uint64_t time)
{
    ++r.rejections;
    r.rejected_ns = time;
    return {Result::rejected, why};
}
bool integer(std::string_view s, std::int32_t &n)
{
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    return !s.empty() && r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
} // namespace
bool CduScreen::lcd() const
{
    return (appearance.value & (1u << lcd_bit[unsigned(unit)])) != 0;
}
ScreenView screen_view(const CduScreen &s) noexcept
{
    ScreenView v{&s.cells,
                 s.unit,
                 0,
                 0,
                 s.appearance.receipt.valid,
                 s.blanking.receipt.valid,
                 s.appearance.value,
                 s.blanking.value};
    for (unsigned row = 0; row < rows; ++row)
    {
        if (s.text[row].receipt.valid)
            v.text_valid |= std::uint16_t(1u << row);
        if (s.colour[row].receipt.valid)
            v.colour_valid |= std::uint16_t(1u << row);
    }
    return v;
}
Model::Model()
{
    for (unsigned u = 0; u < 3; ++u)
        screens[u].unit = Unit(u);
}
void Model::begin_generation()
{
    ++generation;
    for (unsigned u = 0; u < 3; ++u)
    {
        screens[u] = CduScreen{};
        screens[u].unit = Unit(u);
        screens[u].generation = generation;
    }
}
bool Model::relevant(std::string_view s)
{
    return s == "load1" || s == "load2" || s == "load3" || s == "exit" ||
           s.starts_with("version=") || relevant_key(key(s));
}
Outcome Model::receive(std::string_view s, std::uint64_t time)
{
    ++sequence;
    if (s == "load1")
    {
        begin_generation();
        loading = true;
        return {Result::lifecycle};
    }
    if (s == "load2")
        return {Result::lifecycle};
    if (s == "load3")
    {
        loading = false;
        return {Result::lifecycle};
    }
    if (s == "exit")
        return {Result::lifecycle};
    if (s.starts_with("version="))
    {
        if (s.size() > 136 || s.size() == 8)
            return {Result::rejected, Reject::length};
        source_version = s.substr(8);
        return {Result::lifecycle};
    }
    auto k = key(s);
    if (!relevant_key(k))
        return {};
    if (s.find_first_of("\r\n\0", 0, 3) != s.npos)
    {
        ++framing_rejections;
        return {Result::rejected, Reject::framing};
    }
    if (k.type == 's')
    {
        for (unsigned u = 0; u < 3; ++u)
        {
            auto &screen = screens[u];
            bool colour = k.id >= colour_base[u] && k.id < colour_base[u] + rows;
            bool text = k.id >= text_base[u] && k.id < text_base[u] + rows;
            if (!colour && !text)
                continue;
            unsigned row = k.id - (colour ? colour_base[u] : text_base[u]);
            auto &f = colour ? screen.colour[row] : screen.text[row];
            if (colour)
            {
                if (k.value.size() > columns)
                    return reject(f.receipt, Reject::length, time);
                f.value = k.value;
                accept(f.receipt, generation, sequence, time);
                for (unsigned c = 0; c < columns; ++c)
                    screen.cells[row][c].colour =
                        k.value.empty() ? 'w'
                                        : k.value[std::min<std::size_t>(c, k.value.size() - 1)];
            }
            else
            {
                bool scratch = row == 13;
                auto len = k.value.size();
                if (scratch ? len > 24 : len < 24 || len > 48)
                    return reject(f.receipt, Reject::length, time);
                if (!scratch)
                    for (unsigned c = 24; c < len; ++c)
                        if (k.value[c] != '+' && k.value[c] != '-')
                            return reject(f.receipt, Reject::size, time);
                f.value = k.value;
                accept(f.receipt, generation, sequence, time);
                for (unsigned c = 0; c < columns; ++c)
                {
                    auto &cell = screen.cells[row][c];
                    cell.raw = c < std::min<std::size_t>(len, 24) ? k.value[c] : ' ';
                    cell.size = row % 2 == 0 || scratch ? Size::large : Size::small;
                    if (!scratch && len > 24)
                        cell.size = k.value[24 + std::min<std::size_t>(c, len - 25)] == '+'
                                        ? Size::large
                                        : Size::small;
                }
            }
            return {Result::accepted};
        }
    }
    std::int32_t n{};
    if (k.type == 'i' && k.id == 248)
    {
        if (!integer(k.value, n))
        {
            for (auto &screen : screens)
                reject(screen.appearance.receipt, Reject::integer, time);
            return {Result::rejected, Reject::integer};
        }
        for (auto &screen : screens)
        {
            screen.appearance.value = static_cast<std::uint32_t>(n);
            accept(screen.appearance.receipt, generation, sequence, time);
        }
        return {Result::accepted};
    }
    for (unsigned u = 0; u < 3; ++u)
    {
        bool brt = k.type == 'h' && k.id == brt_id[u];
        bool blank = k.type == 'i' && k.id == blank_id[u];
        if (!brt && !blank)
            continue;
        auto &f = brt ? screens[u].brightness : screens[u].blanking;
        if (!integer(k.value, n))
            return reject(f.receipt, Reject::integer, time);
        if (brt && (n < 0 || n > 4713))
            return reject(f.receipt, Reject::range, time);
        f.value = n;
        accept(f.receipt, generation, sequence, time);
        return {Result::accepted};
    }
    return {};
}
} // namespace psx::cdu
