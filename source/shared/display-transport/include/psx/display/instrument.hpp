#pragma once
#include "transport.hpp"
#include <psx/display/instrument_protocol.hpp>

namespace psx::du::transport
{
std::vector<Source> discover_instruments();
bool instrument_identity_json(std::string_view);

struct InstrumentEnvelope
{
    std::array<uint8_t, 16> session{};
    Origin source;
    uint32_t instrument{psx::instrument::captain_stab_trim};
    State state{State::frame};
    uint64_t generation{}, sequence{}, offered{};
    std::span<const uint8_t> frame;
};
} // namespace psx::du::transport
