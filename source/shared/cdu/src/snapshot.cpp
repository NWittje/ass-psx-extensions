#include "psx/cdu/snapshot.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
namespace psx::cdu
{
namespace
{
bool fail(std::string *e, const char *s)
{
    if (e)
        *e = s;
    return false;
}
bool valid(const CduSnapshot &s, std::string *e)
{
    if (unsigned(s.unit) > 2 || unsigned(s.source_state) > 3 || s.source_label_size > 128 ||
        s.text_valid > 0x3fff || s.colour_valid > 0x3fff || s.scalar_valid > 7 || !s.generation ||
        !s.publication_sequence || !s.cache_revision ||
        std::all_of(s.session.begin(), s.session.end(), [](auto b) { return !b; }))
        return fail(e, "Invalid snapshot envelope");
    if ((s.scalar_valid & 2) && (s.brightness < 0 || s.brightness > 4713))
        return fail(e, "Invalid brightness");
    if (!(s.scalar_valid & 1) && s.appearance)
        return fail(e, "Unknown appearance must be zero");
    if (!(s.scalar_valid & 2) && s.brightness)
        return fail(e, "Unknown brightness must be zero");
    if (!(s.scalar_valid & 4) && s.blanking)
        return fail(e, "Unknown blanking must be zero");
    for (unsigned i = 0; i < s.source_label_size; ++i)
        if (s.source_label[i] == '\0' || s.source_label[i] == '\r' || s.source_label[i] == '\n')
            return fail(e, "Invalid source label");
    for (unsigned i = s.source_label_size; i < 128; ++i)
        if (s.source_label[i])
            return fail(e, "Noncanonical source label tail");
    for (unsigned i = 0; i < 31; ++i)
    {
        bool known = i < 14   ? bool(s.text_valid & (1u << i))
                     : i < 28 ? bool(s.colour_valid & (1u << (i - 14)))
                              : bool(s.scalar_valid & (1u << (i - 28)));
        auto r = s.receipts[i];
        if (known ? !r.sequence || r.elapsed_ns > s.host_elapsed_ns : r.sequence || r.elapsed_ns)
            return fail(e, "Invalid component receipt");
    }
    // Accepted sequence and receipt time describe the same single-owner stream.
    // Bounded pair checks avoid allocations or sorting during screen updates.
    for (unsigned i = 0; i < 31; ++i)
        for (unsigned j = i + 1; j < 31; ++j)
        {
            const auto a = s.receipts[i], b = s.receipts[j];
            if (!a.sequence || !b.sequence)
                continue;
            if ((a.sequence == b.sequence && a.elapsed_ns != b.elapsed_ns) ||
                (a.sequence < b.sequence && a.elapsed_ns > b.elapsed_ns) ||
                (a.sequence > b.sequence && a.elapsed_ns < b.elapsed_ns))
                return fail(e, "Inconsistent receipt ordering");
        }
    for (unsigned r = 0; r < rows; ++r)
        for (const auto &c : s.cells[r])
        {
            if (unsigned(c.size) > 1)
                return fail(e, "Invalid glyph size");
            if (s.text_valid & (1u << r))
            {
                if (c.raw == '\0' || c.raw == '\r' || c.raw == '\n')
                    return fail(e, "Invalid raw character");
            }
            else if (c.raw != ' ' || c.size != Size::large)
                return fail(e, "Noncanonical unknown text");
            if (s.colour_valid & (1u << r))
            {
                if (c.colour == '\0' || c.colour == '\r' || c.colour == '\n')
                    return fail(e, "Invalid colour character");
            }
            else if (c.colour != 'w')
                return fail(e, "Noncanonical unknown colour");
        }
    return true;
}
void put(WireBuffer &b, unsigned at, std::uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        b.bytes[at + i] = std::uint8_t(v >> (8 * i));
}
std::uint64_t get(std::span<const std::uint8_t> b, unsigned at, unsigned n)
{
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= std::uint64_t(b[at + i]) << (8 * i);
    return v;
}
} // namespace
CduSnapshot make_snapshot(const CduScreen &s, std::string_view label, SourceState state,
                          const SessionId &id, std::uint64_t pub, std::uint64_t rev,
                          std::uint64_t sample, std::uint64_t origin)
{
    if (label.size() > 128 || sample < origin)
        throw std::runtime_error("Invalid snapshot source metadata");
    CduSnapshot o;
    o.session = id;
    o.generation = s.generation;
    o.publication_sequence = pub;
    o.cache_revision = rev;
    o.host_elapsed_ns = sample - origin;
    o.unit = s.unit;
    o.source_state = state;
    o.source_label_size = std::uint8_t(label.size());
    std::copy(label.begin(), label.end(), o.source_label.begin());
    o.cells = s.cells;
    auto receipt = [&](unsigned i, const Receipt &r) {
        if (r.valid)
        {
            if (r.received_ns < origin || r.received_ns > sample)
                throw std::runtime_error("Receipt outside host clock origin");
            o.receipts[i] = {r.sequence, r.received_ns - origin};
        }
    };
    for (unsigned r = 0; r < rows; ++r)
    {
        if (s.text[r].receipt.valid)
            o.text_valid |= std::uint16_t(1u << r);
        else
            for (auto &c : o.cells[r])
            {
                c.raw = ' ';
                c.size = Size::large;
            }
        if (s.colour[r].receipt.valid)
            o.colour_valid |= std::uint16_t(1u << r);
        else
            for (auto &c : o.cells[r])
                c.colour = 'w';
        receipt(r, s.text[r].receipt);
        receipt(r + 14, s.colour[r].receipt);
    }
    if (s.appearance.receipt.valid)
    {
        o.scalar_valid |= 1;
        o.appearance = s.appearance.value;
    }
    if (s.brightness.receipt.valid)
    {
        o.scalar_valid |= 2;
        o.brightness = s.brightness.value;
    }
    if (s.blanking.receipt.valid)
    {
        o.scalar_valid |= 4;
        o.blanking = s.blanking.value;
    }
    receipt(28, s.appearance.receipt);
    receipt(29, s.brightness.receipt);
    receipt(30, s.blanking.receipt);
    return o;
}
bool encode(const CduSnapshot &s, WireBuffer &out, std::string *e)
{
    if (!valid(s, e))
        return false;
    WireBuffer b;
    b.size = wire_base_bytes + s.source_label_size;
    b.bytes[0] = 'C';
    b.bytes[1] = 'D';
    b.bytes[2] = 'U';
    b.bytes[3] = '1';
    put(b, 4, 1, 2);
    put(b, 6, 96, 2);
    put(b, 8, b.size, 4);
    put(b, 12, 1, 2);
    b.bytes[14] = std::uint8_t(s.unit);
    b.bytes[15] = std::uint8_t(s.source_state);
    std::copy(s.session.begin(), s.session.end(), b.bytes.begin() + 16);
    put(b, 32, s.generation, 8);
    put(b, 40, s.publication_sequence, 8);
    put(b, 48, s.cache_revision, 8);
    put(b, 56, s.host_elapsed_ns, 8);
    put(b, 64, s.appearance, 4);
    put(b, 68, std::bit_cast<std::uint32_t>(s.brightness), 4);
    put(b, 72, std::bit_cast<std::uint32_t>(s.blanking), 4);
    put(b, 76, s.text_valid, 2);
    put(b, 78, s.colour_valid, 2);
    b.bytes[80] = s.scalar_valid;
    b.bytes[81] = s.source_label_size;
    unsigned at = 96;
    for (unsigned i = 0; i < s.source_label_size; ++i)
        b.bytes[at++] = std::uint8_t(s.source_label[i]);
    for (auto r : s.receipts)
    {
        put(b, at, r.sequence, 8);
        put(b, at + 8, r.elapsed_ns, 8);
        at += 16;
    }
    for (const auto &row : s.cells)
        for (auto c : row)
        {
            b.bytes[at++] = std::uint8_t(c.raw);
            b.bytes[at++] = std::uint8_t(c.size);
            b.bytes[at++] = std::uint8_t(c.colour);
        }
    out = b;
    return true;
}
bool decode(std::span<const std::uint8_t> b, CduSnapshot &out, std::string *e)
{
    if (b.size() < wire_base_bytes || b.size() > wire_max_bytes)
        return fail(e, "Invalid wire length");
    if (b[0] != 'C' || b[1] != 'D' || b[2] != 'U' || b[3] != '1' || get(b, 4, 2) != 1 ||
        get(b, 6, 2) != 96 || get(b, 8, 4) != b.size() || get(b, 12, 2) != 1)
        return fail(e, "Unsupported wire envelope");
    for (unsigned i = 82; i < 96; ++i)
        if (b[i])
            return fail(e, "Reserved byte is nonzero");
    CduSnapshot s;
    s.unit = Unit(b[14]);
    s.source_state = SourceState(b[15]);
    std::copy_n(b.begin() + 16, 16, s.session.begin());
    s.generation = get(b, 32, 8);
    s.publication_sequence = get(b, 40, 8);
    s.cache_revision = get(b, 48, 8);
    s.host_elapsed_ns = get(b, 56, 8);
    s.appearance = std::uint32_t(get(b, 64, 4));
    s.brightness = std::bit_cast<std::int32_t>(std::uint32_t(get(b, 68, 4)));
    s.blanking = std::bit_cast<std::int32_t>(std::uint32_t(get(b, 72, 4)));
    s.text_valid = std::uint16_t(get(b, 76, 2));
    s.colour_valid = std::uint16_t(get(b, 78, 2));
    s.scalar_valid = b[80];
    s.source_label_size = b[81];
    if (s.source_label_size > 128 || b.size() != wire_base_bytes + s.source_label_size)
        return fail(e, "Invalid label/total length");
    unsigned at = 96;
    for (unsigned i = 0; i < s.source_label_size; ++i)
        s.source_label[i] = char(b[at++]);
    for (auto &r : s.receipts)
    {
        r = {get(b, at, 8), get(b, at + 8, 8)};
        at += 16;
    }
    for (auto &row : s.cells)
        for (auto &c : row)
        {
            c = {char(b[at]), Size(b[at + 1]), char(b[at + 2])};
            at += 3;
        }
    if (!valid(s, e))
        return false;
    out = s;
    return true;
}
} // namespace psx::cdu
