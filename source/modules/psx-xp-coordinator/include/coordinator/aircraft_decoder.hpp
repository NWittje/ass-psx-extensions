#pragma once
#include <psx/aircraft/state.hpp>
#include <cstdint>
#include <string_view>

namespace coordinator {
enum class DecodeResult {
    ok, unsupported, malformed, missing_session, missing_configuration,
    stale_sequence, configuration_overflow
};
const char* decode_result_name(DecodeResult result) noexcept;

// Offline PSX 10.180 record normalizer. No socket, clock, UI, state cache or writer.
// One owner serializes calls. Output and context change only on success.
class PsxAircraftDecoder {
public:
    // Epoch must be nonzero and greater than this instance's previous session.
    // Resets the engine configuration and accepted ingestion sequence.
    bool begin_session(std::uint64_t epoch) noexcept;
    // Offline checkpoint restore on a fresh decoder only. No sample is emitted.
    bool restore_context(std::uint64_t session, std::uint64_t configuration,
                         psx::aircraft::EngineFamily family, std::uint64_t sequence) noexcept;
    // Complete line WITHOUT CR/LF. Caller assigns one strictly increasing local
    // ingestion sequence across Main and Boost; zero is reserved. QPC is copied,
    // never interpreted as PSX source time. Negative QPC is rejected.
    DecodeResult decode_main(std::string_view line, std::uint64_t sequence,
                             std::int64_t receive_qpc, psx::aircraft::Observation& output) noexcept;
    DecodeResult decode_boost(std::string_view line, std::uint64_t sequence,
                              std::int64_t receive_qpc, psx::aircraft::Observation& output) noexcept;
    std::uint64_t session_epoch() const noexcept { return session_; }
    std::uint64_t configuration_epoch() const noexcept { return configuration_; }
    std::uint64_t last_sequence() const noexcept { return sequence_; }
    psx::aircraft::EngineFamily family() const noexcept { return family_; }
private:
    DecodeResult preflight(std::uint64_t sequence, std::int64_t qpc) const noexcept;
    std::uint64_t session_ = 0, configuration_ = 0, sequence_ = 0;
    psx::aircraft::EngineFamily family_ = psx::aircraft::EngineFamily::unknown;
};
}
