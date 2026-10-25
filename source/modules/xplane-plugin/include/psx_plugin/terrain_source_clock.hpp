#pragma once
#include <psx/terrain/protocol.hpp>

namespace psx::xp::terrain_source_clock {
// Query source stamps retain their immutable affine centers. Only source-age
// admission uses the conservative upper evaluation time supplied by transport.
inline bool fresh(const terrain::Reference& r,std::int64_t age_now,std::uint64_t frequency) noexcept {
    const auto limit=static_cast<std::int64_t>(frequency*terrain::stale_ms/1000);
    return age_now>=r.sample_qpc&&age_now>=r.received_qpc&&age_now-r.sample_qpc<limit&&age_now-r.received_qpc<limit;
}
inline bool fresh_output(const terrain::Reference& r,std::int64_t now,std::int64_t output_qpc,std::int64_t source_age_now,std::uint64_t frequency) noexcept {
    const auto limit=static_cast<std::int64_t>(frequency*terrain::stale_ms/1000);
    return now>=output_qpc&&now-output_qpc<limit&&fresh(r,source_age_now,frequency);
}
inline bool after_cutoff(std::int64_t received_center,std::int64_t cutoff,std::uint64_t margin) noexcept {
    // A source must be wholly after local invalidation, not merely have its
    // center after it. Unknown margins and subtraction underflow cannot qualify.
    return received_center>=0&&margin<=static_cast<std::uint64_t>(received_center)&&
        received_center-static_cast<std::int64_t>(margin)>=cutoff;
}
}
