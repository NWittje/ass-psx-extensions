#pragma once
#include <psx/aircraft_sync/protocol.hpp>
namespace psx::xp {
namespace aircraft=aircraft_sync;
struct AircraftCommand {aircraft::Control control{};std::uint64_t connection{},revision{};std::int64_t received_qpc{};bool source_loss{};};
struct AircraftSample {aircraft::Sample sample{};std::uint64_t connection{},revision{};};
}
