#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace coordinator {
struct TerrainReplayResult {
    bool valid{},complete{};
    std::uint64_t qpc_frequency{},begin_qpc{},cutoff_qpc{},records{},raw_snapshots{};
    std::array<std::uint64_t,4> versions{};
    std::array<std::uint64_t,16> kinds{};
    std::string error;
};
// Read-only, bounded PXTCA1 inspection. Never opens transport or replays authority.
TerrainReplayResult inspect_terrain_capture(const std::wstring&);
}
