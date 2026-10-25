#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace psx::cdu
{
constexpr unsigned columns = 24, rows = 14;
enum class Unit : unsigned
{
    cpt,
    fo,
    centre
};
enum class Size : unsigned char
{
    small,
    large
};
struct Cell
{
    char raw{' '};
    Size size{Size::large};
    char colour{'w'};
    bool operator==(const Cell &) const = default;
    bool blank() const
    {
        return raw == ' ' || raw == '_';
    }
    bool highlighted() const
    {
        return colour == 'y';
    }
};
using Cells = std::array<std::array<Cell, columns>, rows>;
struct ScreenView
{
    const Cells *cells{};
    Unit unit{};
    std::uint16_t text_valid{}, colour_valid{};
    bool appearance_valid{}, blanking_valid{};
    std::uint32_t appearance{};
    std::int32_t blanking{};
    bool lcd() const
    {
        constexpr unsigned bits[]{19, 20, 21};
        return (appearance & (1u << bits[unsigned(unit)])) != 0;
    }
};
struct Receipt
{
    bool valid{};
    std::uint64_t generation{}, sequence{}, received_ns{}, rejections{}, rejected_ns{};
};
template <class T> struct Field
{
    T value{};
    Receipt receipt{};
};
struct CduScreen
{
    Cells cells{};
    std::array<Field<std::string>, rows> text{}, colour{};
    Field<std::int32_t> brightness{}, blanking{};
    Field<std::uint32_t> appearance{};
    std::uint64_t generation{1};
    bool lcd() const;
    bool ng() const
    {
        return (appearance.value & (1u << 13)) != 0;
    }
    Unit unit{};
};
ScreenView screen_view(const CduScreen &) noexcept;
enum class Result
{
    ignored,
    accepted,
    rejected,
    lifecycle
};
enum class Reject
{
    none,
    framing,
    length,
    size,
    integer,
    range,
    lifecycle
};
struct Outcome
{
    Result result{Result::ignored};
    Reject reason{Reject::none};
};
struct Model
{
    std::array<CduScreen, 3> screens{};
    std::uint64_t generation{1}, sequence{}, framing_rejections{};
    std::string source_version;
    bool loading{};
    Model();
    void begin_generation();
    Outcome receive(std::string_view record, std::uint64_t receipt_ns);
    static bool relevant(std::string_view record);
};
constexpr std::string_view glyph_codes =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789<>.,:/%+-=*_()#obudlrt ";
// Receipt time is monotonic observer time, never a source/atomic page timestamp.
} // namespace psx::cdu
