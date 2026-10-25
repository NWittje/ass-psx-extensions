#pragma once
#include "dpf_protocol.hpp"

namespace psx::instrument
{
inline constexpr std::uint32_t magic = 0x31495044;
inline constexpr std::uint32_t version = 1, captain_stab_trim = 1;
inline constexpr std::size_t frame_limit = 1024 * 1024, frame_header = 256;

// This is an instrument identity, never a physical DU number. The legacy
// decoded unit field is UINT32_MAX so accidental DU routing rejects it.
inline psx::recorder::Decoded decode(std::span<const std::uint8_t> bytes)
{
    return psx::recorder::decode_profile(bytes, true);
}
inline psx::recorder::Decoded decode(const std::vector<std::uint8_t> &bytes)
{
    return decode(std::span<const std::uint8_t>(bytes));
}

// DPF's page-less native early return bypasses its power predicate. Only the
// separately qualified DPI snapshot may establish powered blank eligibility.
// DPI bit7 denotes pending blank/show/renderer-clear transitions, not blankAll.
inline bool powered_lower_overlay(const psx::recorder::Decoded &base,
                                  const psx::recorder::Decoded &instrument,
                                  bool allow_page) noexcept
{
    constexpr std::uint64_t seen = (1ull << 0) | (1ull << 3) | (1ull << 5) |
                                   (1ull << 6) | (1ull << 7);
    constexpr std::uint64_t powered = (1ull << 3) | (1ull << 5);
    // An explicit client preference may ignore only the page-present result.
    // Power, completed native observations and transition checks still apply.
    const auto required = seen & ~(allow_page ? (1ull << 6) : 0ull);
    return !base.instrument_id && base.unit == 5 &&
           (base.kind == 2 || (allow_page && base.kind == 6)) &&
           base.assignment >= 10 && base.assignment <= 12 &&
           instrument.instrument_id == captain_stab_trim && instrument.kind != 3 &&
           instrument.lower_eligibility_revision == 1 &&
           instrument.lower_assignment == base.assignment &&
           (instrument.gates_seen & seen) == seen && (instrument.gates_yes & required) == powered;
}
inline bool powered_blank_lower(const psx::recorder::Decoded &base,
                                const psx::recorder::Decoded &instrument) noexcept
{
    return powered_lower_overlay(base, instrument, false);
}
} // namespace psx::instrument
