#pragma once
#include "terrain_provider.hpp"

namespace psx::xp {
// Read-only, opt-in SDK diagnostics. Never publishes a physical Surface.
class TerrainCapture final {
public:
    bool begin(TerrainProvider&) noexcept;
    void retire() noexcept;
    bool tick(const TerrainInput&,TerrainCaptureResult&) noexcept;
private:
    struct Check {
        terrain::ProbeDetail detail{};
        terrain::Query target{};
        double seed{},warm_height{};
        bool pending{},warm{},cold{};
    };
    TerrainProvider* provider_{};
    XPLMProbeRef probe_{};
    std::array<XPLMDataRef,3> xyz_{},angles_{};
    std::array<Check,2> checks_{};
    std::uint64_t connection_{},lease_{},capture_{},host_{};
    std::int64_t last_tick_{};
    unsigned next_{};
    bool sampled_{};
    terrain::Control domain_{};
    void clear() noexcept {checks_={};next_=0;}
};
}
